"""Check Kuzu setup scripts (MATCH ... SET) on a copy of a store:

  original baseline setup  ==  input setup  ==  CICO-rewritten input setup

Every non-constant `SET v.p = e` statement is compared in its read form
`<prefix> RETURN id(v), e` (no write is executed for the comparison). The
indexes the rewrite reads are built on the copy first, with the middleware's
own build statements (`cico build`), so the store passed in stays untouched.

  <kuzu-python> tests/kuzu_setup_check.py --store DB --workdir DIR --queries bi-15 bi-19 --output r.json
"""
import argparse
import collections
import json
import math
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware  # noqa: E402
from cico.neo4j import render, tokenize  # noqa: E402
from cico.statements import classify, parse_fill, split_script, strip_comments  # noqa: E402
import kuzu_optimized_coverage as K  # noqa: E402
import workloads as W  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PREMISES = {
    'at most one KNOWS edge per ordered pair':
        'MATCH (a:PERSON)-[k:KNOWS]->(b:PERSON) WITH a, b, count(k) AS n WHERE n > 1 RETURN count(*)',
    'every POST lies in exactly one FORUM':
        'MATCH (p:POST) OPTIONAL MATCH (p)<-[c:CONTAINEROF]-(:FORUM) WITH p, count(c) AS n WHERE n <> 1 RETURN count(*)',
    'every message has at most one REPLYOF*0.. path to a POST':
        'MATCH (m:POST:COMMENT)-[:REPLYOF*0..]->(p:POST) WITH m, count(*) AS n WHERE n > 1 RETURN count(m)',
}


def norm(value):
    if isinstance(value, float):
        return float(f'{value:.12g}') if math.isfinite(value) else str(value)
    if isinstance(value, dict):
        return tuple(sorted((k, norm(v)) for k, v in value.items()))
    if isinstance(value, (list, tuple)):
        return tuple(norm(v) for v in value)
    return value


def read_form(statement):
    """`<prefix> SET v.p = e` -> `<prefix> RETURN id(v) AS edge, e AS value`,
    or None for DDL, constant resets and other statements."""
    text = strip_comments(statement).strip().rstrip(';')
    tokens = tokenize(text)
    depth, pos = 0, None
    for i, t in enumerate(tokens):
        depth += t.text in ('(', '[', '{')
        depth -= t.text in (')', ']', '}')
        if depth == 0 and t.kind == 'ident' and t.upper == 'SET':
            pos = i
    if pos is None:
        return None
    m = re.fullmatch(r'(\w+)\s*\.\s*(\w+)\s*=\s*(.+)', render(tokens[pos + 1:]), re.S)
    if not m or re.fullmatch(r'[-+0-9.eE]+|NULL|true|false', m[3].strip(), re.I):
        return None
    return f'{render(tokens[:pos])} RETURN id({m[1]}) AS edge, {m[3]} AS value'


def build_statements(index_script):
    out = subprocess.run([sys.executable, '-m', 'cico', 'build', '--schema', str(W.schema_path('ldbc-bi')),
                          '--dialect', 'kuzu', '--index-script', str(index_script)],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [s for s in split_script(out) if strip_comments(s).strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--store', required=True)
    parser.add_argument('--workdir', required=True)
    parser.add_argument('--queries', nargs='+', default=['bi-15', 'bi-19'])
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--buffer-pool-gb', type=int, default=32)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    import kuzu
    work = Path(args.workdir)
    copy = work / 'kuzu-db'
    report = dict(store=args.store, copy=str(copy), build=[], premises={}, checks=[])

    # Rewrites of the input setups (same registration as the coverage report).
    rewrites = {}
    with IndexMiddleware(W.schema_path('ldbc-bi'), dialect='kuzu',
                         constraints=K.constraints_for('ldbc-bi')) as mw:
        regs = K.register_indexes(mw, 'ldbc-bi')
        fills = {e['index_id']: e for e in regs if e['kind'] == 'fill' and e['ok']}
        for name in args.queries:
            path = K.setup_file('ldbc-bi', name)
            rewrites[name] = [mw.execute(s) for s in split_script(Path(path).read_text())
                              if classify(s) != 'noop']
        used = sorted({i for rs in rewrites.values() for r in rs for i in (r.indexes or [])})
        script = work / 'indexes.cypher'
        work.mkdir(parents=True, exist_ok=True)
        texts = []
        for index_id in used:
            for text in mw.fills[index_id]:
                fill = parse_fill(text)
                for column in fill.columns:
                    texts.append(f'ALTER TABLE {fill.table} ADD IF NOT EXISTS {column} INT64 DEFAULT 0')
                texts.append(text)
        script.write_text(';\n'.join(texts) + ';\n')
        report['indexes'] = used

    if not copy.exists():
        shutil.copytree(args.store, copy)
    db = kuzu.Database(str(copy), buffer_pool_size=args.buffer_pool_gb << 30)
    conn = kuzu.Connection(db, num_threads=args.threads)

    def run(text):
        result = conn.execute(text)
        rows = []
        while result.has_next():
            rows.append(result.get_next())
        result.close()
        return rows

    for statement in build_statements(script):
        t0 = time.time()
        run(statement)
        report['build'].append(dict(statement=statement[:160], seconds=round(time.time() - t0, 2)))
    print(f"built {len(report['build'])} statements", flush=True)
    for premise, cypher in PREMISES.items():
        report['premises'][premise] = run(cypher)[0][0]
        print(f'premise violations: {premise}: {report["premises"][premise]}', flush=True)

    bag = lambda rows: collections.Counter(tuple(norm(v) for v in r) for r in rows)  # noqa: E731
    for name in args.queries:
        original = W.DOCS / 'Kuzu/queries/baseline/ldbc-bi' / f'{name}-setup.cypher'
        edited = K.setup_file('ldbc-bi', name)
        forms = {
            'original': [f for f in map(read_form, split_script(original.read_text())) if f],
            'input': [f for f in map(read_form, split_script(Path(edited).read_text())) if f],
            'rewritten': [f for f in (read_form(r.cypher) for r in rewrites[name]) if f],
        }
        row = dict(query=name, forms={k: v for k, v in forms.items()},
                   rewritten_indexes=sorted({i for r in rewrites[name] for i in (r.indexes or [])}))
        if not (len(forms['original']) == len(forms['input']) == len(forms['rewritten']) == 1):
            row['error'] = 'expected one weight statement per setup'
        else:
            results = {}
            for kind, (text,) in forms.items():
                t0 = time.time()
                results[kind] = bag(run(text))
                row[f'{kind}_seconds'] = round(time.time() - t0, 2)
                row[f'{kind}_rows'] = sum(results[kind].values())
            row['input_equal'] = results['input'] == results['original']
            row['rewritten_equal'] = results['rewritten'] == results['original']
        report['checks'].append(row)
        print(f"{name}: rows={row.get('original_rows')} input_equal={row.get('input_equal')} "
              f"rewritten_equal={row.get('rewritten_equal')} "
              f"t={row.get('original_seconds')}/{row.get('input_seconds')}/{row.get('rewritten_seconds')} "
              f"{row.get('error', '')}", flush=True)
    conn.close()
    db.close()
    json.dump(report, open(args.output, 'w'), indent=1, default=str)


if __name__ == '__main__':
    main()

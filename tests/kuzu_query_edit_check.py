"""Semantic check of Kuzu query edits (docs Kuzu/queries/input/ldbc-bi).

For every edited Kuzu BI query and parameter sample, run the docs original and
the edited text on a Kuzu store (read-only, no index involved) and compare the
result multisets.  Data-model premises an edit relies on are counted on the
same store.  This checks the edit, not the middleware; the middleware's rewrite
of the edited text is checked by kuzu_bi_acceptance.py (EXPLAIN) and, with
built indexes, equivalence_check.py.

  <kuzu-python> middleware/tests/kuzu_query_edit_check.py --store <kuzu-bi-sf1> --output report.json \
      [--queries bi-2 bi-6 bi-16] [--samples 3]
"""
import argparse
import collections
import json
import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent), str(HERE)]
import workloads as W  # noqa: E402

# Premises of individual edits, as violation counts (each must be 0).
PREMISES = {
    'bi-6': {'every message has at most one creator':
             'MATCH (m:POST:COMMENT)-[r:HASCREATOR]->() WITH m, count(r) AS c WHERE c > 1 '
             'RETURN count(m) AS violations'},
    'bi-12': {'every message replies to at most one message':
              'MATCH (m:POST:COMMENT)-[r:REPLYOF]->() WITH m, count(r) AS c WHERE c > 1 '
              'RETURN count(m) AS violations',
              'every message has at most one REPLYOF*0.. path to a POST':
              'MATCH (m:POST:COMMENT)-[:REPLYOF*0..]->(p:POST) WITH m, count(*) AS c WHERE c > 1 '
              'RETURN count(m) AS violations'},
}


def norm(value):
    if isinstance(value, float):
        return float(f'{value:.9g}') if math.isfinite(value) else str(value)
    if isinstance(value, (list, tuple)):
        return tuple(norm(v) for v in value)
    if isinstance(value, dict):
        return tuple(sorted((k, norm(v)) for k, v in value.items()))
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--store', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--queries', nargs='*')
    parser.add_argument('--samples', type=int, default=1)
    parser.add_argument('--buffer-pool-gb', type=int, default=16)
    parser.add_argument('--threads', type=int, default=8)
    args = parser.parse_args()
    import kuzu
    db = kuzu.Database(args.store, buffer_pool_size=args.buffer_pool_gb << 30, read_only=True)
    conn = kuzu.Connection(db, num_threads=args.threads)

    def run(text, params):
        result = conn.execute(text, parameters={k: v for k, v in params.items() if '$' + k in text})
        rows = []
        while result.has_next():
            rows.append(result.get_next())
        result.close()
        return rows
    overlay = W.input_dir('kuzu', 'ldbc-bi')
    report = {'store': args.store, 'overlay': str(overlay), 'indexes_used': False, 'checks': [], 'premises': {}}
    for edited in sorted(overlay.glob('bi-*.cypher')):
        name = edited.stem
        if args.queries and name not in args.queries:
            continue
        original = W.DOCS / 'Kuzu/queries/baseline/ldbc-bi' / edited.name
        for premise, cypher in PREMISES.get(name, {}).items():
            report['premises'][f'{name}: {premise}'] = run(cypher, {})[0][0]
        for sample in range(args.samples):
            params = W.parameters('ldbc-bi', original, original.read_text() + edited.read_text(), sample)
            row = {'query': name, 'sample': sample, 'parameters': {k: str(v) for k, v in params.items()}}
            try:
                t0 = time.time()
                before = run(original.read_text(), params)
                t1 = time.time()
                after = run(edited.read_text(), params)
                t2 = time.time()
                key = lambda rows: collections.Counter(tuple(norm(v) for v in r) for r in rows)  # noqa: E731
                row.update(rows=len(before), equal=key(before) == key(after),
                           original_seconds=round(t1 - t0, 2), edited_seconds=round(t2 - t1, 2),
                           head=[list(map(str, r)) for r in before[:3]])
            except Exception as error:  # noqa: BLE001 - recorded
                row['error'] = f'{type(error).__name__}: {error}'[:400]
            report['checks'].append(row)
            print(f"{name:8} s{sample} equal={row.get('equal', '-')} rows={row.get('rows', '-')} "
                  f"{row.get('error', '')}", flush=True)
    conn.close()
    db.close()
    report['summary'] = {'checked': len(report['checks']),
                         'equal': sum(1 for c in report['checks'] if c.get('equal')),
                         'premise_violations': report['premises']}
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps(report, indent=1, ensure_ascii=False, default=str) + '\n')
    print(json.dumps(report['summary'], ensure_ascii=False))
    sys.exit(0 if all(c.get('equal') for c in report['checks']) and not any(report['premises'].values()) else 1)


if __name__ == '__main__':
    main()

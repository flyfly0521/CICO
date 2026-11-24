"""Rewrite coverage of the baseline queries with the NeuG indexes.

For each workload, one middleware session (dialect kuzu):

1. register every per-query NeuG index directory
   `NeuG/queries/index/<workload>/<key>/{index,parameter}.cypher`
   (parameter.cypher is filled with manifest sample 0 of the matching query,
   derived the way the Kuzu parameter scripts derive their windows);
   DROP statements (rebuild hygiene) and write statements (zero resets) are
   skipped, every FILL / ALTER ADD / CREATE REL TABLE is sent to CICO;
2. rewrite every query of `Kuzu/queries/baseline/<workload>/` (the docs
   Kuzu/queries/input version when present, as in workload_coverage.py) and
   its setup, parameters = sample 0 (the optimized file of the same name, if
   any, is reported as well);
3. for BI, check the windowed and staged weight setups and rerun overlapping
   FILL definitions in private catalogs. Count resulting baseline rewrites
   separately from the already optimized queries.

  python3 tests/kuzu_optimized_coverage.py --output cov.json [--workloads ldbc-bi ...]
  python3 tests/kuzu_optimized_coverage.py --system neo4j --output cov.json

With --system neo4j the same NeuG indexes are registered in a Neo4j profile
(schema mapping profiles/<workload>-neo4j.yaml, shared constraints) and the
Neo4j/queries/{optimized,baseline,input} files are rewritten; in step 3 the
windowed setup is the Neo4j bi-15 projection (input bi-15-create-graph) and
the staged weights are input/ldbc-bi/weights/*-create-graph.cypher.
"""
import argparse
import collections
import datetime
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware  # noqa: E402
from cico.statements import classify, parse_fill, split_script, strip_comments  # noqa: E402
import workloads as W  # noqa: E402

CONSTRAINTS = Path(__file__).resolve().parents[1] / 'constraints'
DROP = re.compile(r'^\s*(DROP\s+TABLE|ALTER\s+TABLE\s+\S+\s+DROP)\b', re.I)
DAY = datetime.timedelta(days=1)
SYSTEM = 'kuzu'                         # set by main(); 'kuzu' or 'neo4j'


def system_dir():
    return W.SYSTEM_DIRS[SYSTEM]


def new_middleware(workload, scope='part'):
    """A middleware session for SYSTEM on the workload's target schema."""
    if SYSTEM == 'neo4j':
        from cico.neo4j import neo4j_profile
        constraints = constraints_for(workload)
        return IndexMiddleware(W.schema_path(workload),
                               profile=neo4j_profile(W.mapping_path(workload), constraints), scope=scope)
    return IndexMiddleware(W.schema_path(workload), dialect='kuzu', constraints=constraints_for(workload),
                           scope=scope)


def constraints_for(workload):
    path = CONSTRAINTS / 'ldbc-snb.yaml'
    return str(path) if workload in ('ldbc-bi', 'ldbc-ic') and path.is_file() else None


def index_key(workload, name):
    """Kuzu query file stem -> NeuG index directory name."""
    if workload == 'ldbc-bi':
        return name.replace('-', '')                       # bi-10 -> bi10
    if workload == 'ldbc-ic':
        return 'ic' + name.rsplit('-', 1)[1]               # interactive-complex-3 -> ic3
    if workload == 'finbench':
        return name.replace('-', '')                       # tcr-10 -> tcr10
    return name


def owner_key(workload, key):
    """NeuG index directory -> query key it belongs to (bi19a -> bi19, bi15w -> bi15)."""
    return re.sub(r'(?<=\d)[a-z]$', '', key) if workload == 'ldbc-bi' else key


def baseline_file(workload, name):
    path = W.DOCS / system_dir() / 'queries/baseline' / workload / f'{name}.cypher'
    return W.input_file(path) or path


def query_params(workload, path):
    return W.parameters(workload, str(path), Path(path).read_text()) if Path(path).is_file() else {}


def index_parameters(workload, key):
    """Values for a NeuG parameter.cypher, from sample 0 of the owning query
    (same derivation as the Kuzu parameter scripts)."""
    name = {'ldbc-bi': lambda k: 'bi-' + k[2:]}.get(workload, lambda k: k)(owner_key(workload, key))
    path = W.DOCS / system_dir() / 'queries/baseline' / workload / f'{name}.cypher'
    if key == 'bi15w':
        # The shortest-path query reads a precomputed weight, so its text
        # does not mention the dates needed to build the index.
        raw = W.parameters(workload, path, '$startDate $endDate')
    else:
        raw = query_params(workload, path)
    p = dict(raw)
    if key == 'bi2':        # Kuzu bi2: [date, date+100d), [date+100d, date+200d)
        p.update(windowStart=raw['date'], windowMid=raw['date'] + 100 * DAY, windowEnd=raw['date'] + 200 * DAY)
    if key == 'bi16':       # Kuzu bi16: CAST(creationDate AS DATE) = dateX
        for x in 'AB':
            day = datetime.datetime.combine(raw[f'date{x}'].date(), datetime.time())
            p[f'date{x}Start'], p[f'date{x}End'] = day, day + DAY
    return p, raw


def fill_template(text, key, raw):
    """Positional '{}' templates (filled by the NeuG runner)."""
    text = strip_comments(text)
    if '{}' not in text:
        return text
    if key == 'bi13':
        return text.replace('{}', raw['endDate'].strftime('%Y-%m-%d'))
    if key == 'bi15w':
        start, end = raw.get('startDate'), raw.get('endDate')
        if not isinstance(start, datetime.datetime) or not isinstance(end, datetime.datetime):
            raise ValueError('BI15 window FILLs require startDate and endDate')
        start, end = start.isoformat(sep=' '), end.isoformat(sep=' ')
        # Use distinct columns so the sample's windowed counts and weight
        # can coexist with BI15's unwindowed definitions in one catalog.
        fwd, post_fwd = 'bi15w_reply_fwd', 'bi15w_post_fwd'
        rev, post_rev = 'bi15w_reply_rev', 'bi15w_post_rev'
        values = (fwd, post_fwd, start, end, rev, post_rev, start, end,
                  'bi15w_weight', fwd, rev, post_fwd, post_rev)
        if text.count('{}') != len(values):
            raise ValueError(f'BI15 window script requires {len(values)} positional placeholders')
        replacements = iter(values)
        rendered = re.sub(r'\{\}', lambda _: next(replacements), text)
        # The count FILLs are sparse. The weight FILL needs a zero for
        # any direction without a match, including after a rebuild.
        declarations = ''.join(
            f'ALTER TABLE KNOWS ADD IF NOT EXISTS {column} INT64 DEFAULT 0;\n'
            for column in (fwd, post_fwd, rev, post_rev)
        )
        return declarations + rendered
    raise ValueError('positional {} template without a known runner mapping')


def register_indexes(mw, workload, keys=None):
    """DDL first, then FILLs; FILLs with more target columns first, so that a
    multi-column definition (e.g. bi15's KNOWS(bi14_case1_fwd, bi15_post_fwd))
    owns its carriers before a single-column definition of the same column
    (bi14's KNOWS(bi14_case1_fwd)) is offered (stable order otherwise)."""
    root = W.DOCS / 'NeuG/queries/index' / workload
    out, ddl, fills = [], [], []
    for directory in sorted(p for p in root.iterdir() if p.is_dir()):
        key = directory.name
        if keys is not None and key not in keys:
            continue
        for script in ('index.cypher', 'parameter.cypher'):
            path = directory / script
            if not path.is_file():
                continue
            text = path.read_text()
            params, raw = index_parameters(workload, key) if script == 'parameter.cypher' or '{}' in text else ({}, {})
            template_error = None
            try:
                text = fill_template(text, key, raw)
            except ValueError as error:
                template_error = str(error)
            for statement in split_script(text):
                kind = classify(statement)
                body = strip_comments(statement).strip()
                entry = dict(key=key, file=f'{key}/{script}', kind=kind, statement=re.sub(r'\s+', ' ', body)[:160])
                if kind in ('noop',):
                    continue
                if kind == 'ddl' and DROP.match(body):
                    continue                     # rebuild hygiene; carriers are created fresh here
                if kind == 'query':              # MATCH ... SET x = 0: zero reset (DEFAULT covers it)
                    continue
                if template_error and '{}' in statement:
                    out.append(dict(entry, ok=False, reason='template', detail=template_error))
                    continue
                if kind == 'fill' and params:
                    fill = parse_fill(statement)
                    from cico.middleware import inline_parameters
                    statement = f"FILL {fill.table}({', '.join(fill.columns)}) FROM ({inline_parameters(fill.source, params)})"
                (fills if kind == 'fill' else ddl).append((entry, statement))
    fills.sort(key=lambda item: -len(parse_fill(item[1]).columns))
    for entry, statement in ddl + fills:
        r = mw.execute(statement)
        out.append(dict(entry, ok=r.ok, reason=r.reason, index_id=r.index_id,
                        detail=(r.detail or '')[:300]))
    return out


def setup_file(workload, name):
    """The setup of a baseline query (input version when present): Kuzu
    <name>-setup.cypher; Neo4j <name>-project-graph / -create-graph (GDS)."""
    suffixes = ['setup'] if SYSTEM == 'kuzu' else ['setup', 'project-graph', 'create-graph']
    for suffix in suffixes:
        path = W.DOCS / system_dir() / 'queries/baseline' / workload / f'{name}-{suffix}.cypher'
        edited = W.input_dir(SYSTEM, workload) / path.name
        if W.USE_INPUT and edited.is_file():
            return edited
        if path.is_file():
            return path
    return None


def rewrite_setup(mw, path, parameters=None):
    statements = [mw.execute(s, parameters=parameters).to_dict()
                  for s in split_script(Path(path).read_text()) if classify(s) != 'noop']
    return dict(file=str(path), statements=statements, rewritten=any(r['rewritten'] for r in statements),
                indexes=sorted({i for r in statements if r['rewritten'] for i in r['indexes']}))


def bi_index_coverage(mw, indexes, queries, scope):
    """Count actual substitutes in BI inputs, including setups and overlapping FILLs."""
    workload = 'ldbc-bi'

    def registered(rows):
        return {r['index_id'] for r in rows if r['kind'] == 'fill' and r['ok']}

    def baseline(middleware, name):
        path = baseline_file(workload, name)
        return middleware.execute(path.read_text(), parameters=query_params(workload, path)).to_dict()

    catalog = registered(indexes)
    used = {i for q in queries for branch in ('baseline', 'setup')
            if q.get(branch, {}).get('rewritten') for i in q[branch]['indexes']}
    primary = sorted(catalog & used)
    staged = []
    shifted_window = None
    if W.USE_INPUT:
        inputs = W.input_dir(SYSTEM, workload)
        window = inputs / 'windowed/bi-15-windowed-setup.cypher'
        if SYSTEM == 'neo4j':
            # The Neo4j bi-15 projection is itself windowed (LDBC reference).
            window = setup_file(workload, 'bi-15') or window
        if window.is_file():
            params = index_parameters(workload, 'bi15w')[1]
            staged.append(rewrite_setup(mw, window, params))
            shifted_window = rewrite_setup(mw, window, {**params, 'endDate': params['endDate'] + DAY})
        pattern = '*-setup.cypher' if SYSTEM == 'kuzu' else '*-create-graph.cypher'   # Neo4j: GDS projections
        staged.extend(rewrite_setup(mw, path) for path in sorted((inputs / 'weights').glob(pattern)))
        used.update(i for result in staged for i in result['indexes'])

    # These equivalent candidates can win the same QueryPart. Remove the
    # selected directions in this disposable catalog and check the others.
    bi14 = next(q['baseline'] for q in queries if q['query'] == 'bi-14')
    competitors = [i for i in bi14['indexes'] if i.startswith('KNOWS(')]
    for index in competitors:
        mw.drop_index(index)
    alternate = baseline(mw, 'bi-14')
    if alternate['rewritten']:
        used.update(alternate['indexes'])
    covered = catalog & used

    # Check the four FILL definitions rejected by the combined catalog under
    # their own case's registrations. Two ROOT_POST definitions share an ID.
    isolated = []
    for key in ('bi3', 'bi9', 'bi14'):
        name = 'bi-' + key[2:]
        with new_middleware(workload, scope) as local:
            definitions = register_indexes(local, workload, {key})
            result = baseline(local, name)
        isolated.append(dict(case=name, registration=definitions, result=result))
    all_registered = catalog.union(*(registered(row['registration']) for row in isolated))
    all_used = used | {i for row in isolated if row['result']['rewritten'] for i in row['result']['indexes']}
    return dict(main_catalog=sorted(catalog), primary=primary, covered=sorted(covered),
                uncovered=sorted(catalog - used), setups=staged, shifted_window=shifted_window,
                bi14_alternate=dict(removed=competitors, result=alternate), isolated=isolated,
                all_registered=sorted(all_registered), all_covered=sorted(all_registered & all_used),
                all_uncovered=sorted(all_registered - all_used))


def run(workload, scope):
    out = dict(workload=workload, indexes=[], queries=[])
    with new_middleware(workload, scope) as mw:
        out['indexes'] = register_indexes(mw, workload)
        # The query set is every baseline query of the system (setup and graph
        # projection scripts belong to their query); the input version is used
        # when present. The optimized file of the same name is checked too.
        root = W.DOCS / system_dir() / 'queries/baseline' / workload
        for base in sorted(root.glob('*.cypher')):
            if W.SKIP.search(base.name) or 'project-graph' in base.name:
                continue
            name = base.stem
            path = W.DOCS / system_dir() / 'queries/optimized' / workload / f'{name}.cypher'
            row = dict(query=name, key=index_key(workload, name))
            for variant, file in (('optimized', path), ('baseline', baseline_file(workload, name))):
                if not file.is_file():
                    row[variant] = dict(reason='missing_file')
                    continue
                text = file.read_text()
                if W.is_write(text):
                    row[variant] = dict(reason='write_query', rewritten=False)
                    continue
                r = mw.execute(text, parameters=query_params(workload, file))
                d = r.to_dict()
                row[variant] = {k: d.get(k) for k in ('rewritten', 'reason', 'indexes', 'parts', 'specialized', 'detail', 'cypher')}
                row[variant]['file'] = str(file)
            setup = setup_file(workload, name)
            if setup is not None:
                row['setup'] = rewrite_setup(mw, setup, query_params(workload, setup) or None)
            out['queries'].append(row)
        if workload == 'ldbc-bi':
            out['bi_index_coverage'] = bi_index_coverage(mw, out['indexes'], out['queries'], scope)
    return out


def summarize(report):
    lines = []
    total = collections.Counter()
    for res in report:
        w = res['workload']
        fills = [e for e in res['indexes'] if e['kind'] == 'fill']
        ok = [e for e in fills if e['ok']]
        used = collections.Counter()
        for q in res['queries']:
            for v in ('optimized', 'baseline'):
                for i in q.get(v, {}).get('indexes') or []:
                    used[i] += 1
        lines.append(f'== {w}: FILL {len(ok)}/{len(fills)} registered; '
                     f'optimized queries {len(res["queries"])}')
        for e in res['indexes']:
            if not e['ok']:
                lines.append(f'   [index fail] {e["file"]:26} {e["reason"]}: {e["detail"][:140]}')
        for q in res['queries']:
            for i in (q.get('setup') or {}).get('indexes') or []:
                used[i] += 1
        for kind in ('optimized', 'baseline'):
            c = collections.Counter(q[kind]['reason'].split(':')[0] for q in res['queries'])
            n = sum(1 for q in res['queries'] if q[kind].get('rewritten')
                    or (kind == 'baseline' and (q.get('setup') or {}).get('rewritten')))
            total[kind] += n
            total[kind + '_n'] += len(res['queries'])
            lines.append(f'   {kind:9} rewritten {n}/{len(res["queries"])}  {dict(c)}')
        for q in res['queries']:
            o, b = q['optimized'], q['baseline']
            lines.append(f'   {q["query"]:24} opt={o["reason"]:22} {",".join(o.get("indexes") or [])[:40]:40} '
                         f'base={b["reason"]:22} {",".join(b.get("indexes") or [])[:60]}')
            if q.get('setup'):
                st = q['setup']
                lines.append(f'   {"":24} setup {Path(st["file"]).parent.parent.name}/{Path(st["file"]).name}: '
                             f'{"rewritten" if st["rewritten"] else "not rewritten"} '
                             f'[{", ".join(x["reason"] for x in st["statements"])}] {",".join(st["indexes"])[:80]}')
        if w == 'ldbc-bi' and 'bi_index_coverage' in res:
            bi = res['bi_index_coverage']
            used.update(bi['covered'])
            lines.append(f'   original and setup rewrites in the combined catalog '
                         f'{len(bi["primary"])}/{len(bi["main_catalog"])}')
            lines.append(f'   with staged setups and equivalent candidates '
                         f'{len(bi["covered"])}/{len(bi["main_catalog"])}; '
                         f'with isolated conflicting definitions {len(bi["all_covered"])}/{len(bi["all_registered"])}')
            if bi['all_uncovered']:
                lines.append('   unmatched: ' + ', '.join(bi['all_uncovered']))
        total['fill'] += len(fills)
        total['fill_ok'] += len(ok)
        total['fill_used'] += len({e['index_id'] for e in ok if e['index_id'] in used})
        total['fill_ids'] += len({e['index_id'] for e in ok})
    lines.append(f'TOTAL: FILL registered {total["fill_ok"]}/{total["fill"]}; '
                 f'distinct registered indexes used by some query {total["fill_used"]}/{total["fill_ids"]}; '
                 f'optimized rewritten {total["optimized"]}/{total["optimized_n"]}; '
                 f'baseline rewritten {total["baseline"]}/{total["baseline_n"]}')
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--workloads', nargs='*', default=list(W.WORKLOADS))
    parser.add_argument('--scope', default='part', choices=['part', 'query'])
    parser.add_argument('--system', default='kuzu', choices=['kuzu', 'neo4j'])
    parser.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    global SYSTEM
    SYSTEM = args.system
    report = [run(w, args.scope) for w in args.workloads]
    json.dump(report, open(args.output, 'w'), indent=1, ensure_ascii=False, default=str)
    text = summarize(report)
    Path(args.output).with_suffix('.txt').write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()

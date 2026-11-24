"""Rewrite coverage of the NeuG baseline queries with the NeuG indexes, through
the CICO middleware (dialect neug) instead of NeuG's own index handling.

For each workload, one middleware session on the NeuG database schema
(`<CICO_NEUG_DBS>/<db>/graph.yaml`):

1. every NeuG DDL/FILL of `NeuG/queries/index/<workload>/` is sent to CICO
   (`kuzu_optimized_coverage.register_indexes`: same order, templates and
   parameter values as the Kuzu/Neo4j coverage runs);
2. the query set, the baseline setup of a case and the filling of positional
   `{}` templates come from the NeuG runner itself
   (`NeuG/scripts/benchmark/run_workload.py`: queries_for, baseline_setup_path,
   fill_template, is_write_query); it is imported, never run, and nothing is
   executed on a database;
3. parameters: sample 0 of the shared manifests (the indexes' parameter
   values), converted by the NeuG runner's own functions; window values
   derived like the indexes' (so parameter-dependent FILLs see the values
   they were built for);
4. a case's baseline setup (FILL/DDL of baseline-owned columns) is executed
   through the middleware first, then the query is rewritten.

  python3 tests/neug_coverage.py --output cov.json [--workloads ldbc-bi ...]
      [--query-set baseline|optimization]

--query-set optimization checks NeuG/queries/optimized: the NeuG rewrite
inputs (queries kept in their graph-pattern form for the rewriter, see
NeuG/queries/optimized/README.md), the role of Kuzu/Neo4j queries/input.
Dialect neug is NeuG itself: the queries reach the compiler unchanged.
"""
import argparse
import collections
import importlib.util
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware  # noqa: E402
from cico.statements import classify, split_script  # noqa: E402
import kuzu_optimized_coverage as K  # noqa: E402
import workloads as W  # noqa: E402

RUNNER_NAMES = {'ldbc-ic': 'ic-sf1', 'ldbc-bi': 'bi-sf1', 'lsqb': 'lsqb-sf1',
                'finbench': 'finbench-sf1', 'hetionet': 'hetionet'}


def neug_runner():
    """The docs' NeuG runner module (loaded under its own name: the Kuzu
    runner that workloads.py imports is also called run_workload)."""
    name = 'neug_run_workload'
    if name not in sys.modules:
        path = W.DOCS / 'NeuG/scripts/benchmark/run_workload.py'
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


# The NeuG experiment databases' schemas are the docs' import configs; the
# local databases under CICO_NEUG_DBS are older imports (e.g. IC joinDate as
# INT64 there, TIMESTAMP in the experiments).  Hetionet's schema is generated
# by its import script, so the local database's is used.
IMPORT_SCHEMAS = {'ldbc-bi': 'ldbc/graph-bi.yaml', 'ldbc-ic': 'ldbc/graph.yaml',
                  'finbench': 'finbench/graph-finbench.yaml', 'lsqb': 'lsqb/graph-lsqb.yaml'}


def neug_schema(workload):
    if workload in IMPORT_SCHEMAS:
        return W.DOCS / 'NeuG/scripts/import/configs' / IMPORT_SCHEMAS[workload]
    return W.NEUG_DBS / W.WORKLOADS[workload]['schema'] / 'graph.yaml'


def case_of(rel):
    """`bi14a/query.cypher` -> bi14a, `tcr-11.cypher` -> tcr-11."""
    parts = Path(rel).parts
    return parts[0] if len(parts) > 1 else Path(rel).stem


def case_parameters(workload, case):
    """(params, display) of manifest sample 0 for the case, converted by the
    NeuG runner itself (its defaults, parse_ic_row / parse_finbench_row /
    coerce_value), plus the derived window values the index scripts use
    (index_parameters)."""
    R = neug_runner()
    params = dict(R.default_params_for(RUNNER_NAMES[workload]))
    display = dict(params)
    spec = W.WORKLOADS[workload]
    raw = None
    if spec['manifest']:
        rows = W._manifest_rows(spec['manifest'])
        raw = next((rows[k][0] for k in W._manifest_key(workload, case) if k in rows), None)
    if raw is not None:
        if workload == 'finbench':
            number = int(re.search(r'(\d+)$', W._manifest_key(workload, case)[0])[1])
            ordered = [raw[k] for k in sorted(raw, key=lambda k: int(k.split('_')[1]))]
            try:
                p, d = R.parse_finbench_row(number, ordered, simple=case.startswith('tsr'))
            except ValueError:          # the runner has no column mapping: keep its defaults
                p, d = {}, {}
        elif workload == 'ldbc-ic':
            p, d = R.parse_ic_row(list(raw), list(raw.values()))
        else:
            p, d = {}, {}
            for spec_key, value in raw.items():
                key, _, kind = spec_key.partition(':')
                p[key] = R.coerce_value(value, kind or None, key)
                d[key] = R.display_value(value, kind or None)
        params.update(p)
        display.update(d)
    if workload == 'finbench':
        # The runner passes the window as epoch milliseconds, which NeuG's
        # execution compares with DATETIME properties as that instant; CICO
        # binds parameters by value type, so it is given the same UTC instant
        # typed as a datetime.
        for name in ('start_time', 'end_time'):
            if isinstance(params.get(name), int):
                params[name] = R.epoch_millis_to_datetime(params[name])
    key =re.sub(r'^(bi\d+)[a-z]$', r'\1', case.replace('-', '')) if workload == 'ldbc-bi' else case
    if key in ('bi2', 'bi16'):
        derived, _ = K.index_parameters(workload, key)
        params.update(derived)
        display.update(derived)
    return params, display


def index_dirs_of(workload, case):
    """NeuG index directories belonging to a query case (bi15a -> bi15, bi15u, bi15w)."""
    root = W.DOCS / 'NeuG/queries/index' / workload
    key = case.replace('-', '') if workload == 'finbench' else case
    base = re.sub(r'(?<=\d)[a-z]$', '', key) if workload == 'ldbc-bi' else key
    return sorted(p.name for p in root.iterdir() if p.is_dir()
                  and (p.name == key or K.owner_key(workload, p.name) == base))


def run_statements(mw, text, params):
    out = []
    for statement in split_script(text):
        if classify(statement) == 'noop':
            continue
        used = {k: v for k, v in params.items() if '$' + k in statement}
        out.append(mw.execute(statement, parameters=used or None).to_dict())
    return out


def run(workload, query_set='baseline'):
    R = neug_runner()
    wl = R.workload_for_query_set(R.WORKLOADS[RUNNER_NAMES[workload]], query_set)
    K.SYSTEM = 'kuzu'      # index parameter derivation as in the Kuzu/Neo4j runs
    out = dict(workload=workload, query_set=query_set, indexes=[], queries=[])
    with IndexMiddleware(neug_schema(workload), dialect='neug', constraints=K.constraints_for(workload)) as mw:
        out['indexes'] = K.register_indexes(mw, workload)
        for path in R.queries_for(wl, include_setup=False):
            rel = R.rel_path(wl, path)
            case = case_of(rel)
            text = R.strip_query(path.read_text())
            row = dict(query=rel, case=case, index_dirs=index_dirs_of(workload, case), file=str(path))
            if not text:
                out['queries'].append(dict(row, reason='empty', rewritten=False))
                continue
            if R.is_write_query(wl, rel, text) or W.is_write(text):
                out['queries'].append(dict(row, reason='write_query', rewritten=False))
                continue
            values, display = case_parameters(workload, case)
            setup = R.baseline_setup_path(path)
            if setup is not None:
                statements = run_statements(mw, R.strip_query(setup.read_text()), values)
                row['setup'] = dict(file=str(setup), statements=[
                    {k: s.get(k) for k in ('kind', 'ok', 'rewritten', 'reason', 'index_id', 'indexes', 'detail')}
                    for s in statements])
            try:
                if '{}' in text:                        # as the runner's choose_params
                    text = R.fill_template(wl, rel, text, {**display, **values})
                    params = {}
                else:
                    params = R.params_used_by(text, values)
            except Exception as error:                  # noqa: BLE001  (template without a mapping)
                out['queries'].append(dict(row, reason='template', rewritten=False, detail=str(error)[:300]))
                continue
            statements = [s for s in split_script(text) if classify(s) != 'noop']
            results = [mw.execute(s, parameters={k: v for k, v in params.items() if '$' + k in s} or None).to_dict()
                       for s in statements]
            queries = [r for r in results if r.get('kind') == 'query'] or results
            main = queries[-1]
            row.update(rewritten=any(r.get('rewritten') for r in queries),
                       reason='rewritten' if any(r.get('rewritten') for r in queries) else main.get('reason'),
                       indexes=sorted({i for r in queries if r.get('rewritten') for i in r.get('indexes') or []}),
                       detail=' | '.join((r.get('detail') or '')[:400] for r in queries if r.get('detail')),
                       statements=len(statements), specialized=main.get('specialized'),
                       cypher=[r.get('cypher') for r in queries if r.get('rewritten')])
            out['queries'].append(row)
    return out


def summarize(report):
    lines, total = [], collections.Counter()
    for res in report:
        w = res['workload']
        fills = [e for e in res['indexes'] if e['kind'] == 'fill']
        ok = {e['index_id'] for e in fills if e['ok']}
        used = {i for q in res['queries'] for i in q.get('indexes') or []}
        reads = [q for q in res['queries'] if q.get('reason') != 'write_query']
        done = [q for q in reads if q.get('rewritten')]
        lines.append(f'== {w}: FILL {sum(e["ok"] for e in fills)}/{len(fills)} registered; '
                     f'queries rewritten {len(done)}/{len(reads)} (+{len(res["queries"]) - len(reads)} write); '
                     f'indexes read {len(ok & used)}/{len(ok)}')
        for e in fills:
            if not e['ok']:
                lines.append(f'   [index fail] {e["file"]:26} {e["reason"]}: {e["detail"][:160]}')
        lines.append(f'   {dict(collections.Counter(q.get("reason") for q in res["queries"]))}')
        for q in res['queries']:
            mark = 'OK ' if q.get('rewritten') else '-- '
            own = ','.join(q['index_dirs']) or '(no index dir)'
            lines.append(f'   {mark}{q["query"]:42} {q.get("reason", ""):22} own={own:22} '
                         f'{",".join(q.get("indexes") or [])[:90]}')
        if ok - used:
            lines.append('   indexes not read: ' + ', '.join(sorted(ok - used)))
        total.update(fill=len(fills), fill_ok=len(ok), used=len(ok & used), reads=len(reads), done=len(done))
    lines.append(f'TOTAL: FILL registered {total["fill_ok"]}/{total["fill"]}; indexes read {total["used"]}/'
                 f'{total["fill_ok"]}; queries rewritten {total["done"]}/{total["reads"]} (read-only)')
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--workloads', nargs='*', default=list(W.WORKLOADS))
    parser.add_argument('--query-set', default='baseline', choices=['baseline', 'optimization'])
    parser.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    report = [run(w, args.query_set) for w in args.workloads]
    json.dump(report, open(args.output, 'w'), indent=1, ensure_ascii=False, default=str)
    print(summarize(report))


if __name__ == '__main__':
    main()

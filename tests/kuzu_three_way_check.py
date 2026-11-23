"""Three-way check on a real Kuzu store: baseline == input == CICO(input).

  <kuzu-python> tests/kuzu_three_way_check.py --workload lsqb --store STORE --workdir DIR \
      --queries q2 q4 --inputs INPUT_DIR [--samples 3] [--skip-build] --output r.json

* the store is copied into DIR once (--skip-build reuses the copy and its
  indexes); every NeuG FILL of the workload is registered the way
  tests/kuzu_optimized_coverage.py does and built with the middleware's own
  statements (ddl, reset, fill);
* baseline = docs Kuzu/queries/baseline/<workload>/<name>.cypher;
  input = INPUT_DIR/<name>.cypher (missing: the baseline itself);
* rows are compared as bags (floats within 1e-9 relative: summation order
  may differ), column names and value types are compared as well;
* --manifest selects another parameter manifest (finbench-sf10 for the SF10
  store), --set overrides parameters, --skip-build reuses the store copy and
  --build-index (re)builds only the matching indexes on it.
"""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
import kuzu_optimized_coverage as K  # noqa: E402
import workloads as W  # noqa: E402
import math  # noqa: E402


def _key(row):
    return tuple(('f', f'{v:.6g}') if isinstance(v, float) else (type(v).__name__ == 'Decimal' and 'v' or 'v', str(v))
                 for v in row)


def same(a, b):
    """Bag equality; floats within 1e-9 relative (summation order may differ)."""
    if len(a) != len(b):
        return False
    for x, y in zip(sorted(a, key=_key), sorted(b, key=_key)):
        if len(x) != len(y):
            return False
        for u, v in zip(x, y):
            if isinstance(u, float) or isinstance(v, float):
                if u is None or v is None or not math.isclose(float(u), float(v), rel_tol=1e-9, abs_tol=1e-9):
                    return False
            elif u != v:
                return False
    return True


def types_of(rows):
    return sorted({tuple(type(v).__name__ for v in row) for row in rows})


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--workload', required=True)
    p.add_argument('--store', required=True)
    p.add_argument('--workdir', required=True)
    p.add_argument('--queries', nargs='+', required=True)
    p.add_argument('--inputs', required=True)
    p.add_argument('--samples', type=int, default=1)
    p.add_argument('--threads', type=int, default=8)
    p.add_argument('--buffer-pool-gb', type=int, default=16)
    p.add_argument('--skip-build', action='store_true')
    p.add_argument('--build-index', nargs='*', default=[],
                   help='with --skip-build: (re)build only indexes whose id contains one of these')
    p.add_argument('--manifest', help='override the workload parameter manifest (e.g. finbench-sf10)')
    p.add_argument('--set', nargs='*', default=[], help='parameter overrides name=python-literal')
    p.add_argument('--output', required=True)
    args = p.parse_args()
    import ast
    overrides = {k: ast.literal_eval(v) for k, v in (item.split('=', 1) for item in args.set)}
    if args.manifest:
        W.WORKLOADS[args.workload]['manifest'] = args.manifest
    import kuzu

    copy = Path(args.workdir) / 'kuzu-db'
    if not args.skip_build:
        if copy.exists():
            shutil.rmtree(copy) if copy.is_dir() else copy.unlink()
        Path(args.workdir).mkdir(parents=True, exist_ok=True)
        shutil.copytree(args.store, copy) if Path(args.store).is_dir() else shutil.copy(args.store, copy)
    db = kuzu.Database(str(copy), buffer_pool_size=args.buffer_pool_gb << 30)
    conn = kuzu.Connection(db, num_threads=args.threads)

    def run(text, params):
        t0 = time.time()
        result = conn.execute(text, parameters=params)
        names = result.get_column_names()
        rows = []
        while result.has_next():
            rows.append(result.get_next())
        result.close()
        return names, rows, round(time.time() - t0, 3)

    report = dict(workload=args.workload, store=args.store, copy=str(copy), inputs=args.inputs,
                  build=[], checks=[])
    wl = args.workload
    with IndexMiddleware(W.schema_path(wl), dialect='kuzu', constraints=K.constraints_for(wl)) as mw:
        registered = K.register_indexes(mw, wl)
        report['registration'] = [e for e in registered if not e['ok']]
        if not args.skip_build or args.build_index:
            built = mw.build_statements(include_internal_state=True)
            if args.skip_build:
                built = [e for e in built if any(t in e['index_id'] for t in args.build_index)]
            # all ddl, then all reset, then all fill (a reset must never run
            # after another index's fill)
            for phase in ('ddl', 'reset', 'fill_statements'):
                for entry in built:
                    for statement in entry[phase]:
                        t0 = time.time()
                        try:
                            conn.execute(statement)
                            status = 'ok'
                        except Exception as error:  # noqa: BLE001
                            status = f'error: {str(error)[:300]}'
                        report['build'].append(dict(index_id=entry['index_id'], phase=phase, status=status,
                                                    seconds=round(time.time() - t0, 2), statement=statement[:300]))
                        if status != 'ok':
                            print(f'[build] {entry["index_id"]} {phase}: {status}')
            print('build done:', sum(b['status'] == 'ok' for b in report['build']), 'ok /', len(report['build']))
        for name in args.queries:
            base_path = W.DOCS / 'Kuzu/queries/baseline' / wl / f'{name}.cypher'
            input_path = Path(args.inputs) / f'{name}.cypher'
            base = base_path.read_text()
            text = input_path.read_text() if input_path.is_file() else base
            for sample in range(min(args.samples, W.samples(wl, str(base_path)))):
                params = W.parameters(wl, str(base_path), base, sample)
                params.update({k: v for k, v in overrides.items() if '$' + k in base or '$' + k in text})
                bparams = {k: v for k, v in params.items() if '$' + k in base}
                iparams = {k: v for k, v in params.items() if '$' + k in text}
                r = mw.execute(text, parameters=iparams)
                entry = dict(query=name, sample=sample, input=str(input_path) if input_path.is_file() else None,
                             reason=r.reason, indexes=r.indexes, specialized=r.specialized,
                             detail=(r.detail or '')[:300], rewritten=r.cypher if r.rewritten else None,
                             parameters={k: str(v) for k, v in params.items()})
                try:
                    bn, brows, bs = run(base, bparams)
                    inames, irows, isec = run(text, iparams)
                    entry.update(rows=len(brows), baseline_s=bs, input_s=isec,
                                 input_equal=same(brows, irows), input_names_equal=bn == inames)
                    if r.rewritten:
                        used = {k: v for k, v in iparams.items() if '$' + k in r.cypher}
                        rn, rrows, rs = run(r.cypher, used)
                        entry.update(rewritten_s=rs, rewritten_equal=same(brows, rrows),
                                     rewritten_names_equal=bn == rn,
                                     rewritten_types_equal=types_of(brows) == types_of(rrows) or not brows)
                        if not entry['rewritten_types_equal']:
                            entry.update(baseline_types=types_of(brows)[:3], rewritten_types=types_of(rrows)[:3])
                        if not entry['rewritten_equal']:
                            entry.update(expected=repr(brows[:3])[:400], got=repr(rrows[:3])[:400])
                    if not entry['input_equal']:
                        entry.update(expected=repr(brows[:3])[:400], input_got=repr(irows[:3])[:400])
                    if not entry['input_names_equal'] or not entry.get('rewritten_names_equal', True):
                        entry.update(baseline_names=bn, input_names=inames)
                except Exception as error:  # noqa: BLE001
                    entry.update(error=str(error)[:400])
                report['checks'].append(entry)
                print(f"{name}#{sample:<3} {r.reason:22} rows={entry.get('rows', '-')} "
                      f"input_eq={entry.get('input_equal')} rw_eq={entry.get('rewritten_equal')} "
                      f"names={entry.get('input_names_equal')}/{entry.get('rewritten_names_equal')} "
                      f"types={entry.get('rewritten_types_equal')} "
                      f"{entry.get('baseline_s')}s/{entry.get('input_s')}s/{entry.get('rewritten_s')}s "
                      f"{entry.get('error', '')[:200]}", flush=True)
    conn.close()
    db.close()
    json.dump(report, open(args.output, 'w'), indent=1, default=str, ensure_ascii=False)


if __name__ == '__main__':
    main()

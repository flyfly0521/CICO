"""Does Kuzu compile what the middleware outputs?  (EXPLAIN only: nothing runs.)

  <kuzu-python> tests/kuzu_compile_check.py --workload lsqb --store COPY [--manifest M] --output r.json

COPY is a Kuzu store the index DDL may be applied to (a copy, e.g. the one
tests/kuzu_three_way_check.py made).  Every NeuG FILL of the workload is
registered as tests/kuzu_optimized_coverage.py does; their DDL (ALTER ... ADD
IF NOT EXISTS, CREATE REL TABLE IF NOT EXISTS, hidden FILL state columns) is
applied to COPY, no data is written.  Then, for every baseline query (input
version when present) and every setup statement, the rewritten text and the
original text are compiled with `EXPLAIN` and the given parameters.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
from cico.statements import classify, split_script  # noqa: E402
import kuzu_optimized_coverage as K  # noqa: E402
import workloads as W  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--workload', required=True)
    p.add_argument('--store', required=True)
    p.add_argument('--manifest')
    p.add_argument('--output', required=True)
    args = p.parse_args()
    import kuzu
    wl = args.workload
    if args.manifest:
        W.WORKLOADS[wl]['manifest'] = args.manifest
    db = kuzu.Database(args.store, buffer_pool_size=8 << 30)
    conn = kuzu.Connection(db, num_threads=4)

    def explain(text, params):
        try:
            conn.execute('EXPLAIN ' + text, parameters=params).close()
            return 'ok'
        except Exception as error:  # noqa: BLE001
            return str(error)[:400]

    report = dict(workload=wl, store=args.store, ddl=[], checks=[])
    with IndexMiddleware(W.schema_path(wl), dialect='kuzu', constraints=K.constraints_for(wl)) as mw:
        K.register_indexes(mw, wl)
        for entry in mw.build_statements(include_internal_state=True):
            for statement in entry['ddl']:
                status = None
                try:
                    conn.execute(statement)
                    status = 'ok'
                except Exception as error:  # noqa: BLE001
                    status = str(error)[:200]
                if status != 'ok' and 'already exists' not in status:
                    report['ddl'].append(dict(index_id=entry['index_id'], statement=statement, status=status))
        root = W.DOCS / 'Kuzu/queries/optimized' / wl
        items = []
        for path in sorted(root.glob('*.cypher')):
            base = K.baseline_file(wl, path.stem)
            if base.is_file():
                items.append((path.stem, base, [base.read_text()]))
            setup = K.setup_file(wl, path.stem)
            if setup is not None:
                statements = split_script(Path(setup).read_text())
                for statement in statements:          # the setup's own DDL (its weight column)
                    if classify(statement) == 'ddl':
                        try:
                            conn.execute(statement)
                        except Exception:  # noqa: BLE001
                            pass
                items.append((path.stem + '-setup', setup, [s for s in statements if classify(s) == 'query']))
        for name, path, texts in items:
            original_base = W.DOCS / 'Kuzu/queries/baseline' / wl / f'{name}.cypher'
            for text in texts:
                if W.is_write(text) and not name.endswith('-setup'):
                    continue
                params = K.query_params(wl, original_base if original_base.is_file() else path) \
                    if not name.endswith('-setup') else {}
                params = {k: v for k, v in params.items() if '$' + k in text}
                r = mw.execute(text, parameters=params or None)
                entry = dict(query=name, file=str(path), reason=r.reason, indexes=r.indexes,
                             original=explain(text, params))
                if r.rewritten:
                    used = {k: v for k, v in params.items() if '$' + k in r.cypher}
                    entry['rewritten'] = explain(r.cypher, used)
                    if entry['rewritten'] != 'ok':
                        entry['cypher'] = r.cypher
                report['checks'].append(entry)
                print(f"{name:26} {r.reason:22} original={entry['original'][:60]:60} "
                      f"rewritten={entry.get('rewritten', '-')[:200]}", flush=True)
    conn.close()
    db.close()
    bad = [c for c in report['checks'] if c.get('rewritten') not in (None, 'ok')]
    print(f"\n{wl}: {sum(c.get('rewritten') == 'ok' for c in report['checks'])} rewritten outputs compile, "
          f"{len(bad)} do not; DDL problems: {len(report['ddl'])}")
    json.dump(report, open(args.output, 'w'), indent=1, default=str, ensure_ascii=False)


if __name__ == '__main__':
    main()

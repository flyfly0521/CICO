"""Run docs baseline/input queries and their CICO rewrites on a Kuzu store that
already holds the indexes (read-only) and compare the result bags.

  <kuzu-python> tests/kuzu_rewrite_compare.py --store DB --queries bi-5 bi-12 --samples 3 --output r.json
"""
import argparse
import collections
import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware  # noqa: E402
import kuzu_optimized_coverage as K  # noqa: E402
import workloads as W  # noqa: E402


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
    parser.add_argument('--workload', default='ldbc-bi')
    parser.add_argument('--queries', nargs='+', required=True)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    import kuzu
    db = kuzu.Database(args.store, buffer_pool_size=16 << 30, read_only=True)
    conn = kuzu.Connection(db, num_threads=args.threads)

    def run(text, params):
        result = conn.execute(text, parameters={k: v for k, v in params.items() if '$' + k in text})
        rows = []
        while result.has_next():
            rows.append(result.get_next())
        result.close()
        return rows

    rows = []
    with IndexMiddleware(W.schema_path(args.workload), dialect='kuzu',
                         constraints=K.constraints_for(args.workload)) as mw:
        K.register_indexes(mw, args.workload)
        for name in args.queries:
            path = K.baseline_file(args.workload, name)
            text = Path(path).read_text()
            for sample in range(args.samples):
                params = W.parameters(args.workload, str(path), text, sample)
                r = mw.execute(text, parameters=params)
                row = dict(query=name, file=str(path), sample=sample, reason=r.reason,
                           indexes=r.indexes, parameters={k: str(v) for k, v in params.items()})
                if r.rewritten:
                    t0 = time.time()
                    before = run(text, params)
                    t1 = time.time()
                    after = run(r.cypher, params)
                    t2 = time.time()
                    bag = lambda rs: collections.Counter(tuple(norm(v) for v in x) for x in rs)  # noqa: E731
                    row.update(rows=len(before), equal=bag(before) == bag(after),
                               original_seconds=round(t1 - t0, 2), rewritten_seconds=round(t2 - t1, 2),
                               rewritten=r.cypher)
                rows.append(row)
                print(f"{name:8} s{sample} {r.reason:20} equal={row.get('equal')} rows={row.get('rows')} "
                      f"t={row.get('original_seconds')}->{row.get('rewritten_seconds')} {','.join(r.indexes or [])}",
                      flush=True)
    json.dump(dict(store=args.store, rows=rows), open(args.output, 'w'), indent=1, default=str)


if __name__ == '__main__':
    main()

"""Compare two runs of dump_all_queries.py / neug_fill_bi_status.py.

  python3 tests/dialect_regression.py BASELINE_DIR AFTER_DIR

Every query record must keep rewritten/reason/output/indexes/specialized;
every NeuG-FILL BI case must keep rewritten/reason/output/indexes/premises.
Exit status 1 on any difference (each one is printed).
"""
import json
import sys
from pathlib import Path

QUERY_KEYS = ('rewritten', 'reason', 'output', 'indexes', 'specialized', 'parts')
CASE_KEYS = ('rewritten', 'reason', 'output', 'indexes', 'premises', 'specialized')


def records(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        row = json.loads(line)
        if row.get('type') == 'fills':
            out[('fills',)] = [(f['ok'], f['reason']) for f in row['fills']]
        else:
            out[(row.get('name'), row.get('overlay'))] = {k: row.get(k) for k in QUERY_KEYS}
    return out


def main(before, after):
    before, after = Path(before), Path(after)
    diffs, compared = 0, 0
    for old in sorted(before.glob('*.jsonl')):
        new = after / old.name
        a, b = records(old), records(new)
        for key in sorted(set(a) | set(b), key=str):
            compared += 1
            if a.get(key) != b.get(key):
                diffs += 1
                print(f'{old.name} {key}:\n  before {a.get(key)}\n  after  {b.get(key)}')
    for old in sorted(before.glob('*neug-fill-bi.json')):
        a = {c['case']: {k: c.get(k) for k in CASE_KEYS} for c in json.loads(old.read_text())['cases']}
        b = {c['case']: {k: c.get(k) for k in CASE_KEYS}
             for c in json.loads((after / old.name).read_text())['cases']}
        for key in sorted(set(a) | set(b)):
            compared += 1
            if a.get(key) != b.get(key):
                diffs += 1
                print(f'{old.name} {key}:\n  before {a.get(key)}\n  after  {b.get(key)}')
    print(f'{compared} records compared, {diffs} differ')
    return 1 if diffs else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))

"""Kuzu BI spot check: NeuG FILL definitions -> Kuzu plans, EXPLAIN, rewrites.

Inputs follow the experiment docs: index definitions from
`NeuG/queries/index/ldbc-bi`, original queries from
`Kuzu/queries/baseline/ldbc-bi`, parameters from the SF1 manifest (sample 0
unless --samples). Stages:

  1. registration: every `<case>/index.cypher` imported into a private
     index registry (kuzu dialect); record plans and EXPLAIN them on a copy of the Kuzu
     BI store (DDL inside a rolled-back transaction, nothing is written);
  2. rewrite: register each case (parameter files rendered as in
     neug_fill_bi_status.py) and rewrite the Kuzu original once; EXPLAIN the
     output on the store copy after the case's carrier DDL (rolled back);
  3. --build FILE: import the NeuG script FILE into an index registry whose
     executor is KuzuIndexExecutor on the store copy (the generated
     DDL/reset/fill statements really run), restart (must build nothing), rewrite
     every Kuzu BI query and EXPLAIN outputs whose indexes are all active on
     the built store. --compare-results instead executes original and
     rewritten queries and compares result multisets (slow).

  <kuzu-python> middleware/tests/kuzu_bi_acceptance.py --store <kuzu-bi-sf1> \
      --workdir /tmp/cico_kuzu_bi --output report.json [--build middleware/indexes/ldbc-bi-native.cypher]
"""
import argparse
import collections
import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path[:0] = [str(ROOT), str(HERE)]
import workloads as W  # noqa: E402
from cico import IndexMiddleware, IndexRegistry  # noqa: E402
from cico.targets import KuzuIndexExecutor  # noqa: E402
from cico.statements import classify  # noqa: E402
from neug_fill_bi_status import register_case  # noqa: E402

SCHEMA = ROOT / 'schemas/ldbc-bi.yaml'
# Declared LDBC constraints (internal names): enables the constraint_retry rule.
CONSTRAINTS = ROOT / 'constraints/ldbc-snb.yaml'


def git_head(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def used_parameters(query, parameters):
    """Kuzu rejects parameters a statement does not use; a specialized
    rewrite has some of them inlined as literals."""
    return {k: v for k, v in (parameters or {}).items() if re.search(r'\$`?' + re.escape(k) + r'\b', query)}


def explain_after_ddl(runner, ddl, query, parameters):
    """EXPLAIN `query` once carrier DDL is visible; roll everything back."""
    with runner.rolled_back():
        for statement in ddl:
            runner.query(statement)
        runner.query('EXPLAIN ' + query, used_parameters(query, parameters))


def norm(value):
    if isinstance(value, float):
        return float(f'{value:.9g}') if math.isfinite(value) else str(value)
    if isinstance(value, list):
        return tuple(norm(v) for v in value)
    if isinstance(value, dict):
        return tuple(sorted((k, norm(v)) for k, v in value.items()))
    return value


def registration(docs, runner, tmp):
    rows = []
    for path in sorted((docs / 'NeuG/queries/index/ldbc-bi').glob('*/index.cypher')):
        row = {'case': path.parent.name, 'file': str(path.relative_to(docs)),
               'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        try:
            with IndexMiddleware(SCHEMA, profile='kuzu', constraints=CONSTRAINTS) as mw:
                registry = IndexRegistry(mw, Path(tmp) / f'{path.parent.name}.yaml',
                                         Path(tmp) / f'{path.parent.name}.json', 'offline-kuzu-audit')
                result = registry.import_script(path.read_text())
            statements = [s for item in result['plans'] for phase in ('ddl', 'reset', 'fill_statements')
                          for s in item[phase]]
            row.update(status='planned' if result['pending'] else 'schema_only', indexes=result['pending'],
                       dependencies=result['dependencies'], fingerprints=result['fingerprints'],
                       plans=result['plans'], statements=len(statements))
            if statements:
                try:
                    runner.validate(statements)
                    row['kuzu_explain'] = 'ok'
                except Exception as error:  # noqa: BLE001 - recorded
                    row.update(kuzu_explain='failed', explain_error=f'{type(error).__name__}: {error}'[:600])
                finally:
                    runner.release()
        except Exception as error:  # noqa: BLE001 - recorded
            row.update(status='unsupported', error=f'{type(error).__name__}: {error}'[:600])
        rows.append(row)
        print(f"[register] {row['case']:6} {row['status']:12} {row.get('kuzu_explain', ''):6} "
              f"{','.join(row.get('indexes', []))[:70]} {row.get('error', row.get('explain_error', ''))[:120]}",
              flush=True)
    return rows


def query_path(docs, number):
    original = docs / f'Kuzu/queries/baseline/ldbc-bi/bi-{number}.cypher'
    return original, W.input_file(original)


def rewrites(docs, runner):
    rows = []
    for directory in sorted((docs / 'NeuG/queries/index/ldbc-bi').iterdir(), key=lambda p: p.name):
        if not directory.is_dir():
            continue
        number = int(re.match(r'bi(\d+)', directory.name).group(1))
        original, overlay = query_path(docs, number)
        for variant, path in (('docs', original), ('query_edit', overlay)):
            if path is None or not path.is_file():
                continue
            text = path.read_text()
            params = W.parameters('ldbc-bi', original, text)
            row = {'case': directory.name, 'variant': variant, 'query': str(path),
                   'parameters': {k: str(v) for k, v in params.items()}}
            started = time.time()
            with IndexMiddleware(SCHEMA, profile='kuzu', constraints=CONSTRAINTS) as mw:
                try:
                    log = register_case(mw, directory, number, params)
                except Exception as error:  # noqa: BLE001 - e.g. an unrendered template
                    log = [dict(file='*', ok=False, reason=type(error).__name__, detail=str(error)[:200])]
                result = mw.execute(text, parameters=params)
                ddl = [s for item in mw.build_statements(include_internal_state=True) if item['ok']
                       for s in item['ddl']]
                row.update(registered=sorted(mw.fills), registration_failures=[e for e in log if not e['ok']],
                           rewritten=result.rewritten, reason=result.reason, indexes=result.indexes,
                           specialized=result.specialized, premises=result.premises,
                           detail=(result.detail or '')[:800], output=result.cypher if result.rewritten else '',
                           seconds=round(time.time() - started, 1))
            if result.rewritten:
                try:
                    explain_after_ddl(runner, ddl, result.cypher, params)
                    row['kuzu_explain'] = 'ok'
                except Exception as error:  # noqa: BLE001 - recorded
                    row.update(kuzu_explain='failed', explain_error=f'{type(error).__name__}: {error}'[:600])
                finally:
                    runner.release()
            rows.append(row)
            print(f"[rewrite]  {row['case']:6} {variant:10} {str(row['rewritten']):5} {row['reason']:24} "
                  f"{row.get('kuzu_explain', ''):6} {','.join(row['indexes'])[:70]}", flush=True)
    return rows


def build_and_compare(docs, runner, fill_file, state_file, samples, timeout_rows, execute=False):
    registry_file = state_file.with_name('registry.yaml')
    out = {'fill_file': str(fill_file), 'registry': str(registry_file), 'state_file': str(state_file)}
    with IndexMiddleware(SCHEMA, profile='kuzu', constraints=CONSTRAINTS) as mw:
        registry = IndexRegistry(mw, registry_file, state_file, runner.target, executor=runner)
        started = time.time()
        report = registry.import_script(fill_file.read_text())
        out['reload'] = {k: v for k, v in report.items() if k != 'plans'}
        out['reload_seconds'] = round(time.time() - started, 1)
    with IndexMiddleware(SCHEMA, profile='kuzu', constraints=CONSTRAINTS) as mw:     # restart on the same registry and receipts
        registry = IndexRegistry(mw, registry_file, state_file, runner.target, executor=runner)
        started = time.time()
        again = registry.start()
        out['second_reload'] = {'built': again['built'], 'pending': again['pending'],
                                'unchanged': again['unchanged'], 'seconds': round(time.time() - started, 1)}
        active = set(report['active'])
        comparisons = []
        for path in sorted((docs / 'Kuzu/queries/baseline/ldbc-bi').glob('bi-*.cypher'),
                           key=lambda p: int(re.match(r'bi-(\d+)', p.name)[1]) if re.match(r'bi-(\d+)\.', p.name)
                           else 99):
            if not re.fullmatch(r'bi-\d+\.cypher', path.name) or W.is_write(path.read_text()):
                continue
            _, overlay = query_path(docs, int(re.match(r'bi-(\d+)', path.name)[1]))
            for variant, qpath in (('docs', path), ('query_edit', overlay)):
                if qpath is None:
                    continue
                text = qpath.read_text()
                for sample in range(samples):
                    params = W.parameters('ldbc-bi', path, text, sample)
                    result = mw.execute(text, parameters=params)
                    row = {'query': path.name, 'variant': variant, 'sample': sample, 'rewritten': result.rewritten,
                           'reason': result.reason, 'indexes': result.indexes}
                    if result.rewritten and set(result.indexes) <= active and not execute:
                        # Carriers are physically built: the output must bind
                        # and plan on the real store. Nothing is executed.
                        try:
                            runner.query('EXPLAIN ' + result.cypher, used_parameters(result.cypher, params))
                            row.update(kuzu_explain='ok', output=result.cypher)
                        except Exception as error:  # noqa: BLE001 - recorded
                            row.update(kuzu_explain='failed', output=result.cypher,
                                       explain_error=f'{type(error).__name__}: {error}'[:600])
                    elif result.rewritten and set(result.indexes) <= active:
                        t0 = time.time()
                        before = runner.query(text, params)
                        t1 = time.time()
                        after = runner.query(result.cypher, used_parameters(result.cypher, params))
                        t2 = time.time()
                        equal = (collections.Counter(tuple(norm(v) for v in r) for r in before)
                                 == collections.Counter(tuple(norm(v) for v in r) for r in after))
                        row.update(equal=equal, rows=len(before), original_seconds=round(t1 - t0, 3),
                                   rewritten_seconds=round(t2 - t1, 3), output=result.cypher,
                                   sample_rows=[list(map(str, r)) for r in before[:timeout_rows]])
                    comparisons.append(row)
                    print(f"[compare]  {path.name:12} {variant:10} s{sample} {str(result.rewritten):5} "
                          f"{result.reason:22} explain={row.get('kuzu_explain', '-')} equal={row.get('equal', '-')} "
                          f"{row.get('original_seconds', '')}s -> {row.get('rewritten_seconds', '')}s", flush=True)
        runner.release()
        out['comparisons'] = comparisons
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--docs', default=str(W.DOCS))
    parser.add_argument('--store', required=True, help='Kuzu BI SF1 store (copied, never modified)')
    parser.add_argument('--workdir', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--buffer-pool-gb', type=float, default=16)
    parser.add_argument('--reuse-copy', action='store_true')
    parser.add_argument('--skip-registration', action='store_true')
    parser.add_argument('--skip-rewrite', action='store_true')
    parser.add_argument('--build', help='Managed NeuG FILL file to materialize on the copy, then compare')
    parser.add_argument('--samples', type=int, default=1)
    parser.add_argument('--compare-results', action='store_true',
                        help='After --build, execute original and rewritten queries and compare (slow); '
                             'default only EXPLAINs the rewritten output on the built store')
    args = parser.parse_args()
    docs = Path(args.docs)
    workdir = Path(args.workdir)
    copy = workdir / 'kuzu-bi'
    if not (args.reuse_copy and copy.exists()):
        if copy.exists():
            shutil.rmtree(copy)
        workdir.mkdir(parents=True, exist_ok=True)
        shutil.copytree(args.store, copy)
    runner = KuzuIndexExecutor(copy, args.threads, args.buffer_pool_gb)
    report = {'system': 'kuzu', 'docs_commit': git_head(docs), 'middleware_commit': git_head(ROOT),
              'store': args.store, 'store_copy': str(copy), 'target': json.loads(runner.target),
              'schema': str(SCHEMA), 'index_input': 'NeuG/queries/index/ldbc-bi',
              'queries': 'Kuzu/queries/baseline/ldbc-bi', 'query_edits': str(W.input_dir('kuzu', 'ldbc-bi')) if W.USE_INPUT else None, 'sample': 0,
              'registration_explain_only': True,
              'workload_queries_executed': bool(args.build and args.compare_results)}
    try:
        with tempfile.TemporaryDirectory(prefix='cico-kuzu-bi-') as tmp:
            if not args.skip_registration:
                report['registration'] = registration(docs, runner, tmp)
            if not args.skip_rewrite:
                report['rewrites'] = rewrites(docs, runner)
        if args.build:
            report['build'] = build_and_compare(docs, runner, Path(args.build).resolve(),
                                                workdir / 'build-state.json', args.samples, 5,
                                                execute=args.compare_results)
    finally:
        runner.close()
        reg, rew = report.get('registration', []), report.get('rewrites', [])
        report['summary'] = {
            'registration': dict(collections.Counter(r['status'] for r in reg)),
            'registration_explain': dict(collections.Counter(r.get('kuzu_explain', 'n/a') for r in reg)),
            'explained_statements': sum(r.get('statements', 0) for r in reg if r.get('kuzu_explain') == 'ok'),
            'rewritten_docs': sorted(r['case'] for r in rew if r['rewritten'] and r['variant'] == 'docs'),
            'rewritten_query_edits': sorted(r['case'] for r in rew if r['rewritten'] and r['variant'] == 'query_edit'),
            'rewrite_explain': dict(collections.Counter(r.get('kuzu_explain', 'n/a') for r in rew if r['rewritten'])),
        }
        if 'build' in report:
            compared = [c for c in report['build']['comparisons'] if 'equal' in c]
            report['summary']['compared'] = len(compared)
            report['summary']['equal'] = sum(c['equal'] for c in compared)
            built = report['build']
            report['summary']['materialized'] = built['reload']['built']
            report['summary']['active'] = built['reload']['active']
            report['summary']['second_reload_built'] = built['second_reload']['built']
            explained = [c for c in built['comparisons'] if 'kuzu_explain' in c]
            report['summary']['rewritten_on_built_store'] = sorted({c['query'] + ':' + c['variant']
                                                                    for c in explained})
            report['summary']['built_store_explain'] = dict(collections.Counter(c['kuzu_explain']
                                                                                for c in explained))
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(json.dumps(report, indent=1, ensure_ascii=False, default=str) + '\n')
        print(json.dumps(report['summary'], ensure_ascii=False))


if __name__ == '__main__':
    main()

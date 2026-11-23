"""CLI.

  python3 -m cico rewrite --schema S.yaml [--dialect neug|kuzu|neo4j] [--index-script I.cypher] \
        --query "MATCH ..." [--param-types '{"tag":"STRING"}'] [--json]
  python3 -m cico script  --schema S.yaml [--dialect ...] --file statements.cypher   # DDL/FILL/queries
  python3 -m cico serve   --schema S.yaml [--dialect ...] [--index-script I.cypher] [--port 8765]
  python3 -m cico build   --schema S.yaml --dialect kuzu|neo4j --index-script I.cypher [--json]
        # target-dialect statements that materialize the indexes (DDL, reset, fill)
  python3 -m cico schema  --kuzu DB [--reference graph.yaml] [--output S.yaml]   # schema from a Kuzu store

  Index registry (written by the middleware from FILL / DROP_INDEX; build
  receipts per physical target in --index-state):
  python3 -m cico serve  --schema S.yaml --dialect kuzu --registry R.yaml --index-state state.json \
        --kuzu-db /path/to/kuzu-db [--materialize-new]       # or --dialect neo4j --neo4j-uri bolt://...
  python3 -m cico import --schema S.yaml --dialect kuzu --registry R.yaml --index-state state.json \
        --kuzu-db DB SCRIPT.cypher ...     # one-off import of NeuG DDL+FILL scripts (e.g. experiment docs)
"""
import argparse
import json
import os
import sys
from contextlib import ExitStack
from pathlib import Path

from .middleware import IndexMiddleware


def _common(parser):
    parser.add_argument('--schema', required=True, help='NeuG schema YAML of the target graph')
    parser.add_argument('--stats', help='NeuG statistics JSON (cost-based index choice)')
    parser.add_argument('--dialect', '--profile', dest='profile', default='neug',
                        help='Target dialect declaration: neug | kuzu | neo4j (or a registered one). '
                             'Queries are read and rewrites printed in it; FILL is always NeuG')
    parser.add_argument('--constraints', help='Shared constraints file of the dataset '
                        '(constraints/<dataset>.yaml); overrides the dialect declaration\'s own')
    parser.add_argument('--index-script', help='DDL/FILL script registered at startup')
    parser.add_argument('--index-script-dialect', choices=('profile', 'neug'), default='profile',
                        help='Explicit input dialect for --index-script; output still uses --profile')
    parser.add_argument('--registry', help='Index registry file, written by the middleware from FILL / '
                        'DROP_INDEX statements (created if missing)')
    parser.add_argument('--index-state', help='Build receipts of the registry for this physical target')
    parser.add_argument('--index-target', help='Physical database generation identity (offline planning only)')
    parser.add_argument('--fill-params', default='{}', help='JSON scalar parameters to inline into imported FILLs')
    parser.add_argument('--neo4j-uri', help='Neo4j target; identity is checked using db.info()')
    parser.add_argument('--neo4j-database', default='neo4j')
    parser.add_argument('--neo4j-user', help='Password read from CICO_NEO4J_PASSWORD; omit for auth-disabled DB')
    parser.add_argument('--kuzu-db', help='Embedded Kuzu target (opened only while reloading); '
                        'identity = resolved path + device/inode')
    parser.add_argument('--kuzu-threads', type=int, default=4)
    parser.add_argument('--kuzu-buffer-pool-gb', type=float, default=8,
                        help='Kuzu buffer pool while building (0 = Kuzu default, most of the RAM)')
    parser.add_argument('--materialize-new', action='store_true',
                        help='Build pending registry indexes on the target (--neo4j-uri or --kuzu-db) '
                             'before enabling them; without it they stay inactive')
    parser.add_argument('--neo4j-index-script', action='append', default=[],
                        help='Neo4j materialization script (MATCH/SET or derived MERGE); repeatable')
    parser.add_argument('--no-verify', action='store_true')
    parser.add_argument('--engine')
    parser.add_argument('--engine-log')


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == 'schema':
        from .introspect import main as schema_main
        return schema_main(argv[1:])
    parser = argparse.ArgumentParser(prog='cico')
    commands = parser.add_subparsers(dest='command', required=True)
    rewrite = commands.add_parser('rewrite')
    _common(rewrite)
    rewrite.add_argument('--query', required=True)
    rewrite.add_argument('--param-types', default='{}')
    rewrite.add_argument('--json', action='store_true')
    script = commands.add_parser('script')
    _common(script)
    script.add_argument('--file', required=True)
    build = commands.add_parser('build')
    _common(build)
    build.add_argument('--json', action='store_true')
    server = commands.add_parser('serve')
    _common(server)
    server.add_argument('--host', default='127.0.0.1')
    server.add_argument('--port', type=int, default=8765)
    importer = commands.add_parser('import')
    _common(importer)
    importer.add_argument('scripts', nargs='+', help='NeuG DDL/FILL scripts to import into --registry')
    args = parser.parse_args(argv)

    if args.neo4j_uri and args.kuzu_db:
        parser.error('Choose one target: --neo4j-uri or --kuzu-db')
    if args.command == 'import' and not args.registry:
        parser.error('import requires --registry')
    if args.registry and (not args.index_state or not (args.index_target or args.neo4j_uri or args.kuzu_db)):
        parser.error('--registry requires --index-state and --kuzu-db/--neo4j-uri '
                     '(or --index-target for offline plans)')
    if args.registry and (args.index_script or args.neo4j_index_script):
        parser.error('With --registry, indexes come from FILL statements (or `cico import`), not index scripts')
    if args.materialize_new and not (args.registry and (
            (args.neo4j_uri and args.profile == 'neo4j') or (args.kuzu_db and args.profile == 'kuzu'))):
        parser.error('--materialize-new requires --registry and a matching target: '
                     '--dialect kuzu --kuzu-db, or --dialect neo4j --neo4j-uri')

    with ExitStack() as stack:
        mw = stack.enter_context(IndexMiddleware(args.schema, args.stats, dialect=args.profile, verify=not args.no_verify,
                                                engine_binary=args.engine, engine_log=args.engine_log,
                                                constraints=args.constraints))
        registry, executor = None, None
        if args.registry:
            from .registry import IndexRegistry
            from .targets import KuzuIndexExecutor, Neo4jIndexExecutor
            target = args.index_target
            if args.kuzu_db:
                runner = KuzuIndexExecutor(args.kuzu_db, args.kuzu_threads, args.kuzu_buffer_pool_gb)
                stack.callback(runner.close)
                target = runner.target
                if args.materialize_new:
                    executor = runner
            if args.neo4j_uri:
                auth = (args.neo4j_user, os.environ['CICO_NEO4J_PASSWORD']) if args.neo4j_user else None
                runner = Neo4jIndexExecutor(args.neo4j_uri, args.neo4j_database, auth)
                stack.callback(runner.close)
                target = runner.target
                if args.materialize_new:
                    executor = runner
            registry = IndexRegistry(mw, args.registry, args.index_state, target, executor=executor)
            if args.command == 'import':
                for path in args.scripts:
                    registry.import_script(Path(path).read_text(), json.loads(args.fill_params))
            report = registry.start()
            print(json.dumps({k: v for k, v in report.items() if k != 'plans'}, ensure_ascii=False), file=sys.stderr)
        for path in args.neo4j_index_script:
            mw.register_neo4j_index(Path(path).read_text())
        if args.index_script:
            text = Path(args.index_script).read_text()
            if args.index_script_dialect == 'neug':
                from .statements import classify, split_script
                for statement in split_script(text):
                    kind = classify(statement)
                    if kind == 'ddl':
                        mw.apply_ddl(statement, input_dialect='neug')
                    elif kind == 'fill':
                        mw.register_index([statement], input_dialect='neug')
                    elif kind != 'noop':
                        raise ValueError('--index-script-dialect neug accepts only DDL/FILL')
            else:
                for result in mw.execute_script(text):
                    if not result.ok:
                        print(f'warning: {result.reason}: {result.detail[:200]}', file=sys.stderr)
        if args.command == 'rewrite':
            result = mw.execute(args.query, parameter_types=json.loads(args.param_types))
            if args.json:
                print(json.dumps(result.to_dict(), indent=2, ensure_ascii=False))
            else:
                print(result.cypher)
                print(f'-- {result.reason} {result.indexes} {result.detail[:300]}'.rstrip(), file=sys.stderr)
        elif args.command == 'build':
            built = (registry.last_report['plans'] if registry else
                     mw.build_statements(include_internal_state=True))  # private FILL state, as the registry builds it
            if args.json:
                print(json.dumps(built, indent=2, ensure_ascii=False))
            else:
                for phase in ('ddl', 'reset', 'fill_statements'):
                    print(f'// ---- {phase}')
                    for entry in built:
                        if not entry['ok'] and phase == 'ddl':
                            print(f"// {entry['index_id']}: not materializable: {entry['error']}")
                        for statement in entry[phase]:
                            print(statement + ';')
        elif args.command == 'import':
            print(json.dumps(registry.describe(), indent=2, ensure_ascii=False))
        elif args.command == 'script':
            for result in mw.execute_script(Path(args.file).read_text()):
                print(json.dumps(result.to_dict(), ensure_ascii=False))
        else:
            from .server import serve
            print(f'cico middleware ({args.profile}) on http://{args.host}:{args.port}', file=sys.stderr)
            serve(mw, args.host, args.port)
    return 0


if __name__ == '__main__':
    sys.exit(main())

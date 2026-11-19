"""Index registry: written only by the middleware, from FILL / DROP_INDEX.

Real compiler; recording executors stand in for the target database.
"""
import json
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cico import IndexMiddleware  # noqa: E402
from cico.registry import IndexRegistry, RegistryError  # noqa: E402
from cico.schema import Schema  # noqa: E402
from test_middleware import schema_yaml  # noqa: E402

A = 'FILL Person(cached) FROM (MATCH (p:Person)-[:LIVES_IN]->(c:City) RETURN p.id, count(c))'
A2 = 'FILL Person(cached) FROM (MATCH (p:Person)-[:LIVES_IN]->(c:City) WHERE c.id >= 2 RETURN p.id, count(c))'
B = 'FILL Person(doubled) FROM (MATCH (p:Person) RETURN p.id, p.cached * 2)'
C = 'FILL City(restaurants) FROM (MATCH (c:City)-[:HAS_RESTAURANT]->(r:Restaurant) RETURN c.id, count(r))'
QUERY = 'MATCH (p:Person)-[:LIVES_IN]->(c:City) RETURN p.id AS personId, count(c) AS cnt'


class Recorder:
    """Executor recording statements; `fail(statement)` / `validate` may raise."""

    def __init__(self, fail=None, validate=None):
        self.statements, self.fail, self._validate = [], fail, validate

    def __call__(self, statement):
        if self.fail:
            self.fail(statement)
        self.statements.append(statement)

    def validate(self, statements):
        if self._validate:
            self._validate(statements)

    def wrote(self, column):
        return any(f'.{column} =' in s for s in self.statements)


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / 'registry.yaml'
        self.state = self.root / 'state.json'

    def open(self, executor=None, dialect='kuzu', target='test-db'):
        mw = IndexMiddleware(Schema.from_text(schema_yaml()), dialect=dialect)
        self.addCleanup(mw.close)
        registry = IndexRegistry(mw, self.path, self.state, target, executor=executor)
        registry.start()
        return mw, registry

    def populated(self, executor=None):
        mw, registry = self.open(executor or Recorder())
        for statement in (A, B, C):
            self.assertTrue(mw.execute(statement).reason.startswith('added'))
        return mw, registry

    def entries(self):
        return yaml.safe_load(self.path.read_text())['indexes']

    # ------------------------------------------------------------- FILL
    def test_registry_file_holds_canonical_definitions(self):
        executor = Recorder()
        mw, registry = self.open(executor)
        result = mw.execute(A)
        self.assertEqual((result.reason, result.index_id), ('added:active', 'PERSON(cached)'))
        text = self.path.read_text()
        self.assertTrue(text.startswith('# CICO index registry'))
        (entry,) = self.entries()
        self.assertEqual(entry['id'], 'PERSON(cached)')
        self.assertEqual(entry['fill'], 'FILL PERSON(cached) FROM (MATCH (p:Person)-[cico_r1:LIVES_IN]->(c:City) '
                                        'RETURN p.id AS cico_fill_0, count(c) AS cico_fill_1)')
        self.assertEqual(entry['carrier'], ['ALTER TABLE PERSON ADD cached INT64'])   # inferred
        self.assertEqual(entry['submitted'], A)
        self.assertTrue(mw.execute(QUERY).rewritten)
        self.assertTrue(executor.wrote('cached'))

    def test_equivalent_fill_changes_nothing(self):
        executor = Recorder()
        mw, registry = self.populated(executor)
        before, executor.statements = self.path.read_bytes(), []
        result = mw.execute('/* again */ ' + A.replace('MATCH', 'match').replace('RETURN', '\n  return') + ';')
        self.assertEqual(result.reason, 'unchanged:active')
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(executor.statements, [])

    def test_replacing_a_definition_rebuilds_it_and_its_dependents_in_order(self):
        executor = Recorder()
        mw, registry = self.populated(executor)
        executor.statements = []
        result = mw.execute(A2)
        self.assertEqual(result.reason, 'changed:active')
        self.assertEqual(registry.last_report['built'], ['PERSON(cached)', 'PERSON(doubled)'])   # A, then B
        self.assertFalse(executor.wrote('restaurants'))                                            # not C
        self.assertEqual([e['id'] for e in self.entries()], ['PERSON(cached)', 'PERSON(doubled)', 'CITY(restaurants)'])
        self.assertIn('c.id >= 2', self.entries()[0]['fill'])
        self.assertEqual(self.entries()[1]['depends_on'], ['PERSON(cached)'])
        filtered = QUERY.replace('RETURN', 'WHERE c.id >= 2 RETURN')
        self.assertTrue(mw.execute(filtered).rewritten)
        self.assertFalse(mw.execute(QUERY).rewritten)

    def test_string_literal_case_is_a_change(self):
        mw, registry = self.open(Recorder())
        first = mw.execute("FILL Person(tag) FROM (MATCH (p:Person) RETURN p.id, 'Paris')")
        second = mw.execute("FILL Person(tag) FROM (MATCH (p:Person) RETURN p.id, 'PARIS')")
        self.assertEqual((first.reason, second.reason), ('added:active', 'changed:active'))

    def test_restart_restores_receipts_without_building(self):
        self.populated()
        executor = Recorder()
        mw, registry = self.open(executor)
        self.assertEqual(executor.statements, [])
        self.assertEqual(registry.last_report['unchanged'], ['PERSON(cached)', 'PERSON(doubled)', 'CITY(restaurants)'])
        self.assertTrue(mw.execute(QUERY).rewritten)

    def test_without_an_executor_definitions_stay_inactive(self):
        mw, registry = self.open()
        result = mw.execute(A)
        self.assertEqual(result.reason, 'added:pending')
        self.assertEqual(len(self.entries()), 1)
        self.assertFalse(mw.execute(QUERY).rewritten)
        self.assertEqual(registry.last_report['pending'], ['PERSON(cached)'])

    # ------------------------------------------------------- DROP_INDEX
    def test_drop_index(self):
        executor = Recorder()
        mw, registry = self.populated(executor)
        executor.statements = []
        refused = mw.execute('DROP_INDEX Person(cached)')
        self.assertEqual(refused.reason, 'failed:dependents')
        self.assertIn('PERSON(doubled)', refused.detail)
        self.assertEqual(mw.execute('DROP_INDEX PERSON(doubled)').reason, 'removed')
        self.assertEqual(mw.execute('drop_index Person(cached);').reason, 'removed')
        self.assertEqual([e['id'] for e in self.entries()], ['CITY(restaurants)'])
        self.assertEqual(list(json.loads(self.state.read_text())['indexes']), ['CITY(restaurants)'])
        self.assertEqual(list(mw.fills), ['CITY(restaurants)'])
        self.assertFalse(mw.execute(QUERY).rewritten)
        self.assertEqual(executor.statements, [])          # physical carriers are left alone
        self.assertEqual(mw.execute('DROP_INDEX Person(cached)').reason, 'failed:missing')
        self.assertEqual(mw.execute('DROP_INDEX IF EXISTS Person(cached)').reason, 'unchanged')

    def test_drop_index_without_a_registry(self):
        with IndexMiddleware(Schema.from_text(schema_yaml()), dialect='kuzu') as mw:
            mw.execute(A)
            self.assertEqual(mw.execute('DROP_INDEX PERSON(cached)').reason, 'removed')
            self.assertEqual(mw.fills, {})

    # --------------------------------------------------------- failures
    def test_failed_validation_changes_nothing(self):
        def reject(statements):
            raise RuntimeError('EXPLAIN failed')
        mw, registry = self.populated()
        registry.executor = Recorder(validate=reject)
        before, state = self.path.read_bytes(), self.state.read_bytes()
        self.assertEqual(mw.execute(A2).reason, 'failed:validate')
        self.assertEqual((self.path.read_bytes(), self.state.read_bytes()), (before, state))
        self.assertNotIn('WHERE', mw.fills['PERSON(cached)'][0])
        self.assertTrue(mw.execute(QUERY).rewritten)

    def test_build_failure_keeps_the_definition_and_retries(self):
        def fail(statement):
            if '.cached =' in statement and 'id >= 2' in statement:
                raise RuntimeError('target write failed')
        mw, registry = self.populated()
        registry.executor = Recorder(fail=fail)
        result = mw.execute(A2)
        self.assertEqual((result.reason, result.index_id), ('failed:build', 'PERSON(cached)'))
        self.assertIn('c.id >= 2', self.entries()[0]['fill'])                 # the definition is recorded
        self.assertEqual(registry.last_report['pending'], ['PERSON(cached)', 'PERSON(doubled)'])
        self.assertEqual(list(mw.fills), ['CITY(restaurants)'])               # failed + dependent inactive
        status = {e['id']: e['status'] for e in registry.describe()['indexes']}
        self.assertEqual(status['PERSON(cached)'], 'building')
        registry.executor = Recorder()
        self.assertEqual(registry.build()['built'], ['PERSON(cached)', 'PERSON(doubled)'])

    def test_invalid_definitions_are_refused_before_any_change(self):
        mw, registry = self.populated()
        before = self.path.read_bytes()
        self.assertEqual(mw.execute('FILL Person(x) FROM (MATCH (p:Nope) RETURN p.id, 1)').reason, 'failed:bind')
        nullable = 'FILL City(n) FROM (MATCH (c:City)-[:HAS_RESTAURANT]->(r:Restaurant) RETURN c.id, count(r.name))'
        self.assertEqual(mw.execute(nullable.replace('r.name', 'r.id + 1')).reason, 'failed:bind')
        overlap = mw.execute('FILL Person(cached, other) FROM (MATCH (p:Person) RETURN p.id, 1, 2)')
        self.assertEqual(overlap.reason, 'failed:register')
        self.assertIn('already owns this target property', overlap.detail)
        self.assertEqual(mw.execute('FILL Person(y) FROM (MATCH (p:Person) WHERE p.id = $v RETURN p.id, 1)').reason,
                         'failed:parse')
        self.assertEqual(self.path.read_bytes(), before)

    def test_receipts_belong_to_one_physical_target(self):
        self.populated()
        with self.assertRaisesRegex(ValueError, 'physical target mismatch'):
            self.open(Recorder(), target='another-db')

    # ------------------------------------------------- semantics & misc
    def test_fill_source_keeps_neug_semantics_in_the_neo4j_dialect(self):
        source = 'MATCH (p:Person)-[r1:LIVES_IN]->(c:City)<-[r2:LIVES_IN]-(q:Person) RETURN p.id, count(q)'
        mw, registry = self.open(Recorder(), dialect='neo4j')
        self.assertEqual(mw.execute(f'FILL Person(neighbours) FROM ({source})').reason, 'added:active')
        self.assertNotIn('<>', self.entries()[0]['fill'])     # no Neo4j relationship uniqueness

    def test_standalone_ddl_and_parameters(self):
        mw, registry = self.open(Recorder())
        ddl = mw.execute('ALTER TABLE Person ADD nickname STRING')
        self.assertEqual((ddl.route, ddl.reason), ('target', 'target_ddl'))
        result = mw.execute('FILL City(big) FROM (MATCH (c:City)-[:HAS_RESTAURANT]->(r:Restaurant) '
                            'WHERE r.id > $n RETURN c.id, count(r))', parameters={'n': 3})
        self.assertEqual(result.reason, 'added:active')
        self.assertIn('(r.id > 3)', self.entries()[0]['fill'])

    def test_import_keeps_declared_carriers(self):
        mw, registry = self.open(Recorder())
        registry.import_script('ALTER TABLE Person ADD IF NOT EXISTS cached INT64 DEFAULT 0;\n'
                               'DROP TABLE IF EXISTS Nothing;\n' + A + ';\n' + C + ';')
        entries = self.entries()
        self.assertEqual(entries[0]['carrier'], ['ALTER TABLE Person ADD IF NOT EXISTS cached INT64 DEFAULT 0'])
        self.assertEqual(entries[1]['carrier'], ['ALTER TABLE CITY ADD restaurants INT64'])
        self.assertIn('SET cico_n.cached = 0', ' '.join(s for p in registry.last_report['plans'] for s in p['reset'])
                      or ' '.join(registry.executor.statements))
        # The same FILL sent later keeps the declared carrier: nothing changes.
        self.assertEqual(mw.execute(A).reason, 'unchanged:active')

    def test_http_entry(self):
        from cico.server import make_handler
        mw, registry = self.populated()
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(mw))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f'http://127.0.0.1:{server.server_port}'

        def call(route, body=None):
            request = urllib.request.Request(url + route, data=None if body is None else json.dumps(body).encode(),
                                             headers={'Content-Type': 'application/json'})
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    return response.status, json.load(response)
            except urllib.error.HTTPError as error:
                with error:
                    return error.code, json.load(error)
        try:
            self.assertEqual(call('/statement', {'cypher': A2})[1]['reason'], 'changed:active')
            self.assertEqual(call('/statement', {'cypher': 'DROP_INDEX CITY(restaurants)'})[1]['reason'], 'removed')
            status, body = call('/registry')
            self.assertEqual([e['id'] for e in body['indexes']], ['PERSON(cached)', 'PERSON(doubled)'])
            self.assertTrue(all(e['active'] for e in body['indexes']))
            self.assertEqual(call('/registry/build', {})[1]['built'], [])
            self.assertEqual(call('/index', {'fills': [C]})[0], 400)
            filtered = QUERY.replace('RETURN', 'WHERE c.id >= 2 RETURN')
            results = call('/script', {'script': f'{C}; {filtered}'})[1]['results']
            self.assertEqual([r['reason'] for r in results], ['added:active', 'rewritten'])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_cli_import_and_plan(self):
        script = self.root / 'index.cypher'
        script.write_text(f'{A};\n{B};\n')
        schema = self.root / 'schema.yaml'
        schema.write_text(schema_yaml())
        common = ['--schema', str(schema), '--dialect', 'kuzu', '--registry', str(self.path),
                  '--index-state', str(self.state), '--index-target', 'offline-plan']
        run = lambda *args: subprocess.run([sys.executable, '-m', 'cico', *args], cwd=ROOT,  # noqa: E731
                                           capture_output=True, text=True, timeout=120)
        imported = run('import', *common, str(script))
        self.assertEqual(imported.returncode, 0, imported.stderr)
        self.assertEqual([e['id'] for e in json.loads(imported.stdout)['indexes']],
                         ['PERSON(cached)', 'PERSON(doubled)'])
        planned = run('build', *common, '--json')
        self.assertEqual(planned.returncode, 0, planned.stderr)
        self.assertEqual([p['index_id'] for p in json.loads(planned.stdout)], ['PERSON(cached)', 'PERSON(doubled)'])
        self.assertFalse(self.state.exists())                 # planning writes no receipts


if __name__ == '__main__':
    unittest.main()

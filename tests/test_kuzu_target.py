"""Kuzu as a managed-FILL target: CLI routing and KuzuIndexExecutor.

Executor tests need the `kuzu` package (0.10.x) and are skipped otherwise;
run them with the Kuzu venv:  <kuzu-python> -m unittest tests.test_kuzu_target
"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico.__main__ import main  # noqa: E402
from cico.targets import KuzuIndexExecutor  # noqa: E402

try:
    import kuzu
except ImportError:  # pragma: no cover - depends on the interpreter
    kuzu = None


class CliTargets(unittest.TestCase):
    def _error(self, argv):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            main(argv)
        return stderr.getvalue()

    def test_materialize_new_needs_matching_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            message = self._error(['build', '--schema', 'x.yaml', '--dialect', 'neo4j', '--registry', 'r.yaml',
                                   '--index-state', 's.json', '--kuzu-db', tmp, '--materialize-new'])
        self.assertIn('--dialect kuzu --kuzu-db', message)

    def test_one_target_only(self):
        message = self._error(['build', '--schema', 'x.yaml', '--profile', 'kuzu', '--kuzu-db', '/tmp/a',
                               '--neo4j-uri', 'bolt://x'])
        self.assertIn('Choose one target', message)

    def test_registry_accepts_kuzu_target(self):
        message = self._error(['build', '--schema', 'x.yaml', '--dialect', 'kuzu', '--registry', 'r.yaml'])
        self.assertIn('--kuzu-db', message)


class ExecutorIdentity(unittest.TestCase):
    def test_identity_is_path_and_inode(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = Path(tmp) / 'db'
            store.mkdir()
            target = json.loads(KuzuIndexExecutor(store).target)
            self.assertEqual(target['system'], 'kuzu')
            self.assertEqual(target['path'], str(store.resolve()))
            self.assertEqual(target['inode'], store.stat().st_ino)
            copy = Path(tmp) / 'copy'
            copy.mkdir()
            self.assertNotEqual(KuzuIndexExecutor(copy).target, KuzuIndexExecutor(store).target)

    def test_missing_store(self):
        with self.assertRaises(ValueError):
            KuzuIndexExecutor('/nonexistent/cico/kuzu-db')


@unittest.skipIf(kuzu is None, 'kuzu package not installed')
class ExecutorOnStore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Path(self.tmp.name) / 'db'
        db = kuzu.Database(str(self.store), buffer_pool_size=1 << 28)
        conn = kuzu.Connection(db)
        for statement in ('CREATE NODE TABLE P(id INT64, x INT64, PRIMARY KEY(id))',
                          'CREATE NODE TABLE C(id INT64, PRIMARY KEY(id))',
                          'CREATE REL TABLE D(FROM P TO C, w INT64)', 'CREATE (:P {id: 1})'):
            conn.execute(statement).close()   # results must be closed before their database
        conn.close()
        db.close()
        self.runner = KuzuIndexExecutor(self.store, threads=1, buffer_pool_gb=0.25)

    def tearDown(self):
        self.runner.close()
        self.tmp.cleanup()

    def columns(self):
        rows = self.runner.query("CALL table_info('P') RETURN *")
        self.runner.release()
        return [r[1] for r in rows]

    def test_validate_binds_after_ddl_and_rolls_back(self):
        self.runner.validate(['ALTER TABLE P ADD IF NOT EXISTS cached INT64 DEFAULT 0',
                              'MATCH (p:P) SET p.cached = 1',
                              'CREATE REL TABLE IF NOT EXISTS E(FROM P TO C)',
                              'MATCH (p:P), (c:C) CREATE (p)-[:E]->(c)'])
        self.assertEqual(self.columns(), ['id', 'x'])
        tables = [r[1] for r in self.runner.query('CALL show_tables() RETURN *')]
        self.assertNotIn('E', tables)

    def test_validate_reports_bind_errors(self):
        with self.assertRaises(RuntimeError):
            self.runner.validate(['MATCH (p:P) SET p.missing = 1'])
        self.assertEqual(self.columns(), ['id', 'x'])

    def test_existing_carrier_of_another_type(self):
        with self.assertRaisesRegex(ValueError, 'exists as INT64'):
            self.runner.validate(['ALTER TABLE P ADD IF NOT EXISTS x DOUBLE DEFAULT 0.0'])
        self.runner.validate(['ALTER TABLE P ADD IF NOT EXISTS x INT64 DEFAULT 0'])
        self.runner.validate(['ALTER TABLE P ADD IF NOT EXISTS `x` int64 DEFAULT 0'])

    def test_existing_relationship_carrier_shape(self):
        self.runner.validate(['CREATE REL TABLE IF NOT EXISTS D(FROM P TO C, w INT64 DEFAULT 0)'])
        self.runner.validate(['CREATE REL TABLE IF NOT EXISTS D(FROM P TO C)'])  # extra column: tolerated
        with self.assertRaisesRegex(ValueError, 'relationship table D exists'):
            self.runner.validate(['CREATE REL TABLE IF NOT EXISTS D(FROM P TO C, w INT64, v INT64)'])
        with self.assertRaisesRegex(ValueError, 'relationship table D exists'):
            self.runner.validate(['CREATE REL TABLE IF NOT EXISTS D(FROM P TO C, w DOUBLE)'])
        with self.assertRaisesRegex(ValueError, 'relationship table D exists'):
            self.runner.validate(['CREATE REL TABLE IF NOT EXISTS D(FROM P TO C, FROM C TO P, w INT64)'])
        with self.assertRaisesRegex(ValueError, 'not a relationship table'):
            self.runner.validate(['CREATE REL TABLE IF NOT EXISTS C(FROM P TO C)'])

    def test_execute_commits_and_releases_lock(self):
        self.runner('ALTER TABLE P ADD IF NOT EXISTS cached INT64 DEFAULT 0')
        self.runner('MATCH (p:P) SET p.cached = 7')
        self.runner.release()
        db = kuzu.Database(str(self.store), buffer_pool_size=1 << 28)  # lock was given back
        conn = kuzu.Connection(db)
        result = conn.execute('MATCH (p:P) RETURN p.cached')
        self.assertEqual(result.get_next(), [7])
        result.close()
        conn.close()
        db.close()


if __name__ == '__main__':
    unittest.main()

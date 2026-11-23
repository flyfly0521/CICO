"""guide/example_mydb.py keeps working: the toy dialect rewrites, and a broken
frontend makes the verification guard return the original query."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
from cico.engine import DEFAULT_ENGINE  # noqa: E402

spec = importlib.util.spec_from_file_location('example_mydb', ROOT / 'guide/example_mydb.py')
example = importlib.util.module_from_spec(spec)
spec.loader.exec_module(example)

FILL = 'FILL PERSON(knowsCount) FROM (MATCH (p:PERSON)-[:KNOWS]-(f:PERSON) RETURN p.id, count(*))'
QUERY = ('MATCH (p:PERSON)-[:KNOWS]-(f:PERSON) MATCH (p)-[:ISLOCATEDIN]->(c:PLACE) '
         'RETURN c.name AS city, count(*) AS n')


@unittest.skipUnless(DEFAULT_ENGINE.is_file(), 'needs build/bin/cico_engine')
class GuideExample(unittest.TestCase):
    def middleware(self):
        mw = IndexMiddleware(ROOT / 'schemas/ldbc-bi.yaml', dialect='mydb')
        self.addCleanup(mw.close)
        self.assertTrue(mw.execute(FILL).ok)
        return mw

    def test_rewrite_is_printed_in_the_target_dialect(self):
        result = self.middleware().execute(QUERY)
        self.assertTrue(result.rewritten, result.to_dict())
        self.assertIn('knowsCount', result.cypher)
        self.assertIn('total(', result.cypher)
        self.assertNotIn('sum(', result.cypher)

    def test_guard_falls_back_when_the_frontend_is_wrong(self):
        with patch.dict(example.TO_INTERNAL, clear=True):    # forget total() -> sum()
            result = self.middleware().execute(QUERY)
        self.assertFalse(result.rewritten)
        self.assertTrue(result.reason.startswith('verification'), result.reason)
        self.assertEqual(result.cypher, QUERY)


if __name__ == '__main__':
    unittest.main()

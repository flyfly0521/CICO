"""Regression tests for the semantic guards of relationship projection FILLs."""
from pathlib import Path
import unittest

from cico import IndexMiddleware
from cico.engine import DEFAULT_ENGINE

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / 'schemas/ldbc-bi.yaml'
CONSTRAINTS = ROOT / 'constraints/ldbc-snb.yaml'
EDGE = 'MATCH (a:PERSON)-[k:KNOWS]->(b:PERSON)'
FORMULA = '1.0 / (0.5 * (k.forwardCount + k.reverseCount) + 1.0)'
QUERY = f'{EDGE} RETURN a.id, b.id, {FORMULA} AS weight'
FILL = f'FILL KNOWS(storedWeight) FROM ({EDGE} RETURN a.id, b.id, {FORMULA})'


@unittest.skipUnless(DEFAULT_ENGINE.is_file(), 'needs build/bin/cico_engine')
class RelationshipProjectionTests(unittest.TestCase):
    def middleware(self, fill=FILL, constraints=CONSTRAINTS, scope='part'):
        mw = IndexMiddleware(SCHEMA, dialect='kuzu', scope=scope, constraints=constraints)
        self.addCleanup(mw.close)
        for name in ('forwardCount', 'reverseCount'):
            self.assertTrue(mw.execute(f'ALTER TABLE KNOWS ADD {name} INT64 DEFAULT 0').ok)
        for name in ('storedWeight', 'outputWeight'):
            self.assertTrue(mw.execute(f'ALTER TABLE KNOWS ADD {name} DOUBLE DEFAULT 1.0').ok)
        result = mw.execute(fill)
        self.assertTrue(result.ok, result.to_dict())
        return mw

    def test_read_keeps_relationship_and_replaces_only_projection(self):
        mw = self.middleware()
        result = mw.execute(QUERY)
        self.assertTrue(result.rewritten, result.to_dict())
        self.assertEqual(result.indexes, ['KNOWS(storedWeight)'])
        self.assertEqual(result.premises, ['unique_edge: -[:KNOWS]-> per ordered pair'])
        self.assertEqual(result.cypher, f'{EDGE} RETURN a.id, b.id, k.storedWeight AS weight')

    def test_staged_fill_and_write_prefix(self):
        fill = (f'FILL KNOWS(storedWeight) FROM ({EDGE} '
                'WITH a, b, k.forwardCount + k.reverseCount AS counts '
                'RETURN a.id, b.id, 1.0 / (0.5 * counts + 1.0))')
        for scope in ('part', 'query'):
            with self.subTest(scope=scope):
                mw = self.middleware(fill, scope=scope)
                result = mw.execute(f'{EDGE} WITH k, {FORMULA} AS weight SET k.outputWeight = weight')
                self.assertTrue(result.rewritten, result.to_dict())
                self.assertEqual(result.premises, ['unique_edge: -[:KNOWS]-> per ordered pair'])
                self.assertIn('WITH k, k.storedWeight AS weight', result.cypher)
                self.assertTrue(result.cypher.endswith('SET k.outputWeight = weight'))
                self.assertTrue(mw.drop_index('KNOWS(storedWeight)'))
                self.assertFalse(mw.execute(QUERY).rewritten)

    def test_declared_uniqueness_is_required(self):
        self.assertFalse(self.middleware(constraints=None).execute(QUERY).rewritten)

    def test_filtered_and_reversed_keys_are_rejected(self):
        for source in (FILL.replace(' RETURN', ' WHERE k.forwardCount > 0 RETURN'),
                       FILL.replace('a.id, b.id', 'b.id, a.id')):
            with self.subTest(source=source):
                self.assertFalse(self.middleware(source).execute(QUERY).rewritten)

    def test_own_carrier_cannot_be_a_dependency(self):
        formula = 'k.storedWeight + 1.0'
        mw = self.middleware(FILL.replace(FORMULA, formula))
        self.assertFalse(mw.execute(QUERY.replace(FORMULA, formula)).rewritten)

    def test_nearby_float_does_not_match(self):
        mw = self.middleware()
        self.assertFalse(mw.execute(QUERY.replace('0.5', '0.50000001')).rewritten)
        self.assertFalse(mw.execute(QUERY.replace(' AS weight', ' AS weight, 0.50000001 AS other')).rewritten)

    def test_optional_and_undirected_queries_are_rejected(self):
        mw = self.middleware()
        self.assertFalse(mw.execute('OPTIONAL ' + QUERY).rewritten)
        self.assertFalse(mw.execute(QUERY.replace(']->', ']-')).rewritten)


if __name__ == '__main__':
    unittest.main()

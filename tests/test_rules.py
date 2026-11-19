"""Shared rewrite rules on the internal dialect (relax.py, dialect hooks).

Text-level checks of what the constraint retry may and may not change; the
end-to-end equivalence on a real Kuzu store is tests/kuzu_rules_acceptance.py.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
from cico.constraints import load_constraints  # noqa: E402
from cico.dialects import get_dialect  # noqa: E402
from cico.errors import Unsupported  # noqa: E402
from cico.neo4j import render, tokenize  # noqa: E402
from cico.relax import InternalRelax  # noqa: E402
from cico.surface import check  # noqa: E402

SCHEMA = ROOT / 'schemas/ldbc-bi.yaml'
SHARED = ROOT / 'constraints/ldbc-snb.yaml'
CHAIN = '(a:PERSON)-[:ISLOCATEDIN]->(city:PLACE)-[:ISPARTOF]->(country:PLACE {name: $c})'


def relax(text):
    premises = set()
    out = render(InternalRelax(load_constraints(SHARED)).relax(tokenize(text), premises))
    return ' '.join(out.split()), premises


class ImpliedTypes(unittest.TestCase):
    def test_chain_drops_both_type_conjuncts(self):
        out, premises = relax(f"MATCH {CHAIN} WHERE city.type = 'City' AND country.type = 'Country' RETURN a.id")
        self.assertNotIn("type =", out)
        self.assertIn('endpoint_labels: (:Person)-[:ISLOCATEDIN]->(:City)', premises)
        self.assertIn('endpoint_labels: (:City)-[:ISPARTOF]->(:Country)', premises)

    def test_unknown_source_keeps_the_conjunct(self):
        # the source is any PLACE, not a City: ISPARTOF may end at a Continent
        out, _ = relax("MATCH (x:PLACE)-[:ISPARTOF]->(y:PLACE) WHERE y.type = 'Country' RETURN y.id")
        self.assertIn("y.type = 'Country'", out)

    def test_conjunct_under_or_is_kept(self):
        out, _ = relax(f"MATCH {CHAIN} WHERE city.type = 'City' OR a.id = 1 RETURN a.id")
        self.assertIn("city.type = 'City'", out)

    def test_kept_type_conjunct_justifies_the_next(self):
        out, _ = relax("MATCH (x:PLACE)-[:ISPARTOF]->(y:PLACE) WHERE x.type = 'City' AND y.type = 'Country' "
                       "RETURN y.id")
        self.assertIn("x.type = 'City'", out)       # stated, not implied: kept
        self.assertNotIn("y.type", out)             # implied by it

    def test_other_clause_does_not_justify(self):
        out, _ = relax("MATCH (a:PERSON)-[:ISLOCATEDIN]->(c:PLACE) WITH c MATCH (c)-[:ISPARTOF]->(y:PLACE) "
                       "WHERE c.type = 'City' AND y.type = 'Country' RETURN y.id")
        self.assertNotIn('y.type', out)             # justified by c.type in its own clause
        out, _ = relax("MATCH (a:PERSON)-[:ISLOCATEDIN]->(c:PLACE) WITH c MATCH (c)-[:ISPARTOF]->(y:PLACE) "
                       "WHERE y.type = 'Country' RETURN y.id")
        self.assertIn("y.type = 'Country'", out)    # the edge from the Person is in another clause


class KeyIdentities(unittest.TestCase):
    def test_typed_key_is_deferred_with_its_guard(self):
        out, premises = relax(f"MATCH {CHAIN}, (a)-[:KNOWS]-(b:PERSON) WHERE city.type = 'City' "
                              "AND country.type = 'Country' WITH DISTINCT country, a, b RETURN country.id, a.id")
        self.assertIn('(cico_key1:PLACE {name: $c})', out)
        self.assertIn("WITH DISTINCT a, b MATCH (country:PLACE {name: $c}) WHERE country.type = 'Country' "
                      'WITH DISTINCT country, a, b', out)
        self.assertNotIn('cico_key1.type', out)     # implied along the chain, then dropped
        self.assertIn('unique_property: Country.name', premises)

    def test_hidden_node_is_left_alone(self):
        out, _ = relax(f"MATCH {CHAIN} WHERE country.type = 'Country' RETURN a.id")
        self.assertNotIn('cico_key', out)

    def test_place_without_type_has_no_key(self):
        # PLACE.name is not unique (a city may be called like a country)
        out, _ = relax('MATCH (a:PERSON)-[:ISLOCATEDIN]->(c:PLACE {name: $c}) WITH a, c RETURN c.id, a.id')
        self.assertNotIn('cico_key', out)

    def test_optional_match_is_not_rebound(self):
        out, _ = relax(f"MATCH (a:PERSON) OPTIONAL MATCH (a)-[:ISLOCATEDIN]->(city:PLACE)-[:ISPARTOF]->"
                       f"(country:PLACE {{name: $c}}) WHERE country.type = 'Country' WITH a, country "
                       "RETURN a.id, country.id")
        self.assertNotIn('cico_key', out)

    def test_bound_node_gets_key_and_guard(self):
        out, premises = relax("MATCH (country:PLACE) WITH country MATCH (b:PERSON)-[:ISLOCATEDIN]->(cb:PLACE)"
                              "-[:ISPARTOF]->(country) WHERE cb.type = 'City' RETURN b.id")
        self.assertIn('(cico_key1:PLACE)', out)
        self.assertIn("cico_key1.name = country.name AND country.type = 'Country'", out)
        self.assertNotIn('cb.type', out)
        self.assertIn('required_property: Country.name', premises)


class DialectHooks(unittest.TestCase):
    def test_edge_carrier_orientation(self):
        names = iter(['k1', 'k2'])
        match, expression = get_dialect('kuzu').edge_carrier_test('a', 'b', ':KNOWS', 'x', None, 'F',
                                                                  lambda base: next(names))
        self.assertEqual(match, 'OPTIONAL MATCH (a)-[k1:KNOWS]->(b) OPTIONAL MATCH (b)-[k2:KNOWS]->(a)')
        self.assertEqual(expression, 'CASE WHEN k1 IS NOT NULL THEN coalesce(k1.x, 0) > 0 '
                                     'WHEN k2 IS NOT NULL THEN F ELSE F END')

    def test_shortest_path_printing(self):
        d = get_dialect('kuzu')
        self.assertEqual(d.shortest_length(False, 1, 4), '* SHORTEST 1..4')
        self.assertEqual(d.shortest_length(True, 2, 3), '* ALL SHORTEST 2..3')

    def test_parameter_path_bound_needs_a_literal(self):
        with self.assertRaisesRegex(Unsupported, 'needs literal'):
            check('MATCH (a:PERSON)-[:KNOWS* SHORTEST 1..$m]-(b:PERSON) RETURN b.id')
        check('MATCH (a:PERSON)-[:KNOWS* SHORTEST 1..4]-(b:PERSON) RETURN b.id')

    def test_timestamp_folding(self):
        from cico.normalize import fold_constants
        self.assertEqual(fold_constants("CAST(TIMESTAMP('2010-06-09 00:00:00'), 'TIMESTAMP') + duration('100 days')"),
                         "TIMESTAMP('2010-09-17 00:00:00')")
        self.assertEqual(fold_constants("CAST(TIMESTAMP('2010-06-09 00:00:00') AS TIMESTAMP)"),
                         "TIMESTAMP('2010-06-09 00:00:00')")
        for day_start in ("date_trunc('day', TIMESTAMP('2012-07-26 13:05:00'))",
                          "CAST(CAST(TIMESTAMP('2012-07-26 13:05:00') AS DATE) AS TIMESTAMP)"):
            self.assertEqual(fold_constants(day_start + " + duration('1 day')"), "TIMESTAMP('2012-07-27 00:00:00')")
        self.assertEqual(fold_constants('CAST(m.creationDate AS TIMESTAMP)'), 'CAST(m.creationDate AS TIMESTAMP)')
        with self.assertRaisesRegex(Unsupported, 'needs literal'):
            check("MATCH (m:POST) WHERE m.creationDate >= date_trunc('day', $d) RETURN m.id")

    def test_relax_is_off_outside_the_retry(self):
        text = f"MATCH {CHAIN} WHERE city.type = 'City' RETURN a.id"
        with IndexMiddleware(SCHEMA, dialect='kuzu', constraints=SHARED) as mw:
            self.assertIn("city.type = 'City'", mw.frontend.to_internal(text))
            mw.frontend.relax = True
            try:
                self.assertNotIn('city.type', mw.frontend.to_internal(text))
            finally:
                mw.frontend.relax = False


if __name__ == '__main__':
    unittest.main()

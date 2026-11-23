"""Dialect declarations (profiles.py), shared constraints, FILL = NeuG."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
from cico.constraints import load_constraints  # noqa: E402
from cico.dialects import get_dialect  # noqa: E402
from cico.frontends import KuzuFrontend  # noqa: E402
from cico.neo4j import load_mapping  # noqa: E402
from cico.profiles import Profile, Semantics, get_profile, register, PROFILES  # noqa: E402

SCHEMA = ROOT / 'schemas/ldbc-bi.yaml'
SHARED = ROOT / 'constraints/ldbc-snb.yaml'

# The constraints the Neo4j mapping declared inline before they moved to
# the shared file (Neo4j vocabulary, same order).
LEGACY_NEO4J = [
    {'kind': 'at_most_one', 'rel': 'REPLY_OF', 'from': 'Message'},
    {'kind': 'at_most_one', 'rel': 'CONTAINER_OF', 'to': 'Post'},
    {'kind': 'at_most_one', 'rel': 'IS_LOCATED_IN', 'from': 'Person'},
    {'kind': 'at_most_one', 'rel': 'IS_PART_OF', 'from': 'City'},
    {'kind': 'at_least_one', 'rel': 'IS_LOCATED_IN', 'from': 'Person'},
    {'kind': 'at_least_one', 'rel': 'IS_PART_OF', 'from': 'City'},
    {'kind': 'endpoint_labels', 'rel': 'IS_LOCATED_IN', 'from': 'Person', 'to': 'City'},
    {'kind': 'endpoint_labels', 'rel': 'IS_PART_OF', 'from': 'City', 'to': 'Country'},
    {'kind': 'unique_property', 'label': 'Tag', 'property': 'name'},
    {'kind': 'unique_property', 'label': 'Country', 'property': 'name'},
    {'kind': 'required_property', 'label': 'Tag', 'property': 'name'},
    {'kind': 'required_property', 'label': 'Country', 'property': 'name'},
]


class Declarations(unittest.TestCase):
    def test_builtin_semantics(self):
        kuzu, neo4j = get_profile('kuzu'), get_profile('neo4j')
        self.assertEqual(kuzu.semantics.relationship_uniqueness, 'homomorphism')
        self.assertEqual(kuzu.semantics.variable_length_cap, 30)
        self.assertEqual(neo4j.semantics.relationship_uniqueness, 'isomorphism')
        self.assertEqual(neo4j.semantics.type_classes, 'label')
        self.assertTrue(neo4j.semantics.procedures and neo4j.semantics.native_ddl)

    def test_rules_follow_hooks_and_semantics(self):
        # Rules are shared; each declaration's set is derived, not listed.
        neo4j = get_profile('neo4j')
        self.assertEqual(neo4j.rules, frozenset({'scalar_regions', 'edge_carriers', 'constraint_retry',
                                                 'path_carriers', 'distinct_partitions'}))
        with IndexMiddleware(SCHEMA, dialect='kuzu', constraints=SHARED) as mw:
            self.assertEqual(mw.profile.rules, frozenset({'scalar_regions', 'edge_carriers',
                                                          'rel_projections', 'constraint_retry'}))
            status = mw.profile.rule_status()
            self.assertIn('isomorphism', status['path_carriers'])        # not needed under homomorphism
            self.assertIn('missing hooks', status['distinct_partitions'])
        bare = get_profile('kuzu')                                      # no constraints loaded
        self.assertNotIn('constraint_retry', bare.rules)
        self.assertEqual(bare.rule_status()['constraint_retry'], 'no declared constraints loaded')

    def test_disabled_rules(self):
        kuzu = get_profile('kuzu', disabled={'edge_carriers'})
        self.assertNotIn('edge_carriers', kuzu.rules)
        self.assertEqual(kuzu.rule_status()['edge_carriers'], 'disabled')

    def test_unknown_rule_is_rejected(self):
        with self.assertRaises(ValueError):
            Profile('x', get_dialect('kuzu'), KuzuFrontend(), disabled=frozenset({'no_such_rule'}))
        with self.assertRaises(ValueError):
            get_profile('kuzu', disabled={'no_such_rule'})

    def test_unknown_dialect(self):
        with self.assertRaisesRegex(ValueError, 'Unknown dialect'):
            get_profile('oracle')

    def test_register_a_new_dialect(self):
        register('toy', lambda constraints=None: Profile('toy', get_dialect('kuzu'), KuzuFrontend(),
                                                         description='test dialect'))
        try:
            with IndexMiddleware(SCHEMA, dialect='toy') as mw:
                self.assertEqual(mw.profile.name, 'toy')
                result = mw.execute('MATCH (p:PERSON) RETURN count(p)')
                self.assertTrue(result.ok)
        finally:
            PROFILES.pop('toy')

    def test_native_ddl_follows_the_declaration(self):
        with IndexMiddleware(SCHEMA, dialect='neo4j') as mw:
            self.assertEqual(mw.execute('CREATE INDEX x FOR (n:Person) ON (n.id)').route, 'target')
        with IndexMiddleware(SCHEMA, dialect='kuzu') as mw:
            self.assertNotEqual(mw.execute('CREATE INDEX x FOR (n:Person) ON (n.id)').route, 'target')


class SharedConstraints(unittest.TestCase):
    def test_neo4j_translation_preserves_the_legacy_inline_list(self):
        mapping = load_mapping(ROOT / 'profiles/ldbc-snb-neo4j.yaml')
        self.assertEqual(mapping['constraints'], LEGACY_NEO4J + [{'kind': 'unique_edge', 'rel': 'KNOWS'},
                                                       {'kind': 'unique_edge', 'rel': 'HAS_MEMBER'}])
        self.assertEqual(mapping['acyclic_rel_types'], ['REPLY_OF'])
        self.assertNotIn('constraints_file', mapping)

    def test_internal_names_and_classes(self):
        c = load_constraints(SHARED)
        self.assertEqual(len(c.items), 14)
        self.assertEqual(c.acyclic, {'REPLYOF'})
        self.assertIsNotNone(c.at_most_one_out('REPLYOF', {'COMMENT'}))
        self.assertIsNone(c.at_most_one_out('REPLYOF', {'FORUM'}))
        # A typed class is a predicate: table membership alone never implies it.
        self.assertIsNone(c.at_most_one_out('ISPARTOF', {'PLACE'}))
        self.assertIsNotNone(c.at_most_one_out('ISPARTOF', {'PLACE'}, known={'City'}))

    def test_kuzu_checks_are_internal_cypher(self):
        checks = {x['constraint']: x['cypher'] for x in load_constraints(SHARED).checks()}
        self.assertEqual(checks['endpoint_labels: (:Person)-[:ISLOCATEDIN]->(:City)'],
                         "MATCH (n:PERSON)-[r:ISLOCATEDIN]->(x) WHERE NOT (label(x) = 'PLACE' AND "
                         "x.type = 'City') RETURN count(r) AS violations")
        self.assertIn("n.type = 'Country'", checks['unique_property: Country.name'])
        self.assertEqual(checks['unique_edge: -[:KNOWS]-> per ordered pair'],
                         'MATCH (a)-[r:`KNOWS`]->(b) WITH a, b, count(r) AS c '
                         'WHERE c > 1 RETURN count(*) AS violations')

    def test_middleware_parameter(self):
        with IndexMiddleware(SCHEMA, dialect='kuzu', constraints=SHARED) as mw:
            self.assertEqual(len(mw.constraint_checks()), 14)
        with IndexMiddleware(SCHEMA, dialect='kuzu') as mw:
            self.assertEqual(mw.constraint_checks(), [])

    def test_undeclared_class_is_rejected(self):
        import tempfile
        with tempfile.NamedTemporaryFile('w', suffix='.yaml') as handle:
            handle.write('classes: {}\nconstraints:\n  - {kind: at_most_one, rel: R, from: X}\n')
            handle.flush()
            with self.assertRaisesRegex(ValueError, 'undeclared class X'):
                load_constraints(handle.name)


class FillIsNeug(unittest.TestCase):
    # Same syntax, different semantics: Neo4j requires k1 and k2 to be
    # different edges (relationship isomorphism), NeuG does not.
    SOURCE = 'MATCH (a:PERSON)-[k1:KNOWS]-(b:PERSON)-[k2:KNOWS]-(c:PERSON) RETURN a.id, count(c)'

    def test_neug_semantics_in_every_dialect(self):
        for dialect in ('neo4j', 'kuzu'):
            with self.subTest(dialect=dialect), IndexMiddleware(SCHEMA, dialect=dialect) as mw:
                result = mw.execute(f'FILL PERSON(twoHop) FROM ({self.SOURCE})')
                self.assertTrue(result.ok, result.detail)
                (text,) = mw.fills['PERSON(twoHop)']
                self.assertEqual(text, f'FILL PERSON(twoHop) FROM ({self.SOURCE})')  # no uniqueness added

    def test_target_vocabulary_is_not_translated(self):
        with IndexMiddleware(SCHEMA, dialect='neo4j') as mw:
            result = mw.execute('FILL PERSON(x) FROM (MATCH (m:Message)-[:HAS_CREATOR]->(p:Person) '
                                'RETURN p.id, count(m))')
            self.assertFalse(result.ok)   # Message / HAS_CREATOR are Neo4j names, not NeuG tables
            self.assertNotIn('PERSON(x)', mw.fills)


if __name__ == '__main__':
    unittest.main()

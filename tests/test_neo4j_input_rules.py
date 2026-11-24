"""Neo4j frontend rules added for the Neo4j input rewrites (2026-11-24):
path edge predicates, trail/walk reachability, variable-length table pruning,
datetime.truncate, queries embedded in GDS Cypher projections, acyclic
uniqueness between a fixed edge and a root path, root-path re-anchoring."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cico import IndexMiddleware  # noqa: E402
from cico.errors import Unsupported  # noqa: E402
from cico.neo4j import neo4j_profile, _quote, _unquote  # noqa: E402
import workloads as W  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CONSTRAINTS = ROOT / 'constraints'


def middleware(workload):
    constraints = CONSTRAINTS / 'ldbc-snb.yaml' if workload.startswith('ldbc') else None
    return IndexMiddleware(W.schema_path(workload),
                           profile=neo4j_profile(W.mapping_path(workload), constraints))


class Neo4jInputRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fin = middleware('finbench')
        cls.bi = middleware('ldbc-bi')

    @classmethod
    def tearDownClass(cls):
        cls.fin.close()
        cls.bi.close()

    def internal(self, mw, text, relax=False):
        mw.frontend.relax, mw.frontend.premises = relax, set()
        try:
            return mw.frontend.to_internal(text)
        finally:
            mw.frontend.relax = False

    # ---------------------------------------------- path edge predicates
    TCR11 = '''MATCH path = (source:Person {id: $id})-[:guarantee*1..5]->(person:Person)
        WHERE all(edge IN relationships(path) WHERE $s < edge.timestamp AND edge.timestamp < $e)
        WITH DISTINCT source, person
        MATCH (person)-[:apply]->(loan:Loan)
        RETURN count(loan) AS n'''

    def test_all_relationships_becomes_recursive_predicate(self):
        text = self.internal(self.fin, self.TCR11)
        self.assertIn('*1..5 (edge, cico_hop_node | WHERE $s < edge.timestamp AND edge.timestamp < $e)', text)
        self.assertNotIn('all(', text)
        self.assertNotIn('path', text)

    def test_path_used_elsewhere_is_kept(self):
        text = self.TCR11.replace('RETURN count(loan) AS n', 'RETURN count(loan) AS n, 1 AS one') \
            .replace('WITH DISTINCT source, person', 'WITH DISTINCT source, person, length(path) AS l')
        self.assert_all_kept(text)

    def test_predicate_reading_another_variable_is_kept(self):
        self.assert_all_kept(self.TCR11.replace('$s < edge.timestamp', 'source.id < edge.timestamp'))

    def assert_all_kept(self, text):
        try:
            internal = self.internal(self.fin, text)
        except Unsupported:
            return
        self.assertIn('all(', internal)
        self.assertNotIn('cico_hop_node', internal)

    # ------------------------------------------------ trails and walks
    def test_distinct_reachability_is_a_walk(self):
        text = self.internal(self.fin, self.TCR11)
        self.assertNotIn('TRAIL', text)

    def test_trail_kept_without_distinct(self):
        text = self.internal(self.fin, self.TCR11.replace('WITH DISTINCT source, person', 'WITH source, person'))
        self.assertIn('TRAIL', text)

    def test_trail_kept_when_undirected(self):
        text = self.internal(self.fin, self.TCR11.replace('->(person:Person)', '-(person:Person)'))
        self.assertIn('TRAIL', text)

    def test_trail_kept_for_lower_bound_two(self):
        text = self.internal(self.fin, self.TCR11.replace('*1..5', '*2..5'))
        self.assertIn('TRAIL', text)

    def test_trail_kept_with_aggregate_projection(self):
        text = self.internal(self.fin, self.TCR11.replace('WITH DISTINCT source, person',
                                                          'WITH DISTINCT source, count(person) AS c, person'))
        self.assertIn('TRAIL', text)

    def test_varlen_tables_pruned_from_typed_start(self):
        text = self.internal(self.fin, self.TCR11)
        self.assertIn('[:PersonGuaranteePerson*', text)
        self.assertNotIn('CompanyGuaranteeCompany', text)

    # -------------------------------------------------- datetime.truncate
    def test_datetime_truncate_day(self):
        text = self.internal(self.bi, "MATCH (m:Message) WHERE datetime.truncate('day', $d) <= m.creationDate "
                                      "RETURN count(m) AS n")
        self.assertIn("date_trunc('day', $d)", text)
        with self.assertRaises(Unsupported):
            self.internal(self.bi, "MATCH (m:Message) WHERE datetime.truncate('month', $d) <= m.creationDate "
                                   "RETURN count(m) AS n")

    # ------------------------------------------------- embedded queries
    def test_quote_round_trip(self):
        for value in ("MATCH (a) WHERE a.name = 'x' RETURN a", 'back\\slash', "it's\n"):
            self.assertEqual(_unquote(_quote(value)), value)

    def test_embedded_projection_queries(self):
        text = """CALL gds.graph.project.cypher('g', 'MATCH (p:Person) RETURN id(p) AS id',
            'MATCH (a:Person)-[:KNOWS]->(b:Person) WHERE a.creationDate >= $start RETURN id(a) AS source, id(b) AS target',
            {parameters: {start: $startDate}})"""
        tokens, found = self.bi.frontend.embedded_queries(text)
        self.assertEqual([q[1].split()[0] for q in found], ['MATCH', 'MATCH'])
        self.assertEqual(found[1][2], {'start': 'startDate'})
        spliced = self.bi.frontend.splice(tokens, {found[1][0]: "RETURN 'x' AS y"})
        self.assertIn("'RETURN \\'x\\' AS y'", spliced)

    def test_embedded_query_with_unpassed_parameter_is_skipped(self):
        text = """CALL gds.graph.project.cypher('g', 'MATCH (p:Person) RETURN id(p) AS id',
            'MATCH (a:Person)-[:KNOWS]->(b:Person) WHERE a.id = $x RETURN id(a) AS source, id(b) AS target')"""
        tokens, found = self.bi.frontend.embedded_queries(text)
        self.assertEqual(len(found), 1)          # only the node query

    # ------------------------------------- acyclic root paths, re-anchoring
    ROOT_OPTIONAL = '''MATCH (pA:Person)-[:KNOWS]->(pB:Person)
        OPTIONAL MATCH (pA)<-[:HAS_CREATOR]-(c1:Comment)-[:REPLY_OF]->(t1:Message),
                       ({start})-[:REPLY_OF*0..]->(:Post)<-[:CONTAINER_OF]-(f1:Forum)
        RETURN pA, count(f1) AS n'''

    def test_fixed_edge_into_path_start_needs_no_uniqueness(self):
        text = self.internal(self.bi, self.ROOT_OPTIONAL.format(start='t1'))
        self.assertIn('(t1)-[:REPLYOF*0..', text)
        with self.assertRaises(Unsupported):     # the path from c1 would reuse c1 -> t1 in Neo4j
            self.internal(self.bi, self.ROOT_OPTIONAL.format(start='c1'))

    def test_root_path_reanchored_in_relax_stage(self):
        text = self.internal(self.bi, self.ROOT_OPTIONAL.format(start='t1'), relax=True)
        self.assertIn('(c1)-[:REPLYOF*0..', text)
        self.assertTrue(any('REPLY_OF' in p for p in self.bi.frontend.premises))

    def test_no_reanchor_when_start_may_be_the_end(self):
        text = self.internal(self.bi, self.ROOT_OPTIONAL.format(start='t1').replace('(c1:Comment)', '(c1:Message)'),
                             relax=True)
        self.assertIn('(t1)-[:REPLYOF*0..', text)   # a Post c1 could be its own root


class Neo4jEmbeddedRewrite(unittest.TestCase):
    def test_projection_reads_fill(self):
        with middleware('ldbc-bi') as mw:
            mw.execute('ALTER TABLE KNOWS ADD IF NOT EXISTS bi19_interaction_count INT64 DEFAULT 0')
            r = mw.execute('''FILL KNOWS(bi19_interaction_count) FROM (
                MATCH (pA:PERSON)-[:KNOWS]->(pB:PERSON) WITH pA, pB
                MATCH (pA)<-[:HASCREATOR]-(m1:POST:COMMENT)-[:REPLYOF]-(m2:POST:COMMENT)-[:HASCREATOR]->(pB)
                RETURN pA.id, pB.id, count(m1))''')
            self.assertTrue(r.ok, r.to_dict())
            branch = '''MATCH (personA:Person)-[:KNOWS]->(personB:Person)
                MATCH (personA)<-[:HAS_CREATOR]-(m1:Message)-[:REPLY_OF]-(m2:Message)-[:HAS_CREATOR]->(personB)
                WITH personA, personB, count(m1) AS n
                RETURN id(personA) AS source, id(personB) AS target, n AS weight'''
            r = mw.execute(f"CALL gds.graph.project.cypher('g', 'MATCH (p:Person) RETURN id(p) AS id', '{branch}')")
            self.assertTrue(r.rewritten, r.to_dict())
            self.assertEqual(r.indexes, ['KNOWS(bi19_interaction_count)'])
            self.assertIn("'MATCH (p:Person) RETURN id(p) AS id'", r.cypher)
            self.assertIn('bi19_interaction_count', r.cypher)


if __name__ == '__main__':
    unittest.main()

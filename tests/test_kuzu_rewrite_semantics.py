"""Kuzu-dialect rewrites added for LSQB / FinBench / IC (2026-11-23).

Each case pins one semantic contract of the compiler or the middleware that
the docs' Kuzu input queries rely on (see docs/notes/kuzu-input-rewrites.md).
The FILLs are copies of NeuG/queries/index definitions, so only the engine and
the target schemas in schemas/ are needed.
"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cico import IndexMiddleware  # noqa: E402
from cico.engine import DEFAULT_ENGINE  # noqa: E402

WINDOW = ("TIMESTAMP('2021-07-23 06:10:16.747') < {e}.timestamp "
          "AND {e}.timestamp < TIMESTAMP('2022-11-29 02:52:22.640')")

Q2 = [
    'ALTER TABLE Person_knows_Person ADD IF NOT EXISTS q2_cnt_fwd INT64 DEFAULT 0',
    'ALTER TABLE Person_knows_Person ADD IF NOT EXISTS q2_cnt_rev INT64 DEFAULT 0',
    'FILL Person_knows_Person(q2_cnt_fwd) FROM (MATCH (person1:Person)-[:Person_knows_Person]->(person2:Person) '
    'WITH person1, person2 MATCH (person1)<-[:Comment_hasCreator_Person]-(comment:Comment)'
    '-[:Comment_replyOf_Post]->(:Post)-[:Post_hasCreator_Person]->(person2) '
    'RETURN person1.PersonId, person2.PersonId, count(comment) AS cnt)',
    'FILL Person_knows_Person(q2_cnt_rev) FROM (MATCH (person1:Person)-[:Person_knows_Person]->(person2:Person) '
    'WITH person1, person2 MATCH (person2)<-[:Comment_hasCreator_Person]-(comment:Comment)'
    '-[:Comment_replyOf_Post]->(:Post)-[:Post_hasCreator_Person]->(person1) '
    'RETURN person1.PersonId, person2.PersonId, count(comment) AS cnt)',
]
Q5 = [
    'CREATE REL TABLE q5_index(FROM Post TO Tag, FROM Comment TO Tag, q5_count INT64 DEFAULT 0)',
    'FILL q5_index_Post_Tag() FROM (MATCH (message:Post)<-[:Comment_replyOf_Post]-(comment:Comment)'
    '-[:Comment_hasTag_Tag]->(tag2:Tag) RETURN message.PostId, tag2.TagId, count(*) AS q5_count)',
    'FILL q5_index_Comment_Tag() FROM (MATCH (message:Comment)<-[:Comment_replyOf_Comment]-(comment:Comment)'
    '-[:Comment_hasTag_Tag]->(tag2:Tag) RETURN message.CommentId, tag2.TagId, count(*) AS q5_count)',
]
Q4 = [
    'ALTER TABLE Comment ADD IF NOT EXISTS q4_msg_cnt int64 DEFAULT 0',
    'FILL Comment(q4_msg_cnt) FROM (MATCH (message:Comment)<-[:Person_likes_Comment]-(:Person), '
    '(message)<-[:Comment_replyOf_Comment]-(:Comment) RETURN message.CommentId, count(*) AS cnt)',
]
TRANSFER_AGG = [
    'CREATE REL TABLE FINBENCH_TRANSFER_AGG(FROM Account TO Account, transferCount INT64, transferAmount DOUBLE)',
    'FILL FINBENCH_TRANSFER_AGG() FROM (MATCH (source:Account)-[edge:AccountTransferAccount]->(destination:Account) '
    f'WHERE {WINDOW.format(e="edge")} AND edge.amount > 0.0 '
    'RETURN source.id, destination.id, count(edge) AS transferCount, sum(edge.amount) AS transferAmount)',
]
TSR2_OUT = [
    'ALTER TABLE Account ADD tsr2TransferOutAmount DOUBLE',
    'ALTER TABLE Account ADD tsr2TransferOutMax DOUBLE',
    'ALTER TABLE Account ADD tsr2TransferOutCount INT64 DEFAULT 0',
    'FILL Account(tsr2TransferOutAmount, tsr2TransferOutMax, tsr2TransferOutCount) FROM ('
    'MATCH (src:Account)-[edge1:AccountTransferAccount]->(dst1:Account) '
    f'WHERE {WINDOW.format(e="edge1")} RETURN src.id, sum(edge1.amount), max(edge1.amount), count(edge1))',
]
TCR11 = [
    'ALTER TABLE Person ADD tcr11LoanAmount DOUBLE DEFAULT 0.0',
    'ALTER TABLE Person ADD tcr11LoanCount INT64 DEFAULT 0',
    'FILL Person(tcr11LoanAmount, tcr11LoanCount) FROM (MATCH (source:Person)'
    f'-[:PersonGuaranteePerson*1..5 (edge, node | WHERE {WINDOW.format(e="edge")})]->(person:Person) '
    'WITH DISTINCT source, person MATCH (person)-[:PersonApplyLoan]->(loan:Loan) '
    'WITH DISTINCT source, loan RETURN source.id, sum(loan.loanAmount), count(loan))',
]
ROOT_POST = [
    'CREATE REL TABLE IF NOT EXISTS ROOT_POST(from Comment to Post, from Post to Post)',
    'FILL ROOT_POST() FROM (MATCH (post:Post)<-[e:REPLYOF*0..4294967295]-(message:COMMENT:POST) '
    'RETURN DISTINCT message.id, post.id)',
]


@unittest.skipUnless(DEFAULT_ENGINE.is_file(), 'needs build/bin/cico_engine')
class KuzuRewriteSemantics(unittest.TestCase):
    def middleware(self, schema, statements):
        mw = IndexMiddleware(ROOT / 'schemas' / f'{schema}.yaml', dialect='kuzu')
        self.addCleanup(mw.close)
        for statement in statements:
            self.assertTrue(mw.execute(statement).ok, statement)
        return mw

    def rewrite(self, mw, query, **parameters):
        result = mw.execute(query, parameters=parameters or None)
        self.assertTrue(result.rewritten, result.to_dict())
        return result.cypher

    def test_reverse_member_presence_reads_its_own_column(self):
        # The q2_cnt_rev value must be guarded by q2_cnt_rev, not q2_cnt_fwd.
        text = self.rewrite(self.middleware('lsqb', Q2),
                            'MATCH (person1:Person)-[:Person_knows_Person]->(person2:Person), '
                            '(person2)<-[:Comment_hasCreator_Person]-(comment:Comment)-[:Comment_replyOf_Post]->'
                            '(:Post)-[:Post_hasCreator_Person]->(person1) RETURN count(comment) AS c')
        self.assertIn('q2_cnt_rev <> 0', text)
        self.assertNotIn('q2_cnt_fwd', text)

    def test_global_count_keeps_zero_on_empty_input(self):
        # SUM over no rows is NULL; the replaced COUNT must still return 0.
        text = self.rewrite(self.middleware('lsqb', Q2),
                            'MATCH (person1:Person)-[:Person_knows_Person]->(person2:Person), '
                            '(person1)<-[:Comment_hasCreator_Person]-(comment:Comment)-[:Comment_replyOf_Post]->'
                            '(:Post)-[:Post_hasCreator_Person]->(person2) RETURN count(comment) AS c')
        self.assertRegex(text, r'CASE WHEN \(CAST\(sum\(.*\) AS INT64\) IS NULL\) THEN 0 ELSE')

    def test_rel_group_child_reads_the_stored_count(self):
        # A FILL into one child of a REL group: count(*) becomes SUM(q5_count),
        # never a count of carrier edges.
        text = self.rewrite(self.middleware('lsqb', Q5),
                            'MATCH (message:Post)<-[:Comment_replyOf_Post]-(comment:Comment)-[:Comment_hasTag_Tag]->'
                            '(tag2:Tag) RETURN message.PostId, tag2.TagId, count(*) AS c')
        self.assertIn('q5_index.q5_count', text)
        self.assertNotIn('count(*)', text)

    def test_optional_count_with_residual_on_derived_edge(self):
        text = self.rewrite(self.middleware('lsqb', Q5),
                            'OPTIONAL MATCH (tag1:Tag)<-[:Comment_hasTag_Tag]-(message:Comment)'
                            '<-[:Comment_replyOf_Comment]-(comment:Comment)-[:Comment_hasTag_Tag]->(tag2:Tag) '
                            'WHERE tag1 <> tag2 RETURN count(comment) AS c')
        self.assertIn('OPTIONAL MATCH', text)
        self.assertIn('q5_count', text)

    def test_optional_node_count_uses_public_row_count(self):
        # The OPTIONAL presence test reads the public COUNT(*) column, not the
        # private n_rows state (which is not part of the target schema).
        text = self.rewrite(self.middleware('lsqb', Q4),
                            'OPTIONAL MATCH (:Tag)<-[:Comment_hasTag_Tag]-(message:Comment)'
                            '-[:Comment_hasCreator_Person]->(creator:Person), '
                            '(message)<-[:Person_likes_Comment]-(liker:Person), '
                            '(message)<-[:Comment_replyOf_Comment]-(comment:Comment) RETURN count(comment) AS c')
        self.assertIn('message.q4_msg_cnt > 0', text)
        self.assertNotIn('__neug_fill_state', text)

    def test_fixed_window_fill_matches_mirrored_and_respelled_predicates(self):
        # `$start < e.ts` (normalized to `e.ts > TIMESTAMP(...)`) and
        # TIMESTAMP('...22.64') vs '...22.640' are the FILL's predicates; the
        # COUNT replaced by SUM keeps Kuzu's INT64 type.
        import datetime
        text = self.rewrite(self.middleware('finbench', TRANSFER_AGG),
                            'MATCH (src:Account {id: $id})-[edge:AccountTransferAccount]->(dst:Account) '
                            'WHERE $start_time < edge.timestamp AND edge.timestamp < $end_time '
                            'AND edge.amount > $threshold '
                            'RETURN dst.id AS dstId, count(edge) AS numEdges, sum(edge.amount) AS sumAmount',
                            id=1, threshold=0.0,
                            start_time=datetime.datetime(2021, 7, 23, 6, 10, 16, 747000),
                            end_time=datetime.datetime(2022, 11, 29, 2, 52, 22, 640000))
        self.assertIn('FINBENCH_TRANSFER_AGG.transferAmount', text)
        self.assertRegex(text, r'CAST\(sum\(.*transferCount.*\) AS INT64\) AS numEdges')

    def test_optional_complete_group_keeps_null_for_empty_groups(self):
        text = self.rewrite(self.middleware('finbench', TSR2_OUT),
                            'MATCH (src:Account {id: 1}) OPTIONAL MATCH (src)-[edge1:AccountTransferAccount]->'
                            f'(dst1:Account) WHERE {WINDOW.format(e="edge1")} '
                            'RETURN src, sum(edge1.amount) AS s, max(edge1.amount) AS m, count(edge1) AS c')
        self.assertIn('WHEN (src.tsr2TransferOutCount > 0) THEN src.tsr2TransferOutAmount ELSE CAST(NULL', text)
        self.assertIn('WHEN (src.tsr2TransferOutCount > 0) THEN src.tsr2TransferOutCount ELSE 0', text)

    def test_staged_fill_matches_the_whole_statement(self):
        text = self.rewrite(self.middleware('finbench', TCR11),
                            'MATCH (source:Person {id: 1})-[:PersonGuaranteePerson*1..5 (edge, node | WHERE '
                            f'{WINDOW.format(e="edge")})]->(person:Person) WITH DISTINCT source, person '
                            'MATCH (person)-[:PersonApplyLoan]->(loan:Loan) WITH DISTINCT source, loan '
                            'RETURN round(coalesce(sum(loan.loanAmount), 0.0), 3) AS s, count(loan) AS n')
        self.assertIn('source.tcr11LoanCount', text)

    def test_anchor_split_keeps_earlier_bindings(self):
        # LDBC BI3: `person` is bound by the first QueryPart. Splitting the
        # second part's MATCH at WITH DISTINCT message would drop it and let
        # (forum)-[:HASMODERATOR]->(person) bind any person.
        text = self.rewrite(self.middleware('ldbc-bi', ROOT_POST),
                            'MATCH (person:PERSON)-[:ISLOCATEDIN]->(city:PLACE)-[:ISPARTOF]->(country:PLACE) '
                            "WHERE city.type = 'City' AND country.type = 'Country' "
                            'WITH person, country.name AS countryName WHERE countryName = $country '
                            'MATCH (person)<-[:HASMODERATOR]-(forum:FORUM)-[:CONTAINEROF]->(post:POST) '
                            'MATCH (message:POST:COMMENT)-[:REPLYOF*0..]->(post) '
                            'MATCH (message)-[:HASTAG]->(:TAG)-[:HASTYPE]->(:TAGCLASS {name: $tagClass}) '
                            'RETURN forum.id AS forumId, person.id AS personId, '
                            'count(DISTINCT message) AS messageCount',
                            country='India', tagClass='Congressman')
        self.assertIn('ROOT_POST', text)
        self.assertNotIn('WITH DISTINCT message', text)

    def test_materialization(self):
        mw = self.middleware('lsqb', Q5)
        resets = {e['index_id']: e['reset'] for e in mw.build_statements()}
        # A group child's reset deletes only its own edges.
        self.assertEqual(resets['q5_index_Post_Tag()'],
                         ['MATCH (:Post)-[cico_e:q5_index]->(:Tag) WITH cico_e DELETE cico_e'])
        mw = self.middleware('finbench', TCR11)
        fill = [e for e in mw.build_statements() if e['index_id'].startswith('Person(tcr11')][0]
        # A recursive relationship predicate is printed in Kuzu syntax.
        self.assertRegex(fill['fill_statements'][0], r'\*1\.\.5 \(edge, `node` \| WHERE ')


if __name__ == '__main__':
    unittest.main()

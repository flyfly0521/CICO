"""Kuzu small-graph checks of the index registry (FILL / DROP_INDEX entry).

Builds a private Kuzu store in a temporary directory, so nothing shared is
touched; every index change goes through the middleware's statement entry
and KuzuIndexExecutor. Run with a Python that has `kuzu` (0.10.x):

  <kuzu-python> middleware/tests/kuzu_registry_acceptance.py --output report.json
"""
import argparse
import collections
import json
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware, IndexRegistry  # noqa: E402
from cico.schema import Schema  # noqa: E402
from cico.targets import KuzuIndexExecutor  # noqa: E402

PERSON, CITY, REL = 'PERSON', 'CITY', 'LIVES'
SOURCE = f'MATCH (p:{PERSON})-[:{REL}]->(c:{CITY}) RETURN p.id, count(c)'
A = f'FILL {PERSON}(cached) FROM ({SOURCE})'
A2 = A.replace('RETURN p.id', 'WHERE c.id >= 2 RETURN p.id')
B = f'FILL {PERSON}(doubled) FROM (MATCH (p:{PERSON}) RETURN p.id, p.cached * 2)'
B_DOUBLE = B.replace('p.cached * 2', 'p.cached * 2.0')
DERIVED = f'FILL CACHED_LIVES() FROM (MATCH (p:{PERSON})-[:{REL}]->(c:{CITY}) RETURN DISTINCT p.id, c.id)'
QUERY = SOURCE.replace('RETURN p.id, count(c)', 'RETURN p.id AS personId, count(c) AS cnt ORDER BY personId')
FILTERED = QUERY.replace('RETURN', 'WHERE c.id >= 2 RETURN')
DERIVED_QUERY = f'MATCH (p:{PERSON})-[:{REL}]->(c:{CITY}) RETURN DISTINCT p.id AS personId, c.id AS cityId'


def schema():
    def prop(name):
        return {'property_name': name, 'property_type': {'primitive_type': 'DT_SIGNED_INT64'}}
    return Schema({'schema': {'vertex_types': [
        {'type_name': PERSON, 'type_id': 0, 'properties': [prop('id')], 'primary_keys': ['id']},
        {'type_name': CITY, 'type_id': 1, 'properties': [prop('id'), prop('score')], 'primary_keys': ['id']},
    ], 'edge_types': [{'type_name': REL, 'type_id': 0, 'properties': [], 'vertex_type_pair_relations': [
        {'source_vertex': PERSON, 'destination_vertex': CITY, 'relation': 'MANY_TO_MANY'}]}]}})


def create_store(path):
    import kuzu
    db = kuzu.Database(str(path), buffer_pool_size=1 << 30)
    conn = kuzu.Connection(db)
    for statement in (f'CREATE NODE TABLE {PERSON}(id INT64, PRIMARY KEY(id))',
                      f'CREATE NODE TABLE {CITY}(id INT64, score INT64, PRIMARY KEY(id))',
                      f'CREATE REL TABLE {REL}(FROM {PERSON} TO {CITY})',
                      f'CREATE (:{PERSON} {{id:1}}),(:{PERSON} {{id:2}}),(:{PERSON} {{id:3}}),'
                      f'(:{CITY} {{id:1, score:9}}),(:{CITY} {{id:2}})',
                      f'MATCH (p:{PERSON} {{id:1}}),(c:{CITY}) CREATE (p)-[:{REL}]->(c)',
                      f'MATCH (p:{PERSON} {{id:2}}),(c:{CITY} {{id:2}}) CREATE (p)-[:{REL}]->(c)'):
        conn.execute(statement).close()
    conn.close()
    db.close()


class Recording:
    """KuzuIndexExecutor that also records what it executed."""

    def __init__(self, runner):
        self.runner, self.statements = runner, []
        self.target = runner.target

    def __call__(self, statement):
        self.runner(statement)
        self.statements.append(statement)

    def validate(self, statements):
        self.runner.validate(statements)

    def release(self):
        self.runner.release()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = {'system': 'kuzu', 'entry': 'IndexMiddleware.execute (FILL / DROP_INDEX)',
              'workload_queries_executed': False, 'small_graph_queries_executed': True,
              'store': 'private temporary Kuzu store', 'checks': []}
    with tempfile.TemporaryDirectory(prefix='cico-kuzu-registry-') as tmp:
        store, registry_file, state = Path(tmp) / 'db', Path(tmp) / 'registry.yaml', Path(tmp) / 'state.json'
        create_store(store)
        runner = KuzuIndexExecutor(store, threads=2, buffer_pool_gb=1)
        executor = Recording(runner)
        report['target'] = json.loads(runner.target)

        def compare(mw, text):
            rewritten = mw.execute(text)
            assert rewritten.rewritten, rewritten.to_dict()
            runner.query('EXPLAIN ' + rewritten.cypher)
            before, after = runner.query(text), runner.query(rewritten.cypher)
            runner.release()
            key = lambda rows: collections.Counter(tuple(r) for r in rows)  # noqa: E731
            assert key(before) == key(after), (before, after, rewritten.cypher)
            return {'query': text, 'output': rewritten.cypher, 'rows': before, 'equal': True}

        def statement(mw, text, expected):
            executor.statements = []
            result = mw.execute(text)
            assert result.reason == expected, (text, result.to_dict())
            return {'statement': text, 'reason': result.reason, 'writes': list(executor.statements)}

        def check(name, **fields):
            report['checks'].append({'name': name, **fields})

        def middleware():
            mw = IndexMiddleware(schema(), dialect='kuzu')
            IndexRegistry(mw, registry_file, state, runner.target, executor=executor).start()
            return mw

        try:
            with middleware() as mw:
                step = statement(mw, A, 'added:active')
                hidden = [c for entry in mw.indexes()['indexes'] for c in entry['target_columns']
                          if c.startswith('__neug_fill_state_n_rows_')]
                physical = runner.query(f'MATCH (p:{PERSON}) RETURN p.id, p.cached, p.`{hidden[0]}` ORDER BY p.id')
                runner.release()
                assert physical == [[1, 2, 2], [2, 1, 1], [3, None, 0]] or physical == [[1, 2, 2], [2, 1, 1], [3, 0, 0]], physical
                check('fill_builds_count_and_hidden_state', **step, physical=physical, **compare(mw, QUERY))
            executor.statements = []
            with middleware() as mw:
                assert executor.statements == []
                check('restart_no_rebuild', writes=[], **compare(mw, QUERY))
                step = statement(mw, A.replace('MATCH', 'match\n ').replace('RETURN', 'return\n '), 'unchanged:active')
                assert step['writes'] == []
                check('equivalent_fill_no_rebuild', **step, **compare(mw, QUERY))
                step = statement(mw, B, 'added:active')
                assert not any('.cached =' in s for s in step['writes']), step
                check('added_dependent_builds_only_itself', **step, **compare(mw, QUERY))
                step = statement(mw, A2, 'changed:active')
                order = [s for s in step['writes'] if ' SET ' in s and 'cico_n' not in s]
                assert '.cached =' in order[0] and '.doubled =' in order[-1], order
                check('replaced_definition_rebuilds_dependency_chain', **step, **compare(mw, FILTERED))
                step = statement(mw, DERIVED, 'added:active')
                check('derived_relationship_materializes', **step, **compare(mw, DERIVED_QUERY))
                step = statement(mw, f'DROP_INDEX {PERSON}(cached)', 'failed:dependents')
                check('drop_with_dependents_refused', **step)
                step = statement(mw, f'DROP_INDEX {PERSON}(doubled)', 'removed')
                assert step['writes'] == []
                ids = [e['id'] for e in yaml.safe_load(registry_file.read_text())['indexes']]
                assert ids == ['PERSON(cached)', 'CACHED_LIVES()'], ids
                check('drop_index_removes_the_entry', **step, registry=ids)
                # The old INT64 column is still in the store; Kuzu's IF NOT
                # EXISTS would keep it, so validation refuses the DOUBLE carrier
                # before any write and the registry stays as it was.
                saved = registry_file.read_bytes()
                step = statement(mw, B_DOUBLE, 'failed:validate')
                assert step['writes'] == [] and registry_file.read_bytes() == saved
                check('stale_physical_carrier_refused_before_writes', **step, **compare(mw, FILTERED))
                columns = [r[1] for r in runner.query(f"CALL table_info('{PERSON}') RETURN *")]
                runner.release()
                check('validation_rolled_back', person_columns=columns)
                report['registry'] = yaml.safe_load(registry_file.read_text())
                report['state'] = json.loads(state.read_text())
        finally:
            runner.close()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str) + '\n')
    print(json.dumps({'checks': len(report['checks']), 'output': args.output}))


if __name__ == '__main__':
    main()

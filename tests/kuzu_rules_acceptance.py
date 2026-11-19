"""Kuzu small-graph checks of the shared rewrite rules under the kuzu dialect.

Builds a private Kuzu store in a temporary directory (nothing shared is
touched), registers FILLs through the index registry (KuzuIndexExecutor
really materializes them), then for every query checks that

  * the middleware rewrites it, by the expected rule (route / proofs);
  * Kuzu EXPLAINs the output;
  * original and rewritten results are equal as multisets on the store;
  * the declared constraints hold on the store (every check counts 0).

The graph contains a CITY whose name equals a country's name, so a rewrite
that dropped a type-class guard it needs would return extra rows.

  <kuzu-python> middleware/tests/kuzu_rules_acceptance.py --output report.json
"""
import argparse
import collections
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cico import IndexMiddleware, IndexRegistry  # noqa: E402
from cico.schema import Schema  # noqa: E402
from cico.targets import KuzuIndexExecutor  # noqa: E402

CONSTRAINTS = """
classes:
  Person: {tables: [PERSON]}
  Place: {tables: [PLACE]}
  Message: {tables: [MSG]}
  City: {table: PLACE, property: type, value: City}
  Country: {table: PLACE, property: type, value: Country}
constraints:
  - {kind: at_most_one, rel: ISLOCATEDIN, from: Person}
  - {kind: at_least_one, rel: ISLOCATEDIN, from: Person}
  - {kind: at_most_one, rel: ISPARTOF, from: City}
  - {kind: at_least_one, rel: ISPARTOF, from: City}
  - {kind: endpoint_labels, rel: ISLOCATEDIN, from: Person, to: City}
  - {kind: endpoint_labels, rel: ISPARTOF, from: City, to: Country}
  - {kind: unique_property, label: Country, property: name}
  - {kind: required_property, label: Country, property: name}
"""

FILLS = [
    "ALTER TABLE PERSON ADD IF NOT EXISTS countryName STRING DEFAULT ''",
    'FILL PERSON(countryName) FROM (MATCH (country:PLACE)<-[:ISPARTOF]-(:PLACE)<-[:ISLOCATEDIN]-(person:PERSON) '
    'RETURN person.id, country.name)',
    'FILL KNOWS(likes_fwd) FROM (MATCH (pA:PERSON)-[:KNOWS]->(pB:PERSON) WITH pA, pB '
    'MATCH (pA)-[:LIKES]->(m:MSG)-[:HASCREATOR]->(pB) RETURN pA.id, pB.id, count(m))',
    'FILL KNOWS(likes_rev) FROM (MATCH (pA:PERSON)-[:KNOWS]->(pB:PERSON) WITH pA, pB '
    'MATCH (pB)-[:LIKES]->(m:MSG)-[:HASCREATOR]->(pA) RETURN pA.id, pB.id, count(m))',
    'FILL PERSON(msgCount) FROM (MATCH (m:MSG)-[:HASCREATOR]->(p:PERSON) RETURN p.id, count(m))',
]

# name -> (query for the middleware, parameters, query Kuzu runs as the original, expected rule marker)
CASES = {
    'constraint_retry_key_identities': (
        "MATCH (a:PERSON)-[:ISLOCATEDIN]->(cityA:PLACE)-[:ISPARTOF]->(country:PLACE {name: $country}),"
        " (a)-[:KNOWS]-(b:PERSON) WHERE cityA.type = 'City' AND country.type = 'Country' AND a.id < b.id "
        "WITH DISTINCT country, a, b "
        "MATCH (b)-[:ISLOCATEDIN]->(cityB:PLACE)-[:ISPARTOF]->(country) WHERE cityB.type = 'City' "
        "WITH DISTINCT country, a, b RETURN a.id AS a, b.id AS b, country.id AS c",
        {'country': 'Chile'}, None, 'declared_constraints'),
    'constraint_retry_implied_types_shortest': (
        "MATCH (s:PERSON {id: $pid})-[path:KNOWS* SHORTEST 1..$maxd]-(e:PERSON) WHERE length(path) >= 2 "
        "MATCH (e)-[:ISLOCATEDIN]->(city:PLACE)-[:ISPARTOF]->(country:PLACE {name: $country}) "
        "WHERE city.type = 'City' AND country.type = 'Country' RETURN e.id AS e, length(path) AS d",
        {'pid': 1, 'maxd': 3, 'country': 'Argentina'},
        "MATCH (s:PERSON {id: $pid})-[path:KNOWS* SHORTEST 1..3]-(e:PERSON) WHERE length(path) >= 2 "
        "MATCH (e)-[:ISLOCATEDIN]->(city:PLACE)-[:ISPARTOF]->(country:PLACE {name: $country}) "
        "WHERE city.type = 'City' AND country.type = 'Country' RETURN e.id AS e, length(path) AS d",
        'declared_constraints'),
    'edge_carriers_both_orientations': (
        'MATCH (a:PERSON)-[:KNOWS]-(b:PERSON) WITH a, b, 0 AS score '
        'OPTIONAL MATCH (a)-[:LIKES]->(m:MSG)-[:HASCREATOR]->(b) '
        'WITH DISTINCT a, b, score + (CASE WHEN m IS NULL THEN 0 ELSE 10 END) AS score '
        'RETURN a.id AS a, b.id AS b, score',
        {}, None, 'relationship_carrier_existence'),
    'edge_carriers_null_anchor': (
        'MATCH (a:PERSON) OPTIONAL MATCH (a)-[:KNOWS]->(b:PERSON) WITH a, b '
        'OPTIONAL MATCH (a)-[:LIKES]->(m:MSG)-[:HASCREATOR]->(b) '
        'WITH DISTINCT a, b, m IS NULL AS missing RETURN a.id AS a, b.id AS b, missing',
        {}, None, 'relationship_carrier_existence'),
    'scalar_regions_count_subquery': (
        'MATCH (p:PERSON) RETURN p.id AS p, COUNT { MATCH (m:MSG)-[:HASCREATOR]->(p) } AS c',
        {}, None, 'correlated_graph_count'),
}


def schema():
    def prop(name, t='DT_SIGNED_INT64'):
        if t == 'DT_STRING':
            return {'property_name': name, 'property_type': {'string': {'var_char': {'max_length': 256}}}}
        return {'property_name': name, 'property_type': {'primitive_type': t}}

    ids = iter(range(100))

    def edge(name, src, dst, props=()):
        return {'type_name': name, 'type_id': next(ids), 'properties': list(props), 'vertex_type_pair_relations': [
            {'source_vertex': src, 'destination_vertex': dst, 'relation': 'MANY_TO_MANY'}]}
    return Schema({'schema': {'vertex_types': [
        {'type_name': 'PERSON', 'type_id': 0, 'properties': [prop('id')], 'primary_keys': ['id']},
        {'type_name': 'PLACE', 'type_id': 1, 'properties': [prop('id'), prop('name', 'DT_STRING'),
                                                             prop('type', 'DT_STRING')], 'primary_keys': ['id']},
        {'type_name': 'MSG', 'type_id': 2, 'properties': [prop('id')], 'primary_keys': ['id']},
    ], 'edge_types': [edge('ISLOCATEDIN', 'PERSON', 'PLACE'), edge('ISPARTOF', 'PLACE', 'PLACE'),
                      edge('KNOWS', 'PERSON', 'PERSON'), edge('LIKES', 'PERSON', 'MSG'),
                      edge('HASCREATOR', 'MSG', 'PERSON')]}})


def create_store(path):
    import kuzu
    db = kuzu.Database(str(path), buffer_pool_size=1 << 30)
    conn = kuzu.Connection(db)
    places = [(1, 'Chile', 'Country'), (2, 'Argentina', 'Country'), (11, 'Santiago', 'City'),
              (12, 'Valparaiso', 'City'), (21, 'Cordoba', 'City'), (13, 'Chile', 'City')]  # a city named Chile
    statements = ['CREATE NODE TABLE PERSON(id INT64, PRIMARY KEY(id))',
                  'CREATE NODE TABLE PLACE(id INT64, name STRING, type STRING, PRIMARY KEY(id))',
                  'CREATE NODE TABLE MSG(id INT64, PRIMARY KEY(id))',
                  'CREATE REL TABLE ISLOCATEDIN(FROM PERSON TO PLACE)', 'CREATE REL TABLE ISPARTOF(FROM PLACE TO PLACE)',
                  'CREATE REL TABLE KNOWS(FROM PERSON TO PERSON)', 'CREATE REL TABLE LIKES(FROM PERSON TO MSG)',
                  'CREATE REL TABLE HASCREATOR(FROM MSG TO PERSON)']
    statements += [f"CREATE (:PLACE {{id: {i}, name: '{n}', type: '{t}'}})" for i, n, t in places]
    statements += [f'CREATE (:PERSON {{id: {i}}})' for i in range(1, 10)]  # 9 knows nobody: a NULL anchor
    statements += [f'CREATE (:MSG {{id: {i}}})' for i in range(100, 108)]

    def rel(table, a, at, b, bt):
        return f'MATCH (x:{at} {{id: {a}}}), (y:{bt} {{id: {b}}}) CREATE (x)-[:{table}]->(y)'
    statements += [rel('ISPARTOF', c, 'PLACE', k, 'PLACE') for c, k in ((11, 1), (12, 1), (21, 2), (13, 2))]
    statements += [rel('ISLOCATEDIN', p, 'PERSON', c, 'PLACE')
                   for p, c in ((1, 11), (2, 12), (3, 21), (4, 11), (5, 21), (6, 12), (7, 13), (8, 13), (9, 21))]
    # KNOWS stored in both directions relative to the likes below; 1-2 twice (parallel edges)
    statements += [rel('KNOWS', a, 'PERSON', b, 'PERSON')
                   for a, b in ((1, 2), (2, 1), (3, 2), (1, 4), (4, 5), (5, 3), (2, 6), (6, 1), (7, 8), (8, 3))]
    statements += [rel('HASCREATOR', m, 'MSG', p, 'PERSON')
                   for m, p in ((100, 2), (101, 2), (102, 1), (103, 3), (104, 5), (105, 8), (106, 4), (107, 6))]
    statements += [rel('LIKES', p, 'PERSON', m, 'MSG')
                   for p, m in ((1, 100), (1, 101), (2, 103), (3, 101), (4, 104), (5, 106), (8, 105), (6, 102))]
    for statement in statements:
        conn.execute(statement).close()
    conn.close()
    db.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = {'system': 'kuzu', 'dialect': 'kuzu', 'workload_queries_executed': False,
              'small_graph_queries_executed': True, 'store': 'private temporary Kuzu store', 'cases': []}
    with tempfile.TemporaryDirectory(prefix='cico-kuzu-rules-') as tmp:
        tmp = Path(tmp)
        (tmp / 'constraints.yaml').write_text(CONSTRAINTS)
        create_store(tmp / 'db')
        runner = KuzuIndexExecutor(tmp / 'db', threads=2, buffer_pool_gb=1)
        key = lambda rows: collections.Counter(tuple(r) for r in rows)  # noqa: E731
        try:
            with IndexMiddleware(schema(), dialect='kuzu', constraints=tmp / 'constraints.yaml') as mw:
                report['rules'] = mw.profile.rule_status()
                checks = {}
                for check in mw.constraint_checks():
                    checks[check['constraint']] = runner.query(check['cypher'])[0][0]
                runner.release()
                assert not any(checks.values()), checks
                report['constraint_checks'] = checks
                IndexRegistry(mw, tmp / 'registry.yaml', tmp / 'state.json', runner.target, executor=runner).start()
                for statement in FILLS:
                    result = mw.execute(statement)
                    assert result.ok and 'failed' not in (result.reason or ''), result.to_dict()
                report['indexes'] = sorted(mw.fills)
                failures = []
                for name, (text, params, original, marker) in CASES.items():
                    result = mw.execute(text, params)
                    row = {'case': name, 'query': text, 'parameters': params, 'rewritten': result.rewritten,
                           'reason': result.reason, 'route': result.route, 'indexes': result.indexes,
                           'premises': result.premises, 'specialized': result.specialized,
                           'rules': sorted({p.get('rule') for p in result.proofs if isinstance(p, dict)}),
                           'output': result.cypher if result.rewritten else '', 'detail': (result.detail or '')[:600]}
                    try:
                        assert result.rewritten, 'not rewritten'
                        assert marker in (result.route or '') or marker in row['rules'], ('rule', marker)
                        used = lambda q: {k: v for k, v in params.items() if '$' + k in q}  # noqa: E731
                        runner.query('EXPLAIN ' + result.cypher, used(result.cypher))
                        before = runner.query(original or text, used(original or text))
                        after = runner.query(result.cypher, used(result.cypher))
                        row.update(rows=len(before), equal=key(before) == key(after),
                                   result=sorted(map(list, before)))
                        assert row['equal'], (before, after)
                        assert before, 'empty result: the case tests nothing'
                        if name == 'edge_carriers_null_anchor':
                            assert any(r[1] is None for r in before), 'no NULL anchor row'
                    except Exception as error:  # noqa: BLE001 - recorded, reported at the end
                        row['error'] = f'{type(error).__name__}: {error}'[:600]
                        failures.append(name)
                    finally:
                        runner.release()
                    report['cases'].append(row)
                    print(f"{name:42} rewritten={row['rewritten']!s:5} equal={row.get('equal', '-')!s:5} "
                          f"rows={row.get('rows', '-')} {row.get('error', '')}", flush=True)
                report['failures'] = failures
        finally:
            runner.close()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str) + '\n')
    print(json.dumps({'cases': len(report['cases']), 'failures': report['failures'], 'output': args.output}))
    sys.exit(1 if report['failures'] else 0)


if __name__ == '__main__':
    main()

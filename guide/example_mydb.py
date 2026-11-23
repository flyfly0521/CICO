"""Runnable example for guide/new-database.md: a toy target system "mydb".

mydb speaks Kuzu-style Cypher except that the aggregate sum() is spelled
total().  The declaration below is all CICO needs to rewrite queries for it:

    python3 guide/example_mydb.py            # from the repository root

Copy this file to cico/dialects/<yourdb>.py as a starting point.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from cico import IndexMiddleware  # noqa: E402
from cico.constraints import load_constraints  # noqa: E402
from cico.dialects.neug import KuzuDialect  # noqa: E402
from cico.frontends import KuzuFrontend  # noqa: E402
from cico.neo4j import render, tokenize  # noqa: E402
from cico.profiles import Profile, Semantics, register  # noqa: E402

# Function names that differ: target spelling (upper case) -> internal (NeuG) spelling.
TO_INTERNAL = {'TOTAL': 'sum'}


class MyDbFrontend(KuzuFrontend):
    """Input: mydb Cypher -> internal Cypher.  Rename functions on tokens
    (never with string replacement: names may occur inside literals), then
    apply the Kuzu normalisation.  Raise cico.errors.Unsupported for anything
    that cannot be translated exactly; that QueryPart is then passed through."""
    name = 'mydb'

    def to_internal(self, cypher):
        tokens = tokenize(cypher)
        for tok, nxt in zip(tokens, tokens[1:]):
            if tok.kind == 'ident' and nxt.text == '(' and tok.upper in TO_INTERNAL:
                tok.text = TO_INTERNAL[tok.upper]
        return super().to_internal(render(tokens))


class MyDbDialect(KuzuDialect):
    """Output: bound IR -> mydb Cypher.  Override only what differs
    (see cico/dialects/base.py for every hook)."""
    name = 'mydb'

    def aggregate(self, name, args, distinct):
        text = super().aggregate(name, args, distinct)
        return 'total' + text[len('sum'):] if name == 'SUM' else text


def mydb_profile(constraints=None):
    return Profile(
        name='mydb',
        frontend=MyDbFrontend(),
        dialect=MyDbDialect(),
        semantics=Semantics(
            relationship_uniqueness='homomorphism',  # may two relationships in one MATCH bind the same edge?
            variable_length='walk',                  # semantics of *l..u
            variable_length_cap=None,                # bound the target puts on an open *l..
            type_classes='property'),                # typed classes written as property predicates
        description='toy example: Kuzu Cypher with sum() spelled total()',
        constraints=load_constraints(constraints) if constraints else None)


register('mydb', mydb_profile)


def main():
    schema = ROOT / 'schemas/ldbc-bi.yaml'
    with IndexMiddleware(schema, dialect='mydb') as mw:
        print('rules:', mw.profile.rule_status())
        fill = mw.execute('FILL PERSON(knowsCount) FROM ('
                          'MATCH (p:PERSON)-[:KNOWS]-(f:PERSON) RETURN p.id, count(*))')
        print('fill:', fill.reason, fill.inferred_ddl)
        result = mw.execute('MATCH (p:PERSON)-[:KNOWS]-(f:PERSON) MATCH (p)-[:ISLOCATEDIN]->(c:PLACE) '
                            'RETURN c.name AS city, count(*) AS n')
        print('rewrite:', result.reason)
        print(result.cypher)
        for item in mw.build_statements():
            print('build:', item['ddl'] + item['reset'] + item['fill_statements'])


if __name__ == '__main__':
    main()

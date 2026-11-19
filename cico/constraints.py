"""Declared integrity constraints of the target data model.

They are PREMISES of individual rewrite steps, never of the QueryPart
theorem itself: a step that relies on one records it (Result.premises), and
the equivalence it proves holds on every database that satisfies the
declared constraints.  Each constraint therefore comes with a Cypher check
that counts its violations on the target database; run them (via
IndexMiddleware.constraint_checks()) before trusting rewrites that use it.

They are declared once per DATASET, in internal (NeuG/Kuzu) table names,
in a shared file (constraints/<dataset>.yaml) that every dialect reads:

  classes:                       # node classes of the data model
    Person:  {tables: [PERSON]}
    Message: {tables: [COMMENT, POST]}
    City:    {table: PLACE, property: type, value: City}   # typed class
  acyclic: [REPLYOF]             # relationship types without cycles
  constraints:
    - {kind: at_most_one, rel: REPLYOF, from: Message}

A dialect whose vocabulary differs (Neo4j) translates the file into its own
names when it loads it (neo4j.py: constraints_file); the legacy inline
profile syntax (Neo4j names) is still accepted, e.g.

  constraints:
    - {kind: at_most_one, rel: REPLY_OF, from: Message}   # <= 1 outgoing per Message
    - {kind: at_most_one, rel: CONTAINER_OF, to: Post}    # <= 1 incoming per Post
    - {kind: endpoint_labels, rel: IS_LOCATED_IN, from: Person, to: City}
    - {kind: unique_property, label: Tag, property: name}

Kinds
  at_most_one      every `from` node has at most one outgoing `rel`
                   (or: every `to` node has at most one incoming `rel`)
  at_least_one     every `from` node has at least one outgoing `rel`
                   (or: every `to` node has at least one incoming `rel`)
  endpoint_labels  every `rel` leaving a `from` node ends at a `to` node
  unique_property  no two `label` nodes share a non-null `property` value
  required_property  every `label` node has a non-null `property`
"""
from dataclasses import dataclass

from pathlib import Path

import yaml

KINDS = {'at_most_one', 'at_least_one', 'endpoint_labels', 'unique_property', 'required_property'}


@dataclass(frozen=True)
class Constraint:
    kind: str
    rel: str = ''
    src: str = ''
    dst: str = ''
    label: str = ''
    prop: str = ''

    def describe(self):
        if self.kind in ('at_most_one', 'at_least_one'):
            return (f'{self.kind}: (:{self.src})-[:{self.rel}]->' if self.src
                    else f'{self.kind}: -[:{self.rel}]->(:{self.dst})')
        if self.kind == 'endpoint_labels':
            return f'endpoint_labels: (:{self.src})-[:{self.rel}]->(:{self.dst})'
        return f'{self.kind}: {self.label}.{self.prop}'

    def check(self):
        """Neo4j Cypher returning one row {violations: n}; n must be 0."""
        if self.kind == 'at_most_one' and self.src:
            return (f'MATCH (n:`{self.src}`)-[r:`{self.rel}`]->() WITH n, count(r) AS c '
                    f'WHERE c > 1 RETURN count(n) AS violations')
        if self.kind == 'at_most_one':
            return (f'MATCH ()-[r:`{self.rel}`]->(n:`{self.dst}`) WITH n, count(r) AS c '
                    f'WHERE c > 1 RETURN count(n) AS violations')
        if self.kind == 'at_least_one' and self.src:
            return (f'MATCH (n:`{self.src}`) WHERE NOT EXISTS {{ (n)-[:`{self.rel}`]->() }} '
                    f'RETURN count(n) AS violations')
        if self.kind == 'at_least_one':
            return (f'MATCH (n:`{self.dst}`) WHERE NOT EXISTS {{ ()-[:`{self.rel}`]->(n) }} '
                    f'RETURN count(n) AS violations')
        if self.kind == 'endpoint_labels':
            return (f'MATCH (:`{self.src}`)-[r:`{self.rel}`]->(x) WHERE NOT x:`{self.dst}` '
                    f'RETURN count(r) AS violations')
        if self.kind == 'required_property':
            return f'MATCH (n:`{self.label}`) WHERE n.`{self.prop}` IS NULL RETURN count(n) AS violations'
        return (f'MATCH (n:`{self.label}`) WHERE n.`{self.prop}` IS NOT NULL '
                f'WITH n.`{self.prop}` AS v, count(n) AS c WHERE c > 1 RETURN count(v) AS violations')


class Constraints:
    def __init__(self, entries, labels=None, typed=(), classes=None, acyclic=(), source=None):
        self.classes = classes        # internal class definitions (shared file), None for legacy profiles
        self.acyclic = set(acyclic)   # relationship names declared acyclic, in this vocabulary
        self.source = source
        self.items = []
        for entry in entries or []:
            kind = entry.get('kind')
            if kind not in KINDS:
                raise ValueError(f'unknown constraint kind {kind!r}')
            item = Constraint(kind, entry.get('rel', ''), entry.get('from', ''), entry.get('to', ''),
                              entry.get('label', ''), entry.get('property', ''))
            if kind in ('at_most_one', 'at_least_one') and bool(item.src) == bool(item.dst):
                raise ValueError(f'{kind} needs exactly one of from/to: {entry}')
            if kind == 'endpoint_labels' and not (item.rel and item.src and item.dst):
                raise ValueError(f'endpoint_labels needs rel, from and to: {entry}')
            if kind in ('unique_property', 'required_property') and not (item.label and item.prop):
                raise ValueError(f'{kind} needs label and property: {entry}')
            self.items.append(item)
        self.labels = labels or {}   # Neo4j label -> internal tables (for coverage tests)
        self.typed = set(typed)      # labels that are a predicate on a table (City on PLACE)

    def _covers(self, label, tables, known=()):
        """Does every node with internal tables `tables` (and the labels
        `known` to hold for it) carry `label`?  A typed label (City) is a
        predicate on its table, so table membership never implies it; it must
        be known, i.e. declared on the node or derived from endpoint_labels."""
        if label in known:
            return True
        if label in self.typed:
            return False
        own = {t.upper() for t in self.labels.get(label, [label])}
        return bool(tables) and {t.upper() for t in tables} <= own

    def at_most_one_out(self, rel, tables, known=()):
        return next((c for c in self.items if c.kind == 'at_most_one' and c.rel == rel and c.src
                     and self._covers(c.src, tables, known)), None)

    def at_most_one_in(self, rel, tables, known=()):
        return next((c for c in self.items if c.kind == 'at_most_one' and c.rel == rel and c.dst
                     and self._covers(c.dst, tables, known)), None)

    def at_least_one_out(self, rel, tables, known=()):
        return next((c for c in self.items if c.kind == 'at_least_one' and c.rel == rel and c.src
                     and self._covers(c.src, tables, known)), None)

    def at_least_one_in(self, rel, tables, known=()):
        return next((c for c in self.items if c.kind == 'at_least_one' and c.rel == rel and c.dst
                     and self._covers(c.dst, tables, known)), None)

    def implied_endpoint(self, rel, known):
        """Constraints `(:X)-[:rel]->(:Y)` with X in the known labels of the source."""
        return [c for c in self.items if c.kind == 'endpoint_labels' and c.rel == rel and c.src in known]

    def unique(self, label, prop):
        return next((c for c in self.items if c.kind == 'unique_property'
                     and c.label == label and c.prop == prop), None)

    def key(self, label, prop=None):
        """(property, unique, required) of a declared key of `label`: a
        property that is unique and never null, so it identifies the node."""
        for u in self.items:
            if u.kind == 'unique_property' and u.label == label and (prop is None or u.prop == prop):
                r = next((c for c in self.items if c.kind == 'required_property'
                          and c.label == label and c.prop == u.prop), None)
                if r is not None:
                    return u.prop, u, r
        return None

    def checks(self):
        """[{constraint, cypher}] counting violations; Neo4j Cypher for legacy
        profiles, internal (NeuG/Kuzu) Cypher for shared-file constraints."""
        render = self._internal_check if self.classes is not None else Constraint.check
        return [dict(constraint=c.describe(), cypher=render(c)) for c in self.items]

    # ------------------------------------------- internal (Kuzu/NeuG) checks
    def _node(self, var, name):
        spec = self.classes.get(name)
        if spec is None:
            raise ValueError(f'class {name} is not declared in {self.source or "the constraints file"}')
        if 'table' in spec:
            return f'({var}:{spec["table"]})', f"{var}.{spec['property']} = '{spec['value']}'"
        return f"({var}:{':'.join(spec['tables'])})", ''

    def _member(self, var, name):
        spec = self.classes[name]
        if 'table' in spec:
            return f"(label({var}) = '{spec['table']}' AND {var}.{spec['property']} = '{spec['value']}')"
        return f"label({var}) IN [{', '.join(repr(t) for t in spec['tables'])}]"

    def _internal_check(self, c):
        def where(*conditions):
            conditions = [x for x in conditions if x]
            return (' WHERE ' + ' AND '.join(conditions)) if conditions else ''
        if c.kind in ('at_most_one', 'at_least_one'):
            node, pred = self._node('n', c.src or c.dst)
            edge = f'(n)-[r:{c.rel}]->()' if c.src else f'()-[r:{c.rel}]->(n)'
            if c.kind == 'at_most_one':
                return (f'MATCH {node}{where(pred)} MATCH {edge} WITH n, count(r) AS c '
                        f'WHERE c > 1 RETURN count(n) AS violations')
            plain = f'(n)-[:{c.rel}]->()' if c.src else f'()-[:{c.rel}]->(n)'
            return f'MATCH {node}{where(pred, "NOT EXISTS { MATCH " + plain + " }")} RETURN count(n) AS violations'
        if c.kind == 'endpoint_labels':
            node, pred = self._node('n', c.src)
            return (f'MATCH {node}-[r:{c.rel}]->(x){where(pred, "NOT " + self._member("x", c.dst))} '
                    f'RETURN count(r) AS violations')
        node, pred = self._node('n', c.label)
        if c.kind == 'required_property':
            return f'MATCH {node}{where(pred, f"n.{c.prop} IS NULL")} RETURN count(n) AS violations'
        return (f'MATCH {node}{where(pred, f"n.{c.prop} IS NOT NULL")} WITH n.{c.prop} AS v, count(n) AS c '
                f'WHERE c > 1 RETURN count(v) AS violations')


def read_file(path):
    """Parse a shared constraints file: (classes, acyclic, entries)."""
    doc = yaml.safe_load(Path(path).read_text()) or {}
    classes = {}
    for name, spec in (doc.get('classes') or {}).items():
        if 'tables' in spec:
            classes[name] = {'tables': [t.upper() for t in spec['tables']]}
        elif {'table', 'property', 'value'} <= set(spec):
            classes[name] = {'table': spec['table'].upper(), 'property': spec['property'], 'value': spec['value']}
        else:
            raise ValueError(f'{path}: class {name} needs tables, or table/property/value')
    entries = list(doc.get('constraints') or [])
    for entry in entries:
        for key in ('from', 'to', 'label'):
            if entry.get(key) and entry[key] not in classes:
                raise ValueError(f'{path}: {entry} uses undeclared class {entry[key]}')
        if entry.get('rel'):
            entry['rel'] = entry['rel'].upper()
    return classes, [r.upper() for r in doc.get('acyclic') or []], entries


def load_constraints(path):
    """Constraints in internal names (Kuzu/NeuG dialects) from a shared file."""
    classes, acyclic, entries = read_file(path)
    labels = {name: spec.get('tables') or [spec['table']] for name, spec in classes.items()}
    typed = [name for name, spec in classes.items() if 'table' in spec]
    return Constraints(entries, labels, typed=typed, classes=classes, acyclic=acyclic, source=str(path))

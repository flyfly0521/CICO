"""Neo4j profile: schema mapping + Cypher translation in both directions.

Neo4jFrontend   Neo4j Cypher  -> internal (NeuG) Cypher, for binding/rewriting
Neo4jDialect    Bound IR      -> Neo4j Cypher, for output

Both are driven by a mapping file (profiles/ldbc-snb-neo4j.yaml).  The
translation is token based and pattern aware; anything it cannot translate
faithfully raises Unsupported, so the middleware returns the original query.
Output is verified by translating it back and re-binding.
"""
import re
from pathlib import Path

import yaml

from .dialects.base import Dialect
from .errors import Unsupported
from .statements import strip_comments

UNBOUNDED = 4294967295
TEMPORAL_FIELDS = {'year', 'month', 'day', 'hour', 'minute', 'second', 'millisecond'}
TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<string>'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")
  | (?P<quoted>`(?:[^`]|``)*`)
  | (?P<param>\$(?:[A-Za-z_][A-Za-z0-9_]*|`[^`]+`|\d+))
  | (?P<number>\d+\.\d+(?:[eE][-+]?\d+)?|\d+(?:[eE][-+]?\d+)?)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<punct><-|->|<=|>=|<>|=~|\.\.|.)
""", re.X | re.S)
PATTERN_KEYWORDS = {'MATCH', 'MERGE', 'CREATE'}
CLAUSE_END = {'WHERE', 'WITH', 'RETURN', 'UNWIND', 'ORDER', 'SET', 'DELETE', 'DETACH', 'UNION',
              'LIMIT', 'SKIP', 'CALL', 'OPTIONAL', 'MATCH', 'REMOVE', 'FOREACH', 'MERGE', 'CREATE'}


def closing_index(tokens, opening):
    from .syntax import closing
    return closing(tokens, opening)


def load_mapping(path, constraints=None):
    with open(path) as handle:
        mapping = yaml.safe_load(handle)
    mapping.setdefault('labels', {})
    mapping.setdefault('typed_labels', {})
    mapping.setdefault('rel_types', {})
    mapping.setdefault('acyclic_rel_types', [])  # variable-length walks == trails on these
    mapping.setdefault('constraints', [])        # declared integrity constraints (constraints.py)
    mapping.setdefault('properties', {})         # internal table -> {Neo4j property: internal property}
    shared = constraints or (Path(path).parent / mapping['constraints_file']
                             if mapping.get('constraints_file') else None)
    if shared is not None:
        if mapping['constraints'] or mapping['acyclic_rel_types']:
            raise ValueError(f'{path}: declare constraints either inline or in constraints_file, not both')
        _translate_constraints(mapping, shared)
    return mapping


def _translate_constraints(mapping, path):
    """Shared constraints (internal names) -> this mapping's Neo4j names.

    Class names must be Neo4j labels with the same meaning (same tables, or
    the same typed discriminator); relationship tables map to the Neo4j type
    that contains them."""
    from .constraints import read_file
    classes, acyclic, entries = read_file(path)
    rel_name = {}
    for name, tables in mapping['rel_types'].items():
        for table in tables:
            if rel_name.setdefault(table.upper(), name) != name:
                raise ValueError(f'internal relationship {table} maps to several Neo4j types')

    def rel(table):
        if table not in rel_name:
            raise ValueError(f'{path}: relationship {table} has no Neo4j type in the mapping')
        return rel_name[table]

    for name, spec in classes.items():
        if 'table' in spec:
            typed = mapping['typed_labels'].get(name) or {}
            ok = (typed.get('table', '').upper(), typed.get('property'), typed.get('value')) == \
                (spec['table'], spec['property'], spec['value'])
        else:
            ok = sorted(t.upper() for t in mapping['labels'].get(name, [])) == sorted(spec['tables'])
        if not ok:
            raise ValueError(f'{path}: class {name} does not match the Neo4j label {name} of the mapping')
    translated = []
    for entry in entries:
        entry = dict(entry)
        if entry.get('rel'):
            entry['rel'] = rel(entry['rel'])
        translated.append(entry)
    mapping['constraints'] = translated
    mapping['acyclic_rel_types'] = list(dict.fromkeys(rel(t) for t in acyclic))
    # The mapping is now self-contained (inline form); dumping and reloading
    # it must not read the file a second time.
    mapping.pop('constraints_file', None)
    mapping['constraints_source'] = str(path)


class Tok:
    __slots__ = ('kind', 'text', 'ws')

    def __init__(self, kind, text, ws=''):
        self.kind, self.text, self.ws = kind, text, ws

    @property
    def upper(self):
        return self.text.upper()

    def __repr__(self):
        return f'{self.kind}:{self.text}'


def tokenize(text):
    tokens, ws = [], ''
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind == 'ws':
            ws += m.group()
            continue
        tokens.append(Tok(kind, m.group(), ws))
        ws = ''
    return tokens


def render(tokens):
    return ''.join(t.ws + t.text for t in tokens).strip()


def _ident(tok):
    return tok.text[1:-1].replace('``', '`') if tok.kind == 'quoted' else tok.text


_ESCAPES = {'\\': '\\', "'": "'", '"': '"', 'n': '\n', 't': '\t', 'r': '\r', 'b': '\b', 'f': '\f'}


def _unquote(literal):
    """Value of a single-quoted Cypher string literal."""
    body = literal[1:-1]
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body) and body[i + 1] in _ESCAPES:
            out.append(_ESCAPES[body[i + 1]])
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def _quote(text):
    """A single-quoted Cypher string literal with value `text`."""
    return "'" + text.replace('\\', '\\\\').replace("'", "\\'") + "'"


def _q(name):
    return name if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name) else '`' + name.replace('`', '``') + '`'


class Neo4jFrontend:
    """Neo4j Cypher -> internal Cypher."""
    name = 'neo4j'

    def __init__(self, mapping):
        self.m = mapping
        self.labels = {k: v for k, v in mapping['labels'].items()}
        self.typed = mapping['typed_labels']
        self.rels = mapping['rel_types']
        self.procedure_types = {name.lower(): contract for name, contract in
                                mapping.get('procedure_types', {}).items()}
        self.acyclic = set(mapping['acyclic_rel_types'])
        from .constraints import Constraints
        typed = {k: [v['table']] for k, v in mapping['typed_labels'].items()}
        self.constraints = Constraints(mapping['constraints'], {**mapping['labels'], **typed},
                                       typed=mapping['typed_labels'].keys())
        self.relax = False      # apply constraint-justified simplifications (retry stage)
        self.premises = set()   # constraints the last translations relied on
        self.prop_names = {t.upper(): dict(m) for t, m in mapping['properties'].items()}
        self.rel_pairs = {}     # internal relationship table -> {(src, dst)}
        self.node_names = set()
        self.node_case = {}
        self.var_tables = {}    # per statement: variable -> node tables

    def set_schema(self, schema):
        self.node_names = {v['type_name'].upper() for v in schema.vertices}
        self.node_case = {v['type_name'].upper(): v['type_name'] for v in schema.vertices}
        self.rel_pairs = {}
        for edge in schema.edges:
            pairs = {(p['source_vertex'].upper(), p['destination_vertex'].upper())
                     for p in edge.get('vertex_type_pair_relations') or []}
            self.rel_pairs[edge['type_name'].upper()] = pairs

    # ----------------------------------------------------------- public API
    def to_internal(self, cypher):
        tokens = tokenize(strip_comments(cypher).strip().rstrip(';'))
        tokens = self._count_relationships(tokens)
        tokens = self._drop_hints(tokens)
        tokens = self._shortest_paths(tokens)
        tokens = self._walk_reachability(self._path_edge_predicates(tokens))
        if self.relax:
            tokens = self._implied_typed_labels(self._key_identities(tokens))
        self.var_tables = self._collect_var_tables(tokens)
        from .normalize import entity_id_comparisons, rewrite_coalesce, split_chained_comparisons
        tokens = split_chained_comparisons(rewrite_coalesce(tokens))
        tokens = self._with_uniqueness(tokens)
        if self.relax:
            tokens = self._reanchor_root_paths(tokens)
        multi ={t for t, pairs in self.rel_pairs.items() if len(pairs) > 1}
        internal = render(entity_id_comparisons(tokenize(render(self._translate(tokens))), multi))
        if self.relax:
            internal = self._optional_carrier_edges(internal)
        return internal

    # ------------------------------------------- queries inside procedures
    EMBEDDED = {('gds', 'graph', 'project', 'cypher'): (1, 2)}   # procedure -> query argument positions

    def embedded_queries(self, cypher):
        """Cypher queries passed as string literals to a procedure that runs
        them (GDS Cypher projection: node and relationship query).  The
        procedure's result depends on such a query only through its rows, so
        a rewrite with the same rows (and columns) keeps the statement's
        meaning.  -> (tokens, [(token index, query text, {inner: outer parameter})]),
        or None.  Parameters reach the query through the config map entry
        `parameters: {k: $k}`; a query naming any other parameter is skipped."""
        from .normalize import call_args
        tokens = tokenize(strip_comments(cypher).strip().rstrip(';'))
        found = []
        for i, t in enumerate(tokens):
            for name, positions in self.EMBEDDED.items():
                k = 2 * len(name) - 1
                if not (i + k < len(tokens) and tokens[i + k].text == '('
                        and all(tokens[i + 2 * m].kind == 'ident' and tokens[i + 2 * m].text.lower() == part
                                and (m == 0 or tokens[i + 2 * m - 1].text == '.') for m, part in enumerate(name))):
                    continue
                if i and tokens[i - 1].text == '.':
                    continue
                _, args = call_args(tokens, i + k)
                mapping = self._config_parameters(args[3]) if len(args) > 3 else {}
                if mapping is None:
                    continue
                offset = i + k + 1
                for n, arg in enumerate(args):
                    if n in positions and len(arg) == 1 and arg[0].kind == 'string' and arg[0].text[0] == "'":
                        text = _unquote(arg[0].text)
                        names = set(re.findall(r'\$([A-Za-z_][A-Za-z0-9_]*)', strip_comments(text)))
                        if names <= set(mapping):
                            found.append((tokens.index(arg[0], offset), text, mapping))
        return (tokens, found) if found else None

    @staticmethod
    def _config_parameters(arg):
        """{inner: outer} from a config map literal's `parameters: {k: $v, ...}`
        ({} without that entry; None when the map is not of that form)."""
        if not arg or arg[0].text != '{' or arg[-1].text != '}':
            return None
        body = arg[1:-1]
        for k, t in enumerate(body):
            if t.kind == 'ident' and t.text == 'parameters' and k + 2 < len(body) and body[k + 1].text == ':' \
                    and body[k + 2].text == '{' and (k == 0 or body[k - 1].text == ','):
                close = closing_index(body, k + 2)
                inner, out = body[k + 3:close], {}
                for entry in [inner[m:m + 3] for m in range(0, len(inner), 4)]:
                    if len(entry) != 3 or entry[0].kind != 'ident' or entry[1].text != ':' \
                            or entry[2].kind != 'param':
                        return None
                    out[entry[0].text] = entry[2].text[1:]
                if any(inner[m].text != ',' for m in range(3, len(inner), 4)):
                    return None
                return out
        return {}

    @staticmethod
    def splice(tokens, replacements):
        """Render tokens with string tokens replaced: {token index: query text}."""
        out = []
        for i, t in enumerate(tokens):
            out.append(t.ws + (_quote(replacements[i]) if i in replacements else t.text))
        return ''.join(out).strip()

    def _optional_carrier_edges(self, internal):
        """Relax stage: relax.py E1 on the translated internal text.  The
        internal text has NeuG (homomorphism) semantics with Neo4j's
        relationship uniqueness already explicit, so the E1 argument applies
        unchanged; a unique_edge constraint on a Neo4j type holds for each of
        its internal tables."""
        import dataclasses
        import types
        from .relax import InternalRelax
        items = [dataclasses.replace(c, rel=table) for c in self.constraints.items if c.kind == 'unique_edge'
                 for table in self.rels.get(c.rel, [c.rel])]
        if not items:
            return internal
        relax = InternalRelax.__new__(InternalRelax)
        relax.c = types.SimpleNamespace(items=items)
        return render(relax.optional_carrier_edges(tokenize(internal), self.premises))

    @staticmethod
    def _count_relationships(tokens):
        # In a successful match a relationship and its fresh anonymous end
        # node are both non-null; in an unmatched OPTIONAL they are both null.
        # COUNT (without DISTINCT) therefore has identical multiplicity.
        counts = {tokens[i + 2].text for i, t in enumerate(tokens[:-3])
                  if t.upper == 'COUNT' and tokens[i + 1].text == '('
                  and tokens[i + 2].kind in ('ident', 'quoted') and tokens[i + 3].text == ')'}
        edits, replacements = [], {}
        occupied = {t.text for t in tokens}
        for i, t in enumerate(tokens[:-3]):
            if t.text != '[' or not i or tokens[i - 1].text not in ('-', '<-'):
                continue
            if tokens[i + 1].text not in counts or tokens[i + 2].text != ':':
                continue
            name = tokens[i + 1].text
            uses = [k for k, x in enumerate(tokens) if x.kind in ('ident', 'quoted') and x.text == name]
            if any(k != i + 1 and not (k >= 2 and k + 1 < len(tokens) and tokens[k - 2].upper == 'COUNT'
                                      and tokens[k - 1].text == '(' and tokens[k + 1].text == ')') for k in uses):
                continue
            end = i + 3
            while end < len(tokens) and tokens[end].text != ']':
                end += 1
            if any(x.text == '*' for x in tokens[i:end]):
                continue
            node = end + 2
            if node + 1 >= len(tokens) or tokens[node].text != '(' or tokens[node + 1].text not in (':', ')'):
                continue
            name = 'cico_count_node'
            while name in occupied:
                name += '_'
            occupied.add(name)
            edits.append((node + 1, Tok('ident', name)))
            replacements[tokens[i + 1].text] = name
        for i, t in enumerate(tokens[:-3]):
            if t.upper == 'COUNT' and tokens[i + 1].text == '(' and tokens[i + 3].text == ')':
                token = tokens[i + 2]
                if token.text in replacements:
                    token.text = replacements[token.text]
        for position, token in reversed(edits):
            tokens.insert(position, token)
        return tokens

    # ------------------------------------------------------- plan hints
    HINT_END = {'WHERE', 'WITH', 'RETURN', 'MATCH', 'OPTIONAL', 'UNWIND', 'USING', 'ORDER', 'CALL', 'UNION',
                'CREATE', 'MERGE', 'SET', 'DELETE', 'DETACH', 'REMOVE', 'FOREACH', 'SKIP', 'LIMIT'}

    def _drop_hints(self, tokens):
        """`USING INDEX|SCAN|JOIN ...` only steers Neo4j's planner; dropping
        it keeps the meaning of the query."""
        out, i, depth = [], 0, 0
        while i < len(tokens):
            t = tokens[i]
            if depth == 0 and t.kind == 'ident' and t.upper == 'USING':
                i += 1
                inner = 0
                while i < len(tokens):
                    u = tokens[i]
                    if u.text in ('(', '[', '{'):
                        inner += 1
                    elif u.text in (')', ']', '}'):
                        inner -= 1
                    elif inner == 0 and u.kind == 'ident' and u.upper in self.HINT_END:
                        break
                    i += 1
                continue
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            out.append(t)
            i += 1
        return out

    # ---------------------------------------------------- shortest paths
    def _shortest_paths(self, tokens):
        """`p = shortestPath((a)-[r:T*l..u]-(b))` -> `p = (a)-[r:T* SHORTEST l..u]-(b)`
        (`allShortestPaths` -> `ALL SHORTEST`).  Both return one / all
        shortest paths per endpoint pair.  Neo4j evaluates WHERE predicates
        on the path or its relationships during the search (the shortest path
        that satisfies them); the internal form filters afterwards, so such
        predicates are rejected."""
        out, i = [], 0
        while i < len(tokens):
            t = tokens[i]
            name = t.text.lower() if t.kind == 'ident' else None
            if name in ('shortestpath', 'allshortestpaths') and i + 1 < len(tokens) and tokens[i + 1].text == '(':
                end, inner = self._call_args(tokens, i + 1)
                stars = [k for k, x in enumerate(inner) if x.text == '*']
                opens = [k for k, x in enumerate(inner) if x.text == '[']
                if len(stars) != 1 or len(opens) != 1:
                    raise Unsupported(f'{t.text}() over a pattern with {len(opens)} relationships')
                rel_var = inner[opens[0] + 1].text if inner[opens[0] + 1].kind in ('ident', 'quoted') else None
                path_var = tokens[i - 2].text if i >= 2 and tokens[i - 1].text == '=' else None
                self._no_path_predicate(tokens, end, {v for v in (rel_var, path_var) if v}, t.text)
                mode = ['SHORTEST'] if name == 'shortestpath' else ['ALL', 'SHORTEST']
                k = stars[0] + 1
                inner = inner[:k] + [Tok('ident', w, ' ') for w in mode] + inner[k:]
                if k + len(mode) < len(inner) and inner[k + len(mode)].text not in (']',):
                    inner[k + len(mode)].ws = ' '
                inner[0].ws = t.ws
                out.extend(inner)
                i = end
                continue
            out.append(t)
            i += 1
        return out

    def _no_path_predicate(self, tokens, start, names, function):
        depth, i, in_where = 0, start, False
        while i < len(tokens):
            t = tokens[i]
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper in self.CLAUSE_STARTS:
                if t.upper != 'WHERE':
                    return
                in_where = True
            if in_where and t.kind in ('ident', 'quoted') and t.text in names:
                raise Unsupported(f'WHERE on a {function}() path or relationship: Neo4j applies it during the search')
            i += 1

    # ------------------------------------ variable-length path predicates
    PREDICATE_WORDS = {'AND', 'OR', 'XOR', 'NOT', 'TRUE', 'FALSE', 'NULL', 'IS', 'IN', 'STARTS', 'ENDS',
                       'CONTAINS', 'WITH', 'CASE', 'WHEN', 'THEN', 'ELSE', 'END'}
    AGGREGATE_NAMES = {'COUNT', 'SUM', 'AVG', 'MIN', 'MAX', 'COLLECT', 'STDEV', 'STDEVP',
                       'PERCENTILECONT', 'PERCENTILEDISC'}

    def _clause_bounds(self, tokens):
        """[(position, clause word)] of the top-level clauses, then (n, 'END')."""
        n, depth, bounds = len(tokens), 0, []
        for i, t in enumerate(tokens):
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper in self.CLAUSE_STARTS \
                    and not (i and tokens[i - 1].text == '.'):
                if t.upper == 'MATCH' and bounds and bounds[-1][1] == 'OPTIONAL' and bounds[-1][0] == i - 1:
                    continue
                bounds.append((i, t.upper))
        bounds.append((n, 'END'))
        return bounds

    @staticmethod
    def _depth0(tokens, start, end):
        """Positions in [start, end) at bracket depth 0 relative to start."""
        out, depth = [], 0
        for i in range(start, end):
            t = tokens[i]
            if depth == 0:
                out.append(i)
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
        return out

    def _pattern_rels(self, tokens, start, end):
        """Relationship brackets `-[ ... ]-` at depth 0 of a pattern span: [(open, close)]."""
        return [(i, closing_index(tokens, i)) for i in self._depth0(tokens, start, end)
                if tokens[i].text == '[' and i and tokens[i - 1].text in ('-', '<-')]

    def _reads_only(self, body, name):
        """Does the expression `body` read no variable but `name` (and
        parameters, literals, property keys and function names)?  No list,
        map or pattern syntax."""
        for k, t in enumerate(body):
            if t.text in ('[', '{', '|'):
                return False
            if t.kind not in ('ident', 'quoted'):
                continue
            if t.kind == 'ident' and t.upper in self.PREDICATE_WORDS:
                continue
            if k and body[k - 1].text in ('.', ':'):
                continue                      # property key / label
            if k + 1 < len(body) and body[k + 1].text == '(':
                continue                      # function name
            if _ident(t) != name:
                return False
        return True

    def _path_edge_predicates(self, tokens):
        """`MATCH p = (a)-[:T*l..u]->(b) WHERE all(x IN relationships(p) WHERE P)`
        -> `MATCH (a)-[:T*l..u (x, n | WHERE P)]->(b)`: the internal recursive
        relationship predicate holds on every relationship of the path, as
        all() requires.  Only when p is that path's only use, the path has one
        relationship pattern (variable-length), P reads x alone (with
        parameters and literals), and the WHERE is a conjunction."""
        while True:
            edited = None
            bounds = self._clause_bounds(tokens)
            for k, (start, word) in enumerate(bounds[:-1]):
                if word in ('MATCH', 'OPTIONAL') and bounds[k + 1][1] == 'WHERE':
                    edited = self._path_edge_predicate(tokens, start + (2 if word == 'OPTIONAL' else 1),
                                                       bounds[k + 1][0], bounds[k + 2][0])
                    if edited is not None:
                        break
            if edited is None:
                return tokens
            tokens = edited

    def _path_edge_predicate(self, tokens, ps, pe, we):
        top = self._depth0(tokens, pe + 1, we)
        if any(tokens[i].kind == 'ident' and tokens[i].upper in ('OR', 'XOR') for i in top):
            return None
        ands = [i for i in top if tokens[i].kind == 'ident' and tokens[i].upper == 'AND']
        cuts = [pe] + ands + [we]
        for c in range(len(cuts) - 1):
            cs, ce = cuts[c] + 1, cuts[c + 1]
            x = tokens[cs:ce]
            if not (len(x) >= 11 and x[0].kind == 'ident' and x[0].upper == 'ALL' and x[1].text == '('
                    and x[2].kind in ('ident', 'quoted') and x[3].kind == 'ident' and x[3].upper == 'IN'
                    and x[4].kind == 'ident' and x[4].text.lower() == 'relationships' and x[5].text == '('
                    and x[6].kind in ('ident', 'quoted') and x[7].text == ')'
                    and x[8].kind == 'ident' and x[8].upper == 'WHERE'
                    and closing_index(tokens, cs + 1) == ce - 1):
                continue
            var, path, body = _ident(x[2]), _ident(x[6]), x[9:-1]
            if not body or not self._reads_only(body, var):
                continue
            assign = [i for i in self._depth0(tokens, ps, pe)
                      if i + 2 < pe and tokens[i].kind in ('ident', 'quoted') and _ident(tokens[i]) == path
                      and tokens[i + 1].text == '=' and tokens[i + 2].text == '(']
            if len(assign) != 1:
                continue
            a = assign[0]
            element_end = next((i for i in self._depth0(tokens, a, pe) if tokens[i].text == ','), pe)
            rels = self._pattern_rels(tokens, a + 2, element_end)
            if len(rels) != 1 or not any(t.text == '*' for t in tokens[rels[0][0]:rels[0][1]]):
                continue
            uses = [i for i, t in enumerate(tokens) if t.kind in ('ident', 'quoted')
                    and not (i and tokens[i - 1].text == '.')]
            if any(_ident(tokens[i]) == path for i in uses if i not in (a, cs + 6)):
                continue
            if any(_ident(tokens[i]) == var for i in uses if not cs <= i < ce):
                continue
            node = 'cico_hop_node'
            while any(t.text == node for t in tokens):
                node += '_'
            inner = tokenize(f' ({_q(var)}, {node} | WHERE ') + [Tok(t.kind, t.text, t.ws) for t in body] \
                + [Tok('punct', ')')]
            close = rels[0][1]
            # drop the conjunct (with one adjacent AND), or the whole WHERE
            if len(cuts) == 2:
                drop = range(pe, we)
            elif c + 1 < len(cuts) - 1:
                drop = range(cs, cuts[c + 1] + 1)
            else:
                drop = range(cuts[c], ce)
            out = []
            for i, t in enumerate(tokens):
                if i in drop or i in (a, a + 1):
                    continue
                if i == a + 2:
                    t.ws = tokens[a].ws
                if i == close:
                    out.extend(inner)
                out.append(t)
            return out
        return None

    def _walk_reachability(self, tokens):
        """Mark `* WALK` a variable-length relationship whose Neo4j trails and
        internal walks give the same rows: the only relationship pattern of
        its MATCH clause, directed, lower bound 0 or 1, no path or relationship
        variable used, and the clause followed (after an optional WHERE) by a
        `WITH DISTINCT` without aggregates.  Every walk from a to b of length
        k >= 1 contains a trail from a to b of length 1..k over a subset of its
        edges (shortcut repeated nodes; a directed closed walk contains a
        directed cycle through a), so the distinct bindings of the other
        variables agree, with per-edge predicates and property maps as well."""
        bounds = self._clause_bounds(tokens)
        marks = []
        for k, (start, word) in enumerate(bounds[:-1]):
            if word not in ('MATCH', 'OPTIONAL'):
                continue
            ps, pe = start + (2 if word == 'OPTIONAL' else 1), bounds[k + 1][0]
            rels = self._pattern_rels(tokens, ps, pe)
            if len(rels) != 1:
                continue
            open_, close = rels[0]
            inner = tokens[open_ + 1:close]
            star = next((m for m, t in enumerate(inner) if t.text == '*'), None)
            if star is None:
                continue
            left, right = tokens[open_ - 1].text, tokens[close + 1].text if close + 1 < len(tokens) else ''
            if (left == '<-') == (right == '->'):
                continue                      # undirected (or both arrows)
            after = inner[star + 1:]
            if after and after[0].kind == 'ident' and after[0].upper in ('SHORTEST', 'ALL', 'WALK'):
                continue
            lower = int(after[0].text) if after and after[0].kind == 'number' else 1
            if lower > 1:
                continue                      # *k.. / *k with k > 1
            if any(t.text == '=' for t in (tokens[i] for i in self._depth0(tokens, ps, pe))):
                continue                      # a path variable
            var = _ident(inner[0]) if inner and inner[0].kind in ('ident', 'quoted') and star > 0 \
                and (len(inner) > 1 and inner[1].text in (':', '*')) else None
            if var is not None and any(t.kind in ('ident', 'quoted') and _ident(t) == var
                                       for i, t in enumerate(tokens) if not open_ < i < close):
                continue
            nxt = k + 1
            if bounds[nxt][1] == 'WHERE':
                nxt += 1
            w, end = bounds[nxt][0], bounds[nxt + 1][0] if nxt + 1 < len(bounds) else len(tokens)
            if bounds[nxt][1] != 'WITH' or w + 1 >= len(tokens) or tokens[w + 1].upper != 'DISTINCT':
                continue
            if any(t.kind == 'ident' and t.upper in self.AGGREGATE_NAMES and m + 1 < end
                   and tokens[m + 1].text == '(' for m, t in enumerate(tokens[w:end], w)):
                continue
            marks.append(open_ + 1 + star + 1)
        for position in reversed(marks):
            tokens.insert(position, Tok('ident', 'WALK', ' '))
        return tokens

    # ---------------------------------------------- relationship uniqueness
    CLAUSE_STARTS = {'MATCH', 'OPTIONAL', 'WITH', 'RETURN', 'UNWIND', 'WHERE', 'ORDER', 'SKIP', 'LIMIT',
                     'CALL', 'UNION', 'CREATE', 'MERGE', 'SET', 'DELETE', 'DETACH', 'REMOVE', 'FOREACH'}

    def _with_uniqueness(self, tokens):
        """Neo4j binds the relationships of one MATCH clause to distinct
        edges (relationship isomorphism); the internal semantics is a
        homomorphism.  Make the Neo4j rule explicit: `r1 <> r2` for every pair
        of fixed-length relationships of a clause that could bind the same
        edge.  (Variable-length relationships are trails in Neo4j and walks
        internally; they agree on acyclic relationship types such as REPLY_OF.)"""
        n, depth, bounds = len(tokens), 0, []
        for i, t in enumerate(tokens):
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper in self.CLAUSE_STARTS:
                if t.upper == 'MATCH' and bounds and bounds[-1][1] == 'OPTIONAL' and bounds[-1][0] == i - 1:
                    continue
                bounds.append((i, t.upper))
        bounds.append((n, 'END'))
        edits = []  # (position, tokens to insert)
        counter = [0]
        clauses, part = [], 0
        for k, (start, word) in enumerate(bounds[:-1]):
            if word in ('WITH', 'RETURN', 'UNION'):
                part += 1
            if word not in ('MATCH', 'OPTIONAL'):
                continue
            pattern_end = bounds[k + 1][0]
            where = bounds[k + 1][1] == 'WHERE'
            where_end = bounds[k + 2][0] if where else None
            rels = self._clause_rels(tokens, start + (2 if word == 'OPTIONAL' else 1), pattern_end)
            unequal = self._where_unequal(tokens, pattern_end + 1, where_end) if where else set()
            # The endpoints of a fixed-length relationship of an acyclic type
            # (no self-loops, e.g. REPLY_OF) differ on every row of the clause.
            for rel in rels:
                if not rel['varlen'] and rel['types'] and rel['src'] and rel['dst'] \
                        and all(t in self.acyclic for t in rel['types']):
                    unequal.add(frozenset((rel['src'], rel['dst'])))
            clauses.append((start, word, pattern_end, where, where_end, rels, unequal, part))
        facts = self.part_facts(clauses)
        for start, word, pattern_end, where, where_end, rels, unequal, part in clauses:
            pairs = []
            for i, a in enumerate(rels):
                for b in rels[i + 1:]:
                    if (a['varlen'] or b['varlen']) and (a['tables'] is None or b['tables'] is None
                                                         or a['tables'] & b['tables']):
                        # Neo4j also keeps a variable-length relationship's edges
                        # distinct from the clause's other relationships; that
                        # needs a list predicate the internal dialect lacks.
                        # Unless the part's own filters already remove every
                        # row in which they could share an edge (root paths).
                        if word == 'MATCH' and self._uniqueness_implied(a, b, *facts.get(part, ([], set()))):
                            continue
                        if self._edge_into_path_start(a, b):
                            continue
                        raise Unsupported('relationship uniqueness involving a variable-length relationship')
                    if self._may_share(a, b, unequal):
                        pairs.append((a, b))
            if not pairs:
                continue
            for rel in {id(r): r for pair in pairs for r in pair}.values():
                if rel['var'] is None:
                    counter[0] += 1
                    rel['var'] = f'cico_u{counter[0]}'
                    edits.append((rel['open'] + 1, [Tok('ident', rel['var'])]))
            # Compare internal ids: the binder rejects `r1 <> r2` between
            # relationships over different table sets (NeuG types.cpp assert).
            # Written as `r1 <> r2`; normalize.entity_id_comparisons turns it
            # into `id(r1) <> id(r2)` unless both declare the same tables (NeuG
            # asserts on mixed table sets, and in some multi-pattern MATCHes
            # fails on id() comparisons over equal table sets).
            text = ' AND '.join(f"{_q(a['var'])} <> {_q(b['var'])}" for a, b in pairs)
            if where:
                edits.append((pattern_end + 1, [Tok('punct', '(', ' ')]))
                edits.append((where_end, tokenize(f') AND {text}')))
            else:
                edits.append((pattern_end, tokenize(f' WHERE {text}')))
        if not edits:
            return tokens
        out = list(tokens)
        for position, inserted in sorted(edits, key=lambda e: e[0], reverse=True):
            out[position:position] = inserted
        return out

    def _clause_rels(self, tokens, start, end):
        rels = []
        for i in range(start, end):
            if tokens[i].text != '[' or i == 0 or tokens[i - 1].text not in ('-', '<-'):
                continue
            depth, j = 0, i
            while j < end:
                depth += tokens[j].text == '['
                depth -= tokens[j].text == ']'
                if depth == 0:
                    break
                j += 1
            inner = tokens[i + 1:j]
            var, k = None, 0
            if inner and inner[0].kind in ('ident', 'quoted') and (len(inner) == 1 or inner[1].text in (':', '*', '{')):
                var, k = _ident(inner[0]), 1
            types = []
            if k < len(inner) and inner[k].text == ':':
                k += 1
                while k < len(inner) and inner[k].kind in ('ident', 'quoted'):
                    types.append(_ident(inner[k]))
                    k += 1
                    if k < len(inner) and inner[k].text == '|':
                        k += 1
                        if k < len(inner) and inner[k].text == ':':
                            k += 1
                        continue
                    break
            varlen = k < len(inner) and inner[k].text == '*'
            left = tokens[i - 1].text
            right = tokens[j + 1].text if j + 1 < len(tokens) else ''
            prev_open = None
            if i >= 2 and tokens[i - 2].text == ')':
                d, k = 0, i - 2
                while k >= 0:
                    d += tokens[k].text == ')'
                    d -= tokens[k].text == '('
                    if d == 0:
                        break
                    k -= 1
                prev_open = k
            prev_var = self._peek_node(tokens, prev_open)[0] if prev_open is not None else None
            next_var = self._peek_node(tokens, j + 2)[0] if j + 2 < len(tokens) and tokens[j + 2].text == '(' else None
            before = self._endpoint_tables(tokens, prev_open) if prev_open is not None else None
            after = self._endpoint_tables(tokens, j + 2)
            tables = None
            if types:
                tables = list(dict.fromkeys(t for name in types for t in self.rels.get(name, [name])))
                if self.rel_pairs:
                    tables = [t for t in tables if t.upper() in self.rel_pairs] or tables
                if not varlen:
                    tables = self._prune(tables, left, right, before, after)
                tables = {t.upper() for t in tables}
            if left == '<-' and right != '->':
                src, dst, src_t, dst_t, directed = next_var, prev_var, after, before, True
            elif right == '->' and left != '<-':
                src, dst, src_t, dst_t, directed = prev_var, next_var, before, after, True
            else:
                src, dst, src_t, dst_t, directed = prev_var, next_var, before, after, False
            rels.append(dict(open=i, var=var, tables=tables, varlen=varlen, src=src, dst=dst,
                             src_t=src_t, dst_t=dst_t, directed=directed, types=types))
        return rels

    @staticmethod
    def part_facts(clauses):
        """Facts holding on every row of a QueryPart: mandatory MATCH patterns
        and their WHERE conjuncts (an OPTIONAL clause's WHERE filters nothing)."""
        facts = {}
        for _, word, _, _, _, rels, unequal, part in clauses:
            if word == 'MATCH':
                fact_rels, fact_unequal = facts.setdefault(part, ([], set()))
                fact_rels.extend(rels)
                fact_unequal.update(unequal)
        return facts

    def clause_table(self, tokens):
        """[(start, word, pattern_end, where, where_end, rels, unequal, part)] of a statement."""
        n, depth, bounds = len(tokens), 0, []
        for i, t in enumerate(tokens):
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper in self.CLAUSE_STARTS:
                if t.upper == 'MATCH' and bounds and bounds[-1][1] == 'OPTIONAL' and bounds[-1][0] == i - 1:
                    continue
                bounds.append((i, t.upper))
        bounds.append((n, 'END'))
        clauses, part = [], 0
        for k, (start, word) in enumerate(bounds[:-1]):
            if word in ('WITH', 'RETURN', 'UNION'):
                part += 1
            if word not in ('MATCH', 'OPTIONAL'):
                continue
            pattern_end = bounds[k + 1][0]
            where = bounds[k + 1][1] == 'WHERE'
            where_end = bounds[k + 2][0] if where else None
            rels = self._clause_rels(tokens, start + (2 if word == 'OPTIONAL' else 1), pattern_end)
            unequal = self._where_unequal(tokens, pattern_end + 1, where_end) if where else set()
            # The endpoints of a fixed-length relationship of an acyclic type
            # (no self-loops, e.g. REPLY_OF) differ on every row of the clause.
            for rel in rels:
                if not rel['varlen'] and rel['types'] and rel['src'] and rel['dst'] \
                        and all(t in self.acyclic for t in rel['types']):
                    unequal.add(frozenset((rel['src'], rel['dst'])))
            clauses.append((start, word, pattern_end, where, where_end, rels, unequal, part))
        return clauses

    # ------------------------------------- implied typed labels (relax stage)
    def _type_analysis(self, tokens):
        """Labels known or implied for the node occurrences of a statement.

        Returns (occurrences {'(' position: (key, labels)}, edges [(src key,
        type, dst key, src position, dst position)], known {key: labels},
        implied {key: labels}, used {(key, label): constraint}).  A key is the
        variable name, or ('@', position) for an anonymous node.  Labels
        written on a variable's occurrences are known for all of them, and
        flow through `collect(x) AS L` / `UNWIND L AS v` for names bound once
        in the statement.  Implied labels propagate along fixed-length,
        single-type, directed relationships by endpoint_labels constraints."""
        def node_start(close):
            depth, k = 0, close
            while k >= 0:
                depth += tokens[k].text == ')'
                depth -= tokens[k].text == '('
                if depth == 0:
                    return k
                k -= 1
            return None
        declared = {}
        for i, t in enumerate(tokens):
            if t.text == '(':
                var, labels = self._peek_node(tokens, i)
                if var and labels:
                    declared.setdefault(var, set()).update(labels)
        bound = {}
        for k in range(1, len(tokens)):
            if tokens[k - 1].upper == 'AS' and tokens[k].kind in ('ident', 'quoted'):
                bound[_ident(tokens[k])] = bound.get(_ident(tokens[k]), 0) + 1
        elements = {}
        for k in range(len(tokens) - 5):
            w = [t.text for t in tokens[k:k + 6]]
            if tokens[k].kind == 'ident' and tokens[k].upper == 'COLLECT' and w[1] == '(' and w[3] == ')' \
                    and tokens[k + 4].upper == 'AS' and tokens[k + 2].kind in ('ident', 'quoted'):
                x, target = _ident(tokens[k + 2]), _ident(tokens[k + 5])
                if bound.get(target) == 1 and declared.get(x):
                    elements[target] = set(declared[x])
        for k in range(len(tokens) - 3):
            if tokens[k].kind == 'ident' and tokens[k].upper == 'UNWIND' and tokens[k + 2].upper == 'AS' \
                    and tokens[k + 1].kind in ('ident', 'quoted') and tokens[k + 3].kind in ('ident', 'quoted'):
                source, var = _ident(tokens[k + 1]), _ident(tokens[k + 3])
                if bound.get(var) == 1 and source in elements:
                    declared.setdefault(var, set()).update(elements[source])
        occurrences, edges = {}, []
        for i in range(1, len(tokens)):
            if tokens[i].text != '[' or tokens[i - 1].text not in ('-', '<-'):
                continue
            try:
                close = closing_index(tokens, i)
            except Unsupported:
                continue
            inner = tokens[i + 1:close]
            if any(t.text == '*' for t in inner):
                continue
            types = [_ident(t) for k, t in enumerate(inner) if k and inner[k - 1].text in (':', '|')
                     and t.kind in ('ident', 'quoted')]
            left = node_start(i - 2) if i >= 2 and tokens[i - 2].text == ')' else None
            right = close + 2 if close + 2 < len(tokens) and tokens[close + 2].text == '(' else None
            if left is None or right is None or len(types) != 1:
                continue
            keys = []
            for at in (left, right):
                var, labels = self._peek_node(tokens, at)
                key = var or ('@', at)
                occurrences[at] = (key, labels)
                keys.append(key)
            into_right = tokens[close + 1].text == '->' and tokens[i - 1].text == '-'
            into_left = tokens[i - 1].text == '<-' and tokens[close + 1].text == '-'
            if into_right:
                edges.append((keys[0], types[0], keys[1], left, right))
            elif into_left:
                edges.append((keys[1], types[0], keys[0], right, left))
        known = {}
        for at, (key, labels) in occurrences.items():
            known.setdefault(key, set()).update(labels)
            if isinstance(key, str):
                known[key].update(declared.get(key, ()))
        implied, used = {}, {}
        changed = True
        while changed:
            changed = False
            for src, rel, dst, _, _ in edges:
                for c in self.constraints.implied_endpoint(rel, known.get(src, set()) | implied.get(src, set())):
                    if c.dst not in implied.setdefault(dst, set()):
                        implied[dst].add(c.dst)
                        used[(dst, c.dst)] = c
                        changed = True
        return occurrences, edges, known, implied, used

    def _implied_typed_labels(self, tokens):
        """Replace a typed label (City) by its base label (Place) where
        declared endpoint_labels constraints imply it for every match, e.g.
        (p:Person)-[:IS_LOCATED_IN]->(:City) under (:Person)-[:IS_LOCATED_IN]->(:City).
        The dropped label is a predicate true on every match, so the pattern
        matches the same rows on every database satisfying the constraints.
        Used only by the relax stage."""
        if not self.constraints.items:
            return tokens
        occurrences, _, _, implied, used = self._type_analysis(tokens)
        out = list(tokens)
        for at, (key, labels) in sorted(occurrences.items(), reverse=True):
            for label in labels:
                if label not in self.typed or label not in implied.get(key, ()):
                    continue
                table = self.typed[label]['table'].upper()
                base = next((name for name, tables in self.labels.items()
                             if [t.upper() for t in tables] == [table]), None)
                if base is None:
                    continue
                for k in range(at + 1, len(out)):
                    if out[k].text == ')':
                        break
                    if out[k].kind in ('ident', 'quoted') and _ident(out[k]) == label and out[k - 1].text == ':':
                        out[k] = Tok(out[k].kind, _q(base), out[k].ws)
                        break
                self.premises.update(c.describe() for c in used.values())
        return out

    def _key_identities(self, tokens):
        """Relax stage: express a node's identity through a declared key so
        that a location chain can end in a HIDDEN node a projection carrier
        replaces (theory.md (M_P)(ii): the interface must be public).

        T1  (v:L {p: E}) on a relationship chain, E a parameter or literal
            ->  (k:L {p: E}) on the chain; v is bound again by its key.  When
                v occurs in its QueryPart only there and as a bare item of the
                part's WITH (no ORDER BY/SKIP/LIMIT), the binding is deferred:
                `WITH [DISTINCT] items-v MATCH (v:L {p: E}) WITH [DISTINCT] items`
                (RewriterV4 rejects a part with a disconnected node pattern).
                v is constant on every row of the part (the only L with p = E,
                which exists whenever the part has a row), so both projections
                partition the rows alike.  Otherwise (v:L {p: E}) is added as
                a separate pattern of the same clause.
        T2  (v) bound by an earlier clause, whose incoming relationship forces
            label L (endpoint_labels)
            ->  (k:L) and `k.p = v.p AND v:L` added to the clause's WHERE

        With p unique and never null on L (unique_property + required_property),
        k and v denote the same node on every row of both forms: in T1 both
        are the only L with p = E; in T2 the original row has v:L by the
        endpoint constraint, so the guard drops nothing, and equal keys give
        k = v.  Rows correspond one to one, so multiplicities are kept."""
        if not self.constraints.items:
            return tokens
        occurrences, edges, known, implied, used = self._type_analysis(tokens)
        clauses = self.clause_table(tokens)
        first = {}
        for k, t in enumerate(tokens):
            if t.kind in ('ident', 'quoted') and not (k and tokens[k - 1].text in ('.', ':')):
                first.setdefault(_ident(t), k)
        taken = {_ident(t) for t in tokens if t.kind in ('ident', 'quoted')}

        def fresh():
            n = 1
            while f'cico_key{n}' in taken:
                n += 1
            taken.add(f'cico_key{n}')
            return f'cico_key{n}'

        from .queryparts import _top_level, PROJECTION_TAIL
        top = _top_level(tokens)

        def deferral(var, at, spec):
            """(lo, hi, text) rewriting the WITH that ends v's part, or None."""
            after = [(i, w) for i, w in top if i > at]
            with_at = next((i for i, w in after if w in ('WITH', 'RETURN')), None)
            if with_at is None or tokens[with_at].upper != 'WITH':
                return None
            part_start = max([i for i, w in top if i <= at and w in ('WITH', 'RETURN')] or [-1]) + 1
            i = with_at + 1
            distinct = i < len(tokens) and tokens[i].upper == 'DISTINCT'
            i += distinct
            items, current, depth = [], [], 0
            while i < len(tokens):
                t = tokens[i]
                depth += t.text in ('(', '[', '{')
                depth -= t.text in (')', ']', '}')
                if depth == 0 and t.kind == 'ident' and (t.upper in PROJECTION_TAIL or t.upper in self.CLAUSE_STARTS):
                    break
                if depth == 0 and t.text == ',':
                    items.append(current)
                    current = []
                else:
                    current.append(t)
                i += 1
            items.append(current)
            if i < len(tokens) and tokens[i].upper in ('ORDER', 'SKIP', 'LIMIT'):
                return None
            bare = [it for it in items if len(it) == 1 and it[0].kind in ('ident', 'quoted') and _ident(it[0]) == var]
            if len(bare) != 1:
                return None
            uses = [k for k in range(part_start, i) if tokens[k].kind in ('ident', 'quoted') and _ident(tokens[k]) == var
                    and not (k and tokens[k - 1].text in ('.', ':'))]
            if sorted(uses) != sorted([at + 1, next(k for k in range(with_at, i) if tokens[k] is bare[0][0])]):
                return None
            rest = [render(it).strip() for it in items if it is not bare[0]]
            if not rest:
                return None
            head = 'WITH ' + ('DISTINCT ' if distinct else '')
            text = (head + ', '.join(rest) + f' MATCH ({_q(var)}:{spec}) ' + head
                    + ', '.join(render(it).strip() for it in items) + ' ')
            return with_at, i, text

        replacements, extra_patterns, conjuncts = [], {}, {}
        for at, (key, labels) in sorted(occurrences.items()):
            if not isinstance(key, str):
                continue
            clause = next((c for c in clauses if c[0] <= at < c[2]), None)
            if clause is None:
                continue
            start, word, pattern_end, where, where_end = clause[:5]
            try:
                end = closing_index(tokens, at)
            except Unsupported:
                continue
            body = tokens[at + 1:end]
            if len(labels) == 1 and len(body) == 8 and body[1].text == ':' and body[3].text == '{' \
                    and body[5].text == ':' and body[7].text == '}' and body[6].kind in ('param', 'string', 'number'):
                label, prop = labels[0], _ident(body[4])
                k = self.constraints.key(label, prop)
                if k is None:
                    continue
                if sum(1 for j, t in enumerate(tokens) if t.kind in ('ident', 'quoted') and _ident(t) == key
                       and not (j and tokens[j - 1].text in ('.', ':'))) < 2:
                    continue    # v is used nowhere else: the chain already ends in a hidden node
                name = fresh()
                spec = f'{_q(label)} {{{_q(prop)}: {body[6].text}}}'
                replacements.append((at, end + 1, f'({name}:{spec})'))
                deferred = deferral(key, at, spec)
                if deferred is not None:
                    replacements.append(deferred)
                else:
                    extra_patterns.setdefault(pattern_end, []).append(f'({_q(key)}:{spec})')
                self.premises.update(c.describe() for c in k[1:])
                continue
            if labels or len(body) != 1 or first.get(key, at) >= start:
                continue
            for src, rel, dst, src_at, dst_at in edges:
                if dst_at != at:
                    continue
                found = None
                for c in self.constraints.implied_endpoint(rel, known.get(src, set()) | implied.get(src, set())):
                    k = self.constraints.key(c.dst)
                    if k is not None:
                        found = (c, k)
                        break
                if found is None:
                    continue
                c, (prop, unique, required) = found
                name = fresh()
                replacements.append((at, end + 1, f'({name}:{_q(c.dst)})'))
                conjuncts.setdefault((pattern_end, where, where_end), []).append(
                    f'{name}.{_q(prop)} = {_q(key)}.{_q(prop)} AND {_q(key)}:{_q(c.dst)}')
                self.premises.update(x.describe() for x in (c, unique, required, *used.values()))
                break
        if not replacements:
            return tokens
        edits = [(lo, hi, text) for lo, hi, text in replacements]
        for position, patterns in extra_patterns.items():
            edits.append((position, position, ', ' + ', '.join(patterns) + ' '))
        for (pattern_end, where, where_end), conj in conjuncts.items():
            text = ' AND '.join(f'({x})' for x in conj)
            if where:
                edits.append((pattern_end + 1, pattern_end + 1, ' ('))
                edits.append((where_end, where_end, f') AND {text} '))
            else:
                edits.append((pattern_end, pattern_end, f' WHERE {text} '))
        out = list(tokens)
        for lo, hi, text in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
            out[lo:hi] = tokenize(' ' + text)
        return tokenize(render(out))

    def key_constants(self, cypher):
        """Relax stage, on Neo4j text: (text, premises); T3 of relax.py
        with this mapping's label keys."""
        from .relax import key_constants
        return key_constants(cypher, self.constraints.key)

    def rel_tables(self, name):
        """Internal relationship tables of a Neo4j relationship type."""
        return [t.upper() for t in self.rels.get(name, [name])]

    # ------------------------------------------- root paths (declared constraints)
    def _root_path(self, rel):
        """The constraint making a variable-length relationship the UNIQUE path
        from its start to a root, or None.

        Requires: one acyclic type T, written start -[:T*]-> end; every
        possible start node has at most one outgoing T (declared at_most_one);
        no end table has an outgoing T in the schema (the end is a root).
        Then the path is the start's root path R(start), ending at root(start),
        whatever its length bounds."""
        types = rel.get('types') or []
        if not rel['varlen'] or not rel['directed'] or len(types) != 1 or types[0] not in self.acyclic:
            return None
        if rel['src'] is None or rel['dst'] is None or not rel['src_t'] or not rel['dst_t']:
            return None
        functional = self.constraints.at_most_one_out(types[0], rel['src_t'])
        if functional is None:
            return None
        internal = {t.upper() for t in self.rels.get(types[0], [types[0]])}
        ends = {t.upper() for t in rel['dst_t']}
        if any(src in ends for group in internal for src, _ in self.rel_pairs.get(group, ())):
            return None
        return functional

    def _edge_into_path_start(self, a, b):
        """A fixed x -[:T]-> s and a variable-length s -[:T*]-> ... of one
        acyclic type T never share an edge, in any clause (OPTIONAL too): a
        walk from s that used x -> s would return to s, a cycle."""
        path, fixed = (a, b) if a['varlen'] else (b, a)
        types = path.get('types') or []
        return bool(path['varlen'] and not fixed['varlen'] and path['directed'] and fixed['directed']
                    and len(types) == 1 and types[0] in self.acyclic and fixed.get('types') == types
                    and path['src'] is not None and fixed['dst'] == path['src'])

    def _reanchor_root_paths(self, tokens):
        """Relax stage, after relationship uniqueness is explicit: in a clause
        with c -[:T]-> t, an element `(t)-[:T*0..]->(e ...)` (acyclic T, no
        upper bound) is read as `(c)-[:T*0..]->(e ...)` when c has at most one
        outgoing T (declared at_most_one) and no table of c is one of e's
        (so the path from c has length >= 1).  A walk from c of length k >= 1
        is the edge c -> t followed by a walk from t of length k - 1, one to
        one, so rows and multiplicities agree (internal homomorphism)."""
        bounds = self._clause_bounds(tokens)
        renames = {}
        for k, (start, word) in enumerate(bounds[:-1]):
            if word not in ('MATCH', 'OPTIONAL'):
                continue
            ps, pe = start + (2 if word == 'OPTIONAL' else 1), bounds[k + 1][0]
            rels = self._clause_rels(tokens, ps, pe)
            for path in rels:
                types = path.get('types') or []
                if not (path['varlen'] and path['directed'] and len(types) == 1 and types[0] in self.acyclic
                        and path['var'] is None and path['src'] and path['dst_t']):
                    continue
                close = closing_index(tokens, path['open'])
                inner = [t.text for t in tokens[path['open'] + 1:close]]
                if inner[-3:] != ['*', '0', '..'] or tokens[path['open'] - 1].text != '-' \
                        or tokens[close + 1].text != '->':
                    continue
                node_close = path['open'] - 2
                node_open = node_close - 2
                if not (node_open >= ps and tokens[node_open].text == '(' and tokens[node_close].text == ')'
                        and _ident(tokens[node_open + 1]) == path['src']
                        and (node_open == ps or tokens[node_open - 1].text == ',')):
                    continue
                fixed = next((r for r in rels if not r['varlen'] and r['directed'] and r.get('types') == types
                              and r['dst'] == path['src'] and r['src'] and r['src_t']), None)
                if fixed is None or {t.upper() for t in fixed['src_t']} & {t.upper() for t in path['dst_t']}:
                    continue
                premise = self.constraints.at_most_one_out(types[0], fixed['src_t'])
                if premise is None:
                    continue
                renames[node_open + 1] = fixed['src']
                self.premises.add(premise.describe())
        for position, name in renames.items():
            tokens[position] = Tok('ident', _q(name), tokens[position].ws)
        return tokens

    def _uniqueness_implied(self, a, b, rels, unequal):
        """True when no row surviving the part's filters can bind relationships
        a and b (one variable-length) to a common edge, so Neo4j's uniqueness
        between them removes nothing the filters keep.

        In a forest (acyclic T, at most one outgoing T per node) two root
        paths sharing an edge share their root.  A fixed edge x -[:T]-> y on
        R(s) gives root(s) = root(y); y = s would close a cycle.  The forced
        equality of roots is propagated through fixed relationships declared
        at_most_one (equal targets of an at-most-one-incoming relationship
        have equal sources, and vice versa); reaching a pair the part's
        mandatory filters keep unequal is a contradiction."""
        ra, rb = self._root_path(a), self._root_path(b)
        used = set()
        if ra and rb:
            if a['types'] != b['types']:
                return False
            equal, used = (a['dst'], b['dst']), {ra, rb}
        else:
            path, fixed, premise = (a, b, ra) if a['varlen'] else (b, a, rb)
            if premise is None or fixed['varlen'] or not fixed['directed'] or fixed.get('types') != path['types'] \
                    or fixed['dst'] is None:
                return False
            if fixed['dst'] == path['src']:
                self.premises.add(premise.describe())
                return True             # the fixed edge on R(start) would close a cycle
            other = next((r for r in rels if r is not path and r['varlen'] and r['src'] == fixed['dst']
                          and self._root_path(r)), None)
            if other is None:
                return False
            equal, used = (path['dst'], other['dst']), {premise, self._root_path(other)}
        found = self.contradiction([equal], rels, unequal)
        if found is None:
            return False
        self.premises.update(c.describe() for c in used | found)
        return True

    def contradiction(self, equalities, rels, unequal):
        """Constraints proving that the node equalities cannot hold on a row
        the part keeps, or None.  Equalities propagate through fixed directed
        relationships declared at_most_one (equal targets of an
        at-most-one-incoming relationship have equal sources, and vice
        versa) until a pair the part's mandatory filters keep unequal meets."""
        parent, used = {}, set()

        def find(x):
            while parent.get(x, x) != x:
                x = parent[x]
            return x

        def union(x, y):
            fx, fy = find(x), find(y)
            if fx != fy:
                parent[fx] = fy
                return True
            return False

        for x, y in equalities:
            if x is None or y is None:
                return None
            union(x, y)
        fixed = [r for r in rels if not r['varlen'] and r['directed'] and len(r.get('types') or []) == 1
                 and r['src'] is not None and r['dst'] is not None]
        changed = True
        while changed:
            changed = False
            for i, x in enumerate(fixed):
                for y in fixed[i + 1:]:
                    if x['types'] != y['types']:
                        continue
                    if find(x['dst']) == find(y['dst']) and find(x['src']) != find(y['src']):
                        c = self.constraints.at_most_one_in(x['types'][0], x['dst_t'] or ())
                        if c and self.constraints.at_most_one_in(x['types'][0], y['dst_t'] or ()):
                            changed |= union(x['src'], y['src'])
                            used.add(c)
                    if find(x['src']) == find(y['src']) and find(x['dst']) != find(y['dst']):
                        c = self.constraints.at_most_one_out(x['types'][0], x['src_t'] or ())
                        if c and self.constraints.at_most_one_out(x['types'][0], y['src_t'] or ()):
                            changed |= union(x['dst'], y['dst'])
                            used.add(c)
        if any(len(pair) == 2 and find(min(pair)) == find(max(pair)) for pair in unequal):
            return used
        return None

    @staticmethod
    def _where_unequal(tokens, start, end):
        """Top-level `x <> y` / `NOT x = y` / `x.p <> y.p` conjuncts: variable
        pairs that differ on every row the WHERE keeps."""
        pairs, depth, part = set(), 0, []
        def flush(part):
            texts = [t.text for t in part]
            if len(part) == 3 and part[0].kind == 'ident' and part[2].kind == 'ident' and texts[1] == '<>':
                pairs.add(frozenset((part[0].text, part[2].text)))
            if len(part) == 4 and part[0].upper == 'NOT' and part[1].kind == 'ident' and texts[2] == '=' \
                    and part[3].kind == 'ident':
                pairs.add(frozenset((part[1].text, part[3].text)))
            # x.p <> y.p: one entity has one value of p, so x and y differ.
            if len(part) == 7 and texts[1] == texts[5] == '.' and texts[3] == '<>' \
                    and part[0].kind == part[4].kind == 'ident' and texts[2] == texts[6] \
                    and part[2].kind in ('ident', 'quoted'):
                pairs.add(frozenset((part[0].text, part[4].text)))
        for t in tokens[start:end]:
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            if depth == 0 and t.kind == 'ident' and t.upper == 'AND':
                flush(part)
                part = []
            else:
                part.append(t)
        flush(part)
        return pairs

    @staticmethod
    def _may_share(a, b, unequal):
        if a['varlen'] or b['varlen']:
            return False  # see _with_uniqueness: trails vs walks, not expressible
        if a['tables'] is not None and b['tables'] is not None and not a['tables'] & b['tables']:
            return False
        if a['var'] and b['var'] and frozenset((a['var'], b['var'])) in unequal:
            return False
        def impossible(x, y, tx, ty):
            if x is not None and x == y:
                return False
            if x and y and frozenset((x, y)) in unequal:
                return True
            return bool(tx and ty and not {t.upper() for t in tx} & {t.upper() for t in ty})
        options = [((a['src'], b['src'], a['src_t'], b['src_t']), (a['dst'], b['dst'], a['dst_t'], b['dst_t']))]
        if not (a['directed'] and b['directed']):
            options.append(((a['src'], b['dst'], a['src_t'], b['dst_t']), (a['dst'], b['src'], a['dst_t'], b['src_t'])))
        return not all(impossible(*p) or impossible(*q) for p, q in options)

    def _collect_var_tables(self, tokens):
        found = {}
        for i, t in enumerate(tokens):
            if t.text != '(':
                continue
            var, labels = self._peek_node(tokens, i)
            if var and labels:
                try:
                    tables = {x.upper() for x in self._tables_for(labels)[0]}
                except Unsupported:
                    continue
                found[var] = found.get(var, tables) & tables
        return found

    @staticmethod
    def _peek_node(tokens, i):
        """(variable, labels) of a node pattern starting at tokens[i] == '('."""
        n, j = len(tokens), i + 1
        var = None
        if j < n and tokens[j].kind in ('ident', 'quoted') and not (j + 1 < n and tokens[j + 1].text == '('):
            var = _ident(tokens[j])
            j += 1
        labels = []
        while j + 1 < n and tokens[j].text == ':' and tokens[j + 1].kind in ('ident', 'quoted'):
            labels.append(_ident(tokens[j + 1]))
            j += 2
        if j < n and tokens[j].text not in (')', '{'):
            return None, []
        return var, labels

    def _endpoint_tables(self, tokens, i):
        """Node tables of the node pattern at tokens[i] (None = unconstrained)."""
        if i < 0 or i >= len(tokens) or tokens[i].text != '(':
            return None
        var, labels = self._peek_node(tokens, i)
        if labels:
            try:
                return {x.upper() for x in self._tables_for(labels)[0]}
            except Unsupported:
                return None
        return self.var_tables.get(var) if var else None

    def _prune(self, tables, left_arrow, right_arrow, before, after):
        """Keep relationship tables whose endpoint pairs fit the adjacent
        node tables.  A Neo4j type may map to several tables (HAS_TAG ->
        Comment_hasTag_Tag | Post_hasTag_Tag | ...); keeping incompatible ones
        changes nothing semantically but hides the pattern from FILL matching."""
        if not self.rel_pairs or (before is None and after is None):
            return tables
        def fits(a, b, pair):
            return (a is None or pair[0] in a) and (b is None or pair[1] in b)
        keep = []
        for table in tables:
            pairs = self.rel_pairs.get(table.upper())
            if pairs is None:
                keep.append(table)
                continue
            ok = False
            for pair in pairs:
                if right_arrow == '->' and left_arrow != '<-':
                    ok |= fits(before, after, pair)
                elif left_arrow == '<-' and right_arrow != '->':
                    ok |= fits(after, before, pair)
                else:
                    ok |= fits(before, after, pair) or fits(after, before, pair)
            if ok:
                keep.append(table)
        return keep or tables

    def _prune_varlen(self, tables, left_arrow, right_arrow, before, after):
        """Tables a directed variable-length walk from its typed start node
        can use: a hop leaves a node of the start tables or of the end tables
        of an earlier usable hop (closure).  Other tables never occur in a
        walk, so dropping them keeps the pattern's meaning."""
        if not self.rel_pairs or (left_arrow == '<-') == (right_arrow == '->'):
            return tables
        start, forward = (before, True) if right_arrow == '->' else (after, False)
        if start is None:
            return tables
        reach, keep, grown = set(start), set(), True
        while grown:
            grown = False
            for table in tables:
                pairs = self.rel_pairs.get(table.upper())
                if pairs is None:
                    return tables
                for src, dst in pairs:
                    a, b = (src, dst) if forward else (dst, src)
                    if a in reach:
                        if table not in keep:
                            keep.add(table)
                            grown = True
                        if b not in reach:
                            reach.add(b)
                            grown = True
        return [t for t in tables if t in keep] or tables

    def parameter_types(self, types):
        return {k: ('TIMESTAMP' if v in ('DATETIME', 'ZONED DATETIME', 'LOCAL DATETIME') else v)
                for k, v in (types or {}).items()}

    def table_to_internal(self, name):
        if name in self.typed:
            raise Unsupported(f'typed label {name} cannot be a FILL target')
        tables = self.labels.get(name)
        if tables is None:
            return name
        if len(tables) != 1:
            raise Unsupported(f'label {name} maps to several tables')
        return tables[0]

    def ddl_to_internal(self, statement):
        def table(m):
            return m[1] + _q(self.table_to_internal(m[2].strip('`')))
        return re.sub(r'(\b(?:TABLE(?:\s+IF\s+(?:NOT\s+)?EXISTS)?|FROM|TO)\s+)(`[^`]+`|[A-Za-z_][A-Za-z0-9_]*)',
                      table, statement, flags=re.I)

    def _internal_vocabulary(self, text):
        """True when every label / relationship type in `text` is an internal
        table name: such a FILL (e.g. the NeuG index scripts) is already in
        the internal dialect and keeps its homomorphic matching semantics."""
        if not self.rel_pairs:
            return False
        tables = self.node_names | set(self.rel_pairs)
        text = re.sub(r"'(?:[^'\\]|\\.)*'", "''", strip_comments(text))
        while re.search(r'\{[^{}]*\}', text):  # drop maps / subquery bodies' braces content
            text = re.sub(r'\{([^{}]*)\}', lambda m: '' if re.search(r'\w\s*:', m[1]) and 'MATCH' not in m[1].upper()
                          else m[1], text)
        names = re.findall(r'[:|]\s*`?([A-Za-z_][A-Za-z0-9_]*)', text)
        return bool(names) and all(n.upper() in tables for n in names)

    def fill_to_internal(self, fill_text):
        if self._internal_vocabulary(fill_text):
            return fill_text
        m = re.match(r'\s*FILL\s+(`[^`]+`|[A-Za-z_][A-Za-z0-9_]*)(\s*\([^)]*\)\s*FROM\s*\()(.*)\)\s*;?\s*$',
                     strip_comments(fill_text), re.I | re.S)
        if not m:
            return fill_text
        table = m[1].strip('`')
        target = self.rels.get(table, [table])[0] if table in self.rels else self.table_to_internal(table)
        return f'FILL {_q(target)}{m[2]}{self.to_internal(m[3])})'

    # -------------------------------------------------------- translation
    def _translate(self, tokens, map_body=False):
        out, i, n = [], 0, len(tokens)
        pattern_mode = False
        brace_stack = []  # 'map' | 'block'
        while i < n:
            t = tokens[i]
            up = t.upper if t.kind == 'ident' else None
            if up == 'CALL':
                raise Unsupported('CALL subquery' if i + 1 < n and tokens[i + 1].text in ('{', '(')
                                  else 'CALL procedure: no internal equivalent')
            if not pattern_mode and t.kind in ('ident', 'quoted') and i + 1 < n and tokens[i + 1].text == '{' \
                    and up not in ('EXISTS', 'COUNT', 'COLLECT') and not (out and out[-1].text == '.'):
                raise Unsupported('map projection: no internal map/struct literal')
            if up in PATTERN_KEYWORDS:
                pattern_mode = True
            elif up in CLAUSE_END:
                pattern_mode = False
            if t.text == '{':
                prev = tokens[i - 1] if i else None
                block = prev is not None and prev.kind == 'ident' and prev.upper in ('EXISTS', 'COUNT', 'COLLECT', 'CALL')
                brace_stack.append('block' if block else 'map')
                out.append(t)
                i += 1
                continue
            if t.text == '}':
                if brace_stack and brace_stack.pop() == 'block':
                    pattern_mode = False
                out.append(t)
                i += 1
                continue
            in_map = (bool(brace_stack) and brace_stack[-1] == 'map') or (map_body and not brace_stack)
            if t.text == '(' and not in_map:
                node = self._node(tokens, i)
                if node is not None and (pattern_mode or self._starts_pattern(tokens, node[0])):
                    if not pattern_mode:
                        # Pattern predicate in an expression: (a)-[:R]->(b)
                        end, text = self._pattern_predicate(tokens, i)
                        out.extend(tokenize(text))
                        out[-len(tokenize(text))].ws = t.ws
                        i = end
                        continue
                    end, replacement = node
                    replacement[0].ws = t.ws
                    out.extend(replacement)
                    i = end
                    continue
            if t.text == '[' and pattern_mode and out and out[-1].text in ('-', '<-'):
                end, replacement, both = self._rel(tokens, i)
                if both:  # <-[..]-> : undirected
                    out[-1].text = '-'
                replacement[0].ws = t.ws
                out.extend(replacement)
                i = end
                if both:
                    tokens[i].text = '-'
                continue
            if not pattern_mode and not in_map and t.kind in ('ident', 'quoted') and i + 2 < n \
                    and tokens[i + 1].text == ':' and tokens[i + 2].kind in ('ident', 'quoted') \
                    and not (out and out[-1].text in ('.', '{', ',')):
                # label predicate x:Label (outside patterns and maps)
                text = self._label_predicate(_ident(t), _ident(tokens[i + 2]))
                # Its parentheses only matter next to an operator binding at
                # least as tightly as `=`/IN. Between boolean connectives they
                # are dropped, so the internal text spells e.g.
                # `(label(m) = 'POST' OR label(n) = 'POST')` as a FILL template.
                before = out[-1].upper if out and out[-1].kind == 'ident' else (out[-1].text if out else '(')
                after = (tokens[i + 3].upper if tokens[i + 3].kind == 'ident' else tokens[i + 3].text) \
                    if i + 3 < n else ')'
                if before in ('(', ',', 'AND', 'OR', 'XOR', 'WHEN', 'THEN', 'ELSE', 'CASE', 'WHERE') \
                        and after in (')', ',', 'AND', 'OR', 'XOR', 'WHEN', 'THEN', 'ELSE', 'END', 'AS') \
                        and text.startswith('(') and text.endswith(')'):
                    text = text[1:-1]
                out.extend(tokenize(text))
                out[-len(tokenize(text))].ws = t.ws
                i += 3
                continue
            if t.kind == 'ident' and t.upper == 'CASE' and not (out and out[-1].text == '.'):
                found = self._nullable_sum(tokens, i)
                if found is not None:
                    end, inner = found
                    text = f'sum({render(self._translate(inner))})'
                    new = tokenize(text)
                    new[0].ws = t.ws
                    out.extend(new)
                    i = end
                    continue
            if t.kind == 'ident' and i + 1 < n and tokens[i + 1].text == '(':
                func = t.text.lower()
                if func == 'sum' and not (out and out[-1].text == '.'):
                    # Neo4j's sum() of no (non-null) values is 0, NeuG's NULL.
                    end, inner = self._call_args(tokens, i + 1)
                    body = f'sum({render(self._translate(inner))})'
                    text = f'(CASE WHEN ({body}) IS NOT NULL THEN ({body}) ELSE (0) END)'
                    new = tokenize(text)
                    new[0].ws = t.ws
                    out.extend(new)
                    i = end
                    continue
                if func in ('tofloat', 'tointeger', 'tostring'):
                    end, inner = self._call_args(tokens, i + 1)
                    target = {'tofloat': 'DOUBLE', 'tointeger': 'INT64', 'tostring': 'STRING'}[func]
                    body = render(self._translate(inner))
                    out.extend(tokenize(f'CAST({body} AS {target})'))
                    out[-len(tokenize(f'CAST({body} AS {target})'))].ws = t.ws
                    i = end
                    continue
                if func in ('datetime', 'localdatetime'):
                    end, inner = self._call_args(tokens, i + 1)
                    if inner and inner[0].text == '{':
                        raise Unsupported(f'{t.text}({{...}}) from a map: no internal equivalent')
                    if len(inner) == 1 and inner[0].kind == 'string':
                        # literal: timestamp('...') folds to a TIMESTAMP literal
                        out.append(Tok('ident', 'timestamp', t.ws))
                        i += 1
                        continue
                    # value conversion: the same CAST the binder inserts for an
                    # implicitly typed operand, so printed casts rebind equal
                    text = f'CAST({render(self._translate(inner))} AS TIMESTAMP)'
                    out.extend(tokenize(text))
                    out[-len(tokenize(text))].ws = t.ws
                    i = end
                    continue
                if func == 'round':
                    from .normalize import call_args
                    end, args = call_args(tokens, i + 1)
                    if len(args) == 1:
                        # Neo4j round(x) == round(x, 0) (checked on Neo4j 5.26 for
                        # halves, negatives, large values, null); NeuG has only
                        # round(x, n).
                        text = f'round({render(self._translate(args[0]))}, 0)'
                        out.extend(tokenize(text))
                        out[-len(tokenize(text))].ws = t.ws
                        i = end
                        continue
                if func == 'duration':
                    end, inner = self._call_args(tokens, i + 1)
                    text = self._duration(inner)
                    out.extend(tokenize(text))
                    out[-len(tokenize(text))].ws = t.ws
                    i = end
                    continue
            if t.kind == 'ident' and t.text.lower() in ('datetime', 'localdatetime') and i + 3 < n \
                    and tokens[i + 1].text == '.' and tokens[i + 2].text.lower() == 'truncate' \
                    and tokens[i + 3].text == '(' and not (out and out[-1].text == '.'):
                # datetime.truncate('day', x): midnight of x's day (in x's
                # zone; datetimes are read as UTC TIMESTAMPs, as everywhere
                # in this frontend) == internal date_trunc('day', x).
                from .normalize import call_args
                end, args = call_args(tokens, i + 3)
                if not (len(args) == 2 and len(args[0]) == 1 and args[0][0].kind == 'string'
                        and args[0][0].text[1:-1].lower() == 'day'):
                    raise Unsupported(f'{t.text}.truncate() other than by day')
                text = f"date_trunc('day', {render(self._translate(args[1]))})"
                out.extend(tokenize(text))
                out[-len(tokenize(text))].ws = t.ws
                i = end
                continue
            if t.kind in ('ident', 'quoted') and i + 2 < n and tokens[i + 1].text == '.' \
                    and tokens[i + 2].kind in ('ident', 'quoted') and not (out and out[-1].text == '.') \
                    and not self._temporal(tokens, i):
                renamed = self._property(_ident(t), _ident(tokens[i + 2]))
                if renamed is not None:
                    out.extend([t, tokens[i + 1], Tok(tokens[i + 2].kind, _q(renamed), tokens[i + 2].ws)])
                    i += 3
                    continue
            if (t.kind == 'param' or t.kind == 'ident') and self._temporal(tokens, i):
                end, text = self._temporal(tokens, i)
                out.extend(tokenize(text))
                out[-len(tokenize(text))].ws = t.ws
                i = end
                continue
            out.append(t)
            i += 1
        return out

    ISO_UNITS = {'Y': 'years', 'M': 'months', 'W': 'weeks', 'D': 'days'}
    ISO_TIME_UNITS = {'H': 'hours', 'M': 'minutes', 'S': 'seconds'}
    MAP_UNITS = {'years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds', 'milliseconds'}

    def _duration(self, inner):
        """duration({hours: x}) / duration('PT4H') -> internal duration('4 hours')."""
        if len(inner) == 1 and inner[0].kind == 'string':
            iso = inner[0].text[1:-1]
            m = re.fullmatch(r'P(?:(\d+)Y)?(?:(\d+)M)?(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?', iso)
            if not m or not any(m.groups()):
                raise Unsupported(f'Neo4j duration {iso!r}')
            units = ['years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds']
            parts = [f'{v} {u}' for v, u in zip(m.groups(), units) if v]
            return "duration('" + ' '.join(parts) + "')"
        if inner and inner[0].text == '{' and inner[-1].text == '}':
            body = inner[1:-1]
            if len(body) >= 3 and body[0].kind == 'ident' and body[1].text == ':' \
                    and body[0].text in self.MAP_UNITS and not any(t.text == ',' for t in body):
                value = render(self._translate(body[2:])).strip()
                return f"duration(CAST({value} AS STRING) + ' {body[0].text}')"
        raise Unsupported('Neo4j duration() form')

    @staticmethod
    def _temporal(tokens, i):
        """$p.year / a.b.year -> date_part('year', ...)."""
        n = len(tokens)
        if i and tokens[i - 1].text == '.':
            return None
        j = i + 1
        base = [tokens[i]]
        if tokens[i].kind == 'ident' and j + 1 < n and tokens[j].text == '.' and tokens[j + 1].kind in ('ident', 'quoted'):
            base += [tokens[j], tokens[j + 1]]
            j += 2
        if j + 1 < n and tokens[j].text == '.' and tokens[j + 1].kind == 'ident' \
                and tokens[j + 1].text.lower() in TEMPORAL_FIELDS and (len(base) == 3 or tokens[i].kind == 'param'):
            return j + 2, f"date_part('{tokens[j + 1].text.lower()}', {''.join(b.text for b in base)})"
        return None

    def _nullable_sum(self, tokens, i):
        """`CASE WHEN count([DISTINCT] a) = 0 THEN null ELSE sum([DISTINCT] a) END`
        at tokens[i]: a sum that is NULL on no values, i.e. NeuG's sum(a).
        Returns (end, tokens of `[DISTINCT] a`) or None."""
        n = len(tokens)
        def word(k, w):
            return k < n and tokens[k].kind == 'ident' and tokens[k].upper == w
        if not (word(i + 1, 'WHEN') and word(i + 2, 'COUNT') and i + 3 < n and tokens[i + 3].text == '('):
            return None
        k, counted = self._call_args(tokens, i + 3)
        if not (k + 5 < n and tokens[k].text == '=' and tokens[k + 1].text == '0' and word(k + 2, 'THEN')
                and word(k + 3, 'NULL') and word(k + 4, 'ELSE') and word(k + 5, 'SUM')
                and k + 6 < n and tokens[k + 6].text == '('):
            return None
        k, summed = self._call_args(tokens, k + 6)
        if not word(k, 'END') or render(counted).split() != render(summed).split():
            return None
        return k + 1, summed

    @staticmethod
    def _call_args(tokens, open_index):
        depth, j = 0, open_index
        while j < len(tokens):
            if tokens[j].text in ('(', '[', '{'):
                depth += 1
            elif tokens[j].text in (')', ']', '}'):
                depth -= 1
                if depth == 0:
                    return j + 1, tokens[open_index + 1:j]
            j += 1
        raise Unsupported('unbalanced parentheses')

    # ------------------------------------------------------------- nodes
    def _tables_for(self, labels):
        tables, predicates = None, []
        for label in labels:
            if label in self.typed:
                spec = self.typed[label]
                current = {spec['table']}
                predicates.append((spec['property'], spec['value']))
            elif label in self.labels:
                current = set(self.labels[label])
            else:
                current = {label}
            tables = current if tables is None else {t for t in tables if t.upper() in {c.upper() for c in current}}
        if tables is not None and not tables:
            raise Unsupported(f'labels {labels} have no common table')
        return tables, predicates

    def _node(self, tokens, i):
        """Parse `(var? (:Label)* {map}?)` at tokens[i]; return (end, new tokens)."""
        n, j = len(tokens), i + 1
        var = None
        if j < n and tokens[j].kind in ('ident', 'quoted') and not (j + 1 < n and tokens[j + 1].text == '('):
            var = tokens[j]
            j += 1
        labels = []
        while j + 1 < n and tokens[j].text == ':' and tokens[j + 1].kind in ('ident', 'quoted'):
            labels.append(_ident(tokens[j + 1]))
            j += 2
        union = None
        if j + 1 < n and tokens[j].text == '|' and len(labels) == 1:
            union = [labels[0]]
            while j + 1 < n and tokens[j].text == '|' and tokens[j + 1].kind in ('ident', 'quoted'):
                union.append(_ident(tokens[j + 1]))
                j += 2
        if j < n and tokens[j].text in ('&', '|', '!', '%'):
            raise Unsupported('Neo4j label expression')
        map_tokens = None
        if j < n and tokens[j].text == '{':
            depth, k = 0, j
            while k < n:
                depth += tokens[k].text == '{'
                depth -= tokens[k].text == '}'
                if depth == 0:
                    break
                k += 1
            map_tokens = tokens[j + 1:k]
            j = k + 1
        if j >= n or tokens[j].text != ')':
            return None
        if var is None and not labels and map_tokens is None:
            # `()` anonymous node: only a pattern if followed by a relationship
            pass
        text = '(' + (var.text if var else '')
        if union:
            # (x:A|B): any of the labels -> union of their tables
            if any(label in self.typed for label in union):
                raise Unsupported('label disjunction with a typed label')
            order = [t for label in union for t in (self.labels.get(label) or [label])]
            text += ''.join(':' + _q(t) for t in dict.fromkeys(order))
            tables, predicates = set(order), []
        elif labels:
            tables, predicates = self._tables_for(labels)
            order = [t for label in labels for t in (self.labels.get(label) or [self.typed.get(label, {}).get('table', label)])]
            text += ''.join(':' + _q(t) for t in dict.fromkeys(o for o in order if o in tables))
        else:
            predicates = []
            fresh = var is None or not self._in_scope(tokens, i, var.text)
            inferred = self._adjacent_tables(tokens, i, j + 1) if fresh else None
            if inferred and inferred != self.node_names:
                # An unlabelled node next to a relationship can only be one of
                # the relationship's endpoint tables; naming them is implied by
                # the pattern and keeps NeuG from binding the node to every
                # table (which breaks e.g. `x IN list` casts in the binder).
                text += ''.join(':' + _q(self.node_case.get(t, t)) for t in sorted(inferred))
        if map_tokens is not None and labels:
            map_tokens = self._rename_keys(map_tokens, tables)
        entries = render(self._translate(map_tokens, map_body=True)) if map_tokens is not None else ''
        extra = ', '.join(f"{_q(p)}: '{v}'" for p, v in predicates)
        if entries or extra:
            text += ' {' + ', '.join(x for x in (entries, extra) if x) + '}'
        return j + 1, tokenize(text + ')')

    def _starts_pattern(self, tokens, end):
        return end < len(tokens) and tokens[end].text in ('-', '<-', '<')

    def _pattern_predicate(self, tokens, i):
        """Translate an expression-level pattern `(a)-[..]->(b)...` into EXISTS."""
        parts, j = [], i
        node = self._node(tokens, j)
        parts.append(render(node[1]))
        j = node[0]
        while j < len(tokens) and tokens[j].text in ('-', '<-'):
            left = tokens[j].text
            j += 1
            if j < len(tokens) and tokens[j].text == '[':
                end, rel, both = self._rel(tokens, j)
                j = end
                body = render(rel)
            else:
                body, both = '', False
            if j >= len(tokens) or tokens[j].text not in ('-', '->'):
                raise Unsupported('pattern predicate')
            right = tokens[j].text
            j += 1
            if both:
                left, right = '-', '-'
            node = self._node(tokens, j)
            if node is None:
                raise Unsupported('pattern predicate node')
            parts.append(f'{left}{body}{right}{render(node[1])}')
            j = node[0]
        return j, 'EXISTS { MATCH ' + ''.join(parts) + ' }'

    # -------------------------------------------------------------- rels
    def _rel(self, tokens, i):
        n, j = len(tokens), i + 1
        out = '['
        if j < n and tokens[j].kind in ('ident', 'quoted'):
            out += tokens[j].text
            j += 1
        head = len(out)  # types are inserted after the variable
        types = []
        if j < n and tokens[j].text == ':':
            j += 1
            while j < n and tokens[j].kind in ('ident', 'quoted'):
                types.append(_ident(tokens[j]))
                j += 1
                if j < n and tokens[j].text == '|':
                    j += 1
                    if j < n and tokens[j].text == ':':
                        j += 1
                    continue
                break
        mapped = list(dict.fromkeys(t for name in types for t in self.rels.get(name, [name])))
        if self.rel_pairs:  # a mapping may list tables this store does not have
            mapped = [t for t in mapped if t.upper() in self.rel_pairs] or mapped
        star = j
        if j < n and tokens[j].text == '*':
            j += 1
            mode = None
            if j < n and tokens[j].kind == 'ident' and tokens[j].upper in ('SHORTEST', 'ALL'):
                mode = 'ALL SHORTEST' if tokens[j].upper == 'ALL' else 'SHORTEST'
                j += 2 if mode == 'ALL SHORTEST' else 1
            elif j < n and tokens[j].kind == 'ident' and tokens[j].upper == 'WALK':
                j += 1                        # _walk_reachability: trails == walks here
            elif not types or any(name not in self.acyclic for name in types):
                # Neo4j variable-length relationships are trails (no edge
                # repeated); internally they are walks.  The two agree only on
                # acyclic types (mapping: acyclic_rel_types), so others are
                # translated as trails.
                mode = 'TRAIL'
            lower = upper = None
            dots = False
            if j < n and tokens[j].kind == 'number':
                lower = int(tokens[j].text)
                j += 1
            if j < n and tokens[j].text == '..':
                dots = True
                j += 1
                if j < n and tokens[j].kind == 'number':
                    upper = int(tokens[j].text)
                    j += 1
            if not dots:
                lower, upper = (lower, lower) if lower is not None else (1, UNBOUNDED)
            lower = 1 if lower is None else lower
            if mode in ('SHORTEST', 'ALL SHORTEST') and lower != 1:
                raise Unsupported(f'shortest path with lower bound {lower}')
            out += '*' + (f' {mode} ' if mode else '') + f"{lower}..{UNBOUNDED if upper is None else upper}"
        if j < n and tokens[j].text == '{':
            end, inner = self._call_args(tokens, j)
            out += ' {' + render(self._translate(inner, map_body=True)) + '}'
            j = end
        if j < n and tokens[j].text == '(' and tokens[star].text == '*':
            # (x, n | WHERE P) from _path_edge_predicates
            close = closing_index(tokens, j)
            inner = tokens[j + 1:close]
            bar = next(k for k, t in enumerate(inner) if t.text == '|')
            out += ' (' + render(inner[:bar]) + ' | WHERE ' + render(self._translate(inner[bar + 2:])) + ')'
            j = close + 1
        if j >= n or tokens[j].text != ']':
            raise Unsupported('relationship pattern')
        both = tokens[i - 1].text == '<-' and j + 1 < n and tokens[j + 1].text == '->'
        if mapped:
            # fixed-length: prune by both endpoints; variable-length: by the
            # tables a walk from the start node can reach (_prune_varlen)
            before = None
            if i >= 2 and tokens[i - 2].text == ')':
                depth, k = 0, i - 2
                while k >= 0:
                    depth += tokens[k].text == ')'
                    depth -= tokens[k].text == '('
                    if depth == 0:
                        break
                    k -= 1
                before = self._endpoint_tables(tokens, k)
            right = tokens[j + 1].text if j + 1 < n else ''
            after = self._endpoint_tables(tokens, j + 2)
            if tokens[star].text != '*':
                mapped = self._prune(mapped, tokens[i - 1].text, right, before, after)
            else:
                mapped = self._prune_varlen(mapped, tokens[i - 1].text, right, before, after)
        if mapped:
            out = out[:head] + ':' + '|'.join(_q(t) for t in mapped) + out[head:]
        return j + 1, tokenize(out + ']'), both

    @staticmethod
    def _in_scope(tokens, i, name):
        """Is `name` already bound at tokens[i]?  Bound = projected by the
        last top-level WITH before i, or mentioned after that projection
        (or anywhere before i when there is no WITH)."""
        depth, last_with = 0, None
        for k in range(i):
            t = tokens[k]
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            if depth == 0 and t.kind == 'ident' and t.upper == 'WITH':
                last_with = k
        if last_with is None:
            return any(x.kind in ('ident', 'quoted') and x.text == name for x in tokens[:i])
        # projection items of that WITH
        k, depth, item, names = last_with + 1, 0, [], set()
        ends = {'WHERE', 'ORDER', 'SKIP', 'LIMIT', 'MATCH', 'OPTIONAL', 'UNWIND', 'WITH', 'RETURN', 'CALL'}
        while k < i:
            t = tokens[k]
            if depth == 0 and t.kind == 'ident' and t.upper in ends:
                break
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            if depth == 0 and t.text == ',':
                names.add(item[-1].text if item else None)
                item = []
            elif not (depth == 0 and t.kind == 'ident' and t.upper == 'DISTINCT' and not item):
                item.append(t)
            k += 1
        if item:
            names.add(item[-1].text)  # `x` or `expr AS x`
        if name in names or (len(item) == 1 and item[0].text == '*'):
            return True
        return any(x.kind in ('ident', 'quoted') and x.text == name for x in tokens[k:i])

    def _adjacent_tables(self, tokens, start, end):
        """Node tables implied by the relationships next to the node pattern
        tokens[start:end] (None = unconstrained)."""
        n, found = len(tokens), []
        if end + 1 < n and tokens[end].text in ('-', '<-') and tokens[end + 1].text == '[':
            close, depth = end + 1, 0
            while close < n:
                depth += tokens[close].text == '['
                depth -= tokens[close].text == ']'
                if depth == 0:
                    break
                close += 1
            left, right = tokens[end].text, tokens[close + 1].text if close + 1 < n else ''
            role = 'dst' if left == '<-' and right != '->' else 'src' if right == '->' and left != '<-' else 'either'
            found.append(self._rel_endpoint(tokens[end + 2:close], role))
        if start >= 2 and tokens[start - 1].text in ('-', '->') and tokens[start - 2].text == ']':
            k, depth = start - 2, 0
            while k >= 0:
                depth += tokens[k].text == ']'
                depth -= tokens[k].text == '['
                if depth == 0:
                    break
                k -= 1
            left, right = tokens[k - 1].text if k else '', tokens[start - 1].text
            role = 'dst' if right == '->' and left != '<-' else 'src' if left == '<-' and right != '->' else 'either'
            found.append(self._rel_endpoint(tokens[k + 1:start - 2], role))
        sets = [f for f in found if f is not None]
        if not sets:
            return None
        out = set.intersection(*sets)
        return out or None

    def _rel_endpoint(self, inner, role):
        if not self.rel_pairs:
            return None
        k = 1 if inner and inner[0].kind in ('ident', 'quoted') else 0
        types = []
        if k < len(inner) and inner[k].text == ':':
            k += 1
            while k < len(inner) and inner[k].kind in ('ident', 'quoted'):
                types.append(_ident(inner[k]))
                k += 1
                if k < len(inner) and inner[k].text == '|':
                    k += 1
                    if k < len(inner) and inner[k].text == ':':
                        k += 1
                    continue
                break
        if not types:
            return None
        if k < len(inner) and inner[k].text == '*':
            rest = [t for t in inner[k + 1:] if t.kind != 'ident']
            if not rest or rest[0].kind != 'number' or int(rest[0].text) < 1:
                return None  # a zero-length path: the node may be the other end
        tables = [t for name in types for t in self.rels.get(name, [name]) if t.upper() in self.rel_pairs]
        if not tables:
            return None
        out = set()
        for table in tables:
            for src, dst in self.rel_pairs[table.upper()]:
                if role in ('src', 'either'):
                    out.add(src)
                if role in ('dst', 'either'):
                    out.add(dst)
        return out

    def _renames(self, tables, prop):
        names = {self.prop_names.get(t.upper(), {}).get(prop, prop) for t in tables}
        if len(names) > 1:
            raise Unsupported(f'property {prop} has different internal names on {sorted(tables)}')
        name = names.pop()
        return None if name == prop else name

    def _property(self, var, prop):
        tables = self.var_tables.get(var)
        if not tables or not self.prop_names:
            return None
        return self._renames(tables, prop)

    def _rename_keys(self, tokens, tables):
        if not self.prop_names:
            return tokens
        out, depth = [], 0
        for k, t in enumerate(tokens):
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            if depth == 0 and t.kind in ('ident', 'quoted') and k + 1 < len(tokens) and tokens[k + 1].text == ':' \
                    and (k == 0 or tokens[k - 1].text == ','):
                renamed = self._renames(tables, _ident(t))
                if renamed is not None:
                    t = Tok(t.kind, _q(renamed), t.ws)
            out.append(t)
        return out

    def _label_predicate(self, var, label):
        if label in self.typed:
            spec = self.typed[label]
            return f"({_q(var)}.{_q(spec['property'])} = '{spec['value']}')"
        tables = self.labels.get(label, [label])
        if len(tables) == 1:
            return f"(label({_q(var)}) = '{tables[0]}')"
        return f"(label({_q(var)}) IN [{', '.join(repr(t) for t in tables)}])"


def _conjuncts(e):
    if e is None:
        return []
    if e.get('function') == 'AND' or e.get('k') == 'AND':
        return [c for child in e.get('children') or [] for c in _conjuncts(child)]
    return [e]


def _ref(e):
    """Entity uid referenced by `e` (entity itself or its internal id)."""
    if e is None:
        return None
    if e.get('entity'):
        return e['entity']
    if e.get('k') == 'PROPERTY' and e.get('property') == '_ID':
        return e.get('owner')
    return None


def _unequal(predicate):
    """Entity pairs that differ on every row the predicate keeps: `a <> b`,
    and `a.p <> b.p` (one entity has one value of p)."""
    pairs = set()
    for c in _conjuncts(predicate):
        if c.get('function') == 'NOT_EQUALS' and len(c.get('children') or []) == 2:
            a, b = (_ref(x) for x in c['children'])
            if a and b:
                pairs.add(frozenset((a, b)))
                continue
            x, y = c['children']
            if x.get('k') == y.get('k') == 'PROPERTY' and x.get('property') == y.get('property') \
                    and x.get('owner') and y.get('owner'):
                pairs.add(frozenset((x['owner'], y['owner'])))
    return pairs


def may_share_edge(a, b, predicate, entity_tables, rel_group, distinct_nodes=frozenset()):
    """Could relationships a and b (IR) bind the same edge?  distinct_nodes:
    node pairs known to differ on every row of the clause."""
    def tables(rel):
        return {rel_group.get(t, t).upper() for t in rel.get('tables') or []}
    if not tables(a) & tables(b):
        return False
    unequal = _unequal(predicate) | set(distinct_nodes)
    if frozenset((a['uid'], b['uid'])) in unequal:
        return False
    if a.get('recursive') is None and b.get('recursive') is None:
        def nodes(u):
            return {t.upper() for t in entity_tables.get(u) or []}
        ends = []
        if a.get('direction') == 'SINGLE' and b.get('direction') == 'SINGLE':
            ends = [((a['src'], b['src']), (a['dst'], b['dst']))]
        else:  # undirected: either orientation
            ends = [((a['src'], b['src']), (a['dst'], b['dst'])), ((a['src'], b['dst']), (a['dst'], b['src']))]
        def impossible(pair):
            x, y = pair
            if x == y:
                return False
            if frozenset((x, y)) in unequal:
                return True
            nx, ny = nodes(x), nodes(y)
            return bool(nx and ny and not nx & ny)
        if all(impossible(p) or impossible(q) for p, q in ends):
            return False
    return True


class Neo4jDialect(Dialect):
    """Bound IR -> Neo4j Cypher with the profile's schema mapping."""
    name = 'neo4j'

    def __init__(self, mapping):
        super().__init__()
        self.m = mapping
        self.table_sets = {frozenset(t.upper() for t in tables): label for label, tables in mapping['labels'].items()}
        self.single = {}
        for label, tables in mapping['labels'].items():
            if len(tables) == 1:
                self.single.setdefault(tables[0].upper(), label)
        self.typed = {(spec['table'].upper(), spec['property'], spec['value']): label
                      for label, spec in mapping['typed_labels'].items()}
        self.rel_names = {}
        for name, tables in mapping['rel_types'].items():
            for table in tables:
                self.rel_names.setdefault(table.upper(), name)
        self.entity_tables = {}
        self.prop_back = {t.upper(): {internal: name for name, internal in m.items()}
                          for t, m in mapping['properties'].items()}

    def property(self, owner, prop, uid=None):
        if self.prop_back:
            tables = self.entity_tables.get(uid) or []
            names = {self.prop_back.get(t.upper(), {}).get(prop, prop) for t in tables} or {prop}
            if len(names) > 1:
                raise Unsupported(f'property {prop} has different Neo4j names on {sorted(tables)}')
            prop = names.pop()
        return super().property(owner, prop, uid)

    def node_pattern(self, variable, tables):
        key = frozenset(t.upper() for t in tables)
        if key in self.table_sets:
            labels = [self.table_sets[key]]
        else:
            labels = [self.single.get(t.upper(), t) for t in tables]
        if len(labels) == 1:
            return self.ident(variable) + ':' + self.ident(labels[0])
        return self.ident(variable) + ':' + '|'.join(self.ident(label) for label in labels)

    def rel_types(self, tables):
        groups = list(dict.fromkeys(self.rel_group.get(t, t) for t in tables))
        names = list(dict.fromkeys(self.rel_names.get(g.upper(), g) for g in groups))
        return (':' + '|'.join(self.ident(n) for n in names)) if names else ''

    # Neo4j is schemaless: carriers need no DDL.
    def add_column_ddl(self, table, column, type_text, default):
        return []

    def create_rel_table_ddl(self, item, schema):
        return []

    BATCH_ROWS = 10000
    PARTITION_ROWS = 1000
    create_on_label_union = True

    def fill_statement(self, body, tail, variables, eager=False, partition=None):
        # Large writes in bounded transactions (auto-commit session required).
        scope = ', '.join(self.ident(v) for v in variables)
        if eager and partition is not None:
            # New relationships on nodes the source traverses: drive the build
            # by a label scan of one key node and run the source per node, so
            # no outer read outlives a committed batch and memory stays bounded.
            var, tables = partition
            return (f'MATCH ({self.node_pattern(var, tables)}) CALL ({self.ident(var)}) {{ {body} {tail} }} '
                    f'IN TRANSACTIONS OF {self.PARTITION_ROWS} ROWS')
        if not eager:
            return f'{body} CALL ({scope}) {{ {tail} }} IN TRANSACTIONS OF {self.BATCH_ROWS} ROWS'
        # New relationships on nodes the source still traverses: materialize
        # the source rows first, otherwise the outer read becomes outdated.
        unpack = ', '.join(f'cico_row[{i}] AS {self.ident(v)}' for i, v in enumerate(variables))
        return (f'{body} WITH collect([{scope}]) AS cico_rows UNWIND cico_rows AS cico_row '
                f'CALL (cico_row) {{ WITH {unpack} {tail} }} IN TRANSACTIONS OF {self.BATCH_ROWS} ROWS')

    edge_orientation = 'start_node'

    def edge_carrier_test(self, a, b, rel, column_ab, column_ba, fallback, fresh):
        """One undirected match; startNode() is the stored start in Neo4j."""
        k = self.ident(fresh('cico_k'))
        read = lambda column: f'coalesce({k}.{self.ident(column)}, 0) > 0' if column else fallback  # noqa: E731
        return (f'OPTIONAL MATCH ({a})-[{k}{rel}]-({b})',
                f'CASE WHEN {k} IS NULL THEN {fallback} '
                f'WHEN startNode({k}) = {a} THEN {read(column_ab)} ELSE {read(column_ba)} END')

    def var_length(self, lower, upper):
        return f'*{lower}..' if upper == UNBOUNDED else f'*{lower}..{upper}'

    def path_length(self, owner, uid=None):
        return self.property(owner, '_LENGTH', uid)   # unchanged spelling; the guard rejects it

    def shortest_length(self, all_paths, lower, upper):
        # The relationship of `shortestPath((a)-[r:T*l..u]-(b))`;
        # shortest_function() supplies the wrapper.  Neo4j accepts only the
        # lower bounds 0 and 1 there.
        if lower not in (0, 1):
            raise Unsupported(f'shortest path with lower bound {lower}')
        return self.var_length(lower, upper)

    def shortest_function(self, all_paths):
        return 'allShortestPaths' if all_paths else 'shortestPath'

    def label_of(self, arg):
        raise Unsupported('label() has no exact Neo4j equivalent')

    # ---------------------------------------------- relationship uniqueness
    def match_text(self, keyword, parts, predicate):
        """Neo4j binds the relationships of one MATCH clause to pairwise
        distinct edges; the internal (Kuzu/NeuG) semantics does not.  A
        mandatory MATCH is therefore printed as one MATCH per relationship
        (no uniqueness across clauses).  An OPTIONAL MATCH cannot be split, so
        it is kept only when its relationships can never bind the same edge."""
        rels = [r for _, r in parts if r is not None]
        if len(rels) <= 1:
            return super().match_text(keyword, parts, predicate)
        if keyword == 'MATCH':
            return 'MATCH ' + ' MATCH '.join(text for text, _ in parts)
        # The endpoints of a fixed-length relationship of an acyclic type
        # (no self-loops, e.g. REPLY_OF) differ on every row.
        acyclic = {t.upper() for name in self.m.get('acyclic_rel_types') or []
                   for t in self.m['rel_types'].get(name, [])}
        distinct = {frozenset((r['src'], r['dst'])) for r in rels
                    if r.get('recursive') is None and r.get('tables')
                    and all(t.upper() in acyclic or self.rel_group.get(t, t).upper() in acyclic for t in r['tables'])}
        def into_path_start(a, b):
            # fixed x -[:T]-> s and a walk s -[:T*]-> ... of an acyclic T never
            # share an edge (Neo4jFrontend._edge_into_path_start)
            path, fixed = (a, b) if a.get('recursive') is not None else (b, a)
            return (path.get('recursive') is not None and fixed.get('recursive') is None
                    and path.get('direction') == fixed.get('direction') == 'SINGLE'
                    and path.get('tables') and fixed.get('tables')
                    and all(t.upper() in acyclic or self.rel_group.get(t, t).upper() in acyclic
                            for t in list(path['tables']) + list(fixed['tables']))
                    and fixed.get('dst') == path.get('src'))
        for i, a in enumerate(rels):
            for b in rels[i + 1:]:
                if into_path_start(a, b):
                    continue
                if may_share_edge(a, b, predicate, self.entity_tables, self.rel_group, distinct):
                    raise Unsupported('OPTIONAL MATCH whose relationships Neo4j would force distinct')
        return super().match_text(keyword, parts, predicate)

    def special_expression(self, e, printer):
        """label(x) = 'TABLE'  ->  (x:Label)   (Neo4j label predicate).
        Sums: Neo4j's sum() of no (non-null) values is 0, NeuG's NULL (the
        frontend reads Neo4j `sum(a)` as `CASE WHEN sum(a) IS NOT NULL THEN
        sum(a) ELSE 0 END`), so a NeuG sum that is 0 when NULL prints as
        `sum(a)` and a bare NeuG sum as
        `CASE WHEN count(a) = 0 THEN null ELSE sum(a) END`."""
        from .canonical import sum_or_zero
        total = sum_or_zero(e)
        if total is not None:
            return self._sum(total, printer)
        if e.get('k') == 'AGGREGATE_FUNCTION' and e.get('function') == 'SUM':
            text = self._sum(e, printer)
            return f"CASE WHEN count{text[3:]} = 0 THEN null ELSE {text} END"
        if e.get('function') != 'EQUALS' or len(e.get('children') or []) != 2:
            return None
        for label, literal in (e['children'], e['children'][::-1]):
            arg = (label.get('children') or [None])[0]
            if label.get('function') == 'LABEL' and arg and arg.get('property') == '_ID' \
                    and literal.get('k') == 'LITERAL' and (literal.get('value') or {}).get('type') == 'STRING':
                table = literal['value'].get('text')
                name = self.single.get(str(table).upper())
                if name is None and not any(str(table).upper() == t.upper()
                                            for tables in self.m['labels'].values() for t in tables):
                    name = table  # unmapped: the Neo4j label is the table name
                if name is None:
                    return None
                return f'({self.ident(printer.entity_name(arg["owner"]))}:{self.ident(name)})'
        return None

    def _sum(self, e, printer):
        return Dialect.aggregate(self, 'SUM', [printer.expr(c) for c in e.get('children') or []],
                                 e.get('distinct', False))

    def temporal_literal(self, type_name, text):
        if type_name == 'DATE':
            return f'date({self.string(text)})'
        if type_name.startswith('TIMESTAMP'):
            return f"datetime({self.string(text.replace(' ', 'T', 1))})"
        raise Unsupported(f'literal type {type_name} for Neo4j')

    def literal(self, v):
        if v.get('null') and v.get('type') not in ('ANY', 'NULL', ''):
            # The frontend reads toFloat(null) as CAST(NULL AS DOUBLE), etc.
            function = {'DOUBLE': 'toFloat', 'FLOAT': 'toFloat', 'INT64': 'toInteger',
                        'STRING': 'toString'}.get(v.get('type'))
            if function is None:
                raise Unsupported('typed NULL for Neo4j')
            return f'{function}(null)'
        return super().literal(v)

    def cast(self, arg, type_name):
        function = {'DOUBLE': 'toFloat', 'FLOAT': 'toFloat', 'INT64': 'toInteger', 'INT32': 'toInteger',
                    'STRING': 'toString', 'TIMESTAMP': 'datetime', 'DATE': 'date'}.get(type_name)
        if function is None:
            raise Unsupported(f'CAST to {type_name} for Neo4j')
        return f'{function}({arg})'

    SPELLING = {'TIMESTAMP': 'datetime', 'DATE': 'date', 'LOWER': 'toLower', 'UPPER': 'toUpper',
                'TO_INTERVAL': None, 'INTERVAL': None, 'LABEL': None, 'LABELS': None,
                'RELS': 'relationships'}

    INTERVAL_UNITS = {'year': ('Y', False), 'month': ('M', False), 'week': ('W', False), 'day': ('D', False),
                      'hour': ('H', True), 'minute': ('M', True), 'second': ('S', True)}

    def _iso_duration(self, text):
        m = re.fullmatch(r"'((?:\s*\d+\s+[a-zA-Z]+)+)\s*'", text)
        if not m:
            return None
        date, time = '', ''
        for value, unit in re.findall(r'(\d+)\s+([a-zA-Z]+)', m[1]):
            spec = self.INTERVAL_UNITS.get(unit.lower().rstrip('s'))
            if spec is None:
                return None
            if spec[1]:
                time += value + spec[0]
            else:
                date += value + spec[0]
        return "duration('P" + date + ('T' + time if time else '') + "')"

    def call(self, name, args):
        if name == 'TO_INTERVAL' and len(args) == 1:
            iso = self._iso_duration(args[0])
            if iso is None:
                raise Unsupported('non-literal interval for Neo4j')
            return iso
        if name == 'DATE_PART' and len(args) == 2:
            unit = re.fullmatch(r"'([a-z]+)'", args[0])
            if not unit or unit[1] not in TEMPORAL_FIELDS:
                raise Unsupported('date_part unit')
            if not re.fullmatch(r'(\$?[A-Za-z_][A-Za-z0-9_]*)(\.[A-Za-z_][A-Za-z0-9_]*)?', args[1]):
                raise Unsupported('temporal accessor on an expression')
            return f'{args[1]}.{unit[1]}'
        if name in self.SPELLING:
            spelled = self.SPELLING[name]
            if spelled is None:
                raise Unsupported(f'function {name} for Neo4j')
            return f'{spelled}(' + ', '.join(args) + ')'
        return f'{name.lower()}(' + ', '.join(args) + ')'

    def function(self, name, args, e=None):
        if name == 'EQUALS' and e is not None:
            label = self._typed_label(e)
            if label:
                return label
        if name == 'STRUCT_EXTRACT' and e is not None:
            # `x._ID` of a node/relationship value (e.g. an UNWIND variable
            # over relationships(path)): Neo4j's id(x).
            child = (e.get('children') or [None, None])[1] or {}
            if child.get('k') == 'LITERAL' and str((child.get('value') or {}).get('text')).upper() == '_ID':
                return f'id({args[0]})'
        return super().function(name, args, e)

    def _typed_label_of(self, e):
        """(owner uid, Neo4j label) when `e` is `x.<discriminator> = 'value'`
        of a typed label (e.g. PLACE.type = 'City' -> City)."""
        if e.get('function') != 'EQUALS':
            return None
        left, right = (e.get('children') or [None, None])[:2]
        if not left or not right or left.get('k') != 'PROPERTY' or right.get('k') != 'LITERAL':
            return None
        value = (right.get('value') or {}).get('text')
        tables = self.entity_tables.get(left.get('owner')) or []
        if len(tables) != 1 or (right.get('value') or {}).get('type') != 'STRING':
            return None
        label = self.typed.get((tables[0].upper(), left.get('property'), value))
        return (left.get('owner'), label) if label else None

    def _typed_label(self, e):
        found = self._typed_label_of(e)
        if found is None:
            return None
        owner = self.printer_names.get(found[0])
        return f'({self.ident(owner)}:{self.ident(found[1])})' if owner else None

    def fold_node_labels(self, graphs, predicate, declared):
        """`(x:Place) ... WHERE x:City`  ->  `(x:City)`.  The mapping already
        reads `(:City)` as PLACE with type 'City' (the frontend's translation
        of the input), so both spellings denote the same nodes; Neo4j however estimates a node with two labels by
        multiplying their selectivities, which made it plan bi-3 around a
        near-empty estimate (Apply per message, 1.9s -> 103s).  Only nodes this
        clause declares are folded, one typed label per node."""
        if predicate is None:
            return {}, predicate
        fresh = {n['uid'] for g in graphs or [] for n in g.get('nodes') or [] if n['uid'] not in declared}
        folds, rest = {}, []
        for conjunct in _conjuncts(predicate):
            found = self._typed_label_of(conjunct)
            if found and found[0] in fresh and found[0] not in folds:
                folds[found[0]] = found[1]
            else:
                rest.append(conjunct)
        if not folds:
            return {}, predicate
        if not rest:
            return folds, None
        if len(rest) == 1:
            return folds, rest[0]
        return folds, {**predicate, 'children': rest}


NATIVE_DDL = (r'^\s*(?:CREATE\s+(?:(?:RANGE|TEXT|POINT|FULLTEXT|VECTOR|LOOKUP)\s+)?(?:INDEX|CONSTRAINT)'
              r'|DROP\s+(?:INDEX|CONSTRAINT))\b')


def neo4j_profile(mapping=None, constraints=None):
    """Neo4j dialect declaration. `mapping`: schema mapping YAML (labels,
    typed labels, relationship types; see profiles/*.yaml); `constraints`:
    a shared constraints file overriding the mapping's constraints_file."""
    from .profiles import Profile, Semantics
    path = mapping or Path(__file__).resolve().parents[1] / 'profiles/ldbc-snb-neo4j.yaml'
    m = load_mapping(path, constraints)
    return Profile('neo4j', Neo4jDialect(m), Neo4jFrontend(m),
                   semantics=Semantics(relationship_uniqueness='isomorphism', variable_length='trail',
                                       type_classes='label', procedures=True, native_ddl=NATIVE_DDL),
                   description=f'Neo4j Cypher with schema mapping {Path(path).name}')

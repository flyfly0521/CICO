"""Declared-constraint simplifications (rule `constraint_retry`).

The retry stage rewrites a statement into an equivalent one, on every
database satisfying the declared constraints (constraints.py), whose shape
RewriterV4 can match against a FILL.  The steps are stated on the INTERNAL
dialect (NeuG/Kuzu Cypher, where a type class such as City is the table
PLACE plus the predicate `type = 'City'`); `InternalRelax` implements them
for frontends whose input is internal Cypher (NeuG, Kuzu).  The Neo4j
frontend implements the same steps on Neo4j labels (neo4j.py), because there
a type class is a label rather than a predicate.  `key_constants` (T3) is
shared verbatim: it only needs a key lookup.

Every step records the constraints it relied on (Result.premises).

R1  implied type classes.  A conjunct `v.p = 'x'` of a MATCH/OPTIONAL MATCH
    WHERE, where (v.p, 'x') defines the type class C on v's table, is dropped
    when C is implied for v by endpoint_labels constraints along fixed-length
    directed relationships of the SAME clause, starting from classes known in
    that clause (a node's tables, or type conjuncts kept in its WHERE).  The
    dropped conjunct is true on every candidate match, so the clause matches
    the same rows.  Conjuncts are dropped one at a time, each justified only
    by conjuncts still present (no circular justification).
T1  key identity.  A node (v:T {p: E}) of a mandatory MATCH, E a parameter or
    literal, whose class C (T itself, or a type class on T fixed by a
    conjunct v.tp = 'x' of the clause) has a declared key p (unique +
    required), becomes (k:T {p: E}) with the type conjunct moved to k; v is
    bound again by its key, either deferred to the part's WITH (when v is
    used in its part only there and as a bare WITH item) or as a separate
    pattern of the clause.  Both k and v are the only C with p = E, so rows
    correspond one to one (the Neo4j T1 argument, neo4j.py `_key_identities`).
E1  carrier edge in an OPTIONAL MATCH.  A mandatory MATCH bound
    (s)-[:R]->(t); a later OPTIONAL MATCH of the same scope reads both s and
    t but not an R edge from s to t.  With R declared unique_edge (at most
    one R per ordered pair), adding (s)-[:R]->(t) to that OPTIONAL pattern
    keeps every row: the added edge can only be the one already matched, so
    the optional pattern matches exactly as often, and null-extends exactly
    when, it did before.  A relationship FILL keyed by (s, t) then sees its
    own carrier edge inside the clause it replaces (LDBC IC5: HASMEMBER,
    IC14: KNOWS).
T2  key identity of a bound node.  A bare (v) bound by an earlier clause,
    whose incoming relationship in this clause forces class C
    (endpoint_labels) with key p, becomes (k:T) with `k.p = v.p` and a guard
    that v is a C (true on every original row by the same constraint).
"""
import re

from .errors import Unsupported
from .neo4j import Neo4jFrontend, _ident, _q, render, tokenize
from .syntax import closing

CLAUSE_STARTS = Neo4jFrontend.CLAUSE_STARTS
_peek_node = Neo4jFrontend._peek_node


# --------------------------------------------------------- T3 (shared)
def key_constants(cypher, key_of):
    """Relax stage: (text, premises), or (None, set()).

    T3  A variable v bound once, by a mandatory top-level
        MATCH (v:L {p: E}) with E a parameter or literal and p a declared
        key of L (unique + required), names the same node on every row:
        the only L with p = E.  Wherever every WITH between the binding
        and a use projects v as a bare item (so v is never rebound), a
        bare (v) inside a size([...])/COUNT{}/EXISTS{} region is replaced
        by (k:L {p: E}).  The region then no longer has v as an input
        anchor, so a carrier that filters by the key value (and hides the
        node) satisfies the interface condition (theory.md (M_P)(ii)).

    `key_of(label, prop)` returns (prop, unique, required) or None."""
    from .middleware import strip_comments
    from .queryparts import _top_level, PROJECTION_TAIL
    from .scalar_subqueries import regions
    tokens = tokenize(strip_comments(cypher))
    top = _top_level(tokens)
    as_bound = {_ident(tokens[k]) for k in range(1, len(tokens))
                if tokens[k - 1].upper == 'AS' and tokens[k].kind in ('ident', 'quoted')}
    bindings = {}
    for k, (pos, word) in enumerate(top):
        if word != 'MATCH' or (k and top[k - 1][1] == 'OPTIONAL' and top[k - 1][0] == pos - 1):
            continue
        end = top[k + 1][0] if k + 1 < len(top) else len(tokens)
        for i in range(pos + 1, end):
            if tokens[i].text != '(':
                continue
            body = tokens[i + 1:i + 9]
            if len(body) < 8 or body[1].text != ':' or body[3].text != '{' or body[5].text != ':' \
                    or body[7].text != '}' or tokens[i + 9].text != ')' \
                    or body[6].kind not in ('param', 'string', 'number'):
                continue
            var, label, prop = _ident(body[0]), _ident(body[2]), _ident(body[4])
            key = key_of(label, prop)
            earlier = any(t.kind in ('ident', 'quoted') and _ident(t) == var for t in tokens[:i])
            if key is None or earlier or var in as_bound or var in bindings:
                continue
            bindings[var] = (i, label, prop, body[6].text, key)
    if not bindings:
        return None, set()

    def carried(var, at, upto):
        """Every WITH strictly between the binding and `upto` projects bare var."""
        for pos, word in top:
            if not (at < pos < upto) or word != 'WITH':
                continue
            i = pos + 1 + (tokens[pos + 1].upper == 'DISTINCT')
            items, cur, depth = [], [], 0
            while i < len(tokens):
                t = tokens[i]
                depth += t.text in ('(', '[', '{')
                depth -= t.text in (')', ']', '}')
                if depth == 0 and t.kind == 'ident' and (t.upper in PROJECTION_TAIL or t.upper in CLAUSE_STARTS):
                    break
                if depth == 0 and t.text == ',':
                    items.append(cur)
                    cur = []
                else:
                    cur.append(t)
                i += 1
            items.append(cur)
            if not any(len(it) == 1 and it[0].kind in ('ident', 'quoted') and _ident(it[0]) == var for it in items):
                return False
        return True
    edits, premises = [], set()
    taken = {_ident(t) for t in tokens if t.kind in ('ident', 'quoted')}
    counter = [0]
    for k, (pos, word) in enumerate(top):
        if word not in ('WITH', 'RETURN'):
            continue
        end = top[k + 1][0] if k + 1 < len(top) else len(tokens)
        try:
            found = list(regions(tokens, pos + 1, end))
        except Unsupported:
            continue
        for region in found:
            for i in range(region.start, region.end - 2):
                if tokens[i].text != '(' or tokens[i + 2].text != ')' or tokens[i + 1].kind not in ('ident', 'quoted'):
                    continue
                var = _ident(tokens[i + 1])
                if var not in bindings or not carried(var, bindings[var][0], pos):
                    continue
                _, label, prop, value, key = bindings[var]
                counter[0] += 1
                fresh = f'cico_const{counter[0]}'
                while fresh in taken:
                    fresh += '_'
                taken.add(fresh)
                edits.append((i, i + 3, f'({fresh}:{_q(label)} {{{_q(prop)}: {value}}})'))
                premises.update(c.describe() for c in key[1:])
    if not edits:
        return None, set()
    out = list(tokens)
    for lo, hi, text in sorted(edits, reverse=True):
        out[lo:hi] = tokenize(' ' + text)
    return render(out), premises


# ------------------------------------------------- statement analysis
class _Clause:
    def __init__(self, index, start, word, pattern, where):
        self.index, self.start, self.word = index, start, word
        self.pattern = pattern          # (lo, hi) token range of the pattern
        self.where = where              # (lo, hi) of the WHERE expression, or None
        self.nodes = []                 # [(position of '(', var, labels)]
        self.edges = []                 # [(src binding, rel, dst binding, src pos, dst pos)]

    @property
    def optional(self):
        return self.word == 'OPTIONAL'


class _Analysis:
    """Clauses, variable bindings (scope aware) and conjuncts of a statement."""

    def __init__(self, tokens):
        self.tokens = tokens
        self.bounds = self._bounds()
        self.clauses = []
        self.binding = {}               # '(' position -> binding id
        self.tables = {}                # binding id -> set of tables (intersection of written labels)
        self.first = {}                 # binding id -> first '(' position
        self.var_of = {}                # binding id -> variable name (None if anonymous)
        self.clause_of_binding = {}     # binding id -> index of the clause that binds it
        self._scan()

    def _bounds(self):
        tokens, out, depth = self.tokens, [], 0
        for i, t in enumerate(tokens):
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper in CLAUSE_STARTS and not (i and tokens[i - 1].text == '.'):
                if t.upper == 'MATCH' and out and out[-1][1] == 'OPTIONAL' and out[-1][0] == i - 1:
                    continue
                out.append((i, t.upper))
        out.append((len(tokens), 'END'))
        return out

    def _projection(self, start, end):
        """Names a WITH/RETURN at `start` keeps: bare items keep their binding."""
        tokens = self.tokens
        i = start + 1
        if i < end and tokens[i].upper == 'DISTINCT':
            i += 1
        items, cur, depth = [], [], 0
        while i < end:
            t = tokens[i]
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            if depth == 0 and t.text == ',':
                items.append(cur)
                cur = []
            else:
                cur.append(t)
            i += 1
        items.append(cur)
        keep, star, fresh = set(), False, set()
        for it in items:
            if len(it) == 1 and it[0].text == '*':
                star = True
            elif len(it) == 1 and it[0].kind in ('ident', 'quoted'):
                keep.add(_ident(it[0]))
            elif len(it) >= 2 and it[-2].upper == 'AS' and it[-1].kind in ('ident', 'quoted'):
                fresh.add(_ident(it[-1]))
        return keep, star, fresh

    def _scan(self):
        tokens, bounds = self.tokens, self.bounds
        scope, counter = {}, [0]                       # name -> binding id

        def new_binding(var, at, clause):
            counter[0] += 1
            b = counter[0]
            self.var_of[b], self.first[b], self.clause_of_binding[b] = var, at, clause
            return b
        for k, (start, word) in enumerate(bounds[:-1]):
            end = bounds[k + 1][0]
            if word in ('WITH', 'RETURN'):
                # items end at the next bound (WHERE/ORDER/SKIP/LIMIT are bounds too)
                keep, star, fresh = self._projection(start, end)
                if not star:
                    scope = {n: b for n, b in scope.items() if n in keep}
                for n in fresh:
                    scope[n] = None                    # a value, not a node binding
                continue
            if word == 'UNWIND':
                for j in range(start, end - 1):
                    if tokens[j].upper == 'AS' and tokens[j + 1].kind in ('ident', 'quoted'):
                        scope[_ident(tokens[j + 1])] = None
                continue
            if word not in ('MATCH', 'OPTIONAL'):
                continue
            lo = start + (2 if word == 'OPTIONAL' else 1)
            where = (end + 1, bounds[k + 2][0]) if bounds[k + 1][1] == 'WHERE' else None
            clause = _Clause(len(self.clauses), start, word, (lo, end), where)
            self.clauses.append(clause)
            depth, i = 0, lo
            while i < end:
                t = tokens[i]
                if t.text in ('[', '{'):
                    i = closing(tokens, i) + 1
                    continue
                if t.text == '(':
                    var, labels = _peek_node(tokens, i)
                    if var is None and not (i + 1 < end and tokens[i + 1].text in (':', ')', '{')):
                        i = closing(tokens, i) + 1
                        continue
                    if var is None:
                        b = new_binding(None, i, clause.index)
                    elif scope.get(var) is not None:
                        b = scope[var]
                    else:
                        b = new_binding(var, i, clause.index)
                        scope[var] = b
                    self.binding[i] = b
                    if labels:
                        written = {x.upper() for x in labels}
                        self.tables[b] = self.tables[b] & written if b in self.tables else written
                    clause.nodes.append((i, var, labels))
                    i = closing(tokens, i) + 1
                    continue
                i += 1
            # relationship variables and path names are values here
            for i in range(lo, end - 1):
                if tokens[i].text == '[' and i and tokens[i - 1].text in ('-', '<-') \
                        and tokens[i + 1].kind in ('ident', 'quoted'):
                    scope.setdefault(_ident(tokens[i + 1]), None)
            clause.edges = self._edges(lo, end)

    def _edges(self, lo, end):
        tokens, out = self.tokens, []
        for i in range(lo + 1, end):
            if tokens[i].text != '[' or tokens[i - 1].text not in ('-', '<-'):
                continue
            close = closing(tokens, i)
            inner = tokens[i + 1:close]
            if any(t.text == '*' for t in inner):
                continue
            types = [_ident(t).upper() for k, t in enumerate(inner) if k and inner[k - 1].text in (':', '|')
                     and t.kind in ('ident', 'quoted')]
            if len(types) != 1 or any(t.text == '|' for t in inner):
                continue
            if i < 2 or tokens[i - 2].text != ')' or close + 2 >= end or tokens[close + 2].text != '(':
                continue
            left = self._node_start(i - 2)
            right = close + 2
            if left not in self.binding or right not in self.binding:
                continue
            if tokens[i - 1].text == '-' and tokens[close + 1].text == '->':
                out.append((self.binding[left], types[0], self.binding[right], left, right))
            elif tokens[i - 1].text == '<-' and tokens[close + 1].text == '-':
                out.append((self.binding[right], types[0], self.binding[left], right, left))
        return out

    def _node_start(self, close):
        depth, k = 0, close
        while k >= 0:
            depth += self.tokens[k].text == ')'
            depth -= self.tokens[k].text == '('
            if depth == 0:
                return k
            k -= 1
        return None

    # ---------------------------------------------------------- conjuncts
    def conjuncts(self, where):
        """[(lo, hi)] top-level AND conjuncts of a WHERE range (one span if an
        OR/XOR occurs at top level).  A conjunct wrapped in parentheses is
        split further."""
        lo, hi = where
        return self._split(lo, hi)

    def _split(self, lo, hi):
        tokens, depth, out, cur = self.tokens, 0, [], lo
        if any(tokens[i].upper in ('OR', 'XOR') and self._depth_zero(lo, i) for i in range(lo, hi)):
            return [(lo, hi)]
        for i in range(lo, hi):
            t = tokens[i]
            if t.text in ('(', '[', '{'):
                depth += 1
            elif t.text in (')', ']', '}'):
                depth -= 1
            elif depth == 0 and t.kind == 'ident' and t.upper == 'AND':
                out.append((cur, i))
                cur = i + 1
        out.append((cur, hi))
        result = []
        for a, b in out:
            if b - a >= 2 and tokens[a].text == '(' and closing(tokens, a) == b - 1:
                result.extend(self._split(a + 1, b - 1))
            else:
                result.append((a, b))
        return result

    def _depth_zero(self, lo, i):
        depth = 0
        for k in range(lo, i):
            depth += self.tokens[k].text in ('(', '[', '{')
            depth -= self.tokens[k].text in (')', ']', '}')
        return depth == 0

    def equality(self, span):
        """(var, prop, string value) of `var.prop = 'x'` / `'x' = var.prop`, else None."""
        lo, hi = span
        t = self.tokens[lo:hi]
        if len(t) != 5 or t[3].text != '=' and t[1].text != '=':
            return None
        if t[1].text == '.' and t[3].text == '=' and t[4].kind == 'string' and t[0].kind in ('ident', 'quoted'):
            return _ident(t[0]), _ident(t[2]), _string(t[4].text)
        if t[1].text == '=' and t[3].text == '.' and t[0].kind == 'string' and t[2].kind in ('ident', 'quoted'):
            return _ident(t[2]), _ident(t[4]), _string(t[0].text)
        return None


def _string(text):
    body = text[1:-1]
    return re.sub(r'\\(.)', r'\1', body)


# ------------------------------------------------------- the relax stage
class InternalRelax:
    """R1, T1, T2 above, for internal Cypher and internal-name constraints."""

    def __init__(self, constraints):
        self.c = constraints
        classes = constraints.classes or {}
        self.typed = {n: s for n, s in classes.items() if 'table' in s}
        self.untyped = {n: {t.upper() for t in s['tables']} for n, s in classes.items() if 'tables' in s}

    # ---- class knowledge
    def table_classes(self, tables):
        return {n for n, ts in self.untyped.items() if tables and tables <= ts}

    def typed_class(self, table, prop, value):
        return next((n for n, s in self.typed.items()
                     if s['table'] == table and s['property'] == prop and s['value'] == value), None)

    def class_key(self, name, prop=None):
        return self.c.key(name, prop)

    def key_of_table(self, label, prop):
        """Key of the untyped class that is exactly table `label` (T3)."""
        table = label.upper()
        for name, tables in self.untyped.items():
            if tables == {table}:
                found = self.c.key(name, prop)
                if found is not None:
                    return found
        return None

    def _implied(self, a, clause, typed_facts):
        """{binding: classes implied by endpoint_labels in `clause`}, {(binding, class): constraint}."""
        known = {}
        for b in set(a.binding.values()):
            known.setdefault(b, set()).update(self.table_classes(a.tables.get(b, set())))
        for b, cls in typed_facts:
            known.setdefault(b, set()).add(cls)
        implied, used, changed = {}, {}, True
        while changed:
            changed = False
            for src, rel, dst, _, _ in clause.edges:
                for c in self.c.implied_endpoint(rel, known.get(src, set()) | implied.get(src, set())):
                    if c.dst not in implied.setdefault(dst, set()):
                        implied[dst].add(c.dst)
                        used[(dst, c.dst)] = (c, src)
                        changed = True
        return implied, used

    def _typed_facts(self, a, clause, spans):
        """[(binding, typed class)] stated by the conjuncts `spans` of the clause."""
        by_var = {var: a.binding[pos] for pos, var, _ in clause.nodes if var}
        facts = []
        for span in spans:
            eq = a.equality(span)
            if eq is None or eq[0] not in by_var:
                continue
            b = by_var[eq[0]]
            tables = a.tables.get(b, set())
            if len(tables) != 1:
                continue
            cls = self.typed_class(next(iter(tables)), eq[1], eq[2])
            if cls is not None:
                facts.append((b, cls))
        return facts

    def _chain(self, used, binding, cls):
        """Constraints the implication of `cls` for `binding` relied on."""
        out, todo, seen = set(), [(binding, cls)], set()
        while todo:
            key = todo.pop()
            if key in seen or key not in used:
                continue          # known (tables or a kept conjunct), not implied
            seen.add(key)
            c, src = used[key]
            out.add(c)
            todo.append((src, c.src))
        return out

    # ---- R1
    def implied_type_conjuncts(self, tokens, premises):
        a = _Analysis(tokens)
        drops = []
        for clause in a.clauses:
            if clause.where is None:
                continue
            spans = a.conjuncts(clause.where)
            by_var = {var: a.binding[pos] for pos, var, _ in clause.nodes if var}
            kept = list(spans)
            for span in spans:
                eq = a.equality(span)
                if eq is None or eq[0] not in by_var:
                    continue
                b = by_var[eq[0]]
                tables = a.tables.get(b, set())
                if len(tables) != 1:
                    continue
                cls = self.typed_class(next(iter(tables)), eq[1], eq[2])
                if cls is None:
                    continue
                others = [s for s in kept if s != span]
                implied, used = self._implied(a, clause, self._typed_facts(a, clause, others))
                if cls in implied.get(b, ()):
                    kept = others
                    drops.append((clause.where, span))
                    premises.update(c.describe() for c in self._chain(used, b, cls))
        if not drops:
            return tokens
        return self._remove_conjuncts(tokens, a, drops)

    @staticmethod
    def _remove_conjuncts(tokens, a, drops):
        """Drop conjunct spans; an emptied WHERE disappears."""
        by_where = {}
        for where, span in drops:
            by_where.setdefault(where, []).append(span)
        out = list(tokens)
        for where, spans in sorted(by_where.items(), reverse=True):
            lo, hi = where
            keep = [s for s in a.conjuncts(where) if s not in spans]
            text = ' AND '.join('(' + render(tokens[x:y]) + ')' if any(tokens[k].upper in ('OR', 'XOR')
                                                                        for k in range(x, y))
                                else render(tokens[x:y]) for x, y in keep)
            if keep:
                out[lo:hi] = tokenize(' ' + text + ' ')
            else:
                out[lo - 1:hi] = tokenize(' ')       # also the WHERE keyword
        return tokenize(render(out))

    # ---- T1 / T2
    def key_identities(self, tokens, premises):
        a = _Analysis(tokens)
        taken = {_ident(t) for t in tokens if t.kind in ('ident', 'quoted')}

        def fresh():
            n = 1
            while f'cico_key{n}' in taken:
                n += 1
            taken.add(f'cico_key{n}')
            return f'cico_key{n}'
        uses = {}
        for k, t in enumerate(tokens):
            if t.kind in ('ident', 'quoted') and not (k and tokens[k - 1].text in ('.', ':')) \
                    and not (k + 1 < len(tokens) and tokens[k + 1].text == ':' and k and tokens[k - 1].text in ('{', ',')):
                uses.setdefault(_ident(t), []).append(k)
        edits, conjuncts, patterns = [], {}, {}
        for clause in a.clauses:
            spans = a.conjuncts(clause.where) if clause.where else []
            endpoints = {p for e in clause.edges for p in (e[3], e[4])}
            for pos, var, labels in clause.nodes:
                if pos not in endpoints or var is None:
                    continue
                end = closing(tokens, pos)
                body = tokens[pos + 1:end]
                b = a.binding[pos]
                if not clause.optional and len(labels) == 1 and len(body) == 8 and body[3].text == '{' \
                        and body[5].text == ':' and body[7].text == '}' and body[6].kind in ('param', 'string', 'number') \
                        and a.first[b] == pos:
                    edit = self._t1(tokens, a, clause, spans, pos, end, var, labels[0], body, uses, fresh, premises)
                    if edit:
                        edits.extend(edit[0])
                        for where_key, text in edit[1]:
                            conjuncts.setdefault(where_key, []).append(text)
                        for where_key, text in edit[2]:
                            patterns.setdefault(where_key, []).append(text)
                    continue
                if labels or len(body) != 1 or a.clause_of_binding[b] >= clause.index:
                    continue
                edit = self._t2(a, clause, pos, end, var, b, fresh, premises)
                if edit:
                    edits.append(edit[0])
                    conjuncts.setdefault(self._where_key(clause), []).append(edit[1])
        if not edits and not conjuncts:
            return tokens
        for (pattern_end, where), texts in conjuncts.items():
            text = ' AND '.join(f'({x})' for x in texts)
            if where:
                edits.append((where[0], where[0], ' ('))
                edits.append((where[1], where[1], f') AND {text} '))
            else:
                edits.append((pattern_end, pattern_end, f' WHERE {text} '))
        for (pattern_end, _), texts in patterns.items():
            edits.append((pattern_end, pattern_end, ', ' + ', '.join(texts) + ' '))
        out = list(tokens)
        for lo, hi, text in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
            out[lo:hi] = tokenize(' ' + text)
        return tokenize(render(out))

    @staticmethod
    def _where_key(clause):
        return clause.pattern[1], clause.where

    def _t1(self, tokens, a, clause, spans, pos, end, var, label, body, uses, fresh, premises):
        table, prop, value = label.upper(), _ident(body[4]), body[6].text
        typed_span, cls = None, None
        for span in spans:
            eq = a.equality(span)
            if eq and eq[0] == var:
                found = self.typed_class(table, eq[1], eq[2])
                if found and self.class_key(found, prop):
                    typed_span, cls = span, found
                    break
        if cls is None:
            cls = next((n for n, ts in self.untyped.items() if ts == {table} and self.class_key(n, prop)), None)
        if cls is None:
            return None
        if len(uses.get(var, [])) < 2 + (typed_span is not None):
            return None          # v is used nowhere else: the chain already ends in a hidden node
        key = self.class_key(cls, prop)
        name = fresh()
        guard = ''
        edits, extra_conj, extra_pat = [(pos, end + 1, f'({name}:{_q(label)} {{{_q(prop)}: {value}}})')], [], []
        if typed_span is not None:
            spec = self.typed[cls]
            guard = f"{_q(var)}.{_q(spec['property'])} = '{spec['value']}'"
            v_at = typed_span[0] if tokens[typed_span[0] + 1].text == '.' else typed_span[0] + 2
            edits.append((v_at, v_at + 1, _q(name)))
        spec = f'{_q(label)} {{{_q(prop)}: {value}}}'
        deferred = self._deferral(tokens, a, clause, var, pos, typed_span, spec, guard, uses)
        if deferred is not None:
            edits.append(deferred)
        else:
            extra_pat.append((self._where_key(clause), f'({_q(var)}:{spec})'))
            if guard:
                extra_conj.append((self._where_key(clause), guard))
        premises.update(c.describe() for c in key[1:])
        return edits, extra_conj, extra_pat

    def _deferral(self, tokens, a, clause, var, at, typed_span, spec, guard, uses):
        """(lo, hi, text) rebinding v at the WITH that ends its part, or None."""
        from .queryparts import PROJECTION_TAIL
        after = [(i, w) for i, w in a.bounds if i > at]
        with_at = next((i for i, w in after if w in ('WITH', 'RETURN')), None)
        if with_at is None or tokens[with_at].upper != 'WITH':
            return None
        part_start = max([i for i, w in a.bounds if i <= at and w in ('WITH', 'RETURN')] or [-1]) + 1
        i = with_at + 1
        distinct = i < len(tokens) and tokens[i].upper == 'DISTINCT'
        i += distinct
        items, current, depth = [], [], 0
        while i < len(tokens):
            t = tokens[i]
            depth += t.text in ('(', '[', '{')
            depth -= t.text in (')', ']', '}')
            if depth == 0 and t.kind == 'ident' and (t.upper in PROJECTION_TAIL or t.upper in CLAUSE_STARTS):
                break
            if depth == 0 and t.text == ',':
                items.append(current)
                current = []
            else:
                current.append(t)
            i += 1
        items.append(current)
        if i < len(tokens) and tokens[i].upper in ('ORDER', 'SKIP', 'LIMIT', 'WHERE'):
            return None
        bare = [it for it in items if len(it) == 1 and it[0].kind in ('ident', 'quoted') and _ident(it[0]) == var]
        if len(bare) != 1:
            return None
        bare_at = next(k for k in range(with_at, i) if tokens[k] is bare[0][0])
        allowed = {at + 1, bare_at}
        if typed_span is not None:
            allowed.update(k for k in range(*typed_span) if tokens[k].kind in ('ident', 'quoted')
                           and _ident(tokens[k]) == var)
        mine = {k for k in uses.get(var, []) if part_start <= k < i}
        if mine != allowed:
            return None
        rest = [render(it).strip() for it in items if it is not bare[0]]
        if not rest:
            return None
        head = 'WITH ' + ('DISTINCT ' if distinct else '')
        text = (head + ', '.join(rest) + f' MATCH ({_q(var)}:{spec}) ' + (f'WHERE {guard} ' if guard else '')
                + head + ', '.join(render(it).strip() for it in items) + ' ')
        return with_at, i, text

    def _t2(self, a, clause, pos, end, var, b, fresh, premises):
        spans = a.conjuncts(clause.where) if clause.where else []
        implied, used = self._implied(a, clause, self._typed_facts(a, clause, spans))
        for src, rel, dst, _, dst_at in clause.edges:
            if dst_at != pos:
                continue
            for cls in sorted(implied.get(b, ())):
                key = self.class_key(cls)
                if key is None:
                    continue
                prop, unique, required = key
                tables = a.tables.get(b, set())
                if cls in self.typed:
                    spec = self.typed[cls]
                    if tables != {spec['table']}:
                        continue
                    table = spec['table']
                    guard = f" AND {_q(var)}.{_q(spec['property'])} = '{spec['value']}'"
                else:
                    own = self.untyped[cls]
                    if len(own) != 1 or not tables or not tables <= own:
                        continue
                    table, guard = next(iter(own)), ''
                name = fresh()
                premises.update(x.describe() for x in (unique, required, *self._chain(used, b, cls)))
                return (pos, end + 1, f'({name}:{_q(table)})'), f'{name}.{_q(prop)} = {_q(var)}.{_q(prop)}{guard}'
        return None

    def optional_carrier_edges(self, tokens, premises):
        """E1 above.  Adds `, (s)-[:R]->(t)` to OPTIONAL MATCH patterns."""
        unique = {c.rel.upper(): c for c in self.c.items if c.kind == 'unique_edge' and c.rel}
        if not unique:
            return tokens
        for _ in range(16):                                   # one insertion per pass
            a = _Analysis(tokens)
            insertion = None
            for clause in a.clauses:
                if not clause.optional or clause.edges is None:
                    continue
                bound = {a.binding[pos] for pos, _, _ in clause.nodes if pos in a.binding}
                present = {(e[0], e[1], e[2]) for e in clause.edges}
                for earlier in a.clauses[:clause.index]:
                    if earlier.optional or earlier.edges is None:
                        continue
                    for src, rel, dst, _, _ in earlier.edges:
                        if rel not in unique or src == dst or src not in bound or dst not in bound \
                                or (src, rel, dst) in present:
                            continue
                        s_var, t_var = a.var_of.get(src), a.var_of.get(dst)
                        if not s_var or not t_var:
                            continue
                        insertion = (clause.pattern[1], s_var, unique[rel].rel, t_var, unique[rel])
                        break
                    if insertion:
                        break
                if insertion:
                    break
            if insertion is None:
                return tokens
            at, s_var, rel, t_var, constraint = insertion
            added = tokenize(f', ({_q(s_var)})-[:{_q(rel)}]->({_q(t_var)})')
            tokens = tokens[:at] + added + tokens[at:]
            premises.add(constraint.describe())
        return tokens

    def relax(self, tokens, premises):
        """T1/T2 then R1 (as the Neo4j frontend: key identities first), then E1."""
        if not self.c or not self.c.items:
            return tokens
        try:
            tokens = self.implied_type_conjuncts(self.key_identities(tokens, premises), premises)
        except Unsupported:
            pass
        try:
            return self.optional_carrier_edges(tokens, premises)
        except Unsupported:
            return tokens

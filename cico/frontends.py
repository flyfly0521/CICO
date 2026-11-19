"""Input frontends: translate a source system's Cypher into the internal
(NeuG) dialect the engine binds.  The identity frontend serves NeuG and Kuzu,
whose grammar and LDBC schema match NeuG's.

Hooks used by shared rewrite rules (profiles.RULES):
  rel_tables(name)      relationship name in the constraints' vocabulary ->
                        internal relationship tables        (edge_carriers)
  relax, key_constants  constraint retry on internal Cypher (relax.py)
"""


class IdentityFrontend:
    name = 'identity'

    def __init__(self):
        self.relax = False      # apply constraint-justified simplifications (retry stage)
        self.premises = set()   # constraints the last translations relied on
        self.constraints = None
        self._relaxer = None

    def use_constraints(self, constraints):
        """Declared constraints of the dataset, in internal names."""
        from .relax import InternalRelax
        self.constraints = constraints
        self._relaxer = InternalRelax(constraints) if constraints is not None and constraints.classes else None

    def to_internal(self, cypher):
        if not self.relax:
            return cypher
        from .neo4j import render, tokenize
        from .statements import strip_comments
        return render(self._relaxed(tokenize(strip_comments(cypher).strip().rstrip(';'))))

    def _relaxed(self, tokens):
        if self.relax and self._relaxer is not None:
            return self._relaxer.relax(tokens, self.premises)
        return tokens

    def key_constants(self, cypher):
        """T3 (relax.py) with keys of classes that are exactly one table."""
        if self._relaxer is None:
            return None, set()
        from .relax import key_constants
        return key_constants(cypher, self._relaxer.key_of_table)

    def rel_tables(self, name):
        return [name.upper()]

    def parameter_types(self, types):
        return dict(types or {})

    def fill_to_internal(self, fill_text):
        return fill_text

    def ddl_to_internal(self, statement):
        return statement

    def table_to_internal(self, name):
        return name


class KuzuFrontend(IdentityFrontend):
    """Kuzu Cypher -> internal: same grammar family as NeuG; only spellings
    the internal binder lacks are normalized (see normalize.py)."""
    name = 'kuzu'

    def to_internal(self, cypher):
        from .neo4j import render, tokenize
        from .normalize import entity_id_comparisons, rewrite_coalesce
        from .statements import strip_comments
        tokens = self._relaxed(tokenize(strip_comments(cypher).strip().rstrip(';')))
        return render(entity_id_comparisons(rewrite_coalesce(tokens)))

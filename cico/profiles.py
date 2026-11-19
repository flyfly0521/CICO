"""Dialect declarations: how the middleware talks to one target system.

The middleware is started for ONE target dialect (``IndexMiddleware(...,
dialect='kuzu')``, CLI ``--dialect kuzu``).  Ordinary queries are read in
that dialect and rewrites are printed back in it.  FILL statements are not:
their source query is always read with NeuG semantics, whatever the target
(see README / docs/dialects.md).

A declaration bundles

  frontend   target Cypher -> internal (NeuG) Cypher        (input conversion)
  dialect    bound IR -> target Cypher                        (output printing)
  semantics  how the target's matching differs from NeuG's   (Semantics below)
  disabled   rules switched off explicitly (debugging); optional

Rewrite rules are shared by every dialect.  Each rule (RULES below) states
what it needs: hooks on the frontend/dialect, a semantic premise, declared
constraints.  A rule is enabled for a declaration exactly when the
declaration provides all of them, so a new system only declares its
semantics and implements hooks; `Profile.rule_status()` explains each
decision.  A new system is added by writing these parts and calling
``register(name, factory)``; see docs/dialects.md for the checklist.
Nothing in the core pipeline tests a dialect's *name*.
"""
from dataclasses import dataclass, field

from .dialects import get_dialect
from .frontends import IdentityFrontend, KuzuFrontend


@dataclass(frozen=True)
class Rule:
    """A rewrite rule and what a declaration must provide to enable it."""
    name: str
    summary: str
    hooks: tuple = ()             # 'frontend.<attr>' / 'dialect.<attr>' the rule calls
    applies: object = None        # Semantics -> bool: the rule is meaningful (and sound) here
    premise: str = ''             # human-readable form of `applies`
    constraints: bool = False     # needs declared constraints of the dataset

    def unmet(self, profile):
        """None when `profile` can use the rule, else the reason it cannot."""
        if self.applies is not None and not self.applies(profile.semantics):
            return f'semantics: needs {self.premise}'
        missing = [h for h in self.hooks if not _has_hook(profile, h)]
        if missing:
            return 'missing hooks: ' + ', '.join(missing)
        if self.constraints and not getattr(profile.constraints, 'items', None):
            return 'no declared constraints loaded'
        return None


def _has_hook(profile, hook):
    owner, _, attr = hook.partition('.')
    return hasattr(getattr(profile, owner), attr)


# Shared rewrite rules.  Their proofs are stated on NeuG (internal) semantics;
# the hooks print or analyse the dialect-specific parts.
RULES = {r.name: r for r in (
    Rule('scalar_regions',
         'correlated size()/COUNT{}/EXISTS{} regions rewritten locally (scalar_subqueries.py); '
         'the probe goes through the ordinary frontend, printer and guard'),
    Rule('edge_carriers',
         'OPTIONAL existence part read from a relationship count carrier (edge_carriers.py)',
         hooks=('dialect.edge_carrier_test', 'frontend.rel_tables')),
    Rule('constraint_retry',
         'retry with declared-constraint simplifications: implied type classes, key identities, '
         'constant keys (frontend relax stage + key_constants)',
         hooks=('frontend.relax', 'frontend.key_constants'), constraints=True),
    Rule('path_carriers',
         'relationship carrier for a variable-length path whose relationship isomorphism blocks the '
         'ordinary rewrite (path_carriers.py)',
         hooks=('frontend.clause_table', 'frontend.part_facts', 'frontend.contradiction'),
         applies=lambda s: s.relationship_uniqueness == 'isomorphism',
         premise="relationship_uniqueness == 'isomorphism' (under homomorphism the ordinary "
                 "QueryPart rewrite already applies)"),
    Rule('distinct_partitions',
         'DISTINCT branches of CALL { } bodies read through a functional path (distinct_partitions.py)',
         hooks=('frontend.rels', 'frontend.rel_pairs', 'frontend.constraints')),
)}


@dataclass(frozen=True)
class Semantics:
    # Within one MATCH clause: may two relationship variables bind the same
    # edge?  NeuG/Kuzu: yes (homomorphism).  Neo4j: no (isomorphism); the
    # frontend must add or prove the inequalities.
    relationship_uniqueness: str = 'homomorphism'
    # Variable-length relationships: NeuG walks; Neo4j trails.
    variable_length: str = 'walk'
    # Upper bound the target applies to an open `*l..` (Kuzu: 30), else None.
    variable_length_cap: int = None
    # How a typed node class (City = PLACE with type 'City') is written:
    # 'property' (a predicate on the table) or 'label' (Neo4j dynamic label).
    type_classes: str = 'property'
    # Opaque CALL procedures with declared output type contracts.
    procedures: bool = False
    # Regex of target-native statements (e.g. Neo4j CREATE INDEX) that the
    # middleware never interprets: returned unchanged with route='target'.
    native_ddl: str = None


@dataclass
class Profile:
    """A dialect declaration (historical name: profile)."""
    name: str
    dialect: object
    frontend: object
    semantics: Semantics = field(default_factory=Semantics)
    disabled: frozenset = frozenset()   # rules switched off on purpose (debugging, bisecting)
    description: str = ''
    constraints: object = None     # declared constraints of the dataset (constraints.py), or None

    def __post_init__(self):
        unknown = set(self.disabled) - set(RULES)
        if unknown:
            raise ValueError(f'{self.name}: unknown rules {sorted(unknown)}; known: {sorted(RULES)}')
        self.disabled = frozenset(self.disabled)
        if self.constraints is None:
            self.constraints = getattr(self.frontend, 'constraints', None)

    def enabled(self, rule):
        """Derived from the rule's requirements, evaluated now: constraints
        may be attached after the declaration is built."""
        return rule not in self.disabled and RULES[rule].unmet(self) is None

    @property
    def rules(self):
        return frozenset(r for r in RULES if self.enabled(r))

    def rule_status(self):
        """{rule: 'enabled' | 'disabled' | reason it is unavailable}."""
        return {r: 'disabled' if r in self.disabled else (RULES[r].unmet(self) or 'enabled') for r in RULES}


DialectDeclaration = Profile


def _neug(constraints=None):
    return Profile('neug', get_dialect('neug'), IdentityFrontend(),
                   description='NeuG Cypher (the internal dialect)', constraints=_load(constraints))


def _kuzu(constraints=None):
    return Profile('kuzu', get_dialect('kuzu'), KuzuFrontend(),
                   semantics=Semantics(variable_length_cap=30),
                   description='Kuzu Cypher (NeuG grammar family, same LDBC table names)',
                   constraints=_load(constraints))


def _neo4j(mapping=None, constraints=None):
    from .neo4j import neo4j_profile
    return neo4j_profile(mapping, constraints=constraints)


def _load(constraints):
    if constraints is None or not isinstance(constraints, (str, bytes)) and not hasattr(constraints, '__fspath__'):
        return constraints
    from .constraints import load_constraints
    return load_constraints(constraints)


PROFILES = {'neug': _neug, 'kuzu': _kuzu, 'neo4j': _neo4j}


def register(name, factory):
    """Add a dialect declaration: factory(**options) -> Profile."""
    PROFILES[name] = factory


def get_profile(name, *, disabled=None, **options):
    """The declaration `name`; `disabled` switches rules off explicitly."""
    try:
        factory = PROFILES[name]
    except KeyError:
        raise ValueError(f'Unknown dialect {name!r}; known: {sorted(PROFILES)}') from None
    profile = factory(**{k: v for k, v in options.items() if v is not None})
    if disabled:
        unknown = set(disabled) - set(RULES)
        if unknown:
            raise ValueError(f'unknown rules {sorted(unknown)}; known: {sorted(RULES)}')
        profile.disabled = frozenset(profile.disabled) | frozenset(disabled)
    return profile


get_declaration = get_profile

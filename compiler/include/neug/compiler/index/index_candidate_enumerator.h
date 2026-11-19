/**
 * Copyright 2026 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "neug/utils/api.h"

namespace neug::index {

// Binding identities, rather than printable aliases, identify variables.
enum class ExpressionKind {
  Literal,
  Variable,
  Property,
  Parameter,
  Call,
  Aggregate,
  // An unchanged pattern EXISTS predicate. name retains its bound source text;
  // children explicitly list the correlated bindings required online.
  ResidualPredicate,
  // A typed, single-edge EXISTS with two correlated graph endpoints. Children
  // carry the endpoint bindings and catalog types, so canonical renaming is
  // structural instead of editing opaque subquery text.
  PatternExists
};
struct NEUG_API Expression {
  ExpressionKind kind = ExpressionKind::Literal;
  std::string name;
  std::string binding;
  std::string type = "BOOL";
  std::vector<Expression> children;
  // For literals this is the NULL-value flag; elsewhere it is nullability.
  bool nullable = true;
  bool distinct = false;
  bool deterministic = true;
  bool safeToPrecompute = true;

  static Expression literal(std::string value, std::string type = "INT64");
  static Expression variable(std::string binding, std::string type = "NODE");
  static Expression property(std::string binding, std::string name,
                             std::string type = "INT64", bool nullable = true);
  static Expression parameter(std::string name, std::string type = "INT64");
  static Expression call(std::string name, std::vector<Expression> children,
                         std::string type = "INT64", bool safe = true);
  static Expression aggregate(std::string name,
                              std::vector<Expression> children = {},
                              std::string type = "INT64",
                              bool distinct = false);
  std::string structuralKey() const;
  std::string toCypher() const;
};

struct Node {
  std::string binding;
  std::vector<std::string> labels;
  std::string primaryKey = "id";
  std::string primaryKeyType = "INT64";
};
struct Relationship {
  std::string binding;
  size_t source = 0;
  size_t destination = 0;
  std::vector<std::string> types;
  bool directed = true;
  bool recursive = false;
  uint64_t lowerBound = 1;
  uint64_t upperBound = 1;
  // Edge uniqueness is enforced inside a MATCH clause, not across clauses.
  size_t matchGroup = 0;
  std::string pathSemantics = "WALK";
};
// Ordered clause boundaries are necessary for left-optional matching.
// Predicates here belong to the match condition, before NULL extension.
struct MatchClause {
  size_t position = 0;
  bool optional = false;
  std::vector<size_t> nodes;
  std::vector<size_t> relationships;
  std::vector<Expression> predicates;
  std::set<std::string> boundBefore;
  std::set<std::string> introduced;
};
// A mandatory relationship already proved by the upstream input. Only retained
// endpoint bindings may be named; optional/nullable matches are not facts.
struct CarrierDomain {
  std::string source;
  std::string destination;
  std::vector<std::string> types;
  bool directed = true;
};
struct QueryPart {
  std::string source;
  std::map<std::string, std::string> bindingNames;
  std::vector<Node> nodes;
  std::vector<Relationship> relationships;
  std::vector<MatchClause> clauses;
  std::vector<Expression> predicates;
  std::vector<Expression> groupBy;
  std::vector<Expression> aggregates;
  std::vector<Expression> outputs;
  std::vector<Expression> postExpressions;
  std::set<std::string> inputBindings;
  std::vector<CarrierDomain> carrierDomains;
  bool singletonEmptyInput = true;
  bool distinct = false;
  bool forbidEdgeReuse = true;
  // Unsupported clauses must be reported, never silently omitted.
  std::string unsupportedReason;
};

struct RelationshipConstraint {
  std::vector<std::string> types;
  bool uniqueEndpoints = false;
  bool atMostOneOutgoing = false;
  bool atMostOneIncoming = false;
};
struct RecursiveConstraint {
  std::vector<std::string> sourceLabels;
  std::vector<std::string> relationshipTypes;
  std::vector<std::string> destinationLabels;
  uint64_t lowerBound = 1;
  uint64_t upperBound = std::numeric_limits<uint64_t>::max();
  bool finiteMatches = false;
  bool uniqueDestination = false;
  bool uniqueSource = false;
  bool uniquePath = false;
  std::string pathSemantics = "WALK";
};
struct Constraints {
  std::vector<RelationshipConstraint> relationships;
  std::vector<RecursiveConstraint> recursivePaths;
  // The caller guarantees exact, non-overflowing arithmetic on every
  // admissible materialization input. Required for arithmetic precomputation
  // and SUM/AVG regrouping; integer types or samples do not prove it.
  bool exactArithmetic = false;
  // Floating MIN/MAX must be invariant under regrouping/reordering, including
  // NaN and observable signed-zero behavior; exact arithmetic does not prove
  // it.
  bool orderedExtrema = false;
  // Optional declarations for additional scalar functions. Unknown functions
  // are rejected by the bound-query adapter. Totality is a separate assertion.
  std::set<std::string> deterministicFunctions;
  std::set<std::string> totalFunctions;
  // Additional catalog identity contracts. A non-primary grouping property
  // identifies its graph node only with BOTH uniqueness and non-nullability.
  std::map<std::string, std::set<std::string>> uniqueProperties;
  std::map<std::string, std::set<std::string>> nonNullProperties;
};

enum class CarrierKind {
  VertexProperties,
  ExistingEdgeProperties,
  DerivedEdge
};
struct Carrier {
  CarrierKind kind = CarrierKind::VertexProperties;
  std::vector<std::string> sourceLabels;
  std::vector<std::string> destinationLabels;
  std::string relationshipType;
  bool coversAllKeys = false;
  // At most one physical row per typed key; for an existing edge this means
  // at most one matching directed relationship per endpoint pair. Its source
  // computation can therefore join the carrier domain without multiplying rows.
  bool uniqueKey = false;
  bool preservesNull = false;
  bool preservesGroupPresence = false;
  bool resolvesDestinationId = false;
  // Empty permits all supported scalar types.
  std::set<std::string> scalarTypes;
  // Physical target identity, excluding declarations used to prove legality.
  NEUG_API std::string structuralKey() const;
};
struct Limits {
  size_t path = 3;
  size_t columns = 3;
  // Zero leaves the local candidate count unlimited.
  size_t local = 128;
  // Maximum nonoverlapping source reads in one Usage. Repeated reads of one
  // physical definition consume separate reads but only one selection item.
  size_t views = 1;
  // Maximum equivalent graph labelings examined per definition. Zero requests
  // exhaustive normalization. A capped definition keeps its safe legacy key;
  // enumeration reports truncation instead of claiming complete sharing.
  size_t canonicalPermutations = 4096;
  // Whole-call budgets also bound rejected candidates and failed combinations.
  // Additional physical carrier combinations share the combination budget.
  // Zero means unlimited. Partial results are always flagged as truncated.
  size_t maxGenerated = 100000;
  size_t maxCombinations = 100000;
  // Distinct parameter-free predicate subsets per local computation. Visit the
  // complete safe conjunction first, then no filter, then smaller subsets.
  // Zero opts into exhaustive enumeration; omitted subsets flag truncation.
  size_t predicateSubsets = 64;
  // Complete connected groups also obey path's relationship-atom budget.
  bool complete = true;
  // Explicitly enable exact multi-stage prefix computations.
  bool prefixes = false;
  // Bound unique names whose recursive bounds originate in runtime parameters.
  // They stay in the query; fixed bounds on all other relationships remain legal.
  std::set<std::string> runtimeBoundRelationships;
};

enum class CandidateKind {
  Projection,
  Aggregate,
  CompleteAggregate,
  Existence,
  EndpointMapping,
  Computation
};
enum class ColumnKind {
  Projection,
  CountRows,
  CountNonNull,
  Sum,
  Average,
  DestinationId,
  Minimum,
  Maximum,
  CountDistinctNonNull
};
struct Column {
  ColumnKind kind = ColumnKind::Projection;
  std::optional<Expression> expression;
  std::string type;
  // DISTINCT is part of a stored aggregate's identity. Such scalars can only
  // replace a complete group; local DISTINCT counts must never be summed.
  bool distinct = false;
  std::string structuralKey() const;
};
struct NEUG_API Candidate {
  CandidateKind kind = CandidateKind::Aggregate;
  std::vector<Node> nodes;
  std::vector<Relationship> relationships;
  std::vector<std::string> keys;
  std::vector<Expression> predicates;
  std::vector<Column> columns;
  // Output definitions always select one normalized storage declaration.
  // Intermediate local candidates have no storage until their use is proved.
  std::optional<Carrier> storage;
  // The underlying local statistic before restriction to a physical carrier
  // domain. Use toCypher(), not this key, to cache concrete source bindings.
  std::string logicalDefinitionKey;
  std::string definitionKey;
  // An exact, explicitly composed source supplied by the prefix enumerator.
  // Ordinary local definitions continue to render their structured pattern.
  std::string sourceQuery;
  // A source computation, not a CREATE/FILL command. Calling this API has no
  // materialization side effects. Key expressions retain typed graph objects.
  std::string toCypher() const;
};
struct Recovery {
  size_t aggregate = 0;
  // R9min/R9max apply the same extremum to local summaries, preserving NULLs.
  // R2distinct takes MAX of repeated identical local distinct counts; an
  // OPTIONAL NULL-extended operand contributes zero, never a missing group.
  // R2partition sums disjoint local distinct sets after deduplicating repeated
  // (final-group identities, local-key identities) witnesses. Functional graph
  // constraints must prove that the counted entity fixes its local partition.
  std::string rule;
  std::vector<std::string> requiredColumns;
  std::optional<Expression> outsideExpression;
  // Operand on the one NULL-extended row, before the original aggregate.
  // COUNT(*) uses literal 1. COUNT(e) counts this operand only if non-NULL.
  std::optional<Expression> noMatchOperand;
  std::vector<size_t> reads;
  std::vector<std::string> deduplicateBindings;
};
struct IndexRead {
  bool optional = false;
  size_t clause = 0;
  std::vector<size_t> relationships;
  std::vector<Expression> matchPredicates;
  std::set<std::string> nullBindings;
  // Source binding -> canonical definition binding; source column -> ordinal.
  std::map<std::string, std::string> bindings;
  std::map<std::string, size_t> columns;
  std::map<size_t, Expression> noMatchProjections;
  std::vector<size_t> carriers;
};
struct Usage {
  std::string source;
  std::vector<std::string> definitions;
  std::vector<size_t> remainingRelationships;
  std::vector<Recovery> recoveries;
  std::vector<Expression> remainingFilters;
  // Endpoint comparisons restoring MATCH relationship uniqueness after an
  // identity-preserving lookup. Each predicate stays at its original clause.
  std::vector<std::pair<size_t, Expression>> edgeIdentityPredicates;
  // Same order as definitions. Optional reads retain their original position.
  std::vector<IndexRead> reads;
  std::string scope = "local";
  size_t replacedThroughPart = 0;
  std::string residualQuery;
};
struct Diagnostic {
  std::string source;
  std::string reason;
};
struct EnumerationResult {
  std::vector<Candidate> candidates;
  // Optional witnesses; never part of a definition's identity.
  std::vector<Usage> uses;
  std::vector<Diagnostic> diagnostics;
  size_t generated = 0;
  size_t combinations = 0;
  bool truncated = false;
  std::map<std::string, size_t> rejections;
  // Proven-impossible path/key contexts or combination prefixes skipped before
  // full validation. Counts describe pruning decisions, not skipped subsets.
  std::map<std::string, size_t> pruned;
};

NEUG_API EnumerationResult enumerateCandidates(
    const std::vector<QueryPart>& parts, const Constraints& constraints,
    const std::vector<Carrier>& carriers, const Limits& limits = {},
    bool includeUses = false);

}  // namespace neug::index

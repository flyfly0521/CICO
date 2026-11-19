#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace neug {
namespace catalog { class FillIndexAuxInfo; }
namespace common { class PatternIR; }
namespace main { class ClientContext; }
namespace binder {
class Expression;
class BoundMatchClause;
class NormalizedQueryPart;
class NormalizedSingleQuery;

// Facts already true for every input row of this MATCH. Only unchanged node
// identities carried through each preceding WITH can supply these facts.
// They are proof inputs, never predicates to append to an OPTIONAL clause.
std::vector<std::shared_ptr<Expression>> guaranteedIncomingNodePredicates(
    const BoundMatchClause& match, const NormalizedQueryPart& part,
    const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<std::string>& retainedNodes);

// Additional correlation proof for the current mandatory MATCH only. This
// does not enlarge the materialized-key set or restore aggregated identities.
// A surviving endpoint must be an ordered FILL key or an unmatched node on
// a relationship occurrence that relIndexRewrite keeps in this same graph.
// The optional format exception requires a separate rebind proof of the exact
// single-stage, typed, DISTINCT endpoint-only FILL source. Defaults remain
// conservative for all legacy/flattened compatibility metadata.
bool provesResidualMatchExistsInputsSurvive(
    const Expression& expression, const catalog::FillIndexAuxInfo& info,
    const common::PatternIR& matched, const common::PatternIR& query,
    const BoundMatchClause& match,
    bool provedSingleStageEndpointSetSource = false);

// Shared boundary proof for complete mandatory/optional per-pair counts.
// Every residual value must be independent of removed source identities;
// identities passed in by an earlier WITH remain required inputs.
bool completeKeyGroupInputsAreClosed(
    const catalog::FillIndexAuxInfo& info, const common::PatternIR& matched,
    const common::PatternIR& query, const BoundMatchClause& match,
    const NormalizedQueryPart& part, const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<std::string>* retainedNodes = nullptr,
    const std::unordered_set<std::string>* retainedRels = nullptr,
    const std::unordered_set<const Expression*>* coveredExpressions = nullptr,
    bool provedSingleStageEndpointSetSource = false);

// Prove that a derived relation supplies complete DISTINCT counts for a query
// group retaining both materialized keys, even if outer joins repeat the pair.
bool provesCompleteKeyGroupDistinctCounts(
    const catalog::FillIndexAuxInfo& info, const common::PatternIR& matched,
    const common::PatternIR& query, const BoundMatchClause& match,
    const NormalizedQueryPart& part, main::ClientContext& context,
    const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<const Expression*>& incomingExpressions);
}
}

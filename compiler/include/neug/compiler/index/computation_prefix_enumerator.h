#pragma once

#include "neug/compiler/index/index_candidate_enumerator.h"

namespace neug::binder {
class BoundRegularQuery;
}
namespace neug::index {
// Explicit catalog contracts: a non-NULL property value identifies at most one
// vertex of this label. These are never inferred from workload samples.
using UniqueProperties = std::map<std::string, std::set<std::string>>;

// Enumerate exact, key-unique query prefixes. Earlier WITH/OPTIONAL/filter/order
// boundaries are retained verbatim; only the last projection is given a typed
// graph-key interface. A final global INT64 SUM of earlier COUNT outputs may
// also use a graph-key partition of its exact input prefix followed by SUM of
// those nonnegative partial sums (including the original empty-input result).
// Other aggregates are not distributed across stages. At the first
// boundary, a mandatory unique/non-NULL vertex-property equality to a parameter
// may be deferred when its vertex is retained as an additional grouping key;
// this produces one parameter-independent computation for all selector values.
// Simple parameter comparisons on an already retained grouping vertex may also
// remain online, including range predicates on its non-identity properties.
NEUG_API EnumerationResult enumerateComputationPrefixes(
    const std::string& cypher, const binder::BoundRegularQuery& bound,
    const std::vector<Carrier>& carriers, const Limits& limits,
    const UniqueProperties& uniqueProperties = {},
    const std::string& source = "query",
    const Constraints& constraints = {});
}  // namespace neug::index

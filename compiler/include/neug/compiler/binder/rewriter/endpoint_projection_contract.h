#pragma once

#include <string_view>

namespace neug::binder {

// Persisted in FillIndexAuxInfo::predicateTemplates. This is a row-multiplicity
// contract, not a source predicate: a fixed-hop derived relationship stores
// exactly the set of its two endpoint identities. Local substitution requires
// a duplicate-insensitive consumer and a closed boundary of retained keys.
inline constexpr std::string_view ENDPOINT_SET_CONTRACT =
    "@fill-rel-endpoint-set-v1";

// The source includes at least one recursive relationship. PatternIR does not
// store path bounds or semantics, so every use must rebind sourceQuery and
// prove all matched path contracts, then use a duplicate-insensitive consumer.
// Recursive physical labels retain their whole traversal domain, including
// endpoint partitions reachable only by a zero-hop path.
inline constexpr std::string_view RECURSIVE_ENDPOINT_CONTRACT =
    "@fill-rel-recursive-endpoints-v1";

}  // namespace neug::binder

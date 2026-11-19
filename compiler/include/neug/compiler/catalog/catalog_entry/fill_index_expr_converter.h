#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "neug/utils/api.h"

// Proto package `common` lives in the GLOBAL ::common namespace.
namespace common {
class Expression;
}  // namespace common

namespace neug {
namespace binder {
class Expression;
}

namespace catalog {

// Reserved record tag used by a finalized-aggregate expression for the
// persisted additive state (for example SUM(x) in 1 / (SUM(x) + 1)).
constexpr uint32_t kFillAggregateStateTag = 0x3FFFFFFFU;

// Converts the argument expression of a FILL aggregate column (or a scalar
// FILL column expression) from the binder expression tree into the serialized
// ::common::Expression AST (postfix token stream with braces; Variable tags
// are numeric pattern-node positions). The AST is consumed by
//   - execution::parse_expression for incremental maintenance evaluation, and
//   - the rewrite matcher for structural (alpha-equivalence) comparison.
//
// Variable identity: every graph-variable reference is mapped through
// `resolveVar` (raw or unique binder name -> pattern node position) and
// stored as a numeric tag id. Tags must use the same numbering on the DDL
// side (position in the stored PatternIR) and on the rewrite side (matchedIdx
// into the index PatternIR) so structural comparison converges.
//
// Throws BinderException on any unsupported node kind: FILL creation must
// fail fast rather than silently degrade to the legacy string-template
// heuristics (which mis-compute contributions for expressions outside the
// label()-OR-CASE subset).
std::shared_ptr<::common::Expression> ConvertFillIndexExprToAst(
    const binder::Expression& expr,
    const std::function<std::optional<uint32_t>(const std::string&)>&
        resolveVar);

// Converts a scalar expression containing exactly one aggregate. The
// aggregate node is replaced by kFillAggregateStateTag, so maintenance can
// evaluate the scalar over the newly updated, persisted aggregate state.
std::shared_ptr<::common::Expression> ConvertFillIndexFinalizerToAst(
    const binder::Expression& expr,
    const std::function<std::optional<uint32_t>(const std::string&)>&
        resolveVar);

}  // namespace catalog
}  // namespace neug

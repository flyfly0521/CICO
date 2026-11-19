#pragma once

#include <memory>
#include <vector>

#include "neug/generated/proto/plan/expr.pb.h"
#include "neug/utils/api.h"

namespace neug::binder {

// Proves TRUE(premises) => TRUE(conclusion) for conjunctions of signed integer
// comparisons. Unknown syntax/types, arithmetic and parameters return false;
// false is not a counterexample and must never authorize a rewrite.
NEUG_API bool provesFillIntegerPredicate(
    const std::vector<std::shared_ptr<::common::Expression>>& premises,
    const ::common::Expression& conclusion);

}  // namespace neug::binder

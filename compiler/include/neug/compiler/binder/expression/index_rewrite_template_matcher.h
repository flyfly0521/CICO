#pragma once

#include <memory>
#include <unordered_map>

#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/utils/api.h"

namespace neug {
namespace binder {

class Expression;
class ScalarFunctionExpression;
// Validates that any aggregate function(s) appearing in the query expression
// can be satisfied by the index's aggregate template(s).
//
// This is used by index rewrite validation: if the query contains aggregates,
// we require that each aggregate matches an aggregate template stored in the
// index metadata (e.g., rel-index aggregate nameTemplate).
class NEUG_API IndexRewriteTemplateMatcher {
 public:
  // Maps each matched AggregateFunctionExpression (by pointer identity) to the
  // position in `indexInfo.indexColumnInfos/targetColumns` that supplies the
  // value for rewriting.
  using AggRewriteTargetMap = std::unordered_map<const Expression*, size_t>;

  static bool validateAggregateTemplatesForIndexRewrite(
      const std::shared_ptr<Expression>& rootExpr,
      const catalog::FillIndexAuxInfo& indexInfo,
      const common::PatternIR& queryIR, const common::PatternIR& indexIR,
      AggRewriteTargetMap& outRewriteTargets);
  static bool validateTemplatesForIndexRewrite(
      const ScalarFunctionExpression* rootExpr,
      const catalog::FillIndexAuxInfo& indexInfo,
      const common::PatternIR& queryIR, const common::PatternIR& indexIR,
      AggRewriteTargetMap& outRewriteTargets);
};

}  // namespace binder
}  // namespace neug

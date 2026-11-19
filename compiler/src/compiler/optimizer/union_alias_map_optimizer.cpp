#pragma once
#include "neug/compiler/optimizer/union_alias_map_optimizer.h"
#include "neug/compiler/planner/operator/logical_alias_map.h"
#include "neug/compiler/planner/operator/logical_union.h"
#include "neug/compiler/planner/operator/schema.h"
namespace neug {
namespace optimizer {
void UnionAliasMapOptimizer::rewrite(planner::LogicalPlan* plan) {
  auto root = plan->getLastOperator();
  auto rootOpt = visitOperator(root);
  plan->setLastOperator(rootOpt);
}

std::shared_ptr<planner::LogicalOperator> UnionAliasMapOptimizer::visitOperator(
    const std::shared_ptr<planner::LogicalOperator>& op) {
  // bottom-up traversal
  for (auto i = 0u; i < op->getNumChildren(); ++i) {
    op->setChild(i, visitOperator(op->getChild(i)));
  }
  auto result = visitOperatorReplaceSwitch(op);
  // schema of each operator is unchanged
  // result->computeFlatSchema();
  return result;
}

std::shared_ptr<planner::LogicalOperator>
UnionAliasMapOptimizer::visitUnionReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
  auto& unionOp = op->cast<planner::LogicalUnion>();
  if (unionOp.getNumChildren() == 0) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "union op should have at least one subquery");
  }
  planner::Schema* schema = nullptr;
  if (unionOp.getPreQuery()) {
    if (unionOp.getNumChildren() == 1) {
      THROW_EXCEPTION_WITH_FILE_LINE(
          "union op should have at least one subquery");
    }
    schema = unionOp.getChild(1)->getSchema();
  } else {
    schema = unionOp.getChild(0)->getSchema();
  }
  NEUG_ASSERT(schema);
  binder::expression_vector schemaExprs = schema->getExpressionsInScope();
  const auto firstSubqueryIdx = unionOp.getPreQuery() ? 1u : 0u;
  for (auto i = firstSubqueryIdx; i < unionOp.getNumChildren(); i++) {
    auto subquery = unionOp.getChild(i);
    binder::expression_vector curExprs =
        subquery->getSchema()->getExpressionsInScope();
    if (schemaExprs.size() != curExprs.size()) {
      THROW_EXCEPTION_WITH_FILE_LINE(
          "union subqueries should have same schema");
    }
    bool needAliasMap = false;
    for (auto j = 0u; j < schemaExprs.size(); ++j) {
      auto srcExpr = schemaExprs[j];
      auto dstExpr = curExprs[j];
      if (srcExpr->getUniqueName() != dstExpr->getUniqueName()) {
        needAliasMap = true;
        break;
      }
    }
    // Normalize every UNION branch, including the first one. Otherwise the
    // first branch can keep extra intermediate context columns while later
    // branches are projected to the UNION schema, and execution-side union will
    // try to read aliases that do not exist in the other branch.
    if (needAliasMap || i == firstSubqueryIdx) {
      auto aliasMap = std::make_shared<planner::LogicalAliasMap>(
          curExprs, schemaExprs, subquery);
      unionOp.setChild(i, std::move(aliasMap));
    }
  }
  return op;
}
}  // namespace optimizer
}  // namespace neug

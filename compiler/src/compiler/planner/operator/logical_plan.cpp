#include "neug/compiler/planner/operator/logical_plan.h"

#include "neug/compiler/planner/operator/logical_explain.h"
#include "neug/compiler/planner/operator/logical_projection.h"

namespace neug {
namespace planner {

binder::expression_vector LogicalPlan::getOutputExpressions() const {
  auto op = lastOperator;
  while (op != nullptr) {
    if (op->getOperatorType() == LogicalOperatorType::PROJECTION) {
      return op->constCast<LogicalProjection>().getExpressionsToProject();
    }
    // These operators retain the final projection's output slots. DISTINCT
    // may deduplicate its internal keys, but it must not remove result columns.
    switch (op->getOperatorType()) {
    case LogicalOperatorType::DISTINCT:
    case LogicalOperatorType::ORDER_BY:
    case LogicalOperatorType::LIMIT:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::FLATTEN:
      if (op->getNumChildren() == 1) {
        op = op->getChild(0);
        continue;
      }
      break;
    default:
      break;
    }
    break;
  }
  return getSchema()->getExpressionsInScope();
}

bool LogicalPlan::isProfile() const {
  return lastOperator->getOperatorType() == LogicalOperatorType::EXPLAIN &&
         reinterpret_cast<LogicalExplain*>(lastOperator.get())
                 ->getExplainType() == common::ExplainType::PROFILE;
}

bool LogicalPlan::hasUpdate() const {
  return lastOperator->hasUpdateRecursive();
}

std::unique_ptr<LogicalPlan> LogicalPlan::shallowCopy() const {
  auto plan = std::make_unique<LogicalPlan>();
  plan->lastOperator = lastOperator;  // shallow copy sub-plan
  plan->cost = cost;
  return plan;
}

std::unique_ptr<LogicalPlan> LogicalPlan::deepCopy() const {
  NEUG_ASSERT(!isEmpty());
  auto plan = std::make_unique<LogicalPlan>();
  plan->lastOperator = lastOperator->copy();  // deep copy sub-plan
  plan->cost = cost;
  return plan;
}

}  // namespace planner
}  // namespace neug

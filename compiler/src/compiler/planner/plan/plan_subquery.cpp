#include <iostream>
#include <memory>
#include <unordered_set>
#include "neug/compiler/binder/expression/expression_util.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/node_rel_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/planner/operator/factorization/flatten_resolver.h"
#include "neug/compiler/planner/operator/logical_operator.h"
#include "neug/compiler/planner/operator/logical_plan.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/schema.h"
#include "neug/compiler/planner/planner.h"

using namespace neug::binder;
using namespace neug::common;

namespace neug {
namespace planner {

static expression_vector getDependentExprs(std::shared_ptr<Expression> expr,
                                           const Schema& schema) {
  auto analyzer =
      GroupDependencyAnalyzer(true /* collectDependentExpr */, schema);
  analyzer.visit(expr);
  return analyzer.getDependentExprs();
}

static std::unordered_set<std::string> matchBindings(
    const QueryGraphCollection& collection) {
  std::unordered_set<std::string> result;
  for (const auto& node : collection.getQueryNodes()) {
    result.insert(node->getUniqueName());
  }
  for (const auto& rel : collection.getQueryRels()) {
    result.insert(rel->getUniqueName());
  }
  return result;
}

// Schema::evaluable expands a relationship to its endpoints. That alone does
// not prove that the relationship record exists: an inner edge can connect two
// outer nodes. A guard must read only values already present on the left.
static bool isOuterOptionalGuard(const Expression& expression,
                                 const Schema& schema,
                                 const std::unordered_set<std::string>& inner) {
  if (schema.isExpressionInScope(expression)) {
    return true;
  }
  switch (expression.expressionType) {
  case ExpressionType::LITERAL:
  case ExpressionType::PARAMETER:
    return true;
  case ExpressionType::PATTERN:
    if (expression.getDataType().getLogicalTypeID() == LogicalTypeID::NODE) {
      return schema.isExpressionInScope(
          *expression.constCast<NodeExpression>().getInternalID());
    }
    return !inner.contains(expression.getUniqueName()) &&
           schema.evaluable(expression);
  case ExpressionType::PROPERTY: {
    const auto& variable =
        expression.constCast<PropertyExpression>().getVariableName();
    for (const auto& input : schema.getExpressionsInScope()) {
      if (input->getUniqueName() == variable) {
        return true;
      }
    }
    return false;
  }
  case ExpressionType::AGGREGATE_FUNCTION:
  case ExpressionType::SUBQUERY:
    return false;
  default:
    break;
  }
  auto children = ExpressionChildrenCollector::collectChildren(expression);
  if (children.empty() || ExpressionVisitor::isRandom(expression)) {
    return false;
  }
  for (const auto& child : children) {
    if (!isOuterOptionalGuard(*child, schema, inner)) {
      return false;
    }
  }
  return true;
}

// The legacy CORRELATED path performs independent graph scans, not a scan of
// outer records. Keep predicates it can rebuild from this match, but reject
// hidden outer edge/scalar dependencies before they reach a physical filter.
static bool isIndependentMatchExpression(
    const Expression& expression,
    const std::unordered_set<std::string>& available) {
  switch (expression.expressionType) {
  case ExpressionType::LITERAL:
  case ExpressionType::PARAMETER:
    return true;
  case ExpressionType::PATTERN:
    return available.contains(expression.getUniqueName());
  case ExpressionType::PROPERTY:
    return available.contains(
        expression.constCast<PropertyExpression>().getVariableName());
  case ExpressionType::VARIABLE:
  case ExpressionType::AGGREGATE_FUNCTION:
  case ExpressionType::SUBQUERY:
    return false;
  default:
    break;
  }
  for (const auto& child :
       ExpressionChildrenCollector::collectChildren(expression)) {
    if (!isIndependentMatchExpression(*child, available)) {
      return false;
    }
  }
  return true;
}

expression_vector Planner::getCorrelatedExprs(
    const QueryGraphCollection& collection, const expression_vector& predicates,
    Schema* outerSchema) {
  expression_vector result;
  // for query `MATCH (a:person) OPTIONAL MATCH (a)-[:knows]->(b:person) WHERE
  // b.age > a.age`, a.age = a.age will be put into join condition by the
  // following codes, we skip here.

  // for (auto& predicate : predicates) {
  //   for (auto& expression : getDependentExprs(predicate, *outerSchema)) {
  //     result.push_back(expression);
  //   }
  // }
  for (auto& node : collection.getQueryNodes()) {
    if (outerSchema->isExpressionInScope(*node->getInternalID())) {
      result.push_back(node->getInternalID());
    }
  }
  return ExpressionUtil::removeDuplication(result);
}

class SubqueryPredicatePullUpAnalyzer {
 public:
  SubqueryPredicatePullUpAnalyzer(
      const Schema& schema, const QueryGraphCollection& queryGraphCollection)
      : schema{schema}, queryGraphCollection{queryGraphCollection} {}

  bool analyze(const expression_vector& predicates) {
    expression_vector correlatedPredicates;
    for (auto& predicate : predicates) {
      const bool dependent = !getDependentExprs(predicate, schema).empty();
      // A shared node can be scanned on the right using the same identity.
      // Its properties (including NULL) are therefore evaluable there. Sending
      // such predicates through the correlated expression-scan path can lose
      // outer-only ON filters; keep them on the right before the left join.
      if (canEvaluateInGraph(*predicate) || !dependent) {
        fullScan |= dependent || readsSharedNode(*predicate);
        nonCorrelatedPredicates.push_back(predicate);
      } else {
        correlatedPredicates.push_back(predicate);
      }
    }
    for (auto predicate : correlatedPredicates) {
      auto [left, right] = analyze(predicate);
      if (left == nullptr) {
        return false;
      }
      joinConditions.emplace_back(left, right);
    }
    for (auto& node : queryGraphCollection.getQueryNodes()) {
      if (schema.isExpressionInScope(*node->getInternalID())) {
        joinConditions.emplace_back(node->getInternalID(),
                                    node->getInternalID());
      }
    }
    return true;
  }

  expression_vector getNonCorrelatedPredicates() const {
    return nonCorrelatedPredicates;
  }
  bool needsFullScan() const { return fullScan; }
  std::vector<binder::expression_pair> getJoinConditions() const {
    return joinConditions;
  }

  expression_vector getCorrelatedInternalIDs() const {
    expression_vector exprs;
    for (auto& node : queryGraphCollection.getQueryNodes()) {
      if (schema.isExpressionInScope(*node->getInternalID())) {
        exprs.push_back(node->getInternalID());
      }
    }
    return exprs;
  }

 private:
  bool readsSharedNode(const Expression& e) const {
    if (e.expressionType == ExpressionType::PROPERTY ||
        e.expressionType == ExpressionType::VARIABLE ||
        e.expressionType == ExpressionType::PATTERN) {
      const auto binding = e.expressionType == ExpressionType::PROPERTY
          ? e.constPtrCast<PropertyExpression>()->getVariableName()
          : e.getUniqueName();
      for (const auto& node : queryGraphCollection.getQueryNodes())
        if (node->getUniqueName() == binding &&
            schema.isExpressionInScope(*node->getInternalID()))
          return true;
    }
    for (const auto& child : e.getChildren())
      if (readsSharedNode(*child))
        return true;
    return false;
  }
  bool canEvaluateInGraph(const Expression& e) const {
    if (e.expressionType == ExpressionType::SUBQUERY)
      return false;
    if (e.expressionType == ExpressionType::PROPERTY ||
        e.expressionType == ExpressionType::VARIABLE ||
        e.expressionType == ExpressionType::PATTERN) {
      const auto binding = e.expressionType == ExpressionType::PROPERTY
          ? e.constPtrCast<PropertyExpression>()->getVariableName()
          : e.getUniqueName();
      for (const auto& node : queryGraphCollection.getQueryNodes())
        if (node->getUniqueName() == binding)
          return true;
      for (const auto& rel : queryGraphCollection.getQueryRels())
        if (rel->getUniqueName() == binding)
          return true;
      return false;
    }
    for (const auto& child : e.getChildren())
      if (!canEvaluateInGraph(*child))
        return false;
    return true;
  }
  expression_pair analyze(std::shared_ptr<Expression> predicate) {
    if (predicate->expressionType != common::ExpressionType::EQUALS) {
      return {nullptr, nullptr};
    }
    auto left = predicate->getChild(0);
    auto right = predicate->getChild(1);
    if (isUnnestableJoinCondition(*left, *right)) {
      return {left, right};
    }
    if (isUnnestableJoinCondition(*right, *left)) {
      return {right, left};
    }
    return {nullptr, nullptr};
  }

  bool isUnnestableJoinCondition(const Expression& left,
                                 const Expression& right) {
    return right.expressionType == ExpressionType::PROPERTY &&
           schema.isExpressionInScope(left) &&
           !schema.isExpressionInScope(right);
  }

 private:
  const Schema& schema;
  const QueryGraphCollection& queryGraphCollection;

  expression_vector nonCorrelatedPredicates;
  bool fullScan = false;
  std::vector<binder::expression_pair> joinConditions;
};

void Planner::planOptionalMatch(
    const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& leftPlan,
    std::shared_ptr<BoundJoinHintNode> hint) {
  planOptionalMatch(queryGraphCollection, predicates, nullptr /* mark */,
                    leftPlan, std::move(hint));
}

void Planner::planOptionalMatch(
    const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, std::shared_ptr<Expression> mark,
    LogicalPlan& leftPlan, std::shared_ptr<BoundJoinHintNode> hint) {
  expression_vector correlatedExprs;
  if (!leftPlan.isEmpty()) {
    correlatedExprs = getCorrelatedExprs(queryGraphCollection, predicates,
                                         leftPlan.getSchema());
  }
  auto info = QueryGraphPlanningInfo();
  info.hint = hint;
  info.kind = MatchKind::OPTIONAL;
  if (leftPlan.isEmpty()) {
    // Optional match is the first clause, e.g. OPTIONAL MATCH <pattern> RETURN
    // *
    info.predicates = predicates;
    auto plan = planQueryGraphCollection(queryGraphCollection, info);
    leftPlan.setLastOperator(plan->getLastOperator());
    appendOptionalAccumulate(mark, leftPlan);
    return;
  }
  const auto innerBindings = matchBindings(queryGraphCollection);
  expression_vector outerGuards, matchPredicates;
  for (const auto& predicate : predicates) {
    if (isOuterOptionalGuard(*predicate, *leftPlan.getSchema(), innerBindings) &&
        !getDependentExprs(predicate, *leftPlan.getSchema()).empty()) {
      outerGuards.push_back(predicate);
    } else {
      matchPredicates.push_back(predicate);
    }
  }
  auto requireIndependentPredicates = [&](const expression_vector& expressions) {
    for (const auto& predicate : expressions) {
      if (!isIndependentMatchExpression(*predicate, innerBindings)) {
        THROW_NOT_SUPPORTED_EXCEPTION(
            "OPTIONAL MATCH predicate requires unsupported mixed outer/inner "
            "correlation: " + predicate->toString());
      }
    }
  };
  if (!outerGuards.empty()) {
    // L LEFT JOIN R ON shared_ids AND guard(L)=TRUE. FALSE and NULL therefore
    // produce one null-extended row per outer row, including duplicate rows.
    // Filtering above the LEFT JOIN would incorrectly discard those rows.
    auto analyzer = SubqueryPredicatePullUpAnalyzer(*leftPlan.getSchema(),
                                                     queryGraphCollection);
    std::vector<expression_pair> joinConditions;
    if (analyzer.analyze(matchPredicates)) {
      info.predicates = analyzer.getNonCorrelatedPredicates();
      joinConditions = analyzer.getJoinConditions();
    } else {
      requireIndependentPredicates(matchPredicates);
      info.predicates = matchPredicates;
      for (const auto& expr : correlatedExprs) {
        joinConditions.emplace_back(expr, expr);
      }
    }
    // A guard projection prevents join-to-extend fusion. Build a self-contained
    // RHS instead of a NodeIDScan that would require an outer execution input.
    info.subqueryType = SubqueryPlanningType::NONE;
    auto rightPlan =
        planQueryGraphCollectionInNewContext(queryGraphCollection, info);
    auto appendGuardProjection = [&](const expression_vector& expressions,
                                     LogicalPlan& plan) {
      if (expressions.empty()) {
        return;
      }
      appendProjection(expressions, plan);
      auto& projection = plan.getLastOperator()->cast<LogicalProjection>();
      projection.setAppend(true);
      projection.computeFactorizedSchema();
    };
    expression_vector guardsToCompute;
    for (const auto& guard : outerGuards) {
      if (!leftPlan.getSchema()->isExpressionInScope(*guard)) {
        guardsToCompute.push_back(guard);
      }
    }
    appendGuardProjection(ExpressionUtil::removeDuplication(guardsToCompute),
                          leftPlan);
    auto trueKey = std::make_shared<LiteralExpression>(
        Value(true), "OPTIONAL_GUARD_TRUE(" + outerGuards[0]->getUniqueName() + ")");
    appendGuardProjection({trueKey}, *rightPlan);
    for (const auto& guard : outerGuards) {
      joinConditions.emplace_back(guard, trueKey);
    }
    if (leftPlan.hasUpdate()) {
      appendAccHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan,
                       *rightPlan, leftPlan);
    } else {
      appendHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan, *rightPlan,
                     leftPlan);
    }
    return;
  }
  if (correlatedExprs.empty()) {
    // Plan uncorrelated subquery (think of this as a CTE)
    requireIndependentPredicates(predicates);
    info.predicates = predicates;
    auto rightPlan = planQueryGraphCollection(queryGraphCollection, info);
    if (leftPlan.hasUpdate()) {
      appendAccOptionalCrossProduct(mark, leftPlan, *rightPlan, leftPlan);
    } else {
      appendOptionalCrossProduct(mark, leftPlan, *rightPlan, leftPlan);
    }
    return;
  }
  // Plan correlated subquery
  info.corrExprsCard = leftPlan.getCardinality();
  cardinalityEstimator.addCorrelatedSource(leftPlan.getLastOperator());
  auto analyzer = SubqueryPredicatePullUpAnalyzer(*leftPlan.getSchema(),
                                                  queryGraphCollection);
  std::vector<expression_pair> joinConditions;
  std::unique_ptr<LogicalPlan> rightPlan;
  if (analyzer.analyze(predicates)) {
    // Unnest as left join
    // ID-only correlated scans intentionally omit property filters. A shared
    // endpoint ON predicate requires a normal scan with those filters intact.
    info.subqueryType = analyzer.needsFullScan() ? SubqueryPlanningType::NONE
                                               : SubqueryPlanningType::UNNEST_CORRELATED;
    if (!analyzer.needsFullScan())
      info.corrExprs = analyzer.getCorrelatedInternalIDs();
    info.predicates = analyzer.getNonCorrelatedPredicates();
    rightPlan =
        planQueryGraphCollectionInNewContext(queryGraphCollection, info);
    joinConditions = analyzer.getJoinConditions();
  } else {
    // Unnest as expression scan + distinct & inner join
    requireIndependentPredicates(predicates);
    info.subqueryType = SubqueryPlanningType::CORRELATED;
    info.corrExprs = correlatedExprs;
    info.predicates = predicates;
    for (auto& expr : correlatedExprs) {
      joinConditions.emplace_back(expr, expr);
    }
    rightPlan =
        planQueryGraphCollectionInNewContext(queryGraphCollection, info);
    appendAccumulate(correlatedExprs, leftPlan);
  }
  if (leftPlan.hasUpdate()) {
    appendAccHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan,
                      *rightPlan, leftPlan);
  } else {
    appendHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan, *rightPlan,
                   leftPlan);
  }
}

void Planner::planRegularMatch(const QueryGraphCollection& queryGraphCollection,
                               const expression_vector& predicates,
                               LogicalPlan& leftPlan,
                               std::shared_ptr<BoundJoinHintNode> hint) {
  expression_vector predicatesToPushDown, predicatesToPullUp;
  expression_vector outerFilters;
  const auto innerBindings = matchBindings(queryGraphCollection);
  // E.g. MATCH (a) WITH COUNT(*) AS s MATCH (b) WHERE b.age > s
  // "b.age > s" should be pulled up after both MATCH clauses are joined.
  for (auto& predicate : predicates) {
    if (getDependentExprs(predicate, *leftPlan.getSchema()).empty()) {
      predicatesToPushDown.push_back(predicate);
    } else if (!leftPlan.hasUpdate() &&
        !ExpressionVisitor::isRandom(*predicate) &&
        isOuterOptionalGuard(*predicate, *leftPlan.getSchema(), innerBindings)) {
      // In a mandatory MATCH, a deterministic predicate whose complete input
      // already exists on the left can filter that input before expansion or
      // an independent RHS join. This includes materialized presence flags.
      // OPTIONAL uses its separate ON-guard path and keeps false/NULL rows.
      outerFilters.push_back(predicate);
    } else {
      predicatesToPullUp.push_back(predicate);
    }
  }
  appendFilters(outerFilters, leftPlan);
  auto correlatedExprs = getCorrelatedExprs(
      queryGraphCollection, predicatesToPushDown, leftPlan.getSchema());
  auto joinNodeIDs = ExpressionUtil::getExpressionsWithDataType(
      correlatedExprs, LogicalTypeID::INTERNAL_ID);
  auto info = QueryGraphPlanningInfo();
  info.predicates = predicatesToPushDown;
  info.hint = hint;
  if (joinNodeIDs.empty()) {
    info.subqueryType = SubqueryPlanningType::NONE;
    auto rightPlan =
        planQueryGraphCollectionInNewContext(queryGraphCollection, info);
    if (leftPlan.hasUpdate()) {
      appendCrossProduct(*rightPlan, leftPlan, leftPlan);
    } else {
      appendCrossProduct(leftPlan, *rightPlan, leftPlan);
    }
  } else {
    // TODO(Xiyang): there is a question regarding if we want to plan as a
    // correlated subquery Multi-part query is actually CTE and CTE can be
    // considered as a subquery but does not scan from outer.
    info.subqueryType = SubqueryPlanningType::UNNEST_CORRELATED;
    info.corrExprs = joinNodeIDs;
    info.corrExprsCard = leftPlan.getCardinality();
  cardinalityEstimator.addCorrelatedSource(leftPlan.getLastOperator());
    auto rightPlan =
        planQueryGraphCollectionInNewContext(queryGraphCollection, info);
    if (leftPlan.hasUpdate()) {
      appendHashJoin(joinNodeIDs, JoinType::INNER, *rightPlan, leftPlan,
                     leftPlan);
    } else {
      appendHashJoin(joinNodeIDs, JoinType::INNER, leftPlan, *rightPlan,
                     leftPlan);
    }
  }
  for (auto& predicate : predicatesToPullUp) {
    appendFilter(predicate, leftPlan);
  }
}

// Correlation is defined by the current output scope. In particular, an
// INTERSECT or join can introduce a node that does not occur in its first
// child's scan/extend chain. Walking that chain loses valid correlation keys;
// walking farther into historical operators can also resurrect bindings that a
// projection or aggregation has removed. Keep the visible graph identities;
// property predicates on these graph nodes remain predicates of the inner
// match, rather than redundant scalar hash keys.
std::unique_ptr<Schema> Planner::combineSchema(LogicalPlan& outerPlan) {
  auto combinedSchema = std::make_unique<Schema>();
  for (const auto& expression : outerPlan.getSchema()->getExpressionsInScope()) {
    if (expression->getDataType().getLogicalTypeID() ==
        LogicalTypeID::INTERNAL_ID)
      combinedSchema->insertToScopeMayRepeat(expression, 0);
    else if (expression->expressionType == ExpressionType::PATTERN &&
             expression->getDataType().getLogicalTypeID() == LogicalTypeID::NODE)
      combinedSchema->insertToScopeMayRepeat(
          expression->constPtrCast<NodeExpression>()->getInternalID(), 0);
  }
  return combinedSchema;
}

void Planner::planSubquery(const std::shared_ptr<Expression>& expression,
                           LogicalPlan& outerPlan) {
  NEUG_ASSERT(expression->expressionType == ExpressionType::SUBQUERY);
  auto subquery = expression->ptrCast<SubqueryExpression>();
  auto correlatedExprs = getDependentExprs(expression, *outerPlan.getSchema());
  auto predicates = subquery->getPredicatesSplitOnAnd();
  auto combinedSchema = combineSchema(outerPlan);
  // WITH may expose a graph node as a whole rather than its internal-ID
  // property. Its identity is still a correlation key, even when the subquery
  // has no scalar predicate that happens to expose that dependency.
  auto correlatedNodes = getCorrelatedExprs(*subquery->getQueryGraphCollection(),
                                           predicates, combinedSchema.get());
  correlatedExprs.insert(correlatedExprs.end(), correlatedNodes.begin(),
                         correlatedNodes.end());
  correlatedExprs = ExpressionUtil::removeDuplication(correlatedExprs);
  std::unique_ptr<LogicalPlan> innerPlan;
  auto info = QueryGraphPlanningInfo();
  info.hint = subquery->getHint();
  info.kind = MatchKind::SUBQUERY;
  if (correlatedExprs.empty()) {
    // Plan uncorrelated subquery
    info.subqueryType = SubqueryPlanningType::NONE;
    info.predicates = predicates;
    innerPlan = planQueryGraphCollectionInNewContext(
        *subquery->getQueryGraphCollection(), info);
    expression_vector emptyHashKeys;
    auto projectExprs = expression_vector{subquery->getProjectionExpr()};
    switch (subquery->getSubqueryType()) {
    case common::SubqueryType::EXISTS: {
      auto aggregates = expression_vector{subquery->getCountStarExpr()};
      appendAggregate(emptyHashKeys, aggregates, *innerPlan);
      appendProjection(projectExprs, *innerPlan);
    } break;
    case common::SubqueryType::COUNT: {
      appendAggregate(emptyHashKeys, projectExprs, *innerPlan);
    } break;
    default:
      NEUG_UNREACHABLE;
    }
    appendCrossProduct(outerPlan, *innerPlan, outerPlan);
    return;
  }
  // Plan correlated subquery
  info.corrExprsCard = outerPlan.getCardinality();
  cardinalityEstimator.addCorrelatedSource(outerPlan.getLastOperator());
  auto analyzer = SubqueryPredicatePullUpAnalyzer(
      *combinedSchema, *subquery->getQueryGraphCollection());
  std::vector<expression_pair> joinConditions;
  if (analyzer.analyze(predicates)) {
    // Unnest as inner join
    info.subqueryType = analyzer.needsFullScan() ? SubqueryPlanningType::NONE
                                               : SubqueryPlanningType::UNNEST_CORRELATED;
    if (!analyzer.needsFullScan())
      info.corrExprs = analyzer.getCorrelatedInternalIDs();
    info.predicates = analyzer.getNonCorrelatedPredicates();
    innerPlan = planQueryGraphCollectionInNewContext(
        *subquery->getQueryGraphCollection(), info);
    joinConditions = analyzer.getJoinConditions();
  } else {
    // Unnest as expression scan + distinct & inner join
    info.subqueryType = SubqueryPlanningType::CORRELATED;
    info.corrExprs = correlatedExprs;
    info.predicates = predicates;
    for (auto& expr : correlatedExprs) {
      joinConditions.emplace_back(expr, expr);
    }
    innerPlan = planQueryGraphCollectionInNewContext(
        *subquery->getQueryGraphCollection(), info);
    appendAccumulate(correlatedExprs, outerPlan);
  }
  switch (subquery->getSubqueryType()) {
  case common::SubqueryType::EXISTS: {
    appendMarkJoin(joinConditions, expression, outerPlan, *innerPlan,
                   outerPlan);
  } break;
  case common::SubqueryType::COUNT: {
    expression_vector hashKeys;
    for (auto& joinCondition : joinConditions) {
      hashKeys.push_back(joinCondition.second);
    }
    appendAggregate(hashKeys, expression_vector{subquery->getProjectionExpr()},
                    *innerPlan);
    appendHashJoin(joinConditions, common::JoinType::COUNT, nullptr, outerPlan,
                   *innerPlan, outerPlan);
  } break;
  default:
    NEUG_UNREACHABLE;
  }
}

void Planner::planSubqueryIfNecessary(std::shared_ptr<Expression> expression,
                                      LogicalPlan& plan) {
  auto collector = SubqueryExprCollector();
  collector.visit(expression);
  if (collector.hasSubquery()) {
    for (auto& expr : collector.getSubqueryExprs()) {
      if (plan.getSchema()->isExpressionInScope(*expr)) {
        continue;
      }
      planSubquery(expr, plan);
    }
  }
}

}  // namespace planner
}  // namespace neug

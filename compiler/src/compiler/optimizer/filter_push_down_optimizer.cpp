/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

/**
 * This file is originally from the Kùzu project
 * (https://github.com/kuzudb/kuzu) Licensed under the MIT License. Modified by
 * Zhou Xiaoli in 2025 to support Neug-specific features.
 */

#include "neug/compiler/optimizer/filter_push_down_optimizer.h"
#include <functional>
#include <vector>

#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/planner/operator/extend/logical_extend.h"
#include "neug/compiler/planner/operator/logical_empty_result.h"
#include "neug/compiler/planner/operator/logical_filter.h"
#include "neug/compiler/planner/operator/logical_hash_join.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/logical_table_function_call.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"

using namespace neug::binder;
using namespace neug::common;
using namespace neug::planner;
using namespace neug::storage;

namespace neug {
namespace optimizer {

static bool isNodePrimaryKey(const Expression& expression,
                             const Expression& nodeID,
                             const std::vector<common::table_id_t>& tableIDs);

void FilterPushDownOptimizer::rewrite(LogicalPlan* plan) {
  visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
  switch (op->getOperatorType()) {
  case LogicalOperatorType::FILTER: {
    return visitFilterReplace(op);
  }
  case LogicalOperatorType::CROSS_PRODUCT: {
    return visitCrossProductReplace(op);
  }
  case LogicalOperatorType::HASH_JOIN: {
    return visitHashJoinReplace(op);
  }
  case LogicalOperatorType::EXTEND: {
    return visitExtendReplace(op);
  }
  case LogicalOperatorType::SCAN_NODE_TABLE: {
    return visitScanNodeTableReplace(op);
  }
  case LogicalOperatorType::TABLE_FUNCTION_CALL: {
    return visitTableFunctionCallReplace(op);
  }
  default: {
    return visitChildren(op);
  }
  }
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::visitChildren(
    const std::shared_ptr<LogicalOperator>& op) {
  for (auto i = 0u; i < op->getNumChildren(); ++i) {
    auto optimizer = FilterPushDownOptimizer(context);
    op->setChild(i, optimizer.visitOperator(op->getChild(i)));
  }
  op->computeFlatSchema();
  return finishPushDown(op);
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::visitFilterReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  auto& filter = op->constCast<LogicalFilter>();
  auto predicate = filter.getPredicate();
  if (predicate->expressionType == ExpressionType::LITERAL) {
    auto& literalExpr = predicate->constCast<LiteralExpression>();
    if (literalExpr.isNull() || !literalExpr.getValue().getValue<bool>()) {
      return std::make_shared<LogicalEmptyResult>(*op->getSchema());
    }
  } else {
    predicateSet.addPredicate(predicate);
  }
  return visitOperator(filter.getChild(0));
}

std::shared_ptr<LogicalOperator>
FilterPushDownOptimizer::visitCrossProductReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  auto remainingPSet = PredicateSet();
  auto probePSet = PredicateSet();
  auto buildPSet = PredicateSet();
  for (auto& p : predicateSet.getAllPredicates()) {
    auto inProbe = op->getChild(0)->getSchema()->evaluable(*p);
    auto inBuild = op->getChild(1)->getSchema()->evaluable(*p);
    if (inProbe && !inBuild) {
      probePSet.addPredicate(p);
    } else if (!inProbe && inBuild) {
      buildPSet.addPredicate(p);
    } else {
      remainingPSet.addPredicate(p);
    }
  }
  NEUG_ASSERT(op->getNumChildren() == 2);
  auto probeOptimizer = FilterPushDownOptimizer(context, std::move(probePSet));
  op->setChild(0, probeOptimizer.visitOperator(op->getChild(0)));
  auto buildOptimizer = FilterPushDownOptimizer(context, std::move(buildPSet));
  op->setChild(1, buildOptimizer.visitOperator(op->getChild(1)));

  auto probeSchema = op->getChild(0)->getSchema();
  auto buildSchema = op->getChild(1)->getSchema();
  expression_vector predicates;
  std::vector<join_condition_t> joinConditions;
  for (auto& predicate : remainingPSet.equalityPredicates) {
    auto left = predicate->getChild(0);
    auto right = predicate->getChild(1);
    if (probeSchema->isExpressionInScope(*left) &&
        buildSchema->isExpressionInScope(*right)) {
      joinConditions.emplace_back(left, right);
    } else if (probeSchema->isExpressionInScope(*right) &&
               buildSchema->isExpressionInScope(*left)) {
      joinConditions.emplace_back(right, left);
    } else {
      predicates.push_back(predicate);
    }
  }
  if (joinConditions.empty()) {
    // Only a retained cross product can be flattened into a sequential
    // context-dependent lookup. Hash-join branches execute independently;
    // marking their build scan with a probe alias makes that alias unavailable.
    for (auto& predicate : remainingPSet.equalityPredicates) {
      auto pkSide = predicate->getChild(0);
      auto valSide = predicate->getChild(1);
      if (!buildSchema->isExpressionInScope(*pkSide))
        std::swap(pkSide, valSide);
      if (!buildSchema->isExpressionInScope(*pkSide) ||
          !probeSchema->evaluable(*valSide))
        continue;
      std::function<bool(const std::shared_ptr<LogicalOperator>&)> findAndMark;
      findAndMark = [&](const std::shared_ptr<LogicalOperator>& child) -> bool {
        if (child->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
          auto& scan = child->cast<LogicalScanNodeTable>();
          if (isNodePrimaryKey(*pkSide, *scan.getNodeID(), scan.getTableIDs())) {
            scan.setScanType(LogicalScanNodeTableType::PRIMARY_KEY_SCAN);
            scan.setExtraInfo(std::make_unique<PrimaryKeyScanInfo>(valSide->copy()));
            // Lookup mode does not change the node/property scope. Replacing
            // its Schema here would invalidate buildSchema for later predicates.
            return true;
          }
        }
        for (auto& nested : child->getChildren())
          if (findAndMark(nested)) return true;
        return false;
      };
      findAndMark(op->getChild(1));
    }
    return finishPushDown(op);
  }
  auto hashJoin = std::make_shared<LogicalHashJoin>(
      joinConditions, JoinType::INNER, nullptr /* mark */, op->getChild(0),
      op->getChild(1), 0 /* cardinality */);
  hashJoin->getSIPInfoUnsafe().position = SemiMaskPosition::PROHIBIT;
  hashJoin->computeFlatSchema();
  predicates.insert(predicates.end(),
                    remainingPSet.nonEqualityPredicates.begin(),
                    remainingPSet.nonEqualityPredicates.end());
  if (predicates.empty()) {
    return hashJoin;
  }
  return appendFilters(predicates, hashJoin);
}

static bool isConstantExpression(const std::shared_ptr<Expression> expression) {
  switch (expression->expressionType) {
  case ExpressionType::LITERAL:
  case ExpressionType::PARAMETER: {
    return true;
  }
  case ExpressionType::FUNCTION: {
    auto& func = expression->constCast<ScalarFunctionExpression>();
    if (func.getFunction().name == "CAST") {
      return isConstantExpression(func.getChild(0));
    } else {
      return false;
    }
  }
  default:
    return false;
  }
}

// Only follow operators that preserve the identity of an in-scope node. A
// UNION, aggregate, alias map or other binding boundary requires a separate
// proof and must not inherit a key from one of its inputs.
static const LogicalScanNodeTable* findRequiredPrimaryKeyScan(
    const std::shared_ptr<LogicalOperator>& op, const Expression& nodeID) {
  if (!op->getSchema()->isExpressionInScope(nodeID)) {
    return nullptr;
  }
  switch (op->getOperatorType()) {
  case LogicalOperatorType::SCAN_NODE_TABLE: {
    const auto& scan = op->constCast<LogicalScanNodeTable>();
    if (scan.getNodeID()->getUniqueName() != nodeID.getUniqueName() ||
        scan.getScanType() != LogicalScanNodeTableType::PRIMARY_KEY_SCAN ||
        !scan.getExtraInfo()) {
      return nullptr;
    }
    const auto& key = scan.getExtraInfo()->constCast<PrimaryKeyScanInfo>().key;
    const auto type = key->getDataType().getLogicalTypeID();
    // IN/list lookups can carry repeated keys. Copying that input as a build
    // scan could multiply matches; this rule proves only a scalar binding.
    return isConstantExpression(key) && type != LogicalTypeID::LIST &&
                   type != LogicalTypeID::ARRAY && type != LogicalTypeID::ANY
               ? &scan
               : nullptr;
  }
  case LogicalOperatorType::CROSS_PRODUCT: {
    for (const auto& child : op->getChildren()) {
      if (const auto* scan = findRequiredPrimaryKeyScan(child, nodeID)) {
        return scan;
      }
    }
    return nullptr;
  }
  case LogicalOperatorType::HASH_JOIN: {
    const auto& join = op->constCast<LogicalHashJoin>();
    if (join.getJoinType() == JoinType::LEFT) {
      // A nullable build-side binding is not a required probe binding.
      return findRequiredPrimaryKeyScan(join.getChild(0), nodeID);
    }
    if (join.getJoinType() == JoinType::INNER &&
        LogicalHashJoin::isNodeIDOnlyJoin(join.getJoinConditions())) {
      for (const auto& child : op->getChildren()) {
        if (const auto* scan = findRequiredPrimaryKeyScan(child, nodeID)) {
          return scan;
        }
      }
    }
    return nullptr;
  }
  case LogicalOperatorType::FILTER:
  case LogicalOperatorType::EXTEND:
  case LogicalOperatorType::RECURSIVE_EXTEND:
  case LogicalOperatorType::GET_V:
    return op->getNumChildren() == 1
               ? findRequiredPrimaryKeyScan(op->getChild(0), nodeID)
               : nullptr;
  case LogicalOperatorType::PROJECTION:
    // OPTIONAL guards append expressions without rebinding the retained node.
    return op->constCast<LogicalProjection>().append() &&
                   op->getNumChildren() == 1
               ? findRequiredPrimaryKeyScan(op->getChild(0), nodeID)
               : nullptr;
  default:
    return nullptr;
  }
}

// Restricting the seed of a simple pattern chain preserves all its results for
// that seed. Do not cross aggregates, LIMIT, joins or other operators for which
// filtering an input can change the surviving rows' values or multiplicities.
static LogicalScanNodeTable* getPatternSeedScan(
    const std::shared_ptr<LogicalOperator>& op) {
  switch (op->getOperatorType()) {
  case LogicalOperatorType::SCAN_NODE_TABLE:
    return &op->cast<LogicalScanNodeTable>();
  case LogicalOperatorType::FILTER:
  case LogicalOperatorType::EXTEND:
  case LogicalOperatorType::RECURSIVE_EXTEND:
  case LogicalOperatorType::GET_V:
    return op->getNumChildren() == 1 ? getPatternSeedScan(op->getChild(0))
                                   : nullptr;
  case LogicalOperatorType::PROJECTION:
    return op->constCast<LogicalProjection>().append() &&
                   op->getNumChildren() == 1
               ? getPatternSeedScan(op->getChild(0))
               : nullptr;
  default:
    return nullptr;
  }
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::visitHashJoinReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  for (auto i = 0u; i < op->getNumChildren(); ++i) {
    auto optimizer = FilterPushDownOptimizer(context);
    op->setChild(i, optimizer.visitOperator(op->getChild(i)));
  }
  auto& join = op->cast<LogicalHashJoin>();
  if (join.getJoinType() == JoinType::INNER ||
      join.getJoinType() == JoinType::LEFT) {
    auto* seed = getPatternSeedScan(join.getChild(1));
    // Never replace an existing constraint: it may conflict with the probe
    // and must still make an OPTIONAL pattern fail rather than match.
    if (seed && seed->getScanType() == LogicalScanNodeTableType::SCAN &&
        !seed->getExtraInfo()) {
      for (const auto& [nodeID, buildID] : join.getJoinConditions()) {
        if (nodeID->getDataType().getLogicalTypeID() != LogicalTypeID::INTERNAL_ID ||
            buildID->getDataType().getLogicalTypeID() != LogicalTypeID::INTERNAL_ID ||
            nodeID->getUniqueName() != buildID->getUniqueName()) {
          continue;
        }
        if (seed->getNodeID()->getUniqueName() != nodeID->getUniqueName()) {
          continue;
        }
        const auto* required =
            findRequiredPrimaryKeyScan(join.getChild(0), *nodeID);
        if (required && required->getTableIDs() == seed->getTableIDs()) {
          seed->setScanType(LogicalScanNodeTableType::PRIMARY_KEY_SCAN);
          seed->setExtraInfo(required->getExtraInfo()->copy());
          break;
        }
      }
    }
  }
  // Keep all join conditions and the outer join itself. Neither optional
  // predicates nor duplicate outer rows may be removed by this optimization.
  op->computeFlatSchema();
  return finishPushDown(op);
}

std::shared_ptr<LogicalOperator>
FilterPushDownOptimizer::visitScanNodeTableReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  auto& scan = op->cast<LogicalScanNodeTable>();
  auto nodeID = scan.getNodeID();
  auto tableIDs = scan.getTableIDs();
  std::shared_ptr<Expression> primaryKeyEqualityComparison = nullptr;
  primaryKeyEqualityComparison =
      predicateSet.popNodePKEqualityComparison(*nodeID, tableIDs);
  auto membership = primaryKeyEqualityComparison
                        ? nullptr
                        : predicateSet.popNodePKInComparison(*nodeID, tableIDs);
  if (membership) {
    auto values = membership->getChild(0);
    if (isConstantExpression(values) ||
        values->getDataType().getLogicalTypeID() == LogicalTypeID::LIST) {
      scan.setScanType(LogicalScanNodeTableType::PRIMARY_KEY_SCAN);
      scan.setExtraInfo(std::make_unique<PrimaryKeyScanInfo>(values));
      scan.computeFlatSchema();
    } else
      predicateSet.addPredicate(membership);
  }
  if (primaryKeyEqualityComparison != nullptr) {
    auto rhs = primaryKeyEqualityComparison->getChild(1);
    if (isConstantExpression(rhs)) {
      auto extraInfo = std::make_unique<PrimaryKeyScanInfo>(rhs);
      scan.setScanType(LogicalScanNodeTableType::PRIMARY_KEY_SCAN);
      scan.setExtraInfo(std::move(extraInfo));
      scan.computeFlatSchema();
    } else {
      predicateSet.addPredicate(primaryKeyEqualityComparison);
    }
  }
  return finishPushDown(op);
}

std::shared_ptr<LogicalOperator>
FilterPushDownOptimizer::visitTableFunctionCallReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  return op;
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::visitExtendReplace(
    const std::shared_ptr<LogicalOperator>& op) {
  if (op->ptrCast<BaseLogicalExtend>()->isRecursive() ||
      !context->getClientConfig()->enableZoneMap) {
    return visitChildren(op);
  }
  auto& extend = op->cast<LogicalExtend>();
  return visitChildren(op);
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::finishPushDown(
    std::shared_ptr<LogicalOperator> op) {
  if (predicateSet.isEmpty()) {
    return op;
  }
  auto predicates = predicateSet.getAllPredicates();
  auto root = appendFilters(predicates, op);
  predicateSet.clear();
  return root;
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::appendScanNodeTable(
    std::shared_ptr<binder::Expression> nodeID,
    std::vector<common::table_id_t> nodeTableIDs,
    binder::expression_vector properties,
    std::shared_ptr<planner::LogicalOperator> child) {
  if (properties.empty()) {
    return child;
  }
  auto printInfo = std::make_unique<OPPrintInfo>();
  auto scanNodeTable = std::make_shared<LogicalScanNodeTable>(
      std::move(nodeID), std::move(nodeTableIDs), std::move(properties));
  scanNodeTable->computeFlatSchema();
  return scanNodeTable;
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::appendFilters(
    const expression_vector& predicates,
    std::shared_ptr<LogicalOperator> child) {
  if (predicates.empty()) {
    return child;
  }
  auto root = child;
  for (auto& p : predicates) {
    root = appendFilter(p, root);
  }
  return root;
}

std::shared_ptr<LogicalOperator> FilterPushDownOptimizer::appendFilter(
    std::shared_ptr<Expression> predicate,
    std::shared_ptr<LogicalOperator> child) {
  auto printInfo = std::make_unique<OPPrintInfo>();
  auto filter =
      std::make_shared<LogicalFilter>(std::move(predicate), std::move(child));
  filter->computeFlatSchema();
  return filter;
}

void PredicateSet::addPredicate(std::shared_ptr<Expression> predicate) {
  if (predicate->expressionType == ExpressionType::EQUALS) {
    equalityPredicates.push_back(std::move(predicate));
  } else {
    nonEqualityPredicates.push_back(std::move(predicate));
  }
}

static bool isNodePrimaryKey(const Expression& expression,
                             const Expression& nodeID,
                             const std::vector<common::table_id_t>& tableIDs) {
  if (expression.expressionType != ExpressionType::PROPERTY) {
    return false;
  }
  auto& property = expression.constCast<PropertyExpression>();
  if (property.getVariableName() !=
      nodeID.constCast<PropertyExpression>().getVariableName()) {
    return false;
  }
  if (tableIDs.empty())
    return false;
  for (auto tableID : tableIDs) {
    if (!property.isPrimaryKey(tableID)) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<Expression> PredicateSet::popNodePKEqualityComparison(
    const Expression& nodeID, const std::vector<common::table_id_t>& tableIDs) {
  auto resultPredicateIdx = INVALID_IDX;
  for (auto i = 0u; i < equalityPredicates.size(); ++i) {
    auto predicate = equalityPredicates[i];
    if (isNodePrimaryKey(*predicate->getChild(0), nodeID, tableIDs)) {
      resultPredicateIdx = i;
      break;
    } else if (isNodePrimaryKey(*predicate->getChild(1), nodeID, tableIDs)) {
      auto leftChild = predicate->getChild(0);
      auto rightChild = predicate->getChild(1);
      predicate->setChild(1, leftChild);
      predicate->setChild(0, rightChild);
      resultPredicateIdx = i;
      break;
    }
  }
  if (resultPredicateIdx != INVALID_IDX) {
    auto result = equalityPredicates[resultPredicateIdx];
    equalityPredicates.erase(equalityPredicates.begin() + resultPredicateIdx);
    return result;
  }
  return nullptr;
}

std::shared_ptr<Expression> PredicateSet::popNodePKInComparison(
    const Expression& nodeID, const std::vector<common::table_id_t>& tableIDs) {
  for (size_t i = 0; i < nonEqualityPredicates.size(); ++i) {
    const auto& expression = nonEqualityPredicates[i];
    if (expression->expressionType != ExpressionType::FUNCTION ||
        expression->getNumChildren() != 2)
      continue;
    if (expression->constCast<ScalarFunctionExpression>().getFunction().name !=
        "LIST_CONTAINS")
      continue;
    if (!isNodePrimaryKey(*expression->getChild(1), nodeID, tableIDs))
      continue;
    auto result = expression;
    nonEqualityPredicates.erase(nonEqualityPredicates.begin() + i);
    return result;
  }
  return nullptr;
}

expression_vector PredicateSet::getAllPredicates() {
  expression_vector result;
  result.insert(result.end(), equalityPredicates.begin(),
                equalityPredicates.end());
  result.insert(result.end(), nonEqualityPredicates.begin(),
                nonEqualityPredicates.end());
  return result;
}

}  // namespace optimizer
}  // namespace neug

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

#include "neug/compiler/optimizer/expand_getv_fusion.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/gopt/g_alias_manager.h"
#include "neug/compiler/planner/operator/extend/logical_recursive_extend.h"
#include "neug/compiler/planner/operator/logical_aggregate.h"
#include "neug/compiler/planner/operator/logical_distinct.h"
#include "neug/compiler/planner/operator/logical_filter.h"
#include "neug/compiler/planner/operator/logical_hash_join.h"
#include "neug/compiler/planner/operator/logical_intersect.h"
#include "neug/compiler/planner/operator/logical_limit.h"
#include "neug/compiler/planner/operator/logical_node_label_filter.h"
#include "neug/compiler/planner/operator/logical_order_by.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/logical_unwind.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"

namespace neug {
namespace optimizer {

void ExpandGetVFusion::rewrite(planner::LogicalPlan* plan) {
  auto root = plan->getLastOperator();
  referencedVariables.clear();
  referencesComplete = collectReferences(*root);
  auto rootOpt = visitOperator(root);
  plan->setLastOperator(rootOpt);
}

bool ExpandGetVFusion::collectReferences(
    const std::shared_ptr<binder::Expression>& expression,
    const std::string& localOwner) {
  using common::ExpressionType;
  if (!expression)
    return true;
  switch (expression->expressionType) {
  case ExpressionType::PROPERTY: {
    const auto& owner =
        expression->constCast<binder::PropertyExpression>().getVariableName();
    // RenameDependentVarOpt uses this placeholder only inside an operator's
    // local predicate. Shared property expressions in that same operator's
    // property list may also carry it. Resolve it to the operator's actual
    // binding for liveness, without changing the expression or its scope.
    // A placeholder outside a known local context remains globally opaque.
    referencedVariables.insert(owner == gopt::DEFAULT_ALIAS_NAME &&
                                       !localOwner.empty()
                                   ? localOwner
                                   : owner);
    return true;
  }
  case ExpressionType::VARIABLE:
  case ExpressionType::PATTERN:
    // Whole-edge returns, COUNT(edge), IDs, equality/inequality, and property
    // functions all keep the edge binding. A declaration alone does not.
    referencedVariables.insert(expression->getUniqueName());
    return true;
  case ExpressionType::LITERAL:
  case ExpressionType::PARAMETER:
    return true;
  case ExpressionType::OR:
  case ExpressionType::XOR:
  case ExpressionType::AND:
  case ExpressionType::NOT:
  case ExpressionType::EQUALS:
  case ExpressionType::NOT_EQUALS:
  case ExpressionType::GREATER_THAN:
  case ExpressionType::GREATER_THAN_EQUALS:
  case ExpressionType::LESS_THAN:
  case ExpressionType::LESS_THAN_EQUALS:
  case ExpressionType::IS_NULL:
  case ExpressionType::IS_NOT_NULL:
  case ExpressionType::FUNCTION:
  case ExpressionType::AGGREGATE_FUNCTION:
  case ExpressionType::CASE_ELSE:
    for (const auto& child :
         binder::ExpressionChildrenCollector::collectChildren(*expression))
      if (!collectReferences(child, localOwner))
        return false;
    return true;
  default:
    // Paths, subqueries, graphs, lambdas, and future expression forms may hide
    // identity references outside ordinary children. Do not relax named-edge
    // fusion unless every reference has been checked.
    return false;
  }
}

bool ExpandGetVFusion::collectReferences(const planner::LogicalOperator& op) {
  using namespace planner;
  const auto expressions = [&](const binder::expression_vector& values,
                               const std::string& localOwner = std::string{}) {
    for (const auto& value : values)
      if (!collectReferences(value, localOwner))
        return false;
    return true;
  };
  switch (op.getOperatorType()) {
  case LogicalOperatorType::PROJECTION:
    if (!expressions(
            op.constCast<LogicalProjection>().getExpressionsToProject()))
      return false;
    break;
  case LogicalOperatorType::AGGREGATE: {
    const auto& aggregate = op.constCast<LogicalAggregate>();
    if (!expressions(aggregate.getAllKeys()) ||
        !expressions(aggregate.getAggregates()))
      return false;
    break;
  }
  case LogicalOperatorType::DISTINCT: {
    const auto& distinct = op.constCast<LogicalDistinct>();
    if (!expressions(distinct.getKeys()) ||
        !expressions(distinct.getPayloads()))
      return false;
    break;
  }
  case LogicalOperatorType::FILTER:
    if (!collectReferences(op.constCast<LogicalFilter>().getPredicate()))
      return false;
    break;
  case LogicalOperatorType::ORDER_BY:
    if (!expressions(op.constCast<LogicalOrderBy>().getExpressionsToOrderBy()))
      return false;
    break;
  case LogicalOperatorType::UNWIND:
    if (!collectReferences(op.constCast<LogicalUnwind>().getInExpr()))
      return false;
    break;
  case LogicalOperatorType::LIMIT: {
    const auto& limit = op.constCast<LogicalLimit>();
    if (!collectReferences(limit.getSkipNum()) ||
        !collectReferences(limit.getLimitNum()))
      return false;
    break;
  }
  case LogicalOperatorType::HASH_JOIN: {
    const auto& join = op.constCast<LogicalHashJoin>();
    for (const auto& [left, right] : join.getJoinConditions())
      if (!collectReferences(left) || !collectReferences(right))
        return false;
    if (!collectReferences(join.getMark()) ||
        !expressions(join.getExpressionsToMaterialize()))
      return false;
    break;
  }
  case LogicalOperatorType::INTERSECT: {
    const auto& intersect = op.constCast<LogicalIntersect>();
    if (!collectReferences(intersect.getIntersectNodeID()) ||
        !expressions(intersect.getKeyNodeIDs()))
      return false;
    break;
  }
  case LogicalOperatorType::EXTEND: {
    const auto& extend = op.constCast<LogicalExtend>();
    const auto owner = extend.getRel()->getUniqueName();
    if (!collectReferences(extend.getPredicates(), owner) ||
        !expressions(extend.getProperties(), owner))
      return false;
    break;
  }
  case LogicalOperatorType::GET_V: {
    const auto& getV = op.constCast<LogicalGetV>();
    const auto& nodeID = getV.getNodeID();
    if (!nodeID || nodeID->expressionType != common::ExpressionType::PROPERTY)
      return false;
    const auto owner =
        nodeID->constCast<binder::PropertyExpression>().getVariableName();
    if (!collectReferences(getV.getPredicates(), owner) ||
        !expressions(getV.getProperties(), owner))
      return false;
    // The adjacent GetV consumes an edge only for its endpoint and disappears
    // together with it. Other endpoint readers still require the edge value.
    if (op.getChild(0)->getOperatorType() != LogicalOperatorType::EXTEND ||
        op.getChild(0)->constCast<LogicalExtend>().getRel()->getUniqueName() !=
            getV.getStartAliasName())
      referencedVariables.insert(getV.getStartAliasName());
    break;
  }
  case LogicalOperatorType::SCAN_NODE_TABLE: {
    const auto& scan = op.constCast<LogicalScanNodeTable>();
    const auto& nodeID = scan.getNodeID();
    if (!nodeID || nodeID->expressionType != common::ExpressionType::PROPERTY)
      return false;
    const auto owner =
        nodeID->constCast<binder::PropertyExpression>().getVariableName();
    if (!collectReferences(scan.getPredicates(), owner) ||
        !expressions(scan.getProperties(), owner))
      return false;
    if (const auto* extra = scan.getExtraInfo()) {
      const auto* primary = dynamic_cast<const PrimaryKeyScanInfo*>(extra);
      if (!primary || !collectReferences(primary->key))
        return false;
    }
    break;
  }
  case LogicalOperatorType::NODE_LABEL_FILTER:
    if (!collectReferences(op.constCast<LogicalNodeLabelFilter>().getNodeID()))
      return false;
    break;
  case LogicalOperatorType::CROSS_PRODUCT:
  case LogicalOperatorType::EMPTY_RESULT:
    break;
  default:
    // Preserve the existing behavior for opaque pipeline/update operators.
    return false;
  }
  for (const auto& child : op.getChildren())
    if (!collectReferences(*child))
      return false;
  return true;
}

std::shared_ptr<planner::LogicalOperator> ExpandGetVFusion::visitOperator(
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
ExpandGetVFusion::visitRecursiveExtendReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
  NEUG_ASSERT(op->getOperatorType() ==
              planner::LogicalOperatorType::RECURSIVE_EXTEND);
  auto extendOp = op->ptrCast<planner::LogicalRecursiveExtend>();
  if (extendOp->getResultOpt() == planner::ResultOpt::ALL_V_E) {
    // 'ALL_V_E' means edge must be kept independently, so we cannot fuse
    return op;
  }
  auto recursiveInfo = extendOp->getRel()->getRecursiveInfo();
  // check if the base node need to filter by predicates, if exist, we cannot
  // fuse.
  if (recursiveInfo && recursiveInfo->nodePredicate) {
    return op;
  }

  // check if the base node need to filter by label, if exist, we cannot fuse.
  if (hasLabelFiltering(gopt::GNodeType{*extendOp->getNbrNode()},
                        gopt::GRelType{*extendOp->getRel()},
                        gopt::GNodeType{*extendOp->getBoundNode()},
                        extendOp->getDirection(), catalog)) {
    return op;
  }
  // todo: handle the case of EXPANDV_GETV
  extendOp->setFusionType(neug::optimizer::FusionType::EXPANDV);
  return op;
}

std::shared_ptr<planner::LogicalOperator> ExpandGetVFusion::visitGetVReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
  NEUG_ASSERT(op->getOperatorType() == planner::LogicalOperatorType::GET_V);
  auto child = op->getChild(0);
  if (child->getOperatorType() != planner::LogicalOperatorType::EXTEND) {
    return op;  // no extend operator, nothing to fuse
  }
  auto fusionType = analyze(op, child);
  return perform(op, child, fusionType);
}

FusionType ExpandGetVFusion::analyze(
    std::shared_ptr<planner::LogicalOperator> getV,
    std::shared_ptr<planner::LogicalOperator> expand) {
  auto getVOp = getV->constPtrCast<planner::LogicalGetV>();
  auto expandOp = expand->constPtrCast<planner::LogicalExtend>();
  if (!expandOp->getProperties().empty()) {
    // The edge itself has properties required by later operators. Fusing into
    // ExpandV would only keep the adjacent vertex and drop the edge alias.
    return FusionType::EXPANDE_GETV;
  }
  auto alias = expandOp->getGAliasName();
  if (alias.queryName.has_value()) {
    // Both user text and implicit index replacement can name a relationship
    // that no expression reads. Keeping a name alone must not force edge
    // materialization, but retain every identity/property-dependent case.
    if (!referencesComplete ||
        getVOp->getStartAliasName() != expandOp->getRel()->getUniqueName() ||
        referencedVariables.contains(expandOp->getRel()->getUniqueName()) ||
        referencedVariables.contains(gopt::DEFAULT_ALIAS_NAME) ||
        expandOp->getPredicates() ||
        !expandOp->getPropertyPredicates().empty() || expandOp->isOptional())
      return FusionType::EXPANDE_GETV;
  }
  auto nbrExpr = expandOp->getNbrNode();
  auto getVAlias = getVOp->getGAliasName();
  // the start alias of getV is not aligned with the expand, cannot be fused;
  if (getVOp->getNodeID()->getUniqueName() !=
      nbrExpr->getInternalID()->getUniqueName()) {
    LOG(WARNING) << "Alias name of GetV is not consistent with Expand, skip "
                    "'ExpandGetVFusion' optimization";
    return FusionType::EXPANDE_GETV;
  }
  // todo: support EXTANDV_GETV
  return hasGetVFiltering(getV, expand) ? FusionType::EXPANDE_GETV
                                        : FusionType::EXPANDV;  // can be fused
}

bool ExpandGetVFusion::hasGetVFiltering(
    std::shared_ptr<planner::LogicalOperator> getV,
    std::shared_ptr<planner::LogicalOperator> expand) {
  auto getVOp = getV->constPtrCast<planner::LogicalGetV>();
  auto expandOp = expand->constPtrCast<planner::LogicalExtend>();
  if (getVOp->getPredicates() != nullptr)
    return true;
  auto getVType = getVOp->getNodeType(catalog);
  auto expandType = expandOp->getRelType();
  gopt::GNodeType sourceType(*expandOp->getBoundNode());
  return hasLabelFiltering(*getVType, *expandType, sourceType,
                           expandOp->getDirection(), catalog);
}

// the expand_edge in physical pb only support filtering edge labels by edge
// type itself, we need to lookup the catalog to match all triplets <src, edge,
// dst> by the expandType, thus can determine whether label filtering in getV is
// necessary or not.
gopt::GRelType ExpandGetVFusion::transformExpandType(
    const gopt::GRelType& expandType, catalog::Catalog* catalog) {
  auto& transaction = Constants::DEFAULT_TRANSACTION;
  std::vector<common::table_id_t> targetLabels =
      std::move(expandType.getLabelIds());
  std::vector<catalog::GRelTableCatalogEntry*> transformEntries;
  for (auto& entry : catalog->getTableEntries(&transaction)) {
    if (entry->getType() == catalog::CatalogEntryType::REL_TABLE_ENTRY) {
      auto edgeEntry = entry->ptrCast<catalog::GRelTableCatalogEntry>();
      if (std::find(targetLabels.begin(), targetLabels.end(),
                    edgeEntry->getLabelId()) != targetLabels.end()) {
        // if the edge label is in the expand type, we keep it
        transformEntries.emplace_back(edgeEntry);
      }
    }
  }
  return gopt::GRelType(std::move(transformEntries));
}

bool ExpandGetVFusion::hasLabelFiltering(const gopt::GNodeType& getVType,
                                         const gopt::GRelType& expandType,
                                         const gopt::GNodeType& sourceType,
                                         common::ExtendDirection direction,
                                         catalog::Catalog* catalog) {
  std::vector<common::table_id_t> targetLabels;
  auto transformType = transformExpandType(expandType, catalog);
  for (auto& edge : transformType.relTables) {
    for (auto& node : sourceType.nodeTables) {
      if (direction != common::ExtendDirection::BWD &&
          node->getTableID() == edge->getSrcTableID()) {
        targetLabels.push_back(edge->getDstTableID());
      } else if (direction != common::ExtendDirection::FWD &&
                 node->getTableID() == edge->getDstTableID()) {
        targetLabels.push_back(edge->getSrcTableID());
      }
    }
  }
  std::vector<common::table_id_t> getVLabels;
  for (auto& node : getVType.nodeTables) {
    getVLabels.push_back(node->getTableID());
  }
  for (auto& targetLabel : targetLabels) {
    if (std::find(getVLabels.begin(), getVLabels.end(), targetLabel) ==
        getVLabels.end()) {
      return true;  // at least one label in expand is not in getV
    }
  }
  return false;  // all labels in expand are in getV
}

std::shared_ptr<planner::LogicalOperator> ExpandGetVFusion::perform(
    std::shared_ptr<planner::LogicalOperator> getV,
    std::shared_ptr<planner::LogicalOperator> expand, FusionType fusionType) {
  auto expandOp = expand->ptrCast<planner::LogicalExtend>();
  auto getVOp = getV->ptrCast<planner::LogicalGetV>();
  switch (fusionType) {
  case EXPANDV_GETV:
    // for this condition, expandOp and getVOp will have the same aliasId,
    // this will lead execution issues, so we currently do not support this
    // case.
    // todo: set alias of expand as empty
    expandOp->setExtendOpt(planner::ExtendOpt::VERTEX);
    getVOp->setGetVOpt(planner::GetVOpt::ITSELF);
    return getV;
  case EXPANDV:
    expandOp->setExtendOpt(planner::ExtendOpt::VERTEX);
    // getV has been fused into expandV, let expand as the new operator
    return expand;
  default:
    return getV;  // EXPANDE_GETV, no fusion, return original getV
  }
}

}  // namespace optimizer
}  // namespace neug

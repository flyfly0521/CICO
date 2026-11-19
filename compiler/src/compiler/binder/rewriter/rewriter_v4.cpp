#include "neug/compiler/binder/rewriter/fill_predicate_signature.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/compiler/binder/rewriter/distinct_fill_shape.h"
#include "neug/compiler/binder/rewriter/endpoint_projection_contract.h"
#include "neug/compiler/binder/rewriter/rewriter_v4.h"
#include "neug/compiler/binder/rewriter/bound_rewrite_trace.h"
#include "neug/compiler/binder/rewriter/complete_key_group_rewriter.h"
#include "neug/compiler/binder/rewriter/aggregate_endpoint_set_rewriter.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <map>
#include <bitset>
#include <optional>
#include <queue>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "glog/logging.h"
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/bound_scan_source.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/expression_util.h"
#include "neug/compiler/binder/expression/index_rewrite_template_matcher.h"
#include "neug/compiler/binder/expression/index_predicate_key.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/normalized_query_part.h"
#include "neug/compiler/binder/query/query_graph.h"
#include "neug/compiler/binder/query/query_graph_label_analyzer.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/query/reading_clause/bound_unwind_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/common/assert.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/copier_config/file_scan_info.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/common/types/value/value.h"
#include "neug/compiler/function/built_in_function_utils.h"
#include "neug/compiler/function/schema/vector_node_rel_functions.h"
#include "neug/compiler/function/struct/vector_struct_functions.h"
#include "neug/compiler/common/vector/value_vector.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/index/bound_index_candidate_adapter.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/parser/parser.h"
#include "neug/compiler/parser/expression/parsed_literal_expression.h"
#include "neug/utils/exception/exception.h"
using namespace neug::common;
using namespace neug::catalog;
namespace neug {
namespace binder {

static void filterAllowedRewriteTargets(std::vector<IndexCatalogEntry*>& indexEntries);

namespace {

using RelLabelMap = std::unordered_map<table_id_t, std::string>;

// A FILL target may name a REL group (one FILL routed to several child
// tables, or a FILL into one child of a group); a group is not a table entry
// and Catalog::getTableCatalogEntry throws for it.  Callers treat nullptr as
// "no single table" and fall back to their conservative answer.
template <typename CatalogT, typename TransactionT>
static auto* tableByName(CatalogT* catalog, TransactionT* transaction, const std::string& name,
                         bool useInternal = true) {
  using Entry = decltype(catalog->getTableCatalogEntry(transaction, name, useInternal));
  if (catalog == nullptr || !catalog->containsTable(transaction, name, useInternal)) return Entry{nullptr};
  return catalog->getTableCatalogEntry(transaction, name, useInternal);
}

static std::string tableLabel(const catalog::TableCatalogEntry* entry,
                                     const RelLabelMap& labels) {
  const auto found = labels.find(entry->getTableID());
  return found == labels.end() ? entry->getName() : found->second;
}

static std::shared_ptr<Expression> deepCopyShared(const std::shared_ptr<Expression>& expr) {
  if (expr == nullptr) {
    return nullptr;
  }
  auto copied = expr->copy();
  return std::shared_ptr<Expression>(copied.release());
}

static std::string deferredPropertyKey(const std::string& variableName,
                                       const std::string& propertyName) {
  return variableName + '\x1f' + propertyName;
}

static std::string canonicalizePredicateForMatchedIndex(
    const Expression& predicate, const PatternIR& queryIR,
    const PatternIR& indexIR) {
  try {
    if (auto exists = extractFillExistsPredicate(predicate, queryIR)) {
      const auto mapNode = [&](uint64_t queryPos) -> uint64_t {
        for (size_t i = 0; i < indexIR.nodesInfo.size(); ++i) {
          if (indexIR.nodesInfo[i].matchedIdx == queryPos) return i;
        }
        return UINT64_MAX;
      };
      exists->srcNodePos = mapNode(exists->srcNodePos);
      exists->dstNodePos = mapNode(exists->dstNodePos);
      if (exists->srcNodePos == UINT64_MAX || exists->dstNodePos == UINT64_MAX)
        return "@unmatched-graph-predicate";
      return fillExistsPredicateKey(std::move(*exists));
    }
  } catch (const std::exception&) {
    // Unsupported query predicates must decline a substitution, not make an
    // otherwise valid original query fail to bind.
    return "@unmatched-graph-predicate";
  }
  IndexPredicateBindings bindings;
  for (size_t i = 0; i < indexIR.nodesInfo.size(); ++i) {
    const auto queryPos = indexIR.nodesInfo[i].matchedIdx;
    if (queryPos < queryIR.nodesInfo.size()) {
      bindings.emplace(queryIR.nodesInfo[queryPos].uniqName,
                       "node:" + std::to_string(i));
    }
  }
  for (size_t i = 0; i < indexIR.relsInfo.size(); ++i) {
    const auto queryPos = indexIR.relsInfo[i].matchedIdx;
    if (queryPos < queryIR.relsInfo.size()) {
      bindings.emplace(queryIR.relsInfo[queryPos].uniqName,
                       "rel:" + std::to_string(i));
    }
  }
  return makeIndexPredicateKey(predicate, bindings)
      .value_or("@unmatched-typed-predicate");
}

static bool predicateCoveredByFill(
    const Expression& predicate, const PatternIR& queryIR,
    const PatternIR& indexIR,
    const std::vector<std::string>& coveredPredicateTemplates) {
  const auto canonical =
      canonicalizePredicateForMatchedIndex(predicate, queryIR, indexIR);
  if (std::find(coveredPredicateTemplates.begin(), coveredPredicateTemplates.end(), canonical) !=
      coveredPredicateTemplates.end()) return true;
  auto signature = fillPredicateSignature(predicate, queryIR, true,
      main::MetadataRegistry::getCatalog(), &neug::Constants::DEFAULT_TRANSACTION);
  return signature && std::find(coveredPredicateTemplates.begin(),
      coveredPredicateTemplates.end(), *signature) != coveredPredicateTemplates.end();
}

// These persisted values describe a complete group, not additive partial
// statistics. In particular the old SUM lowering cannot consume AVG, DISTINCT,
// extrema or floating SUM after removing only a local portion of a query.
static bool requiresCompleteAggregateRead(const FillIndexAuxInfo& info,
                                         main::ClientContext* context) {
  bool hasSum = false;
  for (const auto& column : info.getIndexColumnInfos()) {
    // Full-source values may include OPTIONAL clauses and staged finalizers.
    // Their merged PatternIR is dependency metadata, not an additive local
    // MATCH contract. Only complete-source/group proofs may consume them.
    if (column->columnType == IndexColumnType::SOURCE_QUERY_VALUE ||
        (info.requiresSourceRecompute() &&
         column->columnType == IndexColumnType::REL_COMPUATION_WITH_AGGREGATION))
      return true;
    std::string function, spelling;
    if (column->columnType == IndexColumnType::NODE_AGGREGATE) {
      const auto& aggregate = static_cast<const NodeAggregationIndexColnumInfo&>(*column);
      function = aggregate.aggregateFuncName;
      spelling = aggregate.nameTemplate;
    } else if (column->columnType == IndexColumnType::REL_AGGREGATE) {
      const auto& aggregate = static_cast<const RelIndexColnumInfo&>(*column);
      function = aggregate.aggregateFuncName;
      spelling = aggregate.nameTemplate;
    } else {
      continue;
    }
    for (auto* value : {&function, &spelling})
      std::transform(value->begin(), value->end(), value->begin(),
                     [](unsigned char c) { return std::toupper(c); });
    if (spelling.find("DISTINCT") != std::string::npos ||
        (!function.empty() && function != "COUNT" && function != "COUNT_STAR" && function != "SUM") ||
        (function.empty() && (spelling.find("AVG(") != std::string::npos ||
                             spelling.find("MIN(") != std::string::npos ||
                             spelling.find("MAX(") != std::string::npos))) return true;
    hasSum |= function == "SUM" || spelling.find("SUM(") != std::string::npos;
  }
  if (!hasSum) return false;
  const auto* table = tableByName(context->getCatalog(), context->getTransaction(), info.getTableName());
  if (table == nullptr) return true;  // unknown carrier layout: require the complete-read proof
  for (const auto& name : info.getTargetColumns()) {
    if (!table->containsProperty(name)) continue;
    const auto type = table->getProperty(name).getType().getLogicalTypeID();
    if (type == LogicalTypeID::FLOAT || type == LogicalTypeID::DOUBLE) return true;
  }
  return false;
}

// Only a public row count describes the number of source rows removed by a
// local replacement. A nullable COUNT(expr), DISTINCT count, SUM, or private
// maintenance count is not interchangeable with COUNT(*).
static bool isPublicRowCount(const FillIndexAuxInfo& info, size_t column) {
  if ((info.getFillType() != FillEntryType::NODE && info.getFillType() != FillEntryType::REL) ||
      column >= info.getIndexColumnInfos().size() ||
      column >= info.getTargetColumns().size()) return false;
  const auto& target = info.getTargetColumns()[column];
  if (target.starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
      target.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) return false;
  const auto& metadata = info.getIndexColumnInfos()[column];
  if (metadata == nullptr) return false;
  std::string spelling, function;
  if (info.getFillType() == FillEntryType::NODE && metadata->columnType == IndexColumnType::NODE_AGGREGATE) {
    const auto& aggregate = static_cast<const NodeAggregationIndexColnumInfo&>(*metadata);
    spelling = aggregate.nameTemplate;
    function = aggregate.aggregateFuncName;
  } else if (info.getFillType() == FillEntryType::REL && metadata->columnType == IndexColumnType::REL_AGGREGATE) {
    const auto& aggregate = static_cast<const RelIndexColnumInfo&>(*metadata);
    spelling = aggregate.nameTemplate;
    function = aggregate.aggregateFuncName;
  } else {
    return false;
  }
  std::erase_if(spelling, [](unsigned char ch) { return std::isspace(ch); });
  std::transform(spelling.begin(), spelling.end(), spelling.begin(),
                 [](unsigned char ch) { return std::toupper(ch); });
  std::transform(function.begin(), function.end(), function.begin(),
                 [](unsigned char ch) { return std::toupper(ch); });
  return spelling == "COUNT(*)" && (function == "COUNT" || function == "COUNT_STAR");
}

static bool sameCountProperty(const std::shared_ptr<Expression>& left,
                              const std::shared_ptr<Expression>& right) {
  if (!left || !right || left->expressionType != ExpressionType::PROPERTY ||
      right->expressionType != ExpressionType::PROPERTY) return false;
  const auto& a = left->constCast<PropertyExpression>();
  const auto& b = right->constCast<PropertyExpression>();
  return a.getDataType().getLogicalTypeID() == LogicalTypeID::INT64 &&
      b.getDataType().getLogicalTypeID() == LogicalTypeID::INT64 &&
      a.isSingleLabel() && b.isSingleLabel() &&
      a.getSingleTableID() == b.getSingleTableID() &&
      a.getVariableName() == b.getVariableName() &&
      a.getPropertyName() == b.getPropertyName();
}

static bool matchesCountProduct(const std::shared_ptr<Expression>& expression,
                               const expression_vector& factors) {
  expression_vector leaves;
  std::function<bool(const std::shared_ptr<Expression>&)> collect = [&](const auto& e) {
    if (!e) return false;
    if (e->expressionType == ExpressionType::PROPERTY) {
      leaves.push_back(e);
      return true;
    }
    return e->expressionType == ExpressionType::FUNCTION && e->getNumChildren() == 2 &&
        e->template constCast<ScalarFunctionExpression>().getFunction().name == "*" &&
        collect(e->getChild(0)) && collect(e->getChild(1));
  };
  if (!collect(expression) || leaves.size() != factors.size()) return false;
  for (size_t i = 0; i < leaves.size(); ++i)
    if (!sameCountProperty(leaves[i], factors[i])) return false;
  return true;
}

// This value is evaluated on retained rows. An INT64 SUM distributes over
// repeated rows in the execution engine's wrapping arithmetic domain; MIN/MAX
// simply ignore their positive multiplicity. Admit only deterministic, total
// expressions so removing repeated evaluations cannot hide errors or change
// side effects. Floating arithmetic, division, casts, subqueries and arbitrary
// functions require separate proofs. The caller checks every binding survives.
static bool isTotalRetainedCaseCondition(const std::shared_ptr<Expression>& condition);

static bool isTotalStringRowExpression(const std::shared_ptr<Expression>& expression) {
  if (expression == nullptr ||
      expression->getDataType().getLogicalTypeID() != LogicalTypeID::STRING) return false;
  if (expression->expressionType == ExpressionType::LITERAL) return true;
  if (expression->expressionType == ExpressionType::PROPERTY)
    return expression->constPtrCast<PropertyExpression>()->isSingleLabel();
  if (expression->expressionType != ExpressionType::CASE_ELSE) return false;
  const auto* guarded = expression->constPtrCast<CaseExpression>();
  if (!isTotalStringRowExpression(guarded->getElseExpression())) return false;
  for (uint32_t i = 0; i < guarded->getNumCaseAlternatives(); ++i) {
    const auto* alternative = guarded->getCaseAlternative(i);
    if (!isTotalRetainedCaseCondition(alternative->whenExpression) ||
        !isTotalStringRowExpression(alternative->thenExpression)) return false;
  }
  return true;
}

static bool isTotalInt64RowExpression(const std::shared_ptr<Expression>& expression) {
  if (expression == nullptr ||
      expression->getDataType().getLogicalTypeID() != LogicalTypeID::INT64) return false;
  if (expression->expressionType == ExpressionType::LITERAL) return true;
  if (expression->expressionType == ExpressionType::PROPERTY)
    return expression->constPtrCast<PropertyExpression>()->isSingleLabel();
  if (expression->expressionType == ExpressionType::FUNCTION) {
    const auto& name = expression->constPtrCast<ScalarFunctionExpression>()->getFunction().name;
    if ((name != "*" && name != "+" && name != "-") || expression->getNumChildren() != 2)
      return false;
    return isTotalInt64RowExpression(expression->getChild(0)) &&
           isTotalInt64RowExpression(expression->getChild(1));
  }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    const auto* guarded = expression->constPtrCast<CaseExpression>();
    if (!isTotalInt64RowExpression(guarded->getElseExpression())) return false;
    for (uint32_t i = 0; i < guarded->getNumCaseAlternatives(); ++i) {
      const auto* alternative = guarded->getCaseAlternative(i);
      if (!isTotalRetainedCaseCondition(alternative->whenExpression) ||
          !isTotalInt64RowExpression(alternative->thenExpression)) return false;
    }
    return true;
  }
  return false;
}

static bool isTotalRetainedCaseCondition(const std::shared_ptr<Expression>& condition) {
  if (condition == nullptr || condition->getDataType().getLogicalTypeID() != LogicalTypeID::BOOL)
    return false;
  // Binding may fold IS_NULL('constant') into an ordinary BOOL literal.
  if (condition->expressionType == ExpressionType::LITERAL) return true;
  if (condition->getNumChildren() == 1 &&
      (condition->expressionType == ExpressionType::IS_NULL ||
       condition->expressionType == ExpressionType::IS_NOT_NULL)) {
    // COUNT(nullable STRING) becomes a complete executable integer CASE. A
    // later region can weight it without provenance from the first rewrite.
    return isTotalInt64RowExpression(condition->getChild(0)) ||
           isTotalStringRowExpression(condition->getChild(0));
  }
  if (condition->getNumChildren() == 2 &&
      (condition->expressionType == ExpressionType::EQUALS ||
       condition->expressionType == ExpressionType::NOT_EQUALS)) {
    return isTotalInt64RowExpression(condition->getChild(0)) &&
           isTotalInt64RowExpression(condition->getChild(1));
  }
  return false;
}

static std::shared_ptr<PropertyExpression> makeFillPropertyExpressionImpl(
    catalog::TableCatalogEntry* fillTableEntry, table_id_t fillTableId,
    const std::string& varUniqueName, const std::string& rawVariableName,
    const std::string& propertyName) {
  auto propType = fillTableEntry->getProperty(propertyName).getType().copy();
  table_id_map_t<SingleLabelPropertyInfo> infos;
  infos.insert({fillTableId,
                SingleLabelPropertyInfo(true /* exists */,
                                        false /* isPrimaryKey */)});
  return std::make_shared<PropertyExpression>(
      std::move(propType), propertyName, varUniqueName, rawVariableName,
      std::move(infos));
}

static std::shared_ptr<VariableExpression> makeProjectionAliasReference(
    const Expression& projectedExpr, const std::string& alias) {
  auto aliasRef = std::make_shared<VariableExpression>(
      projectedExpr.getDataType().copy(), projectedExpr.getUniqueName(), alias);
  aliasRef->setAlias(alias);
  return aliasRef;
}

static std::shared_ptr<AggregateFunctionExpression> makeValueAggregateExpression(
    main::ClientContext* clientContext, std::shared_ptr<Expression> child,
    const std::string& alias, const std::string& parsedName,
    std::vector<std::string> rawNameVariables,
    const std::string& functionName = "SUM", bool isDistinct = false) {
  expression_vector children;
  children.push_back(std::move(child));
  std::vector<LogicalType> childTypes;
  childTypes.push_back(children[0]->getDataType().copy());
  auto* functionEntry = clientContext->getCatalog()->getFunctionEntry(
      clientContext->getTransaction(), functionName);
  auto function =
      function::BuiltInFunctionsUtils::matchAggregateFunction(
          functionName, childTypes, isDistinct,
          functionEntry->ptrCast<catalog::FunctionCatalogEntry>())
          ->copy();
  std::unique_ptr<function::FunctionBindData> bindData;
  if (function.bindFunc) {
    auto bindInput = function::ScalarBindFuncInput{
        children, &function, clientContext,
        std::vector<std::string>{} /* optionalArguments */};
    bindData = function.bindFunc(bindInput);
  } else {
    bindData =
        std::make_unique<function::FunctionBindData>(
            LogicalType(function.returnTypeID));
  }
  auto uniqueName = AggregateFunctionExpression::getUniqueName(
      function.name, children, function.isDistinct);
  auto aggregate = std::make_shared<AggregateFunctionExpression>(
      std::move(function), std::move(bindData), std::move(children),
      std::move(uniqueName));
  aggregate->setParsedName(parsedName);
  aggregate->setRawNameVariables(std::move(rawNameVariables));
  if (!alias.empty()) {
    aggregate->setAlias(alias);
  }
  return aggregate;
}

static std::shared_ptr<Expression> makeZeroIfNullExpression(
    Binder* binder, std::shared_ptr<Expression> expr) {
  if (binder == nullptr || expr == nullptr) {
    return expr;
  }
  auto zero = Value::createDefaultValue(expr->getDataType());
  auto zeroExpr =
      std::make_shared<LiteralExpression>(zero, zero.toString());
  expression_vector isNullChildren;
  isNullChildren.push_back(deepCopyShared(expr));
  auto isNull = binder->getExpressionBinder()->bindNullOperatorExpression(
      ExpressionType::IS_NULL, isNullChildren);
  const auto uniqueName = "idx_coalesce_zero(" + expr->getUniqueName() + ")";
  auto result = std::make_shared<CaseExpression>(
      expr->getDataType().copy(), std::move(expr), uniqueName);
  result->addCaseAlternative(std::move(isNull), std::move(zeroExpr));
  return result;
}

static bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::optional<std::pair<std::string, std::string>>
directionAwareCounterpartColumns(const catalog::TableCatalogEntry& tableEntry,
                                 const std::string& propertyName) {
  constexpr std::string_view fwdSuffix{"_fwd"};
  constexpr std::string_view revSuffix{"_rev"};
  if (endsWith(propertyName, std::string(fwdSuffix))) {
    auto base = propertyName.substr(0, propertyName.size() - fwdSuffix.size());
    auto reverse = base + std::string(revSuffix);
    if (tableEntry.containsProperty(reverse)) {
      return std::make_pair(propertyName, reverse);
    }
  }
  if (endsWith(propertyName, std::string(revSuffix))) {
    auto base = propertyName.substr(0, propertyName.size() - revSuffix.size());
    auto forward = base + std::string(fwdSuffix);
    if (tableEntry.containsProperty(forward)) {
      return std::make_pair(forward, propertyName);
    }
  }
  return std::nullopt;
}

static std::shared_ptr<Expression> makeDirectionAwareRelPropertyExpression(
    Binder* binder, catalog::TableCatalogEntry* fillTableEntry,
    table_id_t fillTableID, const std::string& varUniqueName,
    const std::string& rawVariableName, const std::string& propertyName,
    const std::string& forwardPropertyName,
    const std::string& reversePropertyName, bool directionAwareSameLabelRel,
    const std::shared_ptr<RelExpression>& targetRel,
    const std::string& directionReferenceNodeUniqueName,
    const std::string& directionReferenceNodeRawName) {
  if (binder == nullptr || targetRel == nullptr ||
      !directionAwareSameLabelRel || forwardPropertyName.empty() ||
      reversePropertyName.empty()) {
    return makeFillPropertyExpressionImpl(fillTableEntry, fillTableID,
                                          varUniqueName, rawVariableName,
                                          propertyName);
  }

  auto forward = makeFillPropertyExpressionImpl(
      fillTableEntry, fillTableID, varUniqueName, rawVariableName,
      forwardPropertyName);
  auto reverse = makeFillPropertyExpressionImpl(
      fillTableEntry, fillTableID, varUniqueName, rawVariableName,
      reversePropertyName);

  expression_vector startNodeArgs;
  startNodeArgs.push_back(targetRel);
  startNodeArgs.push_back(
      binder->getExpressionBinder()->createLiteralExpression(
          InternalKeyword::SRC));
  auto startNode = binder->getExpressionBinder()->bindScalarFunctionExpression(
      startNodeArgs, function::StructExtractFunctions::name);
  auto referenceNode = targetRel->getSrcNode();
  const auto matchesDirectionReference =
      [&](const std::shared_ptr<NodeExpression>& node) {
        return node != nullptr &&
               ((!directionReferenceNodeUniqueName.empty() &&
                 node->getUniqueName() ==
                     directionReferenceNodeUniqueName) ||
                (!directionReferenceNodeRawName.empty() &&
                 node->getVariableName() == directionReferenceNodeRawName));
      };
  if (!matchesDirectionReference(referenceNode) &&
      matchesDirectionReference(targetRel->getDstNode())) {
    referenceNode = targetRel->getDstNode();
  }
  auto isForward =
      binder->getExpressionBinder()->createEqualityComparisonExpression(
          std::move(startNode), referenceNode);
  const auto uniqueName = "idx_direction_aware_rel_property(" +
                          forward->getUniqueName() + "," +
                          reverse->getUniqueName() + "," +
                          referenceNode->getUniqueName() + ")";
  auto result = std::make_shared<CaseExpression>(
      forward->getDataType().copy(), std::move(reverse),
      uniqueName);
  result->addCaseAlternative(std::move(isForward), std::move(forward));
  return result;
}

using ExpressionRewriteRule = std::function<bool(std::shared_ptr<Expression>&)>;

static void rewriteExpressionTree(std::shared_ptr<Expression>& expr,
                                  const ExpressionRewriteRule& rule) {
  if (expr == nullptr) {
    return;
  }
  if (rule(expr) || expr == nullptr) {
    return;
  }

  if (expr->expressionType == ExpressionType::CASE_ELSE) {
    auto* caseExpr = expr->ptrCast<CaseExpression>();
    if (caseExpr != nullptr) {
      for (auto i = 0u; i < caseExpr->getNumCaseAlternatives(); ++i) {
        auto* alt = caseExpr->getCaseAlternative(i);
        if (alt == nullptr) {
          continue;
        }
        rewriteExpressionTree(alt->whenExpression, rule);
        rewriteExpressionTree(alt->thenExpression, rule);
      }
      auto elseExpr = caseExpr->getElseExpression();
      rewriteExpressionTree(elseExpr, rule);
      caseExpr->setElseExpression(std::move(elseExpr));
    }
    return;
  }

  for (auto i = 0u; i < expr->getNumChildren(); ++i) {
    auto child = expr->getChild(i);
    rewriteExpressionTree(child, rule);
    expr->setChild(i, std::move(child));
  }
}

static std::unordered_set<std::string> collectPropertyVarUniqueNames(
    const std::shared_ptr<Expression>& expr) {
  std::unordered_set<std::string> vars;
  if (expr == nullptr) {
    return vars;
  }
  PropertyExprCollector collector;
  collector.visit(expr);
  for (auto& p : collector.getPropertyExprs()) {
    if (p == nullptr || p->expressionType != ExpressionType::PROPERTY) {
      continue;
    }
    auto* prop = p->ptrCast<PropertyExpression>();
    if (prop == nullptr) {
      continue;
    }
    vars.insert(prop->getVariableName());
  }
  return vars;
}

static std::unordered_set<std::string> collectDependentVarNames(
    const std::shared_ptr<Expression>& expr) {
  std::unordered_set<std::string> vars;
  if (expr == nullptr) {
    return vars;
  }
  DependentVarNameCollector collector;
  collector.visit(expr);
  return collector.getVarNames();
}

static void collectAggregateUniqueNames(const std::shared_ptr<Expression>& expr,
                                        std::unordered_set<std::string>& out) {
  if (expr == nullptr) {
    return;
  }
  if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    out.insert(expr->getUniqueName());
    out.insert(expr->rawName());
  }
  for (auto& child : ExpressionChildrenCollector::collectChildren(*expr)) {
    collectAggregateUniqueNames(child, out);
  }
}

static bool validateRetainedVariablesForRelIndexRewrite(
    const std::shared_ptr<Expression>& expr, PatternIR& nodesIR,
    const std::unordered_set<std::string>* retainedRelUniqueNames) {
  auto varNames = collectDependentVarNames(expr);
  if (varNames.empty()) {
    return true;
  }

  for (const auto& [relUniqueName, relPos] : nodesIR.relUniqueNameToPos) {
    // The query may keep an unmatched edge beside the materialized source.
    // Its identity and properties are still evaluated on the original edge.
    if (relPos >= nodesIR.relsInfo.size()) return false;
    if (nodesIR.relsInfo[relPos].matchedIdx == UINT64_MAX) continue;
    if (varNames.contains(relUniqueName)) {
      if (retainedRelUniqueNames != nullptr &&
          retainedRelUniqueNames->contains(relUniqueName)) {
        continue;
      }
      return false;
    }
  }
  return true;
}

static bool isInternalPatternProperty(const PropertyExpression& propExpr) {
  const auto& propertyName = propExpr.getPropertyName();
  return propertyName == common::InternalKeyword::ID ||
         propertyName == common::InternalKeyword::LABEL ||
         propertyName == common::InternalKeyword::SRC ||
         propertyName == common::InternalKeyword::DST ||
         propertyName == common::InternalKeyword::DIRECTION;
}

static bool referencesRemovedPatternVariables(
    const std::shared_ptr<Expression>& expr, PatternIR& nodesIR,
    uint64_t anchorQueryNodeIdx,
    const std::unordered_set<std::string>* retainedNodeUniqueNames = nullptr,
    const std::unordered_set<std::string>* retainedRelUniqueNames = nullptr) {
  if (expr == nullptr) {
    return false;
  }
  DependentVarNameCollector collector;
  collector.visit(expr);
  const auto varNames = collector.getVarNames();
  for (size_t i = 0; i < nodesIR.nodesInfo.size(); ++i) {
    const auto& nodeInfo = nodesIR.nodesInfo[i];
    if (i == anchorQueryNodeIdx || nodeInfo.matchedIdx == UINT64_MAX) {
      continue;
    }
    if (retainedNodeUniqueNames != nullptr &&
        retainedNodeUniqueNames->contains(nodeInfo.uniqName)) {
      continue;
    }
    if (varNames.contains(nodeInfo.uniqName) ||
        varNames.contains(nodeInfo.varName)) {
      return true;
    }
  }
  for (const auto& [relUniqueName, relPos] : nodesIR.relUniqueNameToPos) {
    if (relPos >= nodesIR.relsInfo.size()) return true;
    if (nodesIR.relsInfo[relPos].matchedIdx == UINT64_MAX) continue;
    if (retainedRelUniqueNames != nullptr &&
        retainedRelUniqueNames->contains(relUniqueName)) {
      continue;
    }
    if (varNames.contains(relUniqueName)) {
      return true;
    }
  }
  return false;
}

struct RewriteMatchPreRewriteInfo {
  std::unordered_set<std::string> beforePatternUniques;
  std::unordered_map<std::string, std::string> uniqueToRawVar;
  std::unordered_set<std::string> projectedPatternUniques;
};

static RewriteMatchPreRewriteInfo collectPreRewriteInfo(
    BoundMatchClause& matchClause, NormalizedQueryPart& queryPart) {
  RewriteMatchPreRewriteInfo info;

  if (const auto* collection = matchClause.getQueryGraphCollection();
      collection != nullptr) {
    const auto nodes = collection->getQueryNodes();
    const auto relationships = collection->getQueryRels();
    info.beforePatternUniques.reserve(nodes.size() + relationships.size());
    for (auto& n : nodes) {
      if (n == nullptr) {
        continue;
      }
      info.beforePatternUniques.insert(n->getUniqueName());
      info.uniqueToRawVar.emplace(n->getUniqueName(), n->getVariableName());
    }
    for (const auto& rel : relationships) {
      if (rel == nullptr) {
        continue;
      }
      info.beforePatternUniques.insert(rel->getUniqueName());
      info.uniqueToRawVar.emplace(rel->getUniqueName(),
                                  rel->getVariableName());
    }
  }

  if (queryPart.hasProjectionBody()) {
    auto* body = queryPart.getProjectionBodyUnsafe();
    for (auto& expr : body->getProjectionExpressions()) {
      if (expr != nullptr && ExpressionUtil::isNodePattern(*expr)) {
        auto* node = expr->ptrCast<NodeExpression>();
        if (node != nullptr) {
          info.projectedPatternUniques.insert(node->getUniqueName());
        }
      }
    }
  }

  return info;
}

static std::unordered_set<std::string> computeRemovedPatternUniques(
    BoundMatchClause& matchClause,
    const std::unordered_set<std::string>& beforePatternUniques) {
  std::unordered_set<std::string> afterPatternUniques;
  if (const auto* collection = matchClause.getQueryGraphCollection();
      collection != nullptr) {
    const auto nodes = collection->getQueryNodes();
    const auto relationships = collection->getQueryRels();
    afterPatternUniques.reserve(nodes.size() + relationships.size());
    for (auto& n : nodes) {
      if (n == nullptr) {
        continue;
      }
      afterPatternUniques.insert(n->getUniqueName());
    }
    for (const auto& rel : relationships) {
      if (rel != nullptr) {
        afterPatternUniques.insert(rel->getUniqueName());
      }
    }
  }

  std::unordered_set<std::string> removed;
  for (auto& u : beforePatternUniques) {
    if (!afterPatternUniques.contains(u)) {
      removed.insert(u);
    }
  }
  return removed;
}

static void prunePredicatesForRemovedPatterns(
    BoundMatchClause& matchClause,
    const std::unordered_set<std::string>& removedPatternUniqueNames,
    const std::vector<std::string>& coveredPredicateTemplates,
    const PatternIR& queryIR, const PatternIR& indexIR,
    ExpressionBinder* expressionBinder) {
  if (removedPatternUniqueNames.empty() || !matchClause.hasPredicate() ||
      expressionBinder == nullptr) {
    return;
  }
  std::shared_ptr<Expression> retainedPredicate;
  for (auto& conjunct : matchClause.getConjunctivePredicates()) {
    auto dependencies = collectDependentVarNames(conjunct);
    const auto referencesRemovedPattern = std::any_of(
        dependencies.begin(), dependencies.end(), [&](const auto& dependency) {
          return removedPatternUniqueNames.contains(dependency);
        });
    const auto coveredByFill = predicateCoveredByFill(
        *conjunct, queryIR, indexIR, coveredPredicateTemplates);
    // DependentVarNameCollector does not expose correlated EXISTS endpoints.
    // A structurally proved graph predicate is supplied by the materialized
    // input even when that collector reports no removed variables.
    const auto graphPredicate = canonicalizePredicateForMatchedIndex(
        *conjunct, queryIR, indexIR).starts_with("@exists-fill-v1:");
    if (coveredByFill && (referencesRemovedPattern || graphPredicate)) {
      continue;
    }
    retainedPredicate = expressionBinder->combineBooleanExpressions(
        ExpressionType::AND, std::move(retainedPredicate), conjunct);
  }
  matchClause.setPredicate(std::move(retainedPredicate));
}

static void addRelAndEndpoints(QueryGraph& graph,
                               const std::shared_ptr<RelExpression>& rel) {
  if (rel == nullptr) {
    return;
  }
  graph.addQueryNode(rel->getSrcNode());
  graph.addQueryNode(rel->getDstNode());
  graph.addQueryRel(rel);
}

static bool expressionHasDistinctAggregateOnVar(
    const std::shared_ptr<Expression>& expr, const std::string& varUniqueName) {
  if (expr == nullptr) {
    return false;
  }
  if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    auto* aggregate = expr->ptrCast<AggregateFunctionExpression>();
    if (aggregate != nullptr && aggregate->isDistinct()) {
      for (auto i = 0u; i < expr->getNumChildren(); ++i) {
        if (collectDependentVarNames(expr->getChild(i)).contains(varUniqueName)) {
          return true;
        }
      }
    }
  }
  for (auto i = 0u; i < expr->getNumChildren(); ++i) {
    if (expressionHasDistinctAggregateOnVar(expr->getChild(i), varUniqueName)) {
      return true;
    }
  }
  return false;
}

static bool queryPartHasDistinctAggregateOnVar(
    const NormalizedQueryPart& queryPart, const std::string& varUniqueName) {
  if (!queryPart.hasProjectionBody()) {
    return false;
  }
  const auto* projection = queryPart.getProjectionBody();
  for (auto& expr : projection->getAggregateExpressions()) {
    if (expressionHasDistinctAggregateOnVar(expr, varUniqueName)) {
      return true;
    }
  }
  for (auto& expr : projection->getProjectionExpressions()) {
    if (expressionHasDistinctAggregateOnVar(expr, varUniqueName)) {
      return true;
    }
  }
  return false;
}

static std::unordered_set<std::string> collectProjectionDependencies(
    const NormalizedQueryPart& queryPart) {
  std::unordered_set<std::string> deps;
  if (!queryPart.hasProjectionBody()) {
    return deps;
  }
  const auto* projection = queryPart.getProjectionBody();
  auto addDeps = [&deps](const expression_vector& exprs) {
    for (auto& expr : exprs) {
      for (auto& dep : collectDependentVarNames(expr)) {
        deps.insert(dep);
      }
    }
  };
  addDeps(projection->getProjectionExpressions());
  addDeps(projection->getGroupByExpressions());
  addDeps(projection->getAggregateExpressions());
  addDeps(projection->getOrderByExpressions());
  if (queryPart.hasProjectionBodyPredicate()) {
    for (auto& dep : collectDependentVarNames(queryPart.getProjectionBodyPredicate())) {
      deps.insert(dep);
    }
  }
  return deps;
}

static bool nodeHasTableID(const std::shared_ptr<NodeExpression>& node,
                           uint32_t tableID) {
  if (node == nullptr || tableID == UINT32_MAX) {
    return false;
  }
  const auto& tableIDs = node->getTableIDs();
  return std::find(tableIDs.begin(), tableIDs.end(), tableID) !=
         tableIDs.end();
}

static std::optional<std::pair<std::shared_ptr<NodeExpression>,
                               std::shared_ptr<NodeExpression>>>
getDirectedFillRelEndpoints(
    const std::vector<std::shared_ptr<NodeExpression>>& endpointNodes,
    const catalog::FillIndexAuxInfo& indexInfo) {
  if (endpointNodes.size() != 2 || indexInfo.isFillUndirectedRel()) {
    return std::nullopt;
  }
  if (!indexInfo.getSrcNodeUniqueName().empty() &&
      !indexInfo.getDstNodeUniqueName().empty()) {
    return std::make_pair(endpointNodes[0], endpointNodes[1]);
  }
  if (indexInfo.getSrcTableId() == indexInfo.getDstTableId()) {
    return std::nullopt;
  }
  auto firstIsSrc = nodeHasTableID(endpointNodes[0], indexInfo.getSrcTableId());
  auto firstIsDst = nodeHasTableID(endpointNodes[0], indexInfo.getDstTableId());
  auto secondIsSrc =
      nodeHasTableID(endpointNodes[1], indexInfo.getSrcTableId());
  auto secondIsDst =
      nodeHasTableID(endpointNodes[1], indexInfo.getDstTableId());
  if (firstIsSrc && secondIsDst) {
    return std::make_pair(endpointNodes[0], endpointNodes[1]);
  }
  if (secondIsSrc && firstIsDst) {
    return std::make_pair(endpointNodes[1], endpointNodes[0]);
  }
  return std::nullopt;
}

static std::vector<std::shared_ptr<NodeExpression>>
collectMatchedFillEndpointNodes(
    const QueryGraph& patternGraph, const catalog::FillIndexAuxInfo& indexInfo,
    const std::vector<NodeInfo>& indexNodesInfo) {
  std::vector<std::shared_ptr<NodeExpression>> candidates;
  std::shared_ptr<NodeExpression> explicitSrc;
  std::shared_ptr<NodeExpression> explicitDst;
  for (const auto& nodeInfo : indexNodesInfo) {
    if (nodeInfo.matchedIdx == UINT64_MAX ||
        nodeInfo.matchedIdx >= patternGraph.getNumQueryNodes()) {
      continue;
    }
    auto node = patternGraph.getQueryNode(nodeInfo.matchedIdx);
    if (!indexInfo.getSrcNodeUniqueName().empty() &&
        nodeInfo.uniqName == indexInfo.getSrcNodeUniqueName()) {
      explicitSrc = node;
    }
    if (!indexInfo.getDstNodeUniqueName().empty() &&
        nodeInfo.uniqName == indexInfo.getDstNodeUniqueName()) {
      explicitDst = node;
    }
    if (nodeHasTableID(node, indexInfo.getSrcTableId()) ||
        nodeHasTableID(node, indexInfo.getDstTableId())) {
      candidates.push_back(node);
    }
  }

  if (explicitSrc != nullptr && explicitDst != nullptr) {
    return {explicitSrc, explicitDst};
  }

  if (indexInfo.getSrcTableId() == indexInfo.getDstTableId()) {
    if (candidates.size() == 2) {
      return candidates;
    }
    return {};
  }

  std::shared_ptr<NodeExpression> srcNode;
  std::shared_ptr<NodeExpression> dstNode;
  for (auto& node : candidates) {
    if (srcNode == nullptr && nodeHasTableID(node, indexInfo.getSrcTableId())) {
      srcNode = node;
    }
    if (dstNode == nullptr && nodeHasTableID(node, indexInfo.getDstTableId())) {
      dstNode = node;
    }
  }
  if (srcNode != nullptr && dstNode != nullptr) {
    return {srcNode, dstNode};
  }
  return {};
}

struct OrderedRecursiveCoverage {
  std::shared_ptr<NodeExpression> src;
  std::shared_ptr<NodeExpression> dst;
  std::vector<catalog::TableCatalogEntry*> tables;
};

// Label-dispatched reachability FILLs partition an ordered endpoint domain.
// Replacing a union-label path by only one partition would silently lose rows
// (including the zero-hop same-label partition). Require the complete product
// of endpoint labels, and choose one compatible definition for every pair.
// A pair is covered either by a per-pair FILL or by one FILL into a REL group
// whose source endpoints carry several labels: such a FILL routes every row
// to the child table of its endpoint labels (e.g. ROOT_POST(FROM COMMENT TO
// POST, FROM POST TO POST) filled from (message:COMMENT:POST)), so each child
// whose pair lies in the source label product holds exactly that partition.
static std::optional<OrderedRecursiveCoverage> orderedRecursiveCoverage(
    const catalog::FillIndexAuxInfo& info, const PatternIR& matched,
    const QueryGraph& query, main::ClientContext* context) {
  const auto endpointPosition = [](const auto& definition, const std::string& name) {
    const auto& pattern = definition.getPatternIR();
    const auto found = pattern.nodeUniqNameToPos.find(name);
    return found == pattern.nodeUniqNameToPos.end() ? UINT64_MAX : found->second;
  };
  const auto source = endpointPosition(info, info.getSrcNodeUniqueName());
  const auto destination = endpointPosition(info, info.getDstNodeUniqueName());
  if (!info.usesWorkloadCompatibility() || info.getSourceQuery().empty() ||
      source >= matched.nodesInfo.size() || destination >= matched.nodesInfo.size() ||
      source == destination || matched.relsInfo.size() != 1 ||
      matched.nodesInfo[source].matchedIdx >= query.getNumQueryNodes() ||
      matched.nodesInfo[destination].matchedIdx >= query.getNumQueryNodes()) return std::nullopt;
  OrderedRecursiveCoverage result{
      query.getQueryNode(matched.nodesInfo[source].matchedIdx),
      query.getQueryNode(matched.nodesInfo[destination].matchedIdx), {}};
  const auto sourceLabels = result.src->getTableIDs();
  const auto destinationLabels = result.dst->getTableIDs();
  if (sourceLabels.empty() || destinationLabels.empty()) return std::nullopt;
  auto* catalog = context->getCatalog();
  auto* transaction = context->getTransaction();
  auto entries = catalog->getExtendedIndexEntries(transaction);
  filterAllowedRewriteTargets(entries);
  std::sort(entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) {
    return lhs->getName() < rhs->getName();
  });
  std::set<std::pair<common::table_id_t, common::table_id_t>> covered;
  for (const auto* entry : entries) {
    if (catalog->isExtendedIndexBuilding(entry->getName())) continue;
    const auto& sibling = entry->getAuxInfo().cast<catalog::FillIndexAuxInfo>();
    if (!sibling.isVariadicRelIndex() || !sibling.usesWorkloadCompatibility() ||
        sibling.getSourceQuery().empty() || !sibling.getPredicateTemplates().empty() ||
        !sibling.getExistsPredicates().empty() ||
        sibling.getRecursiveRelLowerBound() != info.getRecursiveRelLowerBound() ||
        sibling.getRecursiveRelUpperBound() != info.getRecursiveRelUpperBound()) continue;
    const auto siblingSource = endpointPosition(sibling, sibling.getSrcNodeUniqueName());
    const auto siblingDestination = endpointPosition(sibling, sibling.getDstNodeUniqueName());
    auto basePattern = info.getPatternIR();
    auto siblingPattern = sibling.getPatternIR();
    if (basePattern.nodesInfo.size() != siblingPattern.nodesInfo.size() ||
        basePattern.relsInfo.size() != siblingPattern.relsInfo.size() ||
        siblingSource >= siblingPattern.nodesInfo.size() ||
        siblingDestination >= siblingPattern.nodesInfo.size()) continue;
    const auto [same, ignored] = PatternIR::findMatch(
        basePattern, siblingPattern, entry->getName(), [&]() {
          return siblingPattern.nodesInfo[siblingSource].matchedIdx == source &&
              siblingPattern.nodesInfo[siblingDestination].matchedIdx == destination &&
              basePattern.relsInfo[0].edgeType == siblingPattern.relsInfo[0].edgeType;
        });
    (void)ignored;
    if (!same) continue;
    // Per-pair FILL: its single carrier table. Group FILL: each child whose
    // endpoint pair lies in the source label product (a pair without a child
    // stays uncovered; label-filtered sources cannot target a whole group,
    // see FillRecursiveGroupMetadata.DynamicLabelFilterCannotAdvertiseWholeGroup).
    const auto& siblingSourceLabels = siblingPattern.nodesInfo[siblingSource].nodeLabels;
    const auto& siblingDestinationLabels =
        siblingPattern.nodesInfo[siblingDestination].nodeLabels;
    const auto inSource = [&](common::table_id_t id) {
      return id == sibling.getSrcTableId() ||
          std::find(siblingSourceLabels.begin(), siblingSourceLabels.end(), id) !=
              siblingSourceLabels.end();
    };
    const auto inDestination = [&](common::table_id_t id) {
      return id == sibling.getDstTableId() ||
          std::find(siblingDestinationLabels.begin(), siblingDestinationLabels.end(), id) !=
              siblingDestinationLabels.end();
    };
    std::vector<common::table_id_t> carriers{entry->getTableID()};
    if (catalog->containsRelGroup(transaction, sibling.getTableName())) {
      carriers = catalog->getRelGroupEntry(transaction, sibling.getTableName())->getRelTableIDs();
    }
    for (const auto id : carriers) {
      auto* table = catalog->getTableCatalogEntry(transaction, id);
      if (table == nullptr || table->getType() != CatalogEntryType::REL_TABLE_ENTRY) continue;
      const auto& rel = table->constCast<catalog::RelTableCatalogEntry>();
      const auto pair = std::make_pair(rel.getSrcTableID(), rel.getDstTableID());
      if (covered.contains(pair) || !inSource(pair.first) || !inDestination(pair.second) ||
          std::find(sourceLabels.begin(), sourceLabels.end(), pair.first) == sourceLabels.end() ||
          std::find(destinationLabels.begin(), destinationLabels.end(), pair.second) ==
              destinationLabels.end()) continue;
      result.tables.push_back(table);
      covered.insert(pair);
    }
  }
  for (const auto src : sourceLabels)
    for (const auto dst : destinationLabels)
      if (!covered.contains({src, dst})) return std::nullopt;
  return result;
}

static std::shared_ptr<RelExpression> findExistingFillRelExpression(
    const std::vector<std::shared_ptr<RelExpression>>& relExpressions,
    const catalog::FillIndexAuxInfo& indexInfo,
    const std::vector<std::shared_ptr<NodeExpression>>& endpointNodes,
    const RelLabelMap& relLabels) {
  if (endpointNodes.size() != 2) {
    return nullptr;
  }
  const auto sameNode = [](const std::shared_ptr<NodeExpression>& lhs,
                           const std::shared_ptr<NodeExpression>& rhs) {
    return lhs == rhs ||
           (lhs != nullptr && rhs != nullptr &&
            lhs->getUniqueName() == rhs->getUniqueName());
  };
  auto directedEndpoints = getDirectedFillRelEndpoints(endpointNodes, indexInfo);
  for (auto& rel : relExpressions) {
    if (rel == nullptr || rel->getSingleEntry() == nullptr ||
        tableLabel(rel->getSingleEntry(), relLabels) !=
            indexInfo.getTableName()) {
      continue;
    }
    if (directedEndpoints.has_value()) {
      if (sameNode(rel->getSrcNode(), directedEndpoints->first) &&
          sameNode(rel->getDstNode(), directedEndpoints->second)) {
        return rel;
      }
      if (indexInfo.getSrcTableId() == indexInfo.getDstTableId() &&
          rel->getDirectionType() == RelDirectionType::BOTH &&
          sameNode(rel->getSrcNode(), directedEndpoints->second) &&
          sameNode(rel->getDstNode(), directedEndpoints->first)) {
        return rel;
      }
      continue;
    }
    if ((sameNode(rel->getSrcNode(), endpointNodes[0]) &&
         sameNode(rel->getDstNode(), endpointNodes[1])) ||
        (sameNode(rel->getSrcNode(), endpointNodes[1]) &&
         sameNode(rel->getDstNode(), endpointNodes[0]))) {
      return rel;
    }
  }
  return nullptr;
}

static bool needsDirectedSameLabelRelAnchor(
    BoundMatchClause& matchClause, const catalog::FillIndexAuxInfo& indexInfo) {
  return indexInfo.getFillType() == FillEntryType::REL &&
         !indexInfo.isFillUndirectedRel() &&
         indexInfo.getSrcTableId() == indexInfo.getDstTableId() &&
         matchClause.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH;
}

// A newly materialized relation stores ordered RETURN endpoint pairs. Unlike
// a property on an existing same-label edge, its orientation does not depend
// on which way an undirected source edge happened to be traversed. Require
// explicit endpoint metadata and prove that the carrier is absent from the
// source; legacy or existing-edge definitions keep the direction guard.
static bool hasOrderedDerivedRelEndpoints(
    const catalog::FillIndexAuxInfo& info, main::ClientContext* context) {
  if (info.getFillType() != FillEntryType::REL ||
      info.isFillUndirectedRel() || info.getSourceQuery().empty() ||
      !info.getTableUniqName().empty() ||
      info.getSrcNodeUniqueName().empty() ||
      info.getDstNodeUniqueName().empty() ||
      info.getSrcNodeUniqueName() == info.getDstNodeUniqueName()) {
    return false;
  }
  const auto* table = tableByName(context->getCatalog(), context->getTransaction(), info.getTableName(),
      context->useInternalCatalogEntry());
  if (table == nullptr) return false;
  const auto& pattern = info.getPatternIR();
  if (std::any_of(pattern.relsInfo.begin(), pattern.relsInfo.end(),
                  [&](const auto& rel) {
                    return std::find(rel.relLabels.begin(), rel.relLabels.end(),
                                     table->getTableID()) != rel.relLabels.end();
                  })) return false;
  const auto endpointHasLabel = [&](const std::string& name, uint32_t label) {
    const auto pos = pattern.nodeUniqNameToPos.find(name);
    return pos != pattern.nodeUniqNameToPos.end() &&
        pos->second < pattern.nodesInfo.size() &&
        pattern.nodesInfo[pos->second].nodeLabels ==
            std::vector<common::oid_t>{label};
  };
  return endpointHasLabel(info.getSrcNodeUniqueName(), info.getSrcTableId()) &&
      endpointHasLabel(info.getDstNodeUniqueName(), info.getDstTableId());
}

// Prove predicates on the actual returned endpoint identities. Flattened
// predicate templates also contain OPTIONAL conditions, which need not hold
// on an emitted zero-count or NULL-extended source row.
static std::set<std::string> mandatoryDerivedEndpointPredicates(
    const FillIndexAuxInfo& info, main::ClientContext& context) {
  if (!hasOrderedDerivedRelEndpoints(info, &context)) return {};
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return {};
    Binder sourceBinder(&context);
    sourceBinder.setAllowIndexRewrite(false);
    const auto bound = sourceBinder.bind(*parsed[0]);
    const auto& regular = bound->constCast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
        !regular.getPreQueryExprs().empty() || regular.getPostSingleQuery()) return {};
    const auto* query = regular.getSingleQuery(0);
    if (query->getNumQueryParts() == 0) return {};
    const auto* first = query->getQueryPart(0);
    const auto* last = query->getQueryPart(query->getNumQueryParts() - 1);
    if (first->getNumReadingClause() != 1 || !last->hasProjectionBody() ||
        first->getReadingClause(0)->getClauseType() != ClauseType::MATCH) return {};
    const auto& match = first->getReadingClause(0)->constCast<BoundMatchClause>();
    if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasHint() ||
        match.getQueryGraphCollection()->getNumQueryGraphs() != 1) return {};
    const auto* graph = match.getQueryGraphCollection()->getQueryGraph(0);
    const auto relationships = graph->getQueryRels();
    if (std::any_of(relationships.begin(), relationships.end(),
                    [](const auto& rel) { return rel->isRecursive(); })) return {};
    const auto identity = [&](const std::shared_ptr<Expression>& expression,
                              table_id_t table) -> std::string {
      std::string name;
      if (expression->expressionType == ExpressionType::PATTERN &&
          expression->getDataType().getLogicalTypeID() == LogicalTypeID::NODE) {
        name = expression->getUniqueName();
      } else if (expression->expressionType == ExpressionType::PROPERTY) {
        const auto& property = expression->constCast<PropertyExpression>();
        if (!property.isSingleLabel() || !property.isPrimaryKey(table)) return {};
        name = property.getVariableName();
      } else return {};
      if (!graph->containsQueryNode(name) ||
          !graph->getQueryNode(name)->getTableIDsSet().contains(table)) return {};
      return name;
    };
    const auto outputs = last->getProjectionBody()->getProjectionExpressions();
    if (outputs.size() < 2) return {};
    const auto src = identity(outputs[0], info.getSrcTableId());
    const auto dst = identity(outputs[1], info.getDstTableId());
    if (src.empty() || dst.empty() || src == dst) return {};
    for (uint32_t i = 0; i < query->getNumQueryParts(); ++i) {
      const auto* part = query->getQueryPart(i);
      if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
          (i != 0 && part->hasReadingClause())) return {};
      const auto* body = part->getProjectionBody();
      if (body->hasOrderByExpressions() || body->hasSkipOrLimit()) return {};
      const auto containsKeys = [&](const auto& expressions) {
        bool source = false, destination = false;
        for (const auto& expression : expressions) {
          source |= identity(expression, info.getSrcTableId()) == src;
          destination |= identity(expression, info.getDstTableId()) == dst;
        }
        return source && destination;
      };
      // Projection/WHERE/DISTINCT can discard input rows but cannot fabricate
      // endpoint identities. Grouped aggregates must retain both keys so an
      // empty input cannot produce a new global-aggregate row.
      if (!containsKeys(body->getProjectionExpressions()) ||
          (body->hasAggregateExpressions() && !containsKeys(body->getGroupByExpressions())))
        return {};
    }
    IndexPredicateBindings bindings{{src, "endpoint:0"}, {dst, "endpoint:1"}};
    std::set<std::string> result;
    for (const auto& predicate : match.getConjunctivePredicates()) {
      if (const auto key = makeIndexPredicateKey(*predicate, bindings)) result.insert(*key);
    }
    return result;
  } catch (const std::exception&) {
    return {};
  }
}

static void pruneGuaranteedDerivedEndpointPredicates(
    BoundMatchClause& match, const FillIndexAuxInfo& primary,
    const std::set<table_id_t>& expectedTables, main::ClientContext& context,
    ExpressionBinder& expressionBinder) {
  if (match.getMatchClauseType() != MatchClauseType::MATCH || !match.hasPredicate() ||
      expectedTables.empty() || !hasOrderedDerivedRelEndpoints(primary, &context)) return;
  auto* catalog = context.getCatalog();
  const auto entries = catalog->getExtendedIndexEntries(context.getTransaction());
  for (const auto& rel : match.getQueryGraphCollection()->getQueryRels()) {
    if (!match.hasPredicate()) return;
    if (rel->isRecursive() || rel->getDirectionType() != RelDirectionType::SINGLE ||
        rel->getSrcNodeName() == rel->getDstNodeName()) continue;
    const auto labels = rel->getTableIDs();
    if (std::set<table_id_t>(labels.begin(), labels.end()) != expectedTables) continue;
    std::optional<std::set<std::string>> guaranteed;
    bool complete = true;
    for (const auto* table : rel->getEntries()) {
      bool found = false;
      for (const auto* entry : entries) {
        const auto& info = entry->getAuxInfo().cast<FillIndexAuxInfo>();
        if (info.getFillType() != FillEntryType::REL ||
            info.getTableName() != table->getName()) continue;
        found = true;
        auto keys = catalog->isExtendedIndexBuilding(entry->getName())
            ? std::set<std::string>{} : mandatoryDerivedEndpointPredicates(info, context);
        if (!guaranteed) guaranteed = std::move(keys);
        else std::erase_if(*guaranteed, [&](const auto& key) { return !keys.contains(key); });
      }
      complete &= found;
    }
    if (!complete || !guaranteed || guaranteed->empty()) continue;
    // This exact mandatory clause scans every proved physical branch. Do not
    // propagate the guarantee into OPTIONAL or through another query part.
    IndexPredicateBindings bindings{{rel->getSrcNodeName(), "endpoint:0"},
                                     {rel->getDstNodeName(), "endpoint:1"}};
    std::shared_ptr<Expression> retained;
    for (const auto& predicate : match.getConjunctivePredicates()) {
      const auto key = makeIndexPredicateKey(*predicate, bindings);
      if (key && guaranteed->contains(*key)) continue;
      retained = expressionBinder.combineBooleanExpressions(
          ExpressionType::AND, std::move(retained), predicate);
    }
    match.setPredicate(std::move(retained));
  }
}

// An ordered materialized carrier does not make a directed source cover an
// undirected request. The latter can contain both endpoint assignments, even
// for the same physical edge. Direction may differ only when schema domains
// prove the reverse assignment impossible. Apply the same proof in either
// direction so an undirected source cannot add rows to a directed request.
static bool sourceDirectionsCoverMatchedRows(
    const PatternIR& source, const PatternIR& query,
    main::ClientContext* context) {
  for (const auto& edge : source.relsInfo) {
    if (edge.matchedIdx >= query.relsInfo.size() ||
        edge.srcNodeIdx >= source.nodesInfo.size() ||
        edge.dstNodeIdx >= source.nodesInfo.size()) return false;
    const auto& requested = query.relsInfo[edge.matchedIdx];
    if (edge.edgeType == requested.edgeType) continue;
    if (edge.relType != QueryRelType::NON_RECURSIVE ||
        requested.relType != QueryRelType::NON_RECURSIVE ||
        !((edge.edgeType == EdgeType::OUTGOING &&
           requested.edgeType == EdgeType::UNDIRECTED) ||
          (edge.edgeType == EdgeType::UNDIRECTED &&
           requested.edgeType == EdgeType::OUTGOING))) return false;
    const auto src = edge.edgeType == EdgeType::OUTGOING
        ? source.nodesInfo[edge.srcNodeIdx].matchedIdx : requested.srcNodeIdx;
    const auto dst = edge.edgeType == EdgeType::OUTGOING
        ? source.nodesInfo[edge.dstNodeIdx].matchedIdx : requested.dstNodeIdx;
    if (src >= query.nodesInfo.size() || dst >= query.nodesInfo.size() ||
        edge.relLabels.empty()) return false;
    const auto& srcLabels = query.nodesInfo[src].nodeLabels;
    const auto& dstLabels = query.nodesInfo[dst].nodeLabels;
    const auto contains = [](const auto& labels, auto id) {
      return std::find(labels.begin(), labels.end(), id) != labels.end();
    };
    for (const auto label : edge.relLabels) {
      const auto* table = context->getCatalog()->getTableCatalogEntry(
          context->getTransaction(), label);
      if (table == nullptr || table->getType() != CatalogEntryType::REL_TABLE_ENTRY)
        return false;
      const auto& physical = table->constCast<RelTableCatalogEntry>();
      if (contains(srcLabels, physical.getDstTableID()) &&
          contains(dstLabels, physical.getSrcTableID())) return false;
    }
  }
  return true;
}

static bool hasEndpointSetContract(const FillIndexAuxInfo& info) {
  const auto& predicates = info.getPredicateTemplates();
  return std::find(predicates.begin(), predicates.end(), ENDPOINT_SET_CONTRACT) !=
      predicates.end();
}

// Property-free endpoint projections use the compatibility catalog format,
// but this exact source has no flattened aggregate or scope boundary. Prove
// its full typed graph, ordered output keys and predicates before allowing the
// residual-correlation helper to inspect this otherwise rejected format.
static bool provesSingleStageEndpointSetSource(
    const FillIndexAuxInfo& info, main::ClientContext& context) {
  if (!hasEndpointSetContract(info) || !info.getTargetColumns().empty() ||
      !info.getIndexColumnInfos().empty() || info.isStagedPipeline() ||
      !hasOrderedDerivedRelEndpoints(info, &context)) return false;
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY) return false;
    Binder sourceBinder(&context);
    sourceBinder.setAllowIndexRewrite(false);
    const auto bound = sourceBinder.bind(*parsed[0]);
    const auto& regular = bound->constCast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
        !regular.getPreQueryExprs().empty() || regular.getPostSingleQuery()) return false;
    const auto* query = regular.getSingleQuery(0);
    if (query->getNumQueryParts() != 1) return false;
    const auto* part = query->getQueryPart(0);
    if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
        part->hasProjectionBodyPredicate() || part->getNumReadingClause() != 1 ||
        part->getReadingClause(0)->getClauseType() != ClauseType::MATCH) return false;
    const auto& match = part->getReadingClause(0)->constCast<BoundMatchClause>();
    const auto* collection = match.getQueryGraphCollection();
    if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasHint() ||
        collection->getNumQueryGraphs() != 1) return false;
    const auto* graph = collection->getQueryGraph(0);
    auto source = convertGraphPatternToIR(*graph);
    auto definition = info.getPatternIR();
    if (source.relsInfo.empty() || source.nodesInfo.size() != definition.nodesInfo.size() ||
        source.relsInfo.size() != definition.relsInfo.size() ||
        std::any_of(source.relsInfo.begin(), source.relsInfo.end(), [](const auto& rel) {
          return rel.relType != QueryRelType::NON_RECURSIVE;
        })) return false;
    const auto* body = part->getProjectionBody();
    const auto values = body->getProjectionExpressions();
    if (!body->isDistinct() || body->hasAggregateExpressions() ||
        body->hasOrderByExpressions() || body->hasSkipOrLimit() || values.size() != 2) return false;
    std::vector<std::string> ordered;
    for (size_t i = 0; i < 2; ++i) {
      const auto table = i == 0 ? info.getSrcTableId() : info.getDstTableId();
      std::string uid;
      if (values[i]->expressionType == ExpressionType::PATTERN &&
          values[i]->getDataType().getLogicalTypeID() == LogicalTypeID::NODE)
        uid = values[i]->getUniqueName();
      else if (values[i]->expressionType == ExpressionType::PROPERTY) {
        const auto& key = values[i]->constCast<PropertyExpression>();
        if (!key.isSingleLabel() || !key.isPrimaryKey(table)) return false;
        uid = key.getVariableName();
      } else return false;
      if (!graph->containsQueryNode(uid) || graph->getQueryNode(uid)->getTableIDsSet() !=
              std::unordered_set<table_id_t>{table}) return false;
      ordered.push_back(uid);
    }
    if (ordered[0] == ordered[1]) return false;
    const auto [matched, ignored] = PatternIR::findMatch(source, definition, "endpoint-set-source", [&] {
      for (size_t i = 0; i < 2; ++i) {
        const auto& key = i == 0 ? info.getSrcNodeUniqueName() : info.getDstNodeUniqueName();
        const auto pos = definition.nodeUniqNameToPos.find(key);
        if (pos == definition.nodeUniqNameToPos.end() ||
            definition.nodesInfo[pos->second].matchedIdx != source.nodeUniqNameToPos.at(ordered[i]))
          return false;
      }
      for (const auto& rel : definition.relsInfo)
        if (rel.matchedIdx >= source.relsInfo.size() ||
            rel.edgeType != source.relsInfo[rel.matchedIdx].edgeType) return false;
      std::multiset<std::string> actual, expected;
      for (const auto& predicate : match.getConjunctivePredicates()) {
        const auto key = canonicalizePredicateForMatchedIndex(*predicate, source, definition);
        if (key.empty()) return false;
        actual.insert(key);
      }
      for (const auto& predicate : info.getPredicateTemplates())
        if (predicate != ENDPOINT_SET_CONTRACT) expected.insert(predicate);
      return actual == expected;
    });
    (void)ignored;
    return matched;
  } catch (const std::exception&) {
    return false;
  }
}

// A private witness COUNT maintains a NODE existence flag. It does not make
// that flag a public bag weight. Rebind the exact source to prove a set of
// typed carrier keys, separately from the set-consuming query proof below.
static bool provesNodePresenceSetSource(const FillIndexAuxInfo& info,
                                        main::ClientContext& context) {
  if (info.getFillType() != FillEntryType::NODE ||
      info.usesWorkloadCompatibility() ||
      info.getAggregateForm() != FillAggregateForm::CANONICAL_BASE ||
      info.getSourceQuery().empty() || info.getPresenceColumns().empty() ||
      info.getRowCountColumn().empty() ||
      !info.getRowCountColumn().starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX))
    return false;
  auto* table = tableByName(context.getCatalog(), context.getTransaction(), info.getTableName(),
      context.useInternalCatalogEntry());
  if (table == nullptr || table->getType() != CatalogEntryType::NODE_TABLE_ENTRY)
    return false;
  const auto& columns = info.getIndexColumnInfos();
  const auto& targets = info.getTargetColumns();
  if (columns.size() != 1 || columns[0] == nullptr ||
      columns[0]->columnType != IndexColumnType::NODE_AGGREGATE ||
      targets.size() != 1 + info.getPresenceColumns().size() ||
      targets.front() != info.getRowCountColumn()) return false;
  const auto& count = static_cast<const NodeAggregationIndexColnumInfo&>(*columns[0]);
  if (count.aggregateFuncName != "COUNT" || count.nameTemplate != "COUNT(*)")
    return false;
  for (size_t i = 0; i < info.getPresenceColumns().size(); ++i) {
    const auto& name = info.getPresenceColumns()[i];
    if (targets[i + 1] != name || !table->containsProperty(name)) return false;
    const auto& property = table->getProperty(name);
    if (property.getType().getLogicalTypeID() != LogicalTypeID::BOOL ||
        property.defaultExpr == nullptr ||
        property.defaultExpr->getExpressionType() != ExpressionType::LITERAL)
      return false;
    const auto value = property.defaultExpr->constCast<parser::ParsedLiteralExpression>().getValue();
    // Omitted primitive BOOL defaults are false in graph storage. An explicit
    // true default would admit anchors with no source group and is unsafe.
    if (!value.isNull() && (value.getDataType().getLogicalTypeID() != LogicalTypeID::BOOL ||
                            value.getValue<bool>())) return false;
  }
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return false;
    Binder sourceBinder(&context);
    sourceBinder.setAllowIndexRewrite(false);
    std::unique_ptr<BoundBaseScanSource> source = std::make_unique<BoundQueryScanSource>(
        sourceBinder.bind(*parsed[0]), BoundQueryScanSourceInfo({}));
    // This existing proof may collapse a typed identity DISTINCT followed by
    // exact LABEL dispatch; it never groups equal scalar IDs across labels.
    normalizeFillAggregateProjection(source, table, &context);
    const auto& regular = source->constCast<BoundQueryScanSource>()
                              .statement->constCast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
        regular.getPostSingleQuery()) return false;
    const auto* query = regular.getSingleQuery(0);
    if (query->getNumQueryParts() != 1) return false;
    const auto* part = query->getQueryPart(0);
    if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
        part->hasProjectionBodyPredicate() || part->getNumReadingClause() != 1)
      return false;
    const auto* clause = part->getReadingClause(0);
    if (clause->getClauseType() != ClauseType::MATCH) return false;
    const auto& match = clause->constCast<BoundMatchClause>();
    if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasHint()) return false;
    const auto relationships = match.getQueryGraphCollection()->getQueryRels();
    if (relationships.empty() || std::any_of(relationships.begin(), relationships.end(),
            [](const auto& rel) { return rel->isRecursive(); })) return false;
    const auto* body = part->getProjectionBody();
    if (!body->isDistinct() || body->hasAggregateExpressions() ||
        body->hasOrderByExpressions() || body->hasSkipOrLimit()) return false;
    const auto values = query->getStatementResult()->getColumns();
    if (values.size() != 1 + info.getPresenceColumns().size() ||
        values[0]->expressionType != ExpressionType::PROPERTY) return false;
    const auto& key = values[0]->constCast<PropertyExpression>();
    if (!key.isSingleLabel() || !key.isPrimaryKey(table->getTableID())) return false;
    bool typedAnchor = false;
    for (const auto& node : match.getQueryGraphCollection()->getQueryNodes())
      if (node->getUniqueName() == key.getVariableName())
        typedAnchor = node->getTableIDsSet() == std::unordered_set<table_id_t>{table->getTableID()};
    if (!typedAnchor) return false;
    return std::all_of(values.begin() + 1, values.end(), [](const auto& value) {
      if (value->expressionType != ExpressionType::LITERAL ||
          value->getDataType().getLogicalTypeID() != LogicalTypeID::BOOL) return false;
      const auto& literal = value->template constCast<LiteralExpression>().getValue();
      return !literal.isNull() && literal.template getValue<bool>();
    });
  } catch (const std::exception&) {
    return false;
  }
}

static bool hasRecursiveEndpointContract(const FillIndexAuxInfo& info) {
  const auto& predicates = info.getPredicateTemplates();
  return std::find(predicates.begin(), predicates.end(), RECURSIVE_ENDPOINT_CONTRACT) !=
      predicates.end();
}

struct RecursiveEndpointEdge {
  size_t position;
  QueryRelType type;
  RelDirectionType direction;
  uint64_t lowerBound, upperBound;
  PathSemantic semantic;
  std::vector<table_id_t> tables, stepTables, nodeTables, nodeCopyTables;
  bool operator==(const RecursiveEndpointEdge&) const = default;
};

static std::optional<RecursiveEndpointEdge> recursiveEndpointEdge(
    const RelExpression& edge, size_t position) {
  if (edge.getRelType() != QueryRelType::VARIABLE_LENGTH_WALK &&
      edge.getRelType() != QueryRelType::VARIABLE_LENGTH_TRAIL &&
      edge.getRelType() != QueryRelType::VARIABLE_LENGTH_ACYCLIC) return std::nullopt;
  const auto* recursive = edge.getRecursiveInfo();
  if (recursive == nullptr || recursive->bindData == nullptr ||
      recursive->node == nullptr || recursive->nodeCopy == nullptr ||
      recursive->rel == nullptr || recursive->nodePredicate != nullptr ||
      recursive->relPredicate != nullptr ||
      recursive->bindData->weightPropertyExpr != nullptr) return std::nullopt;
  const auto sorted = [](auto values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
  };
  return RecursiveEndpointEdge{position, edge.getRelType(), edge.getDirectionType(),
      recursive->bindData->lowerBound, recursive->bindData->upperBound,
      recursive->bindData->semantic, sorted(edge.getTableIDs()),
      sorted(recursive->rel->getTableIDs()), sorted(recursive->node->getTableIDs()),
      sorted(recursive->nodeCopy->getTableIDs())};
}

// PatternIR retains topology but omits bounds and path semantics. Rebind the
// saved source with the normal label/type normalization and no index rewrite,
// then associate each typed path with its exact registered source position.
static std::optional<std::vector<RecursiveEndpointEdge>> recursiveEndpointSource(
    const FillIndexAuxInfo& info, main::ClientContext* context) {
  if (!hasRecursiveEndpointContract(info) || info.getSourceQuery().empty() ||
      !info.getTargetColumns().empty() ||
      !hasOrderedDerivedRelEndpoints(info, context)) return std::nullopt;
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return std::nullopt;
    Binder sourceBinder(context);
    sourceBinder.setAllowIndexRewrite(false);
    const auto bound = sourceBinder.bind(*parsed[0]);
    const auto& regular = bound->constCast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
        regular.getPostSingleQuery()) return std::nullopt;
    const auto* query = regular.getSingleQuery(0);
    if (query->getNumQueryParts() == 0) return std::nullopt;
    const auto* part = query->getQueryPart(0);
    if (part->getNumReadingClause() != 1 ||
        part->getReadingClause(0)->getClauseType() != ClauseType::MATCH) return std::nullopt;
    const auto& match = part->getReadingClause(0)->constCast<BoundMatchClause>();
    const auto* graphs = match.getQueryGraphCollection();
    if (match.getMatchClauseType() != MatchClauseType::MATCH ||
        graphs->getNumQueryGraphs() != 1) return std::nullopt;
    const auto* graph = graphs->getQueryGraph(0);
    const auto source = convertGraphPatternToIR(*graph);
    const auto& pattern = info.getPatternIR();
    if (source.nodesInfo.size() != pattern.nodesInfo.size() ||
        source.relsInfo.size() != pattern.relsInfo.size()) return std::nullopt;
    for (size_t i = 0; i < source.nodesInfo.size(); ++i) {
      const auto& actual = source.nodesInfo[i];
      const auto& saved = pattern.nodesInfo[i];
      if (actual.varName != saved.varName) return std::nullopt;
      const bool endpoint = saved.uniqName == info.getSrcNodeUniqueName() ||
          saved.uniqName == info.getDstNodeUniqueName();
      if (endpoint) {
        for (const auto label : saved.nodeLabels)
          if (std::find(actual.nodeLabels.begin(), actual.nodeLabels.end(), label) ==
              actual.nodeLabels.end()) return std::nullopt;
      } else if (!PatternIR::labelsEqual(actual.nodeLabels, saved.nodeLabels))
        return std::nullopt;
    }
    std::vector<RecursiveEndpointEdge> contracts;
    const auto edges = graph->getQueryRels();
    for (size_t i = 0; i < source.relsInfo.size(); ++i) {
      const auto& actual = source.relsInfo[i];
      const auto& saved = pattern.relsInfo[i];
      if (actual.varName != saved.varName || actual.srcNodeIdx != saved.srcNodeIdx ||
          actual.dstNodeIdx != saved.dstNodeIdx || actual.edgeType != saved.edgeType ||
          actual.relType != saved.relType) return std::nullopt;
      if (actual.relType == QueryRelType::NON_RECURSIVE) continue;
      if (!PatternIR::labelsEqual(actual.relLabels, saved.relLabels)) return std::nullopt;
      auto contract = recursiveEndpointEdge(*edges[i], i);
      if (!contract) return std::nullopt;
      contracts.push_back(std::move(*contract));
    }
    if (contracts.empty()) return std::nullopt;
    return contracts;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

static bool matchesRecursiveEndpointSource(
    const FillIndexAuxInfo& info, const PatternIR& matchedIndex,
    const PatternIR& queryIR, const BoundMatchClause& match,
    main::ClientContext* context) {
  const auto expected = recursiveEndpointSource(info, context);
  if (!expected) return false;
  const auto edges = match.getQueryGraphCollection()->getQueryRels();
  for (const auto& contract : *expected) {
    if (contract.position >= matchedIndex.relsInfo.size()) return false;
    const auto& relation = matchedIndex.relsInfo[contract.position];
    if (relation.matchedIdx >= queryIR.relsInfo.size()) return false;
    const auto& name = queryIR.relsInfo[relation.matchedIdx].uniqName;
    const auto actual = std::find_if(edges.begin(), edges.end(),
        [&](const auto& edge) { return edge->getUniqueName() == name; });
    if (actual == edges.end()) return false;
    const auto observed = recursiveEndpointEdge(**actual, contract.position);
    if (!observed || *observed != contract) return false;
  }
  return true;
}

// Removing duplicate witnesses is sound only before a proved set-consuming
// projection. Every observable value must depend on retained keys or on the
// untouched outer graph; neither hidden identities nor row-volatile functions
// can survive the substitution.
static bool provesEndpointSetConsumer(
    const FillIndexAuxInfo& info, const PatternIR& matchedIndex,
    const PatternIR& queryIR, const BoundMatchClause& match,
    const NormalizedQueryPart& part, main::ClientContext* context,
    bool allowStoredValues = false, bool allowRecursive = false,
    const NormalizedSingleQuery* wholeQuery = nullptr,
    bool nodePresence = false, bool provedSingleStageEndpointSetSource = false) {
  const bool optional = match.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH;
  if ((nodePresence ? (info.getFillType() != FillEntryType::NODE || optional ||
                       match.hasHint() || part.hasUpdatingClause()) :
        (info.getFillType() != FillEntryType::REL ||
         (!info.getTargetColumns().empty() && !allowStoredValues) ||
         !hasOrderedDerivedRelEndpoints(info, context))) ||
      (!optional && match.getMatchClauseType() != MatchClauseType::MATCH) ||
      !part.hasProjectionBody() || wholeQuery == nullptr) return false;
  const auto* body = part.getProjectionBody();
  const auto countDistinct = [](const Expression& expression) {
    if (expression.expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
    const auto& aggregate = expression.constCast<AggregateFunctionExpression>();
    return aggregate.isDistinct() && aggregate.getFunction().name == "COUNT" &&
        aggregate.getNumChildren() == 1;
  };
  const auto aggregates = body->getAggregateExpressions();
  const auto setConsumer = [&](const NormalizedQueryPart& candidate) {
    if (!candidate.hasProjectionBody() || candidate.hasUpdatingClause()) return false;
    const auto* projection = candidate.getProjectionBody();
    const auto values = projection->getAggregateExpressions();
    return values.empty() ? projection->isDistinct() :
        std::all_of(values.begin(), values.end(),
                    [&](const auto& value) { return countDistinct(*value); });
  };
  std::vector<const NormalizedQueryPart*> checkedParts{&part};
  if (!setConsumer(part)) {
    // Recursive endpoint sets may cross deterministic MATCH/filter/projection
    // stages until DISTINCT consumes path multiplicity. Reject intervening
    // bag aggregates, pagination, updates, and unsafe expressions.
    if (!allowRecursive || optional || nodePresence || allowStoredValues) return false;
    bool after = false, foundConsumer = false;
    for (uint32_t i = 0; i < wholeQuery->getNumQueryParts(); ++i) {
      const auto* current = wholeQuery->getQueryPart(i);
      if (current == &part) after = true;
      if (!after) continue;
      if (current != &part) checkedParts.push_back(current);
      if (setConsumer(*current)) { foundConsumer = true; break; }
      if (!current->hasProjectionBody() || current->hasUpdatingClause()) return false;
      const auto* projection = current->getProjectionBody();
      if (!projection->getAggregateExpressions().empty() ||
          projection->hasSkipOrLimit() || projection->hasOrderByExpressions()) return false;
    }
    if (!foundConsumer) return false;
  }
  std::unordered_set<std::string> retained, removed, boundBefore;
  const std::vector<std::string> keys = nodePresence
      ? std::vector<std::string>{info.getTableUniqName()}
      : std::vector<std::string>{info.getSrcNodeUniqueName(), info.getDstNodeUniqueName()};
  for (const auto& name : keys) {
    const auto source = matchedIndex.nodeUniqNameToPos.find(name);
    if (source == matchedIndex.nodeUniqNameToPos.end()) return false;
    const auto pos = matchedIndex.nodesInfo[source->second].matchedIdx;
    if (pos >= queryIR.nodesInfo.size()) return false;
    retained.insert(queryIR.nodesInfo[pos].uniqName);
  }
  for (const auto& node : matchedIndex.nodesInfo) {
    if (node.matchedIdx >= queryIR.nodesInfo.size()) return false;
    const auto& name = queryIR.nodesInfo[node.matchedIdx].uniqName;
    if (!retained.contains(name)) removed.insert(name);
  }
  for (const auto& rel : matchedIndex.relsInfo) {
    if ((!allowRecursive && rel.relType != QueryRelType::NON_RECURSIVE) ||
        rel.matchedIdx >= queryIR.relsInfo.size()) return false;
    removed.insert(queryIR.relsInfo[rel.matchedIdx].uniqName);
  }
  bool foundPart = false;
  for (uint32_t i = 0; i < wholeQuery->getNumQueryParts(); ++i) {
    const auto* earlier = wholeQuery->getQueryPart(i);
    if (earlier == &part) { foundPart = true; break; }
    if (!earlier->hasProjectionBody()) continue;
    for (const auto& expression : earlier->getProjectionBody()->getProjectionExpressions()) {
      // An input graph identity already constrains this MATCH even when it is
      // absent from this part's outputs. A pair index cannot forget that key.
      for (const auto& name : collectDependentVarNames(expression)) {
        if (removed.contains(name)) return false;
        boundBefore.insert(name);
      }
      boundBefore.insert(expression->getUniqueName());
      if (removed.contains(expression->getUniqueName())) return false;
    }
  }
  if (!foundPart) return false;
  if (optional) {
    // Replace only a complete endpoint set inside this same OPTIONAL clause.
    // One anchor is an input and the counted endpoint is newly introduced;
    // keeping the remaining clause preserves both its matches and its single
    // null-extended row when no endpoint satisfies the remaining pattern.
    if (allowStoredValues || !hasEndpointSetContract(info) || aggregates.empty() ||
        retained.size() != 2) return false;
    bool foundMatch = false;
    for (uint32_t i = 0; i < part.getNumReadingClause(); ++i) {
      const auto* earlier = part.getReadingClause(i);
      if (earlier == &match) { foundMatch = true; break; }
      if (earlier->getClauseType() != ClauseType::MATCH) return false;
      const auto* graphs = earlier->constCast<BoundMatchClause>().getQueryGraphCollection();
      for (const auto& node : graphs->getQueryNodes())
        boundBefore.insert(node->getUniqueName());
      for (const auto& rel : graphs->getQueryRels())
        boundBefore.insert(rel->getUniqueName());
    }
    if (!foundMatch) return false;
    std::string fresh;
    for (const auto& key : retained) {
      if (boundBefore.contains(key)) continue;
      if (!fresh.empty()) return false;
      fresh = key;
    }
    if (fresh.empty()) return false;
    for (const auto& aggregate : aggregates) {
      const auto& child = aggregate->constCast<AggregateFunctionExpression>().getChild(0);
      if (child->expressionType != ExpressionType::PATTERN ||
          child->dataType.getLogicalTypeID() != LogicalTypeID::NODE ||
          child->getUniqueName() != fresh) return false;
    }
  }

  std::function<bool(const std::shared_ptr<Expression>&, bool)> safeExpression =
      [&](const auto& expression, bool inMatchedPredicate) {
    if (!expression) return true;
    switch (expression->expressionType) {
    case ExpressionType::PROPERTY:
      return !removed.contains(
          expression->template constCast<PropertyExpression>().getVariableName());
    case ExpressionType::PATTERN:
    case ExpressionType::VARIABLE:
      return !removed.contains(expression->getUniqueName());
    case ExpressionType::LITERAL:
    case ExpressionType::PARAMETER:
      return true;
    case ExpressionType::AGGREGATE_FUNCTION:
      if (!countDistinct(*expression)) return false;
      break;
    case ExpressionType::FUNCTION: {
      static const std::set<std::string> deterministic = {
          "+", "-", "*", "/", "%", "LABEL", "STRUCT_EXTRACT", "TIMESTAMP",
          "CAST_TO_INT64", "CAST_TO_DOUBLE", "CAST_TO_STRING", "CAST_TO_DATE",
          "CAST_TO_TIMESTAMP"};
      if (!deterministic.contains(expression->template constCast<ScalarFunctionExpression>()
                                      .getFunction().name)) return false;
      break;
    }
    case ExpressionType::SUBQUERY:
      try {
        if (const auto exists = extractFillExistsPredicate(*expression, queryIR)) {
          if (exists->srcNodePos < queryIR.nodesInfo.size() &&
              exists->dstNodePos < queryIR.nodesInfo.size() &&
              retained.contains(queryIR.nodesInfo[exists->srcNodePos].uniqName) &&
              retained.contains(queryIR.nodesInfo[exists->dstNodePos].uniqName))
            return true;
        }
      } catch (const std::exception&) {}
      // DISTINCT removes duplicate witnesses, not correlations. An endpoint
      // outside the materialized keys may remain bound by an untouched edge
      // in this same mandatory MATCH. Reuse the exact UID/type/scope proof;
      // do not turn those residual bindings into materialized retained keys
      // or apply it to predicates belonging to another clause/projection.
      return inMatchedPredicate && provesResidualMatchExistsInputsSurvive(
          *expression, info, matchedIndex, queryIR, match,
          provedSingleStageEndpointSetSource);
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
    case ExpressionType::CASE_ELSE:
      break;
    default:
      return false;
    }
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      if (!safeExpression(child, inMatchedPredicate)) return false;
    return true;
  };
  for (const auto* checkedPart : checkedParts) {
    for (uint32_t i = 0; i < checkedPart->getNumReadingClause(); ++i) {
      const auto* clause = checkedPart->getReadingClause(i);
      if (clause->getClauseType() != ClauseType::MATCH) return false;
      const auto& other = clause->constCast<BoundMatchClause>();
      if (nodePresence && (other.getMatchClauseType() != MatchClauseType::MATCH || other.hasHint()))
        return false;
      if (&other != &match) {
        for (const auto& node : other.getQueryGraphCollection()->getQueryNodes())
          if (removed.contains(node->getUniqueName())) return false;
        for (const auto& rel : other.getQueryGraphCollection()->getQueryRels())
          if (removed.contains(rel->getUniqueName())) return false;
      }
      for (const auto& predicate : other.getConjunctivePredicates()) {
        if (&other == &match && predicateCoveredByFill(
                *predicate, queryIR, matchedIndex, info.getPredicateTemplates())) continue;
        if (!safeExpression(predicate, &other == &match)) return false;
      }
      for (const auto& rel : other.getQueryGraphCollection()->getQueryRels()) {
        if (!rel->isRecursive()) continue;
        const auto* recursive = rel->getRecursiveInfo();
        if (!safeExpression(recursive->nodePredicate, false) ||
            !safeExpression(recursive->relPredicate, false)) return false;
      }
    }
    if (!safeExpression(checkedPart->getProjectionBodyPredicate(), false)) return false;
    const auto* checkedBody = checkedPart->getProjectionBody();
    for (const auto& expressions : {checkedBody->getProjectionExpressions(),
                                    checkedBody->getGroupByExpressions(),
                                    checkedBody->getOrderByExpressions(), checkedBody->getAggregateExpressions()})
      for (const auto& expression : expressions)
        if (!safeExpression(expression, false)) return false;
  }
  return true;
}

// A bound value projected by the immediately preceding WITH is an input
// column here, even when its expression object still contains an older
// aggregate tree. Binder carries these scope values by shared-object identity;
// a newly bound expression can have the same structural unique name, so its
// name and type alone do not prove it is an input. Only current occurrences
// of those actual incoming objects are opaque to the local dependency proof.
static std::unordered_set<const Expression*> carriedProjectionInputs(
    const NormalizedQueryPart& part, const NormalizedSingleQuery* wholeQuery) {
  std::unordered_set<const Expression*> result;
  if (wholeQuery == nullptr) return result;
  const NormalizedQueryPart* previous = nullptr;
  bool found = false;
  for (uint32_t i = 0; i < wholeQuery->getNumQueryParts(); ++i) {
    const auto* current = wholeQuery->getQueryPart(i);
    if (current == &part) { found = true; break; }
    previous = current;
  }
  if (!found || previous == nullptr || !previous->hasProjectionBody()) return result;
  const auto inputs = previous->getProjectionBody()->getProjectionExpressions();
  std::function<void(const std::shared_ptr<Expression>&)> visit = [&](const auto& value) {
    if (!value) return;
    if (std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) {
          return input.get() == value.get() &&
                 input->getUniqueName() == value->getUniqueName() &&
                 input->getDataType() == value->getDataType();
        })) {
      result.insert(value.get());
      return;
    }
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*value)) visit(child);
  };
  if (part.hasProjectionBody()) {
    const auto* body = part.getProjectionBody();
    for (const auto& values : {body->getProjectionExpressions(), body->getGroupByExpressions(),
                               body->getOrderByExpressions()})
      for (const auto& value : values) visit(value);
  }
  visit(part.getProjectionBodyPredicate());
  for (uint32_t i = 0; i < part.getNumReadingClause(); ++i) {
    const auto* clause = part.getReadingClause(i);
    if (clause->getClauseType() == ClauseType::MATCH)
      visit(clause->constCast<BoundMatchClause>().getPredicate());
  }
  return result;
}

// A complete DISTINCT edge count for one endpoint pair is constant within a
// query group that retains both identities. MAX reads that value once even
// when upstream paths duplicate the pair. The OPTIONAL null row contributes
// zero. No union across different materialization keys is permitted here.
static bool provesCompleteOptionalPairCount(
    const catalog::FillIndexAuxInfo& info, const PatternIR& matchedIndex,
    const PatternIR& queryIR, const BoundMatchClause& match,
    const NormalizedQueryPart& part, main::ClientContext* context,
    const NormalizedSingleQuery* wholeQuery) {
  if (match.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH ||
      !part.hasProjectionBody() ||
      !hasOrderedDerivedRelEndpoints(info, context) ||
      !info.getPredicateTemplates().empty() || !info.getExistsPredicates().empty() ||
      matchedIndex.nodesInfo.size() != 2 || matchedIndex.relsInfo.size() != 1 ||
      queryIR.nodesInfo.size() != 2 || queryIR.relsInfo.size() != 1 ||
      matchedIndex.relsInfo[0].relType != QueryRelType::NON_RECURSIVE ||
      matchedIndex.relsInfo[0].edgeType != queryIR.relsInfo[0].edgeType) return false;
  const auto src = matchedIndex.nodeUniqNameToPos.find(info.getSrcNodeUniqueName());
  const auto dst = matchedIndex.nodeUniqNameToPos.find(info.getDstNodeUniqueName());
  if (src == matchedIndex.nodeUniqNameToPos.end() || dst == matchedIndex.nodeUniqNameToPos.end()) return false;
  const auto querySrc = matchedIndex.nodesInfo[src->second].matchedIdx;
  const auto queryDst = matchedIndex.nodesInfo[dst->second].matchedIdx;
  if (querySrc >= queryIR.nodesInfo.size() || queryDst >= queryIR.nodesInfo.size()) return false;
  // The counted edge disappears. A different clause must not consume its
  // identity or attributes after this replacement.
  const auto& countedEdge = queryIR.relsInfo[0].uniqName;
  for (uint32_t i = 0; i < part.getNumReadingClause(); ++i) {
    const auto* clause = part.getReadingClause(i);
    if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) continue;
    const auto& other = clause->constCast<BoundMatchClause>();
    if (&other == &match) continue;
    if (collectDependentVarNames(other.getPredicate()).contains(countedEdge)) return false;
    const auto* collection = other.getQueryGraphCollection();
    for (uint32_t j = 0; j < collection->getNumQueryGraphs(); ++j) {
      if (collection->getQueryGraph(j)->containsQueryRel(countedEdge)) return false;
    }
  }
  const std::set<std::string> keys{queryIR.nodesInfo[querySrc].uniqName,
                                   queryIR.nodesInfo[queryDst].uniqName};
  // Reusing a node with an earlier property map can repeat that condition on
  // this OPTIONAL MATCH. A deterministic condition on the retained endpoint
  // keys accepts either every edge in a pair or none, so the complete count
  // remains valid. Keep the predicate at this boundary for null extension.
  // Edge predicates, subqueries and arbitrary functions can select individual
  // source rows and must not consume a precomputed complete count.
  std::function<bool(const std::shared_ptr<Expression>&)> pairPredicate =
      [&](const auto& expression) {
    if (expression == nullptr) return true;
    switch (expression->expressionType) {
    case ExpressionType::LITERAL:
    case ExpressionType::PARAMETER:
      return true;
    case ExpressionType::PROPERTY:
      return keys.contains(expression->template constCast<PropertyExpression>()
                               .getVariableName());
    case ExpressionType::PATTERN:
      return expression->getDataType().getLogicalTypeID() == LogicalTypeID::NODE &&
          keys.contains(expression->getUniqueName());
    default:
      if (!ExpressionTypeUtil::isBoolean(expression->expressionType) &&
          !ExpressionTypeUtil::isComparison(expression->expressionType) &&
          !ExpressionTypeUtil::isNullOperator(expression->expressionType)) return false;
      for (const auto& child : expression->getChildren())
        if (!pairPredicate(child)) return false;
      return true;
    }
  };
  if (!pairPredicate(match.getPredicate())) return false;
  std::set<std::string> groups;
  const auto* body = part.getProjectionBody();
  for (const auto& group : body->getGroupByExpressions()) {
    if (group->expressionType == ExpressionType::PATTERN &&
        group->getDataType().getLogicalTypeID() == LogicalTypeID::NODE &&
        keys.contains(group->getUniqueName())) {
      groups.insert(group->getUniqueName());
    } else if (group->expressionType == ExpressionType::PROPERTY) {
      const auto& property = group->constCast<PropertyExpression>();
      if (property.isSingleLabel() && property.isPrimaryKey() &&
          keys.contains(property.getVariableName()))
        groups.insert(property.getVariableName());
    }
  }
  if (groups != keys) return false;
  auto covered = carriedProjectionInputs(part, wholeQuery);
  for (const auto& aggregate : body->getAggregateExpressions()) covered.insert(aggregate.get());
  if (!completeKeyGroupInputsAreClosed(info, matchedIndex, queryIR, match, part, wholeQuery,
                                      nullptr, nullptr, &covered))
    return false;
  const auto aggregates = body->getAggregateExpressions();
  if (aggregates.empty()) return false;
  return std::all_of(aggregates.begin(), aggregates.end(), [&](const auto& expression) {
    if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
    const auto& aggregate = expression->template constCast<AggregateFunctionExpression>();
    return aggregate.isDistinct() && aggregate.getFunction().name == "COUNT" &&
        aggregate.getNumChildren() == 1 &&
        aggregate.getChild(0)->getUniqueName() == queryIR.relsInfo[0].uniqName;
  });
}

// A directed single-edge source emits every physical relationship exactly once
// within its ordered endpoint group, including parallel edges as separate
// entities. COUNT(edge) and COUNT(DISTINCT edge) in that source therefore both
// supply a complete-pair DISTINCT count. FILL registration can canonicalize
// the latter to COUNT(*) while preserving its original source text, so the
// query and stored aggregate templates intentionally differ. Do not infer this
// from a small PatternIR alone: a prior stage, disconnected binding, or another
// source relationship can duplicate that edge before aggregation.
static std::optional<size_t> canonicalSingleEdgePairCountColumn(
    const Expression& expression, const catalog::FillIndexAuxInfo& info,
    const PatternIR& indexIR, const PatternIR& queryIR,
    const BoundMatchClause& match, main::ClientContext* context) {
  if (expression.expressionType != ExpressionType::AGGREGATE_FUNCTION ||
      match.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH ||
      info.usesWorkloadCompatibility() || info.getSourceQuery().empty() ||
      !hasOrderedDerivedRelEndpoints(info, context) ||
      indexIR.nodesInfo.size() != 2 || indexIR.relsInfo.size() != 1 ||
      queryIR.nodesInfo.size() != 2 || queryIR.relsInfo.size() != 1 ||
      indexIR.relsInfo[0].relType != QueryRelType::NON_RECURSIVE ||
      indexIR.relsInfo[0].edgeType == EdgeType::UNDIRECTED)
    return std::nullopt;
  const auto& aggregate = expression.constCast<AggregateFunctionExpression>();
  if (!aggregate.isDistinct() || aggregate.getFunction().name != "COUNT" ||
      aggregate.getNumChildren() != 1 ||
      aggregate.getChild(0)->expressionType != ExpressionType::PATTERN ||
      aggregate.getChild(0)->getUniqueName() != queryIR.relsInfo[0].uniqName)
    return std::nullopt;
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return std::nullopt;
    Binder sourceBinder(context);
    sourceBinder.setAllowIndexRewrite(false);
    const auto statement = sourceBinder.bind(*parsed[0]);
    const auto& source = statement->constCast<BoundRegularQuery>();
    if (source.getNumSingleQueries() != 1 || !source.getPreQueryPart().empty() ||
        source.getPostSingleQuery() || source.getSingleQuery(0)->getNumQueryParts() != 1)
      return std::nullopt;
    const auto& part = *source.getSingleQuery(0)->getQueryPart(0);
    if (part.hasUpdatingClause() || part.getNumReadingClause() != 1 ||
        !part.hasProjectionBody() || part.hasProjectionBodyPredicate() ||
        part.getReadingClause(0)->getClauseType() != ClauseType::MATCH)
      return std::nullopt;
    const auto& sourceMatch = part.getReadingClause(0)->constCast<BoundMatchClause>();
    const auto* graphs = sourceMatch.getQueryGraphCollection();
    if (sourceMatch.getMatchClauseType() != MatchClauseType::MATCH ||
        sourceMatch.getPredicate() != nullptr || graphs->getNumQueryGraphs() != 1)
      return std::nullopt;
    const auto* graph = graphs->getQueryGraph(0);
    const auto pattern = convertGraphPatternToIR(*graph);
    if (pattern.nodesInfo.size() != 2 || pattern.relsInfo.size() != 1 ||
        pattern.relsInfo[0].relType != QueryRelType::NON_RECURSIVE ||
        pattern.relsInfo[0].edgeType == EdgeType::UNDIRECTED)
      return std::nullopt;
    const auto& body = *part.getProjectionBody();
    if (body.isDistinct() || body.hasSkipOrLimit() || body.hasOrderByExpressions())
      return std::nullopt;
    const auto identity = [](const std::shared_ptr<Expression>& value) {
      if (value->expressionType == ExpressionType::PATTERN &&
          value->getDataType().getLogicalTypeID() == LogicalTypeID::NODE)
        return value->getUniqueName();
      if (value->expressionType == ExpressionType::PROPERTY) {
        const auto& property = value->constCast<PropertyExpression>();
        if (property.isSingleLabel() && property.isPrimaryKey())
          return property.getVariableName();
      }
      return std::string{};
    };
    const auto values = body.getProjectionExpressions();
    if (values.size() < 3) return std::nullopt;
    const auto& edge = pattern.relsInfo[0];
    const std::set<std::string> endpoints{
        pattern.nodesInfo.at(edge.srcNodeIdx).uniqName,
        pattern.nodesInfo.at(edge.dstNodeIdx).uniqName};
    const std::set<std::string> keys{identity(values[0]), identity(values[1])};
    if (keys.size() != 2 || keys.contains("") || keys != endpoints)
      return std::nullopt;
    std::set<std::string> groups;
    for (const auto& group : body.getGroupByExpressions()) groups.insert(identity(group));
    if (groups != keys) return std::nullopt;
    for (size_t i = 2; i < values.size(); ++i) {
      const auto column = i - 2;
      if (column >= info.getTargetColumns().size() ||
          info.getTargetColumns()[column].starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
          info.getTargetColumns()[column].starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX) ||
          values[i]->expressionType != ExpressionType::AGGREGATE_FUNCTION)
        continue;
      const auto& count = values[i]->constCast<AggregateFunctionExpression>();
      if (count.getFunction().name == "COUNT" &&
          count.getNumChildren() == 1 &&
          count.getChild(0)->expressionType == ExpressionType::PATTERN &&
          count.getChild(0)->getUniqueName() == edge.uniqName)
        return column;
    }
  } catch (const std::exception&) {
    // Missing or opaque provenance cannot justify a DISTINCT relaxation.
  }
  return std::nullopt;
}

static std::unordered_set<std::string> collectEndpointNodeNames(
    const QueryGraph& graph, const catalog::FillIndexAuxInfo& indexInfo) {
  std::unordered_set<std::string> endpointNames;
  for (auto& node : graph.getQueryNodes()) {
    if (nodeHasTableID(node, indexInfo.getSrcTableId()) ||
        nodeHasTableID(node, indexInfo.getDstTableId())) {
      endpointNames.insert(node->getUniqueName());
    }
  }
  return endpointNames;
}

static bool queryPartHasDirectedRelAnchor(
    NormalizedQueryPart& queryPart, BoundMatchClause& matchClause,
    const catalog::FillIndexAuxInfo& indexInfo, const RelLabelMap& relLabels) {
  auto* currentGraph =
      matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  if (currentGraph == nullptr) {
    return false;
  }
  auto endpointNames = collectEndpointNodeNames(*currentGraph, indexInfo);
  if (endpointNames.size() < 2) {
    return false;
  }

  for (uint32_t i = 0; i < queryPart.getNumReadingClause(); ++i) {
    auto* clause = queryPart.getReadingClause(i);
    if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
      continue;
    }
    auto& anchorMatch = clause->cast<BoundMatchClause>();
    if (anchorMatch.getMatchClauseType() != MatchClauseType::MATCH) {
      continue;
    }
    auto* collection = anchorMatch.getQueryGraphCollection();
    for (uint32_t graphIdx = 0; graphIdx < collection->getNumQueryGraphs();
         ++graphIdx) {
      auto* graph = collection->getQueryGraph(graphIdx);
      if (graph == nullptr) {
        continue;
      }
      for (auto& rel : graph->getQueryRels()) {
        if (rel == nullptr || rel->getSingleEntry() == nullptr ||
            rel->getDirectionType() != RelDirectionType::SINGLE ||
            tableLabel(rel->getSingleEntry(), relLabels) !=
                indexInfo.getTableName()) {
          continue;
        }
        if (endpointNames.contains(rel->getSrcNode()->getUniqueName()) &&
            endpointNames.contains(rel->getDstNode()->getUniqueName())) {
          return true;
        }
      }
    }
  }
  return false;
}

static bool indexHasDirectionAwareSiblingColumns(
    main::ClientContext* clientContext,
    const catalog::FillIndexAuxInfo& indexInfo) {
  if (clientContext == nullptr) {
    return false;
  }
  auto* tableEntry = tableByName(clientContext->getCatalog(), clientContext->getTransaction(),
      indexInfo.getTableName(), clientContext->useInternalCatalogEntry());
  if (tableEntry == nullptr) {
    return false;
  }
  for (auto& property : tableEntry->getProperties()) {
    if (directionAwareCounterpartColumns(*tableEntry, property.getName())
            .has_value()) {
      return true;
    }
  }
  const auto counterpartName = [](const std::string& propertyName)
      -> std::optional<std::string> {
    constexpr std::string_view fwdSuffix{"_fwd"};
    constexpr std::string_view revSuffix{"_rev"};
    if (endsWith(propertyName, std::string(fwdSuffix))) {
      return propertyName.substr(0, propertyName.size() - fwdSuffix.size()) +
             std::string(revSuffix);
    }
    if (endsWith(propertyName, std::string(revSuffix))) {
      return propertyName.substr(0, propertyName.size() - revSuffix.size()) +
             std::string(fwdSuffix);
    }
    return std::nullopt;
  };
  for (const auto& targetColumn : indexInfo.getTargetColumns()) {
    const auto counterpart = counterpartName(targetColumn);
    if (!counterpart.has_value()) {
      continue;
    }
    for (auto* rawEntry : clientContext->getCatalog()->getExtendedIndexEntries(
             clientContext->getTransaction())) {
      if (rawEntry == nullptr) {
        continue;
      }
      const auto& sibling = rawEntry->getAuxInfoUnsafe()
                                .cast<catalog::FillIndexAuxInfo>();
      if (sibling.getFillType() != catalog::FillEntryType::REL ||
          sibling.getTableName() != indexInfo.getTableName()) {
        continue;
      }
      if (std::find(sibling.getTargetColumns().begin(),
                    sibling.getTargetColumns().end(), *counterpart) !=
          sibling.getTargetColumns().end()) {
        return true;
      }
    }
  }
  return false;
}

struct SplitRelPattern {
  PatternIR interactionPattern;
  size_t anchorRelPos;
};

static std::optional<SplitRelPattern> splitFillAnchorRelationship(
    const PatternIR& fullPattern, const catalog::FillIndexAuxInfo& indexInfo,
    catalog::Catalog* catalog, const transaction::Transaction* transaction,
    const RelLabelMap& relLabels) {
  if (catalog == nullptr || indexInfo.getFillType() != FillEntryType::REL) {
    return std::nullopt;
  }
  size_t anchorRelPos = UINT64_MAX;
  for (size_t relPos = 0; relPos < fullPattern.relsInfo.size(); ++relPos) {
    const auto& rel = fullPattern.relsInfo[relPos];
    const auto isFillAnchor = std::any_of(
        rel.relLabels.begin(), rel.relLabels.end(), [&](const auto tableID) {
          auto* entry = catalog->getTableCatalogEntry(transaction, tableID);
          return entry != nullptr &&
                 tableLabel(entry, relLabels) ==
                     indexInfo.getTableName();
        });
    if (!isFillAnchor) {
      continue;
    }
    if (anchorRelPos != UINT64_MAX) {
      return std::nullopt;
    }
    anchorRelPos = relPos;
  }
  if (anchorRelPos == UINT64_MAX) {
    return std::nullopt;
  }

  auto interaction = fullPattern;
  interaction.relsInfo.erase(interaction.relsInfo.begin() + anchorRelPos);
  interaction.relUniqueNameToPos.clear();
  interaction.matchedNodeStarts.clear();
  for (auto& node : interaction.nodesInfo) {
    node.matchedIdx = UINT64_MAX;
    node.indexName.clear();
    node.undirected.clear();
    node.outgoing.clear();
    node.incoming.clear();
  }
  for (size_t relPos = 0; relPos < interaction.relsInfo.size(); ++relPos) {
    auto& rel = interaction.relsInfo[relPos];
    rel.matchedIdx = UINT64_MAX;
    rel.indexName.clear();
    interaction.relUniqueNameToPos[rel.uniqName] = relPos;
    if (rel.edgeType == EdgeType::UNDIRECTED) {
      interaction.nodesInfo[rel.srcNodeIdx].undirected.emplace_back(
          rel.dstNodeIdx, rel.relLabels);
      interaction.nodesInfo[rel.dstNodeIdx].undirected.emplace_back(
          rel.srcNodeIdx, rel.relLabels);
    } else {
      interaction.nodesInfo[rel.srcNodeIdx].outgoing.emplace_back(
          rel.dstNodeIdx, rel.relLabels);
      interaction.nodesInfo[rel.dstNodeIdx].incoming.emplace_back(
          rel.srcNodeIdx, rel.relLabels);
    }
  }
  return SplitRelPattern{std::move(interaction), anchorRelPos};
}

static std::shared_ptr<RelExpression> synthesizeFillRelExpression(
    Binder* binder, main::ClientContext* clientContext,
    const catalog::FillIndexAuxInfo& indexInfo,
    const std::vector<std::shared_ptr<NodeExpression>>& endpointNodes) {
  if (binder == nullptr) {
    THROW_INDEX_EXCEPTION(
        "Rewriter requires a Binder to synthesize relationship indexes.");
  }
  if (endpointNodes.size() != 2) {
    return nullptr;
  }

  auto relEntries = binder->bindRelTableEntries({indexInfo.getTableName()});
  if (relEntries.empty()) {
    THROW_INDEX_EXCEPTION("Cannot find fill relationship table: " +
                          indexInfo.getTableName());
  }

  auto directionType = indexInfo.isFillUndirectedRel() ? RelDirectionType::BOTH
                                                       : RelDirectionType::SINGLE;
  auto srcNode = endpointNodes[0];
  auto dstNode = endpointNodes[1];
  if (auto directedEndpoints =
          getDirectedFillRelEndpoints(endpointNodes, indexInfo);
      directedEndpoints.has_value()) {
    srcNode = directedEndpoints->first;
    dstNode = directedEndpoints->second;
  } else if (!indexInfo.isFillUndirectedRel()) {
    LOG(INFO) << "Falling back to BOTH direction while synthesizing "
              << indexInfo.getTableName()
              << " because fill endpoints could not be oriented.";
    directionType = RelDirectionType::BOTH;
  }

  return binder->createNonRecursiveQueryRel(indexInfo.getTableName(), relEntries,
                                            srcNode, dstNode, directionType);
}

// Find anchor node position in the index PatternIR by table uniqName.
static size_t findAnchorInIndexIR(const catalog::FillIndexAuxInfo& indexInfo) {
  auto& ir = indexInfo.getPatternIR();
  if (indexInfo.getFillType() == FillEntryType::REL) {
    auto src = ir.nodeUniqNameToPos.find(indexInfo.getSrcNodeUniqueName());
    return src == ir.nodeUniqNameToPos.end() ? 0 : src->second;
  }
  auto it = ir.nodeUniqNameToPos.find(indexInfo.getTableUniqName());
  if (it != ir.nodeUniqNameToPos.end()) {
    return it->second;
  }
  // Fallback: search by varName or tableName labels
  for (size_t i = 0; i < ir.nodesInfo.size(); ++i) {
    if (ir.nodesInfo[i].uniqName == indexInfo.getTableUniqName() ||
        ir.nodesInfo[i].varName == indexInfo.getTableVariableName()) {
      return i;
    }
  }
  NEUG_ASSERT(false && "Anchor node not found in index PatternIR");
  return 0;
}

static bool sameNodeAggregateColumnDefinition(
    const std::shared_ptr<catalog::IndexColnumInfo>& lhs,
    const std::shared_ptr<catalog::IndexColnumInfo>& rhs) {
  if (lhs != nullptr && rhs != nullptr &&
      lhs->columnType == catalog::IndexColumnType::REL_AGGREGATE &&
      rhs->columnType == catalog::IndexColumnType::REL_AGGREGATE) {
    const auto* l = dynamic_cast<catalog::RelIndexColnumInfo*>(lhs.get());
    const auto* r = dynamic_cast<catalog::RelIndexColnumInfo*>(rhs.get());
    return l != nullptr && r != nullptr && l->nameTemplate == r->nameTemplate;
  }
  if (lhs == nullptr || rhs == nullptr ||
      lhs->columnType != catalog::IndexColumnType::NODE_AGGREGATE ||
      rhs->columnType != catalog::IndexColumnType::NODE_AGGREGATE ||
      lhs->propertyName != rhs->propertyName) {
    return false;
  }
  auto* lhsAggregate =
      dynamic_cast<catalog::NodeAggregationIndexColnumInfo*>(lhs.get());
  auto* rhsAggregate =
      dynamic_cast<catalog::NodeAggregationIndexColnumInfo*>(rhs.get());
  return lhsAggregate != nullptr && rhsAggregate != nullptr &&
         lhsAggregate->aggregateFuncName == rhsAggregate->aggregateFuncName &&
         lhsAggregate->nameTemplate == rhsAggregate->nameTemplate;
}

static bool sameNodeAggregateIndexExceptAnchorLabel(
    const catalog::FillIndexAuxInfo& lhs,
    const catalog::FillIndexAuxInfo& rhs, catalog::Catalog* catalog,
    const transaction::Transaction* transaction, const RelLabelMap& relLabels,
    main::ClientContext* context) {
  const auto traceReject = [&](std::string_view reason) {
    VLOG(1) << "Multi-label aggregate group rejected "
            << lhs.getTableName() << " + " << rhs.getTableName() << ": "
            << reason;
    return false;
  };
  // Each physical-label member hashes its own source query into the private
  // row-count name. That storage detail must not split an otherwise identical
  // public aggregate family; the column definitions are still checked below.
  const auto& lhsTargets = lhs.getTargetColumns();
  const auto& rhsTargets = rhs.getTargetColumns();
  const bool sameTargets = std::equal(
      lhsTargets.begin(), lhsTargets.end(), rhsTargets.begin(), rhsTargets.end(),
      [](const auto& left, const auto& right) {
        return left == right ||
               (left.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX) &&
                right.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX));
      });
  if (lhs.getFillType() != rhs.getFillType() ||
      (lhs.getFillType() != catalog::FillEntryType::NODE &&
       lhs.getFillType() != catalog::FillEntryType::REL) ||
      !sameTargets ||
      lhs.getPredicateTemplates() != rhs.getPredicateTemplates()) {
    return traceReject("fill type or target columns differ");
  }
  const bool derivedFamily = hasOrderedDerivedRelEndpoints(lhs, context);
  if (derivedFamily != hasOrderedDerivedRelEndpoints(rhs, context)) {
    return traceReject("derived and existing relationship carriers differ");
  }
  if (derivedFamily) {
    if (hasRecursiveEndpointContract(lhs)) {
      const auto left = recursiveEndpointSource(lhs, context);
      const auto right = recursiveEndpointSource(rhs, context);
      if (!left || !right || *left != *right)
        return traceReject("recursive source contracts differ");
    }
    const auto existsKeys = [](const auto& info) {
      std::vector<std::string> keys;
      for (const auto& predicate : info.getExistsPredicates())
        keys.push_back(fillExistsPredicateKey(predicate));
      std::sort(keys.begin(), keys.end());
      return keys;
    };
    if (existsKeys(lhs) != existsKeys(rhs))
      return traceReject("derived family correlated predicates differ");
  }
  if (lhs.getFillType() == catalog::FillEntryType::REL && !derivedFamily) {
    auto* lhsTable = tableByName(catalog, transaction, lhs.getTableName());
    auto* rhsTable = tableByName(catalog, transaction, rhs.getTableName());
    if (lhsTable == nullptr || rhsTable == nullptr ||
        tableLabel(lhsTable, relLabels) != tableLabel(rhsTable, relLabels) ||
        lhs.getDstTableId() != rhs.getDstTableId()) {
      return traceReject("relationship groups or destination labels differ");
    }
  }
  const auto& lhsColumns = lhs.getIndexColumnInfos();
  const auto& rhsColumns = rhs.getIndexColumnInfos();
  if (lhsColumns.size() != rhsColumns.size()) {
    return traceReject("index column counts differ");
  }
  for (size_t i = 0; i < lhsColumns.size(); ++i) {
    if (!sameNodeAggregateColumnDefinition(lhsColumns[i], rhsColumns[i])) {
      return traceReject("aggregate column definitions differ");
    }
  }

  const auto& lhsPattern = lhs.getPatternIR();
  const auto& rhsPattern = rhs.getPatternIR();
  if (lhsPattern.nodesInfo.size() != rhsPattern.nodesInfo.size() ||
      lhsPattern.relsInfo.size() != rhsPattern.relsInfo.size()) {
    return traceReject("pattern sizes differ");
  }
  const auto lhsAnchor = findAnchorInIndexIR(lhs);
  const auto rhsAnchor = findAnchorInIndexIR(rhs);
  if (lhsAnchor != rhsAnchor || lhsAnchor >= lhsPattern.nodesInfo.size()) {
    return traceReject("anchor positions differ");
  }
  std::set<size_t> partitionPositions{lhsAnchor};
  if (derivedFamily) {
    const auto lhsDst = lhsPattern.nodeUniqNameToPos.at(lhs.getDstNodeUniqueName());
    const auto rhsDst = rhsPattern.nodeUniqNameToPos.at(rhs.getDstNodeUniqueName());
    if (lhsDst != rhsDst) return traceReject("ordered destination positions differ");
    partitionPositions.insert(lhsDst);
  }
  for (size_t i = 0; i < lhsPattern.nodesInfo.size(); ++i) {
    const auto& lhsNode = lhsPattern.nodesInfo[i];
    const auto& rhsNode = rhsPattern.nodesInfo[i];
    if (lhsNode.varName != rhsNode.varName ||
        (!partitionPositions.contains(i) &&
         !PatternIR::labelsEqual(lhsNode.nodeLabels, rhsNode.nodeLabels))) {
      return traceReject("non-anchor node definitions differ");
    }
  }
  for (size_t i = 0; i < lhsPattern.relsInfo.size(); ++i) {
    const auto& lhsRel = lhsPattern.relsInfo[i];
    const auto& rhsRel = rhsPattern.relsInfo[i];
    if (lhsRel.varName != rhsRel.varName ||
        lhsRel.srcNodeIdx != rhsRel.srcNodeIdx ||
        lhsRel.dstNodeIdx != rhsRel.dstNodeIdx ||
        lhsRel.edgeType != rhsRel.edgeType ||
        lhsRel.relType != rhsRel.relType) {
      return traceReject("relationship topology differs");
    }
    std::vector<std::string> lhsLabels;
    std::vector<std::string> rhsLabels;
    for (const auto label : lhsRel.relLabels) {
      lhsLabels.push_back(tableLabel(catalog->getTableCatalogEntry(transaction, label), relLabels));
    }
    for (const auto label : rhsRel.relLabels) {
      rhsLabels.push_back(tableLabel(catalog->getTableCatalogEntry(transaction, label), relLabels));
    }
    std::sort(lhsLabels.begin(), lhsLabels.end());
    std::sort(rhsLabels.begin(), rhsLabels.end());
    lhsLabels.erase(std::unique(lhsLabels.begin(), lhsLabels.end()),
                    lhsLabels.end());
    rhsLabels.erase(std::unique(rhsLabels.begin(), rhsLabels.end()),
                    rhsLabels.end());
    // A union-label anchor can use different physical relationship tables for
    // each anchor table. The combined PatternIR below still has to match the
    // query's complete relationship-label union. Relationships that do not
    // touch the anchor remain identical across grouped indexes.
    if (lhsLabels != rhsLabels &&
        !partitionPositions.contains(lhsRel.srcNodeIdx) &&
        !partitionPositions.contains(lhsRel.dstNodeIdx)) {
      return traceReject("non-anchor relationship labels differ");
    }
  }
  return true;
}

static std::optional<size_t> findLocalAnchorNodePos(
    const PatternIR& nodesIR, const catalog::FillIndexAuxInfo& indexInfo) {
  const auto anchorIndexPos = findAnchorInIndexIR(indexInfo);
  for (size_t nodePos = 0; nodePos < nodesIR.nodesInfo.size(); ++nodePos) {
    if (nodesIR.nodesInfo[nodePos].matchedIdx == anchorIndexPos) {
      return nodePos;
    }
  }
  return std::nullopt;
}

static std::optional<std::pair<std::string, std::string>>
findNodeBoundaryProperty(size_t indexNodePos,
                         const catalog::FillIndexAuxInfo& indexInfo,
                         main::ClientContext* context) {
  const auto& indexIR = indexInfo.getPatternIR();
  if (indexNodePos >= indexIR.nodesInfo.size()) {
    return std::nullopt;
  }
  const auto& node = indexIR.nodesInfo[indexNodePos];
  // Reconstruct a graph identity only from its declared primary key. An
  // ordinary property (even with the same storage type) can match several
  // vertices and would multiply the surviving suffix rows.
  if (context == nullptr || node.nodeLabels.size() != 1) return std::nullopt;
  const auto* source = context->getCatalog()->getTableCatalogEntry(
      context->getTransaction(), node.nodeLabels[0]);
  const auto* target = tableByName(context->getCatalog(), context->getTransaction(),
      indexInfo.getTableName(), context->useInternalCatalogEntry());
  if (source == nullptr || target == nullptr ||
      source->getType() != CatalogEntryType::NODE_TABLE_ENTRY)
    return std::nullopt;
  const auto& primary = source->constCast<NodeTableCatalogEntry>().getPrimaryKeyDefinition();
  const auto& indexColumns = indexInfo.getIndexColumnInfos();
  const auto& targetColumns = indexInfo.getTargetColumns();
  for (size_t columnPos = 0;
       columnPos < indexColumns.size() && columnPos < targetColumns.size();
       ++columnPos) {
    const auto& column = indexColumns[columnPos];
    if (column->columnType == IndexColumnType::NODE_PROPERTY &&
        (column->nodeUnique == node.uniqName ||
         (!node.varName.empty() && column->nodeUnique == node.varName)) &&
        column->propertyName == primary.getName() &&
        target->containsProperty(targetColumns[columnPos]) &&
        target->getProperty(targetColumns[columnPos]).getType() == primary.getType()) {
      return std::make_pair(column->propertyName,
                            targetColumns[columnPos]);
    }
  }
  return std::nullopt;
}

// Removing a source edge and recovering its endpoint by a stored key can
// disconnect the remaining topology. QueryGraph represents one connected
// component; predicates join separate components at collection level. Repair
// that invariant after all local rewrites, while retaining every expression
// object, node identity and cross-component equality predicate.
static void restoreConnectedQueryGraphs(NormalizedSingleQuery& query) {
  const auto components = [](const QueryGraphCollection& collection) {
    using Identities = std::set<std::string>;
    std::multiset<std::pair<Identities, Identities>> result;
    for (uint32_t i = 0; i < collection.getNumQueryGraphs(); ++i) {
      const auto* graph = collection.getQueryGraph(i);
      Identities nodes, relationships;
      for (const auto& node : graph->getQueryNodes()) {
        nodes.insert(node->getUniqueName());
      }
      for (const auto& relationship : graph->getQueryRels()) {
        relationships.insert(relationship->getUniqueName());
      }
      result.emplace(std::move(nodes), std::move(relationships));
    }
    return result;
  };
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi) {
    auto* part = query.getQueryPartUnsafe(pi);
    for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
      auto* clause = part->getReadingClause(ci);
      if (clause->getClauseType() != ClauseType::MATCH) continue;
      auto* collection = clause->cast<BoundMatchClause>().getQueryGraphCollectionUnsafe();
      QueryGraphCollection connected;
      for (const auto& node : collection->getQueryNodes()) {
        QueryGraph singleton;
        singleton.addQueryNode(node);
        connected.addAndMergeQueryGraphIfConnected(std::move(singleton));
      }
      for (const auto& rel : collection->getQueryRels()) {
        QueryGraph edge;
        addRelAndEndpoints(edge, rel);
        connected.addAndMergeQueryGraphIfConnected(std::move(edge));
      }
      if (connected.getNumQueryGraphs() == 0) continue;
      connected.finalize();
      // Repair connectivity only. Rebuilding an already valid component
      // changes node/edge order and can perturb unrelated OPTIONAL plans.
      if (components(*collection) == components(connected)) continue;
      *collection = std::move(connected);
    }
  }
}

}  // namespace

bool RewriterV4::provesNodePresenceSetSourceCached(const catalog::FillIndexAuxInfo& info) {
  auto [cached, inserted] = nodePresenceSetSourceProofs.try_emplace(&info, false);
  if (inserted) cached->second = provesNodePresenceSetSource(info, *clientContext);
  return cached->second;
}

bool RewriterV4::provesSingleStageEndpointSetSourceCached(const catalog::FillIndexAuxInfo& info) {
  auto [cached, inserted] = endpointSetSourceProofs.try_emplace(&info, false);
  if (inserted) cached->second = provesSingleStageEndpointSetSource(info, *clientContext);
  return cached->second;
}

// --- FillTableContext ---

std::optional<RewriterV4::FillTableContext> RewriterV4::getFillTableContext(
    const catalog::FillIndexAuxInfo& indexInfo) const {
  auto* catalog = clientContext->getCatalog();
  auto* transaction = clientContext->getTransaction();
  // A REL-group carrier (one FILL routed to several child tables) has no
  // single table entry; callers handle the group through its children.
  if (!catalog->containsTable(transaction, indexInfo.getTableName(),
                              clientContext->useInternalCatalogEntry())) {
    return std::nullopt;
  }
  auto* fillTableEntry = catalog->getTableCatalogEntry(
      transaction, indexInfo.getTableName(), clientContext->useInternalCatalogEntry());
  if (fillTableEntry == nullptr) {
    return std::nullopt;
  }
  return FillTableContext{fillTableEntry, fillTableEntry->getTableID()};
}

std::shared_ptr<PropertyExpression> RewriterV4::makeFillPropertyExpression(
    const FillTableContext& fillTable,
    const PropertyRewriteTarget& target) const {
  auto propType =
      fillTable.entry->getProperty(target.newPropertyName).getType().copy();
  common::table_id_map_t<SingleLabelPropertyInfo> infos;
  infos.insert({fillTable.tableID,
                SingleLabelPropertyInfo(true /* exists */,
                                        false /* isPrimaryKey */)});
  for (const auto& additional : additionalFillTables) {
    if (additional.tableID == fillTable.tableID ||
        !additional.entry->containsProperty(target.newPropertyName) ||
        additional.entry->getProperty(target.newPropertyName).getType() !=
            propType) {
      continue;
    }
    infos.insert({additional.tableID,
                  SingleLabelPropertyInfo(true /* exists */,
                                          false /* isPrimaryKey */)});
  }
  return std::make_shared<PropertyExpression>(
      std::move(propType), target.newPropertyName, target.newVarUniqueName,
      target.newVarRawName, std::move(infos));
}

// --- merged query IR ---

PatternIR RewriterV4::buildMergedQueryIR(
    const NormalizedSingleQuery& query) const {
  std::vector<PatternIR> patterns;
  for (uint32_t i = 0; i < query.getNumQueryParts(); ++i) {
    auto* queryPart = query.getQueryPart(i);
    if (queryPart == nullptr || queryPart->getNumReadingClause() == 0) {
      continue;
    }
    for (uint32_t j = 0; j < queryPart->getNumReadingClause(); ++j) {
      auto* clause = queryPart->getReadingClause(j);
      if (clause == nullptr ||
          clause->getClauseType() != ClauseType::MATCH) {
        continue;
      }
      auto& matchClause = clause->cast<BoundMatchClause>();
      auto* graphCollection = matchClause.getQueryGraphCollectionUnsafe();
      if (graphCollection == nullptr) {
        continue;
      }
      for (uint32_t g = 0; g < graphCollection->getNumQueryGraphs(); ++g) {
        auto* graph = graphCollection->getQueryGraph(g);
        if (graph == nullptr) {
          continue;
        }
        patterns.emplace_back(convertGraphPatternToIR(*graph));
      }
    }
  }
  if (patterns.empty()) {
    return PatternIR{};
  }
  if (patterns.size() == 1) {
    return patterns[0];
  }
  // These graphs belong to one already bound query. A WITH alias carries its
  // unique identity, whereas a reused spelling after a scope boundary may
  // denote a different node (and even a different table). The generic IR
  // merger also serves separately bound sources and permits a raw-name
  // fallback; disable that fallback only for this bound-query merge.
  std::unordered_map<std::string, std::string> originalNames;
  for (auto& pattern : patterns) {
    for (auto& node : pattern.nodesInfo) {
      originalNames.try_emplace(node.uniqName, node.varName);
      node.varName.clear();
    }
  }
  std::vector<std::vector<uint32_t>> nodeRemaps;
  auto merged = PatternIR::merge(patterns, nodeRemaps);
  for (auto& node : merged.nodesInfo) {
    node.varName = originalNames.at(node.uniqName);
  }
  return merged;
}

void RewriterV4::propagateMatchedIndices(const PatternIR& mergedQueryIR,
                                          PatternIR& localIR) const {
  // Propagate node matchedIdx from merged to local using uniqName.
  for (size_t i = 0; i < localIR.nodesInfo.size(); ++i) {
    const auto& uniq = localIR.nodesInfo[i].uniqName;
    auto it = mergedQueryIR.nodeUniqNameToPos.find(uniq);
    if (it != mergedQueryIR.nodeUniqNameToPos.end()) {
      localIR.nodesInfo[i].matchedIdx =
          mergedQueryIR.nodesInfo[it->second].matchedIdx;
    }
  }
  // Propagate rel matchedIdx.
  for (size_t i = 0; i < localIR.relsInfo.size(); ++i) {
    const auto& uniq = localIR.relsInfo[i].uniqName;
    auto it = mergedQueryIR.relUniqueNameToPos.find(uniq);
    if (it != mergedQueryIR.relUniqueNameToPos.end()) {
      localIR.relsInfo[i].matchedIdx =
          mergedQueryIR.relsInfo[it->second].matchedIdx;
    }
  }
}

// --- candidate creation ---

std::optional<RewriterV4::IndexRewriteCandidate>
RewriterV4::makeRewriteCandidate(BoundMatchClause& matchClause,
                                  PatternIR& nodesIR,
                                  catalog::IndexCatalogEntry* entry,
                                  const PatternIR& preMatchedIndexIR) {
  auto* queryGraph =
      matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  if (queryGraph == nullptr || entry == nullptr) {
    return std::nullopt;
  }
  auto& indexInfo = entry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  (void)preMatchedIndexIR;

  auto localIndexIR = indexInfo.getPatternIR();
  auto [matchedLocally, _] =
      PatternIR::findMatch(nodesIR, localIndexIR, entry->getName());
  IndexRewriteCandidate candidate{entry, &indexInfo, std::move(localIndexIR),
                                  queryGraph};
  if (!matchedLocally) {
    // The EXISTS proof uses a complete local endpoint mapping. Partial REL
    // anchors and recursive weight coverage do not establish that proof.
    if (!indexInfo.getExistsPredicates().empty()) return std::nullopt;
    if (auto split = splitFillAnchorRelationship(
            indexInfo.getPatternIR(), indexInfo, clientContext->getCatalog(),
            clientContext->getTransaction(), relLabels);
        split.has_value()) {
      auto [matchedInteraction, splitStart] = PatternIR::findMatch(
          nodesIR, split->interactionPattern, entry->getName());
      (void)splitStart;
      if (matchedInteraction) {
        auto splitCandidateIR = indexInfo.getPatternIR();
        for (size_t nodePos = 0;
             nodePos < splitCandidateIR.nodesInfo.size(); ++nodePos) {
          splitCandidateIR.nodesInfo[nodePos].matchedIdx =
              split->interactionPattern.nodesInfo[nodePos].matchedIdx;
        }
        for (auto& rel : splitCandidateIR.relsInfo) {
          rel.matchedIdx = UINT64_MAX;
          auto matchedRel = split->interactionPattern.relUniqueNameToPos.find(
              rel.uniqName);
          if (matchedRel !=
              split->interactionPattern.relUniqueNameToPos.end()) {
            rel.matchedIdx = split->interactionPattern
                                 .relsInfo[matchedRel->second]
                                 .matchedIdx;
          }
        }
        candidate.indexIR = std::move(splitCandidateIR);
        candidate.splitRelAnchor = true;
        return candidate;
      }
    }
    // A recursive relationship can consume a filled weight property without
    // deleting the pattern. All structural rewrites require a complete local
    // match so merged graph offsets can never leak into a QueryGraph.
    candidate.indexIR = preMatchedIndexIR;
    if (matchesWeightedRecursiveRelPropertyIndex(candidate)) {
      candidate.weightedRecursiveRelCoverageOnly = true;
      return candidate;
    }
    return std::nullopt;
  }

  if (indexInfo.fillType == FillEntryType::VARIADIC_REL) {
    if (!matchesVariadicRelationshipIndex(candidate, nodesIR)) {
      return std::nullopt;
    }
  }
  return candidate;
}

bool RewriterV4::matchesWeightedRecursiveRelPropertyIndex(
    const IndexRewriteCandidate& candidate) const {
  auto& indexInfo = *candidate.info;
  if (!indexInfo.isRewriteReady()) return false;
  auto* queryGraph = candidate.queryGraph;
  if (indexInfo.fillType != FillEntryType::REL || queryGraph == nullptr ||
      indexInfo.targetColumns.empty()) {
    return false;
  }
  auto* catalog = clientContext->getCatalog();
  auto* transaction = clientContext->getTransaction();
  for (auto& rel : queryGraph->getQueryRels()) {
    if (rel == nullptr || rel->getRecursiveInfo() == nullptr ||
        rel->getSingleEntry() == nullptr ||
        tableLabel(rel->getSingleEntry(), relLabels) !=
            indexInfo.getTableName()) {
      continue;
    }
    auto& bindData = rel->getRecursiveInfo()->bindData;
    if (bindData == nullptr || bindData->weightPropertyExpr == nullptr ||
        bindData->weightPropertyExpr->expressionType !=
            ExpressionType::PROPERTY) {
      continue;
    }
    const auto weightPropName =
        bindData->weightPropertyExpr->constPtrCast<PropertyExpression>()
            ->getPropertyName();
    if (std::find(indexInfo.targetColumns.begin(), indexInfo.targetColumns.end(),
                  weightPropName) == indexInfo.targetColumns.end()) {
      continue;
    }
    const auto srcTableIDs = rel->getSrcNode()->getTableIDs();
    const auto dstTableIDs = rel->getDstNode()->getTableIDs();
    const auto srcMatches =
        std::find(srcTableIDs.begin(), srcTableIDs.end(), indexInfo.srcTableId) !=
        srcTableIDs.end();
    const auto dstMatches =
        std::find(dstTableIDs.begin(), dstTableIDs.end(), indexInfo.dstTableId) !=
        dstTableIDs.end();
    const auto reversedMatches =
        rel->getDirectionType() == RelDirectionType::BOTH &&
        std::find(srcTableIDs.begin(), srcTableIDs.end(), indexInfo.dstTableId) !=
            srcTableIDs.end() &&
        std::find(dstTableIDs.begin(), dstTableIDs.end(), indexInfo.srcTableId) !=
            dstTableIDs.end();
    if (srcMatches && dstMatches) {
      return true;
    }
    if (reversedMatches) {
      return true;
    }
  }
  return false;
}

bool RewriterV4::matchesVariadicRelationshipIndex(
    const IndexRewriteCandidate& candidate, PatternIR& nodesIR) const {
  auto& indexInfo = *candidate.info;
  auto& indexIR = candidate.indexIR;

  auto* queryGraph = candidate.queryGraph;
  NEUG_ASSERT(indexIR.nodesInfo.size() == 2 &&
              indexIR.nodesInfo[0].matchedIdx != UINT64_MAX &&
              indexIR.nodesInfo[1].matchedIdx != UINT64_MAX);
  const auto localPos1 = indexIR.nodesInfo[0].matchedIdx;
  const auto localPos2 = indexIR.nodesInfo[1].matchedIdx;
  if (localPos1 >= nodesIR.nodesInfo.size() ||
      localPos2 >= nodesIR.nodesInfo.size()) {
    return false;
  }
  const auto& name1 = nodesIR.nodesInfo[localPos1].uniqName;
  const auto& name2 = nodesIR.nodesInfo[localPos2].uniqName;
  auto qIt1 = queryGraph->queryNodeNameToPosMap.find(name1);
  auto qIt2 = queryGraph->queryNodeNameToPosMap.find(name2);
  if (qIt1 == queryGraph->queryNodeNameToPosMap.end() ||
      qIt2 == queryGraph->queryNodeNameToPosMap.end()) {
    return false;
  }

  auto pos1 = qIt1->second;
  auto pos2 = qIt2->second;
  neug::binder::RelExpression* varLengthExpr = nullptr;
  for (auto& rel : queryGraph->getQueryRels()) {
    if ((rel->getSrcNode() == queryGraph->getQueryNode(pos1) &&
         rel->getDstNode() == queryGraph->getQueryNode(pos2)) ||
        (rel->getSrcNode() == queryGraph->getQueryNode(pos2) &&
         rel->getDstNode() == queryGraph->getQueryNode(pos1))) {
      varLengthExpr = rel.get();
      break;
    }
  }
  // The materialized shortcut edge does not retain path depth. Therefore a
  // query with narrower bounds cannot filter an unbounded shortcut correctly;
  // only identical recursive bounds are safe to rewrite.
  if (varLengthExpr == nullptr ||
      varLengthExpr->getRecursiveInfo() == nullptr ||
      varLengthExpr->getRecursiveInfo()->bindData->lowerBound !=
          indexInfo.getRecursiveRelLowerBound() ||
      varLengthExpr->getRecursiveInfo()->bindData->upperBound !=
          indexInfo.getRecursiveRelUpperBound()) {
    return false;
  }

  if (!indexInfo.getSrcNodeUniqueName().empty() ||
      !indexInfo.getDstNodeUniqueName().empty()) {
    if (indexIR.relsInfo.size() != 1 ||
        indexIR.relsInfo[0].matchedIdx >= nodesIR.relsInfo.size() ||
        nodesIR.relsInfo[indexIR.relsInfo[0].matchedIdx].edgeType != indexIR.relsInfo[0].edgeType ||
        nodesIR.relsInfo[indexIR.relsInfo[0].matchedIdx].uniqName != varLengthExpr->getUniqueName() ||
        varLengthExpr->getRecursiveInfo()->nodePredicate != nullptr ||
        varLengthExpr->getRecursiveInfo()->relPredicate != nullptr ||
        !orderedRecursiveCoverage(indexInfo, indexIR, *queryGraph, clientContext)) return false;
  }

  auto& bindData = varLengthExpr->getRecursiveInfo()->bindData;
  if (bindData->weightPropertyExpr == nullptr) {
    return true;
  }
  auto weightPropName =
      bindData->weightPropertyExpr->constPtrCast<PropertyExpression>()
          ->getPropertyName();
  return std::find(indexInfo.targetColumns.begin(), indexInfo.targetColumns.end(),
                   weightPropName) != indexInfo.targetColumns.end();
}

RewriterV4::RetainedPatternVariables
RewriterV4::collectRetainedPatternVariables(
    BoundMatchClause& matchClause,
    const catalog::FillIndexAuxInfo& indexInfo,
    const PatternIR& matchedIndexIR) const {
  RetainedPatternVariables retained;
  if (indexInfo.fillType != FillEntryType::REL) {
    return retained;
  }
  auto* graph =
      matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  if (graph == nullptr) {
    return retained;
  }
  // Equal labels do not identify retained variables: a source may contain
  // several Compound/Gene nodes while the materialization keeps only two.
  for (const auto& node : collectMatchedFillEndpointNodes(
           *graph, indexInfo, matchedIndexIR.nodesInfo)) {
    retained.nodeUniqueNames.insert(node->getUniqueName());
  }
  for (auto& rel : graph->getQueryRels()) {
    if (rel != nullptr && rel->getSingleEntry() != nullptr &&
        tableLabel(rel->getSingleEntry(), relLabels) ==
            indexInfo.getTableName()) {
      retained.relUniqueNames.insert(rel->getUniqueName());
    }
  }
  return retained;
}

bool RewriterV4::validateRewriteCandidate(
    BoundMatchClause& matchClause, NormalizedQueryPart& queryPart,
    PatternIR& nodesIR, IndexRewriteCandidate& candidate) {
  if (!sourceDirectionsCoverMatchedRows(candidate.indexIR, nodesIR,
                                        clientContext)) return false;
  aggregateEndpointPresenceOnlyCandidate =
      (provesAggregateEndpointSetSource(*candidate.info, *clientContext) &&
      provesEndpointSetConsumer(*candidate.info, candidate.indexIR, nodesIR,
          matchClause, queryPart, clientContext, true /* allowStoredValues */,
          false /* allowRecursive */, currentQuery_)) ||
      (completeAggregateInputIsClosed && provesNodePresenceSetSourceCached(*candidate.info) &&
       provesEndpointSetConsumer(*candidate.info, candidate.indexIR, nodesIR,
          matchClause, queryPart, clientContext, true /* allowStoredValues */,
          false /* allowRecursive */, currentQuery_, true /* nodePresence */));
  completePairDistinctCountCandidate = provesCompleteOptionalPairCount(
      *candidate.info, candidate.indexIR, nodesIR, matchClause, queryPart,
      clientContext, currentQuery_) ||
      provesCompleteKeyGroupDistinctCounts(*candidate.info, candidate.indexIR,
          nodesIR, matchClause, queryPart, *clientContext, currentQuery_,
          carriedProjectionInputs(queryPart, currentQuery_));
  if (requiresCompleteAggregateRead(*candidate.info, clientContext) &&
      !completePairDistinctCountCandidate &&
      !aggregateEndpointPresenceOnlyCandidate) return false;
  if (candidate.weightedRecursiveRelCoverageOnly) {
    return true;
  }
  auto& indexInfo = *candidate.info;
  auto& indexIR = candidate.indexIR;
  const bool endpointSetResidualSource = provesSingleStageEndpointSetSourceCached(indexInfo);
  if (indexInfo.getFillType() == FillEntryType::NODE &&
      !indexInfo.getPresenceColumns().empty()) {
    // A mandatory projection FILL must filter anchors without a source row.
    // Optional projection substitution needs null-extension of every output;
    // this local path cannot prove that contract.
    const auto fillTable = getFillTableContext(indexInfo);
    if (binder == nullptr || !fillTable.has_value() ||
        matchClause.getMatchClauseType() != MatchClauseType::MATCH) return false;
    for (const auto& name : indexInfo.getPresenceColumns()) {
      if (!fillTable->entry->containsProperty(name) ||
          fillTable->entry->getProperty(name).getType().getLogicalTypeID() !=
              LogicalTypeID::BOOL) return false;
    }
  }
  // Older version3 catalogs lack the text templates required by this matcher.
  if (indexInfo.getPredicateExpr() != nullptr &&
      indexInfo.getPredicateTemplates().empty()) return false;
  if (!indexInfo.getExistsPredicates().empty()) {
    // Graph predicate substitution currently proves mandatory fixed-hop
    // scopes. OPTIONAL and compatibility-flattened scopes need separate proofs.
    if (matchClause.getMatchClauseType() != MatchClauseType::MATCH ||
        indexInfo.usesWorkloadCompatibility()) return false;
    const auto predicates = matchClause.getConjunctivePredicates();
    for (const auto& expected : indexInfo.getExistsPredicates()) {
      const auto key = fillExistsPredicateKey(expected);
      if (std::none_of(predicates.begin(), predicates.end(), [&](const auto& p) {
            return canonicalizePredicateForMatchedIndex(*p, nodesIR, indexIR) == key;
          })) return false;
    }
  }
  auto retained = collectRetainedPatternVariables(matchClause, indexInfo, indexIR);
  const bool recursiveEndpoint = hasRecursiveEndpointContract(indexInfo);
  if (recursiveEndpoint && !matchesRecursiveEndpointSource(
          indexInfo, indexIR, nodesIR, matchClause, clientContext)) return false;
  if ((hasEndpointSetContract(indexInfo) || recursiveEndpoint) &&
      !provesEndpointSetConsumer(indexInfo, indexIR, nodesIR, matchClause,
                                 queryPart, clientContext, false, recursiveEndpoint,
                                 currentQuery_, false, endpointSetResidualSource)) return false;

  if (indexInfo.fillType == FillEntryType::REL) {
    // Every source variable incident to a surviving edge must remain one of
    // the materialized keys. A same-label hidden node cannot be reconstructed
    // from an aggregate on a different endpoint.
    for (const auto& rel : nodesIR.relsInfo) {
      if (rel.matchedIdx != UINT64_MAX) continue;
      for (const auto nodePos : {rel.srcNodeIdx, rel.dstNodeIdx}) {
        if (nodePos >= nodesIR.nodesInfo.size()) return false;
        const auto& node = nodesIR.nodesInfo[nodePos];
        if (node.matchedIdx != UINT64_MAX &&
            !retained.nodeUniqueNames.contains(node.uniqName)) return false;
      }
    }
  }

  if (candidate.splitRelAnchor) {
    if (matchClause.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH) {
      return false;
    }
    auto split = splitFillAnchorRelationship(
        indexInfo.getPatternIR(), indexInfo, clientContext->getCatalog(),
        clientContext->getTransaction(), relLabels);
    if (!split.has_value() ||
        split->anchorRelPos >= indexIR.relsInfo.size()) {
      return false;
    }
    const auto& anchor = indexIR.relsInfo[split->anchorRelPos];
    if (anchor.srcNodeIdx >= indexIR.nodesInfo.size() ||
        anchor.dstNodeIdx >= indexIR.nodesInfo.size()) {
      return false;
    }
    const auto localSrc = indexIR.nodesInfo[anchor.srcNodeIdx].matchedIdx;
    const auto localDst = indexIR.nodesInfo[anchor.dstNodeIdx].matchedIdx;
    if (localSrc == UINT64_MAX || localDst == UINT64_MAX ||
        localSrc >= nodesIR.nodesInfo.size() ||
        localDst >= nodesIR.nodesInfo.size()) {
      return false;
    }
    const auto& srcUnique = nodesIR.nodesInfo[localSrc].uniqName;
    const auto& dstUnique = nodesIR.nodesInfo[localDst].uniqName;
    bool foundMandatoryAnchor = false;
    for (uint32_t clauseIdx = 0;
         clauseIdx < queryPart.getNumReadingClause() &&
         !foundMandatoryAnchor;
         ++clauseIdx) {
      auto* clause = queryPart.getReadingClause(clauseIdx);
      if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
        continue;
      }
      auto& anchorMatch = clause->cast<BoundMatchClause>();
      if (&anchorMatch == &matchClause ||
          anchorMatch.getMatchClauseType() != MatchClauseType::MATCH) {
        continue;
      }
      auto* collection = anchorMatch.getQueryGraphCollection();
      for (uint32_t graphIdx = 0;
           graphIdx < collection->getNumQueryGraphs() &&
           !foundMandatoryAnchor;
           ++graphIdx) {
        auto* graph = collection->getQueryGraph(graphIdx);
        for (const auto& rel : graph->getQueryRels()) {
          if (rel == nullptr || rel->getSingleEntry() == nullptr ||
              tableLabel(rel->getSingleEntry(), relLabels) !=
                  indexInfo.getTableName()) {
            continue;
          }
          const auto& relSrc = rel->getSrcNode()->getUniqueName();
          const auto& relDst = rel->getDstNode()->getUniqueName();
          foundMandatoryAnchor =
              (relSrc == srcUnique && relDst == dstUnique) ||
              (relSrc == dstUnique && relDst == srcUnique);
          if (foundMandatoryAnchor) {
            break;
          }
        }
      }
    }
    if (!foundMandatoryAnchor && currentQuery_ != nullptr) {
      for (uint32_t partIdx = 0;
           partIdx < currentQuery_->getNumQueryParts() &&
           !foundMandatoryAnchor;
           ++partIdx) {
        auto* part = currentQuery_->getQueryPartUnsafe(partIdx);
        for (uint32_t clauseIdx = 0;
             clauseIdx < part->getNumReadingClause() &&
             !foundMandatoryAnchor;
             ++clauseIdx) {
          auto* clause = part->getReadingClause(clauseIdx);
          if (clause == nullptr ||
              clause->getClauseType() != ClauseType::MATCH) {
            continue;
          }
          auto& anchorMatch = clause->cast<BoundMatchClause>();
          if (&anchorMatch == &matchClause ||
              anchorMatch.getMatchClauseType() != MatchClauseType::MATCH) {
            continue;
          }
          auto* collection = anchorMatch.getQueryGraphCollection();
          for (uint32_t graphIdx = 0;
               graphIdx < collection->getNumQueryGraphs() &&
               !foundMandatoryAnchor;
               ++graphIdx) {
            auto* graph = collection->getQueryGraph(graphIdx);
            for (const auto& rel : graph->getQueryRels()) {
              if (rel == nullptr || rel->getSingleEntry() == nullptr ||
                  tableLabel(rel->getSingleEntry(), relLabels) !=
                      indexInfo.getTableName()) {
                continue;
              }
              const auto& relSrc = rel->getSrcNode()->getUniqueName();
              const auto& relDst = rel->getDstNode()->getUniqueName();
              foundMandatoryAnchor =
                  (relSrc == srcUnique && relDst == dstUnique) ||
                  (relSrc == dstUnique && relDst == srcUnique);
              if (foundMandatoryAnchor) {
                break;
              }
            }
          }
        }
      }
    }
    if (!foundMandatoryAnchor) {
      return false;
    }
  }

  if (indexInfo.fillType == FillEntryType::NODE) {
    const auto anchorIndexPos = findAnchorInIndexIR(indexInfo);
    for (const auto& rel : candidate.queryGraph->queryRels) {
      const auto relation = nodesIR.relUniqueNameToPos.find(rel->getUniqueName());
      if (relation != nodesIR.relUniqueNameToPos.end() &&
          nodesIR.relsInfo[relation->second].matchedIdx != UINT64_MAX) continue;
      // Any surviving edge needs both of its endpoints, including an edge
      // between two matched nodes that was not part of this FILL source.
      for (const auto& node : {rel->getSrcNode(), rel->getDstNode()}) {
        const auto found = nodesIR.nodeUniqNameToPos.find(node->getUniqueName());
        if (found == nodesIR.nodeUniqNameToPos.end()) continue;
        const auto boundary = nodesIR.nodesInfo[found->second].matchedIdx;
        if (boundary != UINT64_MAX && boundary != anchorIndexPos &&
            !findNodeBoundaryProperty(boundary, indexInfo, clientContext).has_value())
          return false;
      }
    }
  }

  const auto matchesUndirectedQueryRel = std::any_of(
      indexIR.relsInfo.begin(), indexIR.relsInfo.end(),
      [&](const auto& rel) {
        return rel.matchedIdx != UINT64_MAX &&
               rel.matchedIdx < nodesIR.relsInfo.size() &&
               nodesIR.relsInfo[rel.matchedIdx].edgeType ==
                   EdgeType::UNDIRECTED;
      });
  const auto needsDirectionDisambiguation =
      needsDirectedSameLabelRelAnchor(matchClause, indexInfo) ||
      (indexInfo.getFillType() == FillEntryType::REL &&
       !indexInfo.isFillUndirectedRel() &&
       indexInfo.getSrcTableId() == indexInfo.getDstTableId() &&
       matchesUndirectedQueryRel);
  if (needsDirectionDisambiguation &&
      !hasOrderedDerivedRelEndpoints(indexInfo, clientContext) &&
      !queryPartHasDirectedRelAnchor(
          queryPart, matchClause, indexInfo, relLabels) &&
      !indexHasDirectionAwareSiblingColumns(clientContext, indexInfo)) {
    LOG(INFO) << "Skip rel aggregate rewrite for optional same-label edge "
              << indexInfo.getTableName()
              << ": no directed anchor or forward/reverse filled columns are "
                 "available for the edge property.";
    return false;
  }

  std::vector<std::string> queryPredicateTemplates;
  if (matchClause.hasPredicate()) {
    for (const auto& predicate : matchClause.getConjunctivePredicates()) {
      queryPredicateTemplates.push_back(
          canonicalizePredicateForMatchedIndex(*predicate, nodesIR, indexIR));
      if (auto signature = fillPredicateSignature(*predicate, nodesIR, true,
              clientContext->getCatalog(), clientContext->getTransaction()))
        queryPredicateTemplates.push_back(*signature);
      std::function<bool(const std::shared_ptr<Expression>&)> containsSubquery =
          [&](const auto& expression) {
            if (expression == nullptr) return false;
            if (expression->expressionType == ExpressionType::SUBQUERY) return true;
            for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression)) {
              if (containsSubquery(child)) return true;
            }
            return false;
          };
      // Property collectors do not expose correlated EXISTS endpoints. An
      // uncovered graph predicate can remain when its correlated inputs are
      // materialized keys or proved surviving nodes in this mandatory MATCH.
      // Keep materialized keys separate from residual bindings: the boundary
      // checks above must still reject hidden aggregated source identities.
      // Keep the original subquery in place for the planner to evaluate.
      if (containsSubquery(predicate) && !predicateCoveredByFill(
              *predicate, nodesIR, indexIR, indexInfo.getPredicateTemplates())) {
        bool retainedExists = false;
        if (indexInfo.getFillType() == FillEntryType::REL &&
            matchClause.getMatchClauseType() == MatchClauseType::MATCH) {
          try {
            if (const auto exists = extractFillExistsPredicate(*predicate, nodesIR)) {
              retainedExists = exists->srcNodePos < nodesIR.nodesInfo.size() &&
                  exists->dstNodePos < nodesIR.nodesInfo.size() &&
                  retained.nodeUniqueNames.contains(
                      nodesIR.nodesInfo[exists->srcNodePos].uniqName) &&
                  retained.nodeUniqueNames.contains(
                      nodesIR.nodesInfo[exists->dstNodePos].uniqName);
            }
          } catch (const std::exception&) {
            // Unsupported subquery shapes remain in the original query.
          }
        }
        if (!retainedExists) {
          retainedExists = provesResidualMatchExistsInputsSurvive(
              *predicate, indexInfo, indexIR, nodesIR, matchClause,
              endpointSetResidualSource);
        }
        if (!retainedExists) return false;
      }
      if (validatePropertyCoverageForIndexRewrite(
              predicate, nodesIR, indexInfo, indexIR,
              &retained.nodeUniqueNames, &retained.relUniqueNames)) {
        continue;
      }
      const auto& coveredPredicates = indexInfo.getPredicateTemplates();
      if (predicateCoveredByFill(*predicate, nodesIR, indexIR,
                                 coveredPredicates)) {
        continue;
      }
      return false;
    }
  }
  // A filter on an unchanged incoming endpoint may have been evaluated in
  // an earlier MATCH/WITH. Use it only to prove this source restriction;
  // replaying it inside OPTIONAL would change its null-extension boundary.
  if (completePairDistinctCountCandidate) {
    for (const auto& predicate : guaranteedIncomingNodePredicates(
             matchClause, queryPart, currentQuery_, retained.nodeUniqueNames)) {
      queryPredicateTemplates.push_back(
          canonicalizePredicateForMatchedIndex(*predicate, nodesIR, indexIR));
    }
  }
  for (const auto& fillPredicate : indexInfo.getPredicateTemplates()) {
    if (fillPredicate.starts_with(DISTINCT_FILL_SHAPE_PREFIX)) continue;
    if (fillPredicate == ENDPOINT_SET_CONTRACT) continue;
    if (fillPredicate == RECURSIVE_ENDPOINT_CONTRACT) continue;
    if (std::find(queryPredicateTemplates.begin(), queryPredicateTemplates.end(),
                  fillPredicate) == queryPredicateTemplates.end()) {
      return false;
    }
  }

  if (indexInfo.fillType == FillEntryType::VARIADIC_REL) {
    if (!indexInfo.getSrcNodeUniqueName().empty()) {
      // The materialization is a set of reachable endpoint pairs. It cannot
      // preserve a query's number of paths or expose the removed path value.
      if (!queryPart.hasProjectionBody()) return false;
      const auto* body = queryPart.getProjectionBody();
      const auto aggregates = body->getAggregateExpressions();
      bool insensitive = body->isDistinct();
      if (!aggregates.empty()) {
        insensitive = std::all_of(aggregates.begin(),
            aggregates.end(), [](const auto& expression) {
              if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
              const auto& aggregate = expression->template constCast<AggregateFunctionExpression>();
              return aggregate.isDistinct() && aggregate.getFunction().name == "COUNT";
            });
      }
      if (!insensitive) return false;
      const auto retainsPath = [&](const auto& expression) {
        const auto names = collectDependentVarNames(expression);
        for (const auto& rel : indexIR.relsInfo) {
          if (rel.matchedIdx < nodesIR.relsInfo.size() &&
              names.contains(nodesIR.relsInfo[rel.matchedIdx].uniqName)) return true;
        }
        return false;
      };
      if (retainsPath(matchClause.getPredicate()) ||
          retainsPath(queryPart.getProjectionBodyPredicate())) return false;
      for (const auto& expressions : {body->getProjectionExpressions(),
                                      body->getGroupByExpressions(),
                                      body->getOrderByExpressions()}) {
        if (std::any_of(expressions.begin(), expressions.end(), retainsPath)) return false;
      }
    }
    return true;
  }
  if (!queryPart.hasProjectionBody()) {
    return false;
  }
  if (queryPart.hasProjectionBodyPredicate()) {
    // WITH ..., count(DISTINCT m) AS c WHERE c > 0 refers to this part's own
    // aggregate. That subtree is not a residual read of removed bindings: every
    // aggregate must become a registered target below (or the candidate is
    // rejected), and its replacement keeps the aggregate's unique name. Check
    // only the remaining property reads of the post-aggregation predicate.
    std::unordered_set<std::string> ownAggregates;
    for (const auto& aggregate : queryPart.getProjectionBody()->getAggregateExpressions())
      ownAggregates.insert(aggregate->getUniqueName());
    expression_vector residualReads;
    std::function<void(const std::shared_ptr<Expression>&)> collect =
        [&](const std::shared_ptr<Expression>& expression) {
      if (!expression || ownAggregates.contains(expression->getUniqueName())) return;
      if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION ||
          expression->expressionType == ExpressionType::SUBQUERY ||
          expression->expressionType == ExpressionType::PROPERTY) {
        residualReads.push_back(expression);
        return;
      }
      for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
        collect(child);
    };
    collect(queryPart.getProjectionBodyPredicate());
    for (const auto& read : residualReads) {
      if (!validatePropertyCoverageForIndexRewrite(read, nodesIR, indexInfo, indexIR,
              &retained.nodeUniqueNames, &retained.relUniqueNames)) {
        return false;
      }
    }
  }
  auto* projectionBody = queryPart.getProjectionBodyUnsafe();
  std::unordered_set<std::string> projectedAggregateNames;
  for (auto& expr : projectionBody->getProjectionExpressions()) {
    collectAggregateUniqueNames(expr, projectedAggregateNames);
    if (!validateProjectionExpressionForIndexRewrite(
            expr, matchClause, nodesIR, indexInfo, indexIR,
            &retained.nodeUniqueNames, &retained.relUniqueNames)) {
      return false;
    }
  }
  for (const auto& expressions : {projectionBody->getGroupByExpressions(),
                                  projectionBody->getOrderByExpressions()}) {
    for (const auto& expr : expressions) {
      if (!validateProjectionExpressionForIndexRewrite(
              expr, matchClause, nodesIR, indexInfo, indexIR,
              &retained.nodeUniqueNames, &retained.relUniqueNames)) return false;
    }
  }
  for (auto& expr : projectionBody->getAggregateExpressions()) {
    if (!projectedAggregateNames.contains(expr->getUniqueName()) &&
        !projectedAggregateNames.contains(expr->rawName())) {
      continue;
    }
    if (!validateProjectionExpressionForIndexRewrite(
            expr, matchClause, nodesIR, indexInfo, indexIR,
            &retained.nodeUniqueNames, &retained.relUniqueNames)) {
      return false;
    }
  }
  // An aggregate FILL collapses graph multiplicity. Every current aggregate
  // must consume a proved replacement: weighting COUNT while leaving an
  // unrelated SUM/AVG unchanged loses the source rows counted by that value.
  const auto hasAggregateColumn = std::any_of(
      indexInfo.indexColumnInfos.begin(), indexInfo.indexColumnInfos.end(),
      [](const auto& column) {
        return column != nullptr &&
            (column->columnType == IndexColumnType::NODE_AGGREGATE ||
             column->columnType == IndexColumnType::REL_AGGREGATE ||
             column->columnType == IndexColumnType::REL_COMPUATION_WITH_AGGREGATION);
      });
  if (aggregateEndpointPresenceOnlyCandidate) return true;
  auto retainedNodes = retained.nodeUniqueNames;
  if (indexInfo.getFillType() == FillEntryType::NODE) {
    const auto anchor = findAnchorInIndexIR(indexInfo);
    if (anchor >= indexIR.nodesInfo.size() ||
        indexIR.nodesInfo[anchor].matchedIdx >= nodesIR.nodesInfo.size()) return false;
    retainedNodes.insert(nodesIR.nodesInfo[indexIR.nodesInfo[anchor].matchedIdx].uniqName);
    // nodeIndexRewrite restores only endpoints of surviving relationships,
    // joined to the carrier by an actually stored primary key. Merely having
    // some property of a hidden vertex is not an identity recovery proof.
    for (const auto& rel : candidate.queryGraph->getQueryRels()) {
      const auto position = nodesIR.relUniqueNameToPos.find(rel->getUniqueName());
      if (position != nodesIR.relUniqueNameToPos.end() &&
          nodesIR.relsInfo[position->second].matchedIdx != UINT64_MAX) continue;
      for (const auto& node : {rel->getSrcNode(), rel->getDstNode()}) {
        const auto found = nodesIR.nodeUniqNameToPos.find(node->getUniqueName());
        if (found == nodesIR.nodeUniqNameToPos.end()) continue;
        const auto boundary = nodesIR.nodesInfo[found->second].matchedIdx;
        if (boundary != UINT64_MAX &&
            findNodeBoundaryProperty(boundary, indexInfo, clientContext).has_value())
          retainedNodes.insert(node->getUniqueName());
      }
    }
  }
  std::unordered_set<const Expression*> coveredExpressions;
  for (const auto& [expression, target] : propertyRewriteTargets)
    coveredExpressions.insert(expression);
  {
    // A previous WITH output is an input column of this part, even when its
    // bound object is that earlier aggregate (BI13 groups by zombieLikeCount).
    // completeKeyGroupInputsAreClosed separately rejects any earlier output
    // that depends on a binding removed here.
    const auto incoming = carriedProjectionInputs(queryPart, currentQuery_);
    coveredExpressions.insert(incoming.begin(), incoming.end());
  }
  if (!completeKeyGroupInputsAreClosed(indexInfo, indexIR, nodesIR, matchClause,
          queryPart, currentQuery_, &retainedNodes, &retained.relUniqueNames,
          &coveredExpressions, endpointSetResidualSource)) return false;
  if (!hasAggregateColumn) return true;
  if (propertyRewriteTargets.empty()) return false;
  const auto& aggregates = projectionBody->getAggregateExpressions();
  const auto hasPublicAggregateColumn = [&]() {
    for (size_t i = 0; i < indexInfo.indexColumnInfos.size(); ++i) {
      const auto& name = indexInfo.getTargetColumns()[i];
      if (name.starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
          name.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) continue;
      const auto type = indexInfo.indexColumnInfos[i]->columnType;
      if (type == IndexColumnType::NODE_AGGREGATE || type == IndexColumnType::REL_AGGREGATE ||
          type == IndexColumnType::REL_COMPUATION_WITH_AGGREGATION) return true;
    }
    return false;
  }();
  return std::all_of(aggregates.begin(), aggregates.end(), [&](const auto& expression) {
    if (propertyRewriteTargets.contains(expression.get())) return true;
    // A property-copy FILL's private row counter must not make an unrelated
    // DISTINCT count look like an unweighted additive aggregate. It sees the
    // same surviving identity set after duplicate source rows collapse.
    if (hasPublicAggregateColumn || !expression ||
        expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
    const auto& aggregate = expression->template constCast<AggregateFunctionExpression>();
    const auto anchor = findAnchorInIndexIR(indexInfo);
    return aggregate.isDistinct() && aggregate.getFunction().name == "COUNT" &&
        anchor < indexIR.nodesInfo.size() &&
        !referencesRemovedPatternVariables(expression, nodesIR,
            indexIR.nodesInfo[anchor].matchedIdx, &retainedNodes, &retained.relUniqueNames);
  });
}

bool RewriterV4::validateProjectionExpressionForIndexRewrite(
    const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
    PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
    common::PatternIR& indexIR,
    const std::unordered_set<std::string>* retainedNodeUniqueNames,
    const std::unordered_set<std::string>* retainedRelUniqueNames) {
  if (expr == nullptr) {
    return true;
  }
  if (expr->expressionType == ExpressionType::PATTERN &&
      indexInfo.fillType == FillEntryType::REL) {
    const auto& name = expr->getUniqueName();
    if (const auto node = nodesIR.nodeUniqNameToPos.find(name);
        node != nodesIR.nodeUniqNameToPos.end() &&
        nodesIR.nodesInfo[node->second].matchedIdx != UINT64_MAX) {
      return retainedNodeUniqueNames != nullptr &&
          retainedNodeUniqueNames->contains(name);
    }
    if (const auto rel = nodesIR.relUniqueNameToPos.find(name);
        rel != nodesIR.relUniqueNameToPos.end() &&
        nodesIR.relsInfo[rel->second].matchedIdx != UINT64_MAX) {
      return retainedRelUniqueNames != nullptr &&
          retainedRelUniqueNames->contains(name);
    }
  }
  if (expr->expressionType == ExpressionType::PATTERN &&
      indexInfo.fillType == FillEntryType::NODE) {
    // A NODE FILL keeps only its anchor. Rewriting a structurally identical
    // earlier MATCH must not discard a node or relationship that the current
    // WITH clause carries into a later query part (for example TCR11's
    // pathEdge1). Property expressions are handled by the coverage/deferred
    // rewrite paths below, but a projected pattern value has no equivalent on
    // the filled anchor.
    const auto& uniqueName = expr->getUniqueName();
    if (auto relIt = nodesIR.relUniqueNameToPos.find(uniqueName);
        relIt != nodesIR.relUniqueNameToPos.end() &&
        nodesIR.relsInfo[relIt->second].matchedIdx != UINT64_MAX) {
      return false;
    }
    if (auto nodeIt = nodesIR.nodeUniqNameToPos.find(uniqueName);
        nodeIt != nodesIR.nodeUniqNameToPos.end() &&
        nodesIR.nodesInfo[nodeIt->second].matchedIdx != UINT64_MAX) {
      const auto anchorIndexPos = findAnchorInIndexIR(indexInfo);
      return nodesIR.nodesInfo[nodeIt->second].matchedIdx == anchorIndexPos;
    }
  }
  if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    if (registerCountBackedAggregateTarget(expr, matchClause, nodesIR, indexInfo,
                                      indexIR, retainedNodeUniqueNames,
                                      retainedRelUniqueNames)) return true;
    if (registerAggregateRewriteTargets(expr, matchClause, nodesIR, indexInfo,
                                        indexIR)) {
      return true;
    }
    auto anchorIdx = findAnchorInIndexIR(indexInfo);
    auto anchorQueryNodeIdx = indexIR.nodesInfo[anchorIdx].matchedIdx;
    if (anchorQueryNodeIdx == UINT64_MAX &&
        indexInfo.fillType == FillEntryType::REL) {
      anchorQueryNodeIdx = 0;  // rel index — anchor may not be matched yet
    }
    if (!referencesRemovedPatternVariables(
            expr, nodesIR, anchorQueryNodeIdx, retainedNodeUniqueNames,
            retainedRelUniqueNames)) {
      return validatePropertyCoverageForIndexRewrite(expr, nodesIR, indexInfo,
                                                     indexIR,
                                                     retainedNodeUniqueNames,
                                                     retainedRelUniqueNames);
    }
    return false;
  }
  if (expr->expressionType == ExpressionType::FUNCTION &&
      registerAggregateRewriteTargets(expr, matchClause, nodesIR, indexInfo,
                                      indexIR)) {
    return true;
  }
  return validatePropertyCoverageForIndexRewrite(expr, nodesIR, indexInfo,
                                                 indexIR,
                                                 retainedNodeUniqueNames,
                                                 retainedRelUniqueNames);
}

bool RewriterV4::registerCountBackedAggregateTarget(
    const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
    PatternIR& nodesIR, FillIndexAuxInfo& indexInfo, PatternIR& indexIR,
    const std::unordered_set<std::string>* retainedNodeUniqueNames,
    const std::unordered_set<std::string>* retainedRelUniqueNames) {
  if (aggregateEndpointPresenceOnlyCandidate || binder == nullptr || currentQuery_ == nullptr ||
      (indexInfo.getFillType() != FillEntryType::NODE && indexInfo.getFillType() != FillEntryType::REL) ||
      indexIR.relsInfo.empty() ||
      indexInfo.usesWorkloadCompatibility() ||
      matchClause.getMatchClauseType() != MatchClauseType::MATCH ||
      expr == nullptr || expr->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
  // Existing physical edges can carry per-edge values or directional pairs.
  // A newly derived relation has one row per complete ordered endpoint group,
  // so its public COUNT(*) is the exact bag multiplicity of that group.
  if (indexInfo.getFillType() == FillEntryType::REL &&
      !hasOrderedDerivedRelEndpoints(indexInfo, clientContext)) return false;
  const auto* aggregate = expr->constPtrCast<AggregateFunctionExpression>();
  const auto& aggregateName = aggregate->getFunction().name;
  if ((aggregateName != "SUM" && aggregateName != "MIN" && aggregateName != "MAX" && aggregateName != "COUNT") ||
      aggregate->getNumChildren() != 1) return false;
  const bool stringExtremum = (aggregateName == "MIN" || aggregateName == "MAX") &&
      expr->getDataType().getLogicalTypeID() == LogicalTypeID::STRING;
  if (expr->getDataType().getLogicalTypeID() != LogicalTypeID::INT64 && !stringExtremum)
    return false;
  // The physical SUM operator currently drops its DISTINCT flag. Until its
  // execution contract is fixed, do not turn duplicate rows into a distinct
  // value set based only on the bound expression's flag.
  if (aggregateName == "SUM" && aggregate->isDistinct()) return false;
  // A scalar COUNT weight is a bag-inner-join rule. Keep OPTIONAL, recursive
  // and other reading boundaries on their existing independent paths.
  for (uint32_t pi = 0; pi < currentQuery_->getNumQueryParts(); ++pi) {
    const auto* part = currentQuery_->getQueryPart(pi);
    for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
      const auto* clause = part->getReadingClause(ci);
      if (clause->getClauseType() != ClauseType::MATCH ||
          clause->constPtrCast<BoundMatchClause>()->getMatchClauseType() != MatchClauseType::MATCH)
        return false;
    }
  }
  auto* collection = matchClause.getQueryGraphCollectionUnsafe();
  if (collection->getNumQueryGraphs() != 1) return false;
  for (const auto& relationship : collection->getQueryGraph(0)->getQueryRels())
    if (relationship->getRecursiveInfo() != nullptr) return false;
  const auto anchor = findAnchorInIndexIR(indexInfo);
  if (anchor >= indexIR.nodesInfo.size()) return false;
  const auto localAnchor = indexIR.nodesInfo[anchor].matchedIdx;
  if (localAnchor >= nodesIR.nodesInfo.size() ||
      nodesIR.nodesInfo[localAnchor].nodeLabels.size() != 1) return false;
  const auto operand = aggregate->getChild(0);
  // String extrema and counts depend on the same retained values/NULLs after
  // duplicate rows collapse. They need no arithmetic on the string itself.
  // SUM remains in the exact INT64 wrapping domain.
  const bool safeOperand = isTotalInt64RowExpression(operand) ||
      (aggregateName != "SUM" && isTotalStringRowExpression(operand));
  if (!safeOperand ||
      referencesRemovedPatternVariables(operand, nodesIR, localAnchor,
                                        retainedNodeUniqueNames, retainedRelUniqueNames)) return false;
  // completeKeyGroupInputsAreClosed treats a registered aggregate as proved.
  // Its operand must therefore independently pass coverage before registering
  // the aggregate; a scalar property on a deleted internal node is not enough.
  if (!validatePropertyCoverageForIndexRewrite(operand, nodesIR, indexInfo, indexIR,
          retainedNodeUniqueNames, retainedRelUniqueNames)) return false;
  const auto fill = getFillTableContext(indexInfo);
  if (!fill.has_value()) return false;
  for (size_t column = 0; column < indexInfo.getTargetColumns().size(); ++column) {
    const auto& name = indexInfo.getTargetColumns()[column];
    if (!isPublicRowCount(indexInfo, column) || !fill->entry->containsProperty(name) ||
        fill->entry->getProperty(name).getType().getLogicalTypeID() != LogicalTypeID::INT64) continue;
    PropertyRewriteTarget target;
    target.newPropertyName = name;
    target.countAggregateOperand = deepCopyShared(operand);
    if (indexInfo.getFillType() == FillEntryType::NODE) {
      target.newVarUniqueName = nodesIR.nodesInfo[localAnchor].uniqName;
      target.newVarRawName = nodesIR.nodesInfo[localAnchor].varName;
      if (target.newVarRawName.empty()) target.newVarRawName = target.newVarUniqueName;
      for (const auto& node : collection->getQueryGraph(0)->getQueryNodes())
        if (node->getUniqueName() == target.newVarUniqueName) target.targetNode = node;
      if (target.targetNode == nullptr) return false;
    }
    // relIndexRewrite binds every aggregate target to the retained or newly
    // synthesized relationship after its complete endpoint proof succeeds.
    propertyRewriteTargets[expr.get()] = std::move(target);
    return true;
  }
  return false;
}

bool RewriterV4::registerAggregateRewriteTargets(
    const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
    PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
    common::PatternIR& indexIR) {
  // Endpoint-set reads preserve the original DISTINCT expressions. Stored
  // AVG, COUNT or other values must not accidentally enter SUM/MAX lowering.
  if (aggregateEndpointPresenceOnlyCandidate) return false;
  IndexRewriteTemplateMatcher::AggRewriteTargetMap aggregateTargets;
  if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    const auto canonicalPairCount = completePairDistinctCountCandidate
        ? canonicalSingleEdgePairCountColumn(*expr, indexInfo, indexIR,
                                             nodesIR, matchClause, clientContext)
        : std::nullopt;
    if (canonicalPairCount) {
      aggregateTargets.emplace(expr.get(), *canonicalPairCount);
    } else if (!IndexRewriteTemplateMatcher::validateAggregateTemplatesForIndexRewrite(
            expr, indexInfo, nodesIR, indexIR, aggregateTargets)) {
      // Every graph identity in a successful mandatory MATCH row is non-null.
      // For OPTIONAL, only a newly introduced non-key identity contributes
      // zero on the padded row. Earlier bound keys remain non-null when the
      // optional pattern fails and cannot use this row-count substitution.
      // Only public COUNT(*) outputs are eligible, never private state for
      // another aggregate, nullable properties or DISTINCT identities.
      const auto& aggregate = expr->constCast<AggregateFunctionExpression>();
      if (aggregate.getFunction().name != "COUNT" || aggregate.isDistinct() ||
          aggregate.getNumChildren() != 1) return false;
      const auto& child = aggregate.getChild(0);
      if (child == nullptr || child->expressionType != ExpressionType::PATTERN)
        return false;
      const auto node = nodesIR.nodeUniqNameToPos.find(child->getUniqueName());
      const auto rel = nodesIR.relUniqueNameToPos.find(child->getUniqueName());
      // A mandatory row also makes identities in the residual graph non-null.
      // COUNT of such an outside identity counts each materialized source row,
      // so the stored COUNT(*) is its multiplicity weight. OPTIONAL requires
      // the counted identity to belong to the replaced, null-extended region.
      if (node == nodesIR.nodeUniqNameToPos.end() &&
          rel == nodesIR.relUniqueNameToPos.end()) return false;
      const bool countedInside =
          (node != nodesIR.nodeUniqNameToPos.end() &&
           nodesIR.nodesInfo[node->second].matchedIdx != UINT64_MAX) ||
          (rel != nodesIR.relUniqueNameToPos.end() &&
           nodesIR.relsInfo[rel->second].matchedIdx != UINT64_MAX);
      bool mandatoryNodeCount = indexInfo.getFillType() == FillEntryType::NODE &&
          !indexInfo.usesWorkloadCompatibility() &&
          matchClause.getMatchClauseType() == MatchClauseType::MATCH;
      if (mandatoryNodeCount) {
        const auto table = getFillTableContext(indexInfo);
        bool publicInt64Count = false;
        for (size_t i = 0; table.has_value() && i < indexInfo.getTargetColumns().size(); ++i) {
          const auto& name = indexInfo.getTargetColumns()[i];
          publicInt64Count |= isPublicRowCount(indexInfo, i) &&
              table->entry->containsProperty(name) &&
              table->entry->getProperty(name).getType().getLogicalTypeID() == LogicalTypeID::INT64;
        }
        mandatoryNodeCount = publicInt64Count;
      }
      if (!countedInside && !mandatoryNodeCount) {
        // A grouped query has no row on empty input. A global COUNT has a
        // zero row instead, and needs a separate SUM-on-empty proof.
        if (!hasOrderedDerivedRelEndpoints(indexInfo, clientContext) ||
            !provesAggregateEndpointSetSource(indexInfo, *clientContext)) return false;
        bool grouped = false;
        if (currentQuery_ != nullptr)
          for (uint32_t pi = 0; pi < currentQuery_->getNumQueryParts(); ++pi) {
            const auto* part = currentQuery_->getQueryPart(pi);
            for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci)
              if (part->getReadingClause(ci) == &matchClause)
                grouped = part->hasProjectionBody() &&
                    !part->getProjectionBody()->getGroupByExpressions().empty() &&
                    completeKeyGroupInputsAreClosed(indexInfo, indexIR, nodesIR,
                        matchClause, *part, currentQuery_);
          }
        if (!grouped) return false;
      }
      if (matchClause.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH) {
        if (!countedInside) return false;
        // Keep this proof local to a complete optional source at the end of
        // its reading part. A following clause could consume a hidden node;
        // a wrapper can supply bindings not represented in this query.
        if (currentQuery_ == nullptr || !completeAggregateInputIsClosed ||
            nodesIR.nodesInfo.size() != indexIR.nodesInfo.size() ||
            nodesIR.relsInfo.size() != indexIR.relsInfo.size()) return false;
        if (node != nodesIR.nodeUniqNameToPos.end()) {
          const auto matchedNode = nodesIR.nodesInfo[node->second].matchedIdx;
          if (matchedNode >= indexIR.nodesInfo.size()) return false;
          if (indexInfo.getFillType() == FillEntryType::NODE &&
              matchedNode == findAnchorInIndexIR(indexInfo)) return false;
          if (indexInfo.getFillType() == FillEntryType::REL) {
            if (indexInfo.getSrcNodeUniqueName().empty() ||
                indexInfo.getDstNodeUniqueName().empty() ||
                indexIR.nodesInfo[matchedNode].uniqName == indexInfo.getSrcNodeUniqueName() ||
                indexIR.nodesInfo[matchedNode].uniqName == indexInfo.getDstNodeUniqueName()) return false;
          }
        }
        bool foundBoundary = false;
        for (uint32_t pi = 0; pi < currentQuery_->getNumQueryParts() && !foundBoundary; ++pi) {
          const auto* part = currentQuery_->getQueryPart(pi);
          for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
            const auto* clause = part->getReadingClause(ci);
            if (clause == &matchClause) {
              // SUM over no input is NULL, unlike COUNT's zero. Nonempty
              // grouping preserves the original empty-input row contract.
              if (ci + 1 != part->getNumReadingClause() || !part->hasProjectionBody() ||
                  part->getProjectionBody()->getGroupByExpressions().empty()) return false;
              foundBoundary = true;
              break;
            }
            // An UNWIND only binds its output variable; it cannot bind the
            // counted identity unless it is that variable (BI13: UNWIND
            // zombies AS zombie before the OPTIONAL like count).
            if (clause->getClauseType() == ClauseType::UNWIND) {
              if (clause->constCast<BoundUnwindClause>().getOutExpr()->getUniqueName() ==
                  child->getUniqueName()) return false;
              continue;
            }
            if (clause->getClauseType() != ClauseType::MATCH) return false;
            const auto* collection = clause->constCast<BoundMatchClause>().getQueryGraphCollection();
            for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi) {
              const auto* graph = collection->getQueryGraph(gi);
              if (graph->containsQueryNode(child->getUniqueName()) ||
                  graph->containsQueryRel(child->getUniqueName())) return false;
            }
          }
        }
        if (!foundBoundary) return false;
      } else if (matchClause.getMatchClauseType() != MatchClauseType::MATCH) {
        return false;
      }
      aggregateTargets.clear();
      for (size_t i = 0; i < indexInfo.indexColumnInfos.size() &&
                         i < indexInfo.targetColumns.size(); ++i) {
        if (indexInfo.targetColumns[i].starts_with(
                InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
            indexInfo.targetColumns[i].starts_with(
                InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) continue;
        const auto& column = indexInfo.indexColumnInfos[i];
        std::string spelling;
        if (column->columnType == IndexColumnType::NODE_AGGREGATE) {
          spelling = static_cast<const NodeAggregationIndexColnumInfo&>(*column).nameTemplate;
        } else if (column->columnType == IndexColumnType::REL_AGGREGATE) {
          spelling = static_cast<const RelIndexColnumInfo&>(*column).nameTemplate;
        }
        std::erase_if(spelling, [](unsigned char ch) { return std::isspace(ch); });
        std::transform(spelling.begin(), spelling.end(), spelling.begin(),
                       [](unsigned char ch) { return std::toupper(ch); });
        if (spelling == "COUNT(*)") {
          aggregateTargets.emplace(expr.get(), i);
          break;
        }
      }
      if (aggregateTargets.empty()) return false;
    }
  } else {
    auto* scalarFuncExpr = expr->ptrCast<ScalarFunctionExpression>();
    if (scalarFuncExpr == nullptr ||
        !IndexRewriteTemplateMatcher::validateTemplatesForIndexRewrite(
            scalarFuncExpr, indexInfo, nodesIR, indexIR, aggregateTargets)) {
      return false;
    }
  }

  if (aggregateTargets.empty()) {
    return true;
  }

  PropertyRewriteTarget target;
  if (indexInfo.fillType == FillEntryType::NODE) {
    auto anchorIdx = findAnchorInIndexIR(indexInfo);
    auto matchedQueryIdx = indexIR.nodesInfo[anchorIdx].matchedIdx;
    if (matchedQueryIdx == UINT64_MAX ||
        matchedQueryIdx >= nodesIR.nodesInfo.size()) {
      return false;
    }
    target.newVarUniqueName = nodesIR.nodesInfo[matchedQueryIdx].uniqName;
    target.newVarRawName = nodesIR.nodesInfo[matchedQueryIdx].varName;
    auto* patternGraph =
        matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
    for (const auto& node : patternGraph->getQueryNodes()) {
      if (node != nullptr &&
          node->getUniqueName() == target.newVarUniqueName) {
        target.targetNode = node;
        break;
      }
    }
  } else {
    auto* patternGraph =
        matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
    auto* catalog = clientContext->getCatalog();
    auto* transaction = clientContext->getTransaction();
    for (auto& rel : patternGraph->getQueryRels()) {
      if (tableLabel(rel->getSingleEntry(), relLabels) ==
          indexInfo.getTableName()) {
        target.newVarUniqueName = rel->getUniqueName();
        target.newVarRawName = rel->getVariableName();
        break;
      }
    }
  }

  if (target.newVarRawName.empty() && indexInfo.fillType == FillEntryType::NODE) {
    // Anonymous query nodes still have a unique bound identity. The property
    // expression resolves through that identity; its printable name need not
    // come from a user-written alias.
    if (target.newVarUniqueName.empty()) return false;
    target.newVarRawName = target.newVarUniqueName;
  }
  if (target.newVarRawName.empty() && indexInfo.fillType == FillEntryType::REL) {
    for (const auto& [exprPtr, _] : aggregateTargets) {
      if (exprPtr == nullptr ||
          exprPtr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
      }
      auto* aggExpr = exprPtr->constPtrCast<AggregateFunctionExpression>();
      if (aggExpr == nullptr) {
        return false;
      }
      auto functionName = aggExpr->getFunction().name;
      std::transform(functionName.begin(), functionName.end(),
                     functionName.begin(), [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                     });
      if (functionName != "COUNT" && functionName != "COUNT_STAR" &&
          functionName != "SUM") {
        return false;
      }
    }
  }

  for (const auto& [exprPtr, indexColPos] : aggregateTargets) {
    if (exprPtr == nullptr) {
      continue;
    }
    if (indexColPos >= indexInfo.targetColumns.size()) {
      return false;
    }
    auto targetForExpr = target;
    targetForExpr.newPropertyName = indexInfo.targetColumns[indexColPos];
    targetForExpr.completePairDistinctCount = completePairDistinctCountCandidate;
    if (indexInfo.fillType == FillEntryType::REL &&
        !indexInfo.isFillUndirectedRel() &&
        indexInfo.getSrcTableId() == indexInfo.getDstTableId()) {
      auto* tableEntry = tableByName(clientContext->getCatalog(), clientContext->getTransaction(),
          indexInfo.getTableName(), clientContext->useInternalCatalogEntry());
      if (tableEntry != nullptr) {
        if (auto columns = directionAwareCounterpartColumns(
                *tableEntry, targetForExpr.newPropertyName);
            columns.has_value()) {
          const auto matchedForwardColumn =
              targetForExpr.newPropertyName == columns->first;
          targetForExpr.forwardPropertyName = matchedForwardColumn
                                                  ? columns->first
                                                  : columns->second;
          targetForExpr.reversePropertyName = matchedForwardColumn
                                                  ? columns->second
                                                  : columns->first;
          targetForExpr.directionAwareSameLabelRel = true;
          if (auto split = splitFillAnchorRelationship(
                  indexIR, indexInfo, clientContext->getCatalog(),
                  clientContext->getTransaction(), relLabels);
              split.has_value() &&
              split->anchorRelPos < indexIR.relsInfo.size()) {
            const auto& anchor = indexIR.relsInfo[split->anchorRelPos];
            const auto matchedSrc = anchor.srcNodeIdx < indexIR.nodesInfo.size()
                                        ? indexIR.nodesInfo[anchor.srcNodeIdx]
                                              .matchedIdx
                                        : UINT64_MAX;
            if (matchedSrc < nodesIR.nodesInfo.size()) {
              targetForExpr.directionReferenceNodeUniqueName =
                  nodesIR.nodesInfo[matchedSrc].uniqName;
              targetForExpr.directionReferenceNodeRawName =
                  nodesIR.nodesInfo[matchedSrc].varName;
            }
          }
        }
      }
    }
    propertyRewriteTargets[exprPtr] = std::move(targetForExpr);
  }
  return true;
}

std::optional<RewriterV4::PropertyRewriteTarget>
RewriterV4::getTargetForNodeProperty(size_t queryNodePos,
                                     const std::string& propertyName,
                                     PatternIR& nodesIR,
                                     catalog::FillIndexAuxInfo& indexInfo,
                                     common::PatternIR& indexIR) {
  auto anchorQueryNodeIdx = findLocalAnchorNodePos(nodesIR, indexInfo);
  if (!anchorQueryNodeIdx.has_value()) {
    return std::nullopt;
  }
  auto indexNodePos = nodesIR.nodesInfo[queryNodePos].matchedIdx;
  if (indexNodePos == UINT64_MAX || indexNodePos >= indexIR.nodesInfo.size()) {
    return std::nullopt;
  }

  const auto& indexNodeInfo = indexIR.nodesInfo[indexNodePos];
  for (size_t i = 0; i < indexInfo.indexColumnInfos.size(); ++i) {
    const auto& info = indexInfo.indexColumnInfos[i];
    if ((info->columnType == IndexColumnType::NODE_PROPERTY ||
         info->columnType == IndexColumnType::NODE_AGGREGATE) &&
        (info->nodeUnique == indexNodeInfo.uniqName ||
         info->nodeUnique == indexNodeInfo.varName) &&
        info->propertyName == propertyName &&
        i < indexInfo.targetColumns.size()) {
      return PropertyRewriteTarget{
          nodesIR.nodesInfo[*anchorQueryNodeIdx].uniqName,
          nodesIR.nodesInfo[*anchorQueryNodeIdx].varName,
          indexInfo.targetColumns[i]};
    }
  }
  return std::nullopt;
}

std::optional<RewriterV4::PropertyRewriteTarget>
RewriterV4::getTargetForRelProperty(PropertyExpression& propExpr,
                                    const std::string& propertyName,
                                    PatternIR& nodesIR,
                                    catalog::FillIndexAuxInfo& indexInfo,
                                    common::PatternIR& indexIR) {
  auto anchorQueryNodeIdx = findLocalAnchorNodePos(nodesIR, indexInfo);
  if (indexInfo.fillType == FillEntryType::NODE &&
      !anchorQueryNodeIdx.has_value()) {
    return std::nullopt;
  }

  const auto& relUniqueName = propExpr.getVariableName();
  std::unordered_set<std::string> candidateRelNames{relUniqueName,
                                                    propExpr.getRawVariableName()};
  if (auto relIt = nodesIR.relUniqueNameToPos.find(relUniqueName);
      relIt != nodesIR.relUniqueNameToPos.end()) {
    auto indexRelPos = nodesIR.relsInfo[relIt->second].matchedIdx;
    if (indexRelPos != UINT64_MAX && indexRelPos < indexIR.relsInfo.size()) {
      candidateRelNames.insert(indexIR.relsInfo[indexRelPos].uniqName);
      candidateRelNames.insert(indexIR.relsInfo[indexRelPos].varName);
    }
  }
  for (size_t i = 0; i < indexInfo.indexColumnInfos.size(); ++i) {
    const auto& base = indexInfo.indexColumnInfos[i];
    if (base->columnType != IndexColumnType::REL_PROPERTY &&
        base->columnType != IndexColumnType::REL_AGGREGATE) {
      continue;
    }
    if (!candidateRelNames.contains(base->nodeUnique) ||
        base->propertyName != propertyName || i >= indexInfo.targetColumns.size()) {
      continue;
    }
    if (indexInfo.fillType == FillEntryType::NODE) {
      return PropertyRewriteTarget{
          nodesIR.nodesInfo[*anchorQueryNodeIdx].uniqName,
          nodesIR.nodesInfo[*anchorQueryNodeIdx].varName,
          indexInfo.targetColumns[i]};
    }
    return PropertyRewriteTarget{propExpr.getVariableName(),
                                 propExpr.getRawVariableName(),
                                 indexInfo.targetColumns[i]};
  }
  return std::nullopt;
}

std::optional<RewriterV4::PropertyRewriteTarget>
RewriterV4::getTargetForPropertyExpression(PropertyExpression& propExpr,
                                           PatternIR& nodesIR,
                                           catalog::FillIndexAuxInfo& indexInfo,
                                           common::PatternIR& indexIR) {
  const auto& varUniqueName = propExpr.getVariableName();
  const auto& propertyName = propExpr.getPropertyName();
  if (auto nodeIt = nodesIR.nodeUniqNameToPos.find(varUniqueName);
      nodeIt != nodesIR.nodeUniqNameToPos.end()) {
    return getTargetForNodeProperty(nodeIt->second, propertyName, nodesIR,
                                    indexInfo, indexIR);
  }
  if (nodesIR.relUniqueNameToPos.contains(varUniqueName)) {
    return getTargetForRelProperty(propExpr, propertyName, nodesIR, indexInfo,
                                   indexIR);
  }
  return std::nullopt;
}

bool RewriterV4::validatePropertyCoverageForIndexRewrite(
    const std::shared_ptr<Expression>& rootExpr, PatternIR& nodesIR,
    catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR,
    const std::unordered_set<std::string>* retainedNodeUniqueNames,
    const std::unordered_set<std::string>* retainedRelUniqueNames) {
  if (!rootExpr || rootExpr->expressionType == ExpressionType::PATTERN) {
    return true;
  }
  if (indexInfo.fillType == FillEntryType::VARIADIC_REL) {
    return true;
  }
  if (indexInfo.fillType == FillEntryType::REL &&
      !validateRetainedVariablesForRelIndexRewrite(rootExpr, nodesIR,
                                                   retainedRelUniqueNames)) {
    return false;
  }

  auto anchorIdx = findAnchorInIndexIR(indexInfo);
  auto anchorQueryNodeIdx = indexIR.nodesInfo[anchorIdx].matchedIdx;
  if (anchorQueryNodeIdx == UINT64_MAX) {
    if (indexInfo.fillType == FillEntryType::REL) {
      anchorQueryNodeIdx = 0;
    } else {
      return false;
    }
  }

  PropertyExprCollector propCollector;
  propCollector.visit(rootExpr);
  for (auto& prop : propCollector.getPropertyExprs()) {
    if (!prop || prop->expressionType != ExpressionType::PROPERTY) {
      continue;
    }
    auto* propExpr = prop->ptrCast<PropertyExpression>();
    if (propExpr == nullptr || isInternalPatternProperty(*propExpr)) {
      continue;
    }
    const auto& varUniqueName = propExpr->getVariableName();
    if (auto it = nodesIR.nodeUniqNameToPos.find(varUniqueName); it != nodesIR.nodeUniqNameToPos.end()) {
      if (indexInfo.fillType == FillEntryType::REL &&
          retainedNodeUniqueNames != nullptr &&
          retainedNodeUniqueNames->contains(varUniqueName)) {
        continue;
      }
      auto queryNodePos = it->second;
      if (indexInfo.fillType == FillEntryType::NODE &&
          queryNodePos == anchorQueryNodeIdx) {
        continue;
      }
      if (nodesIR.nodesInfo[queryNodePos].matchedIdx != UINT64_MAX) {
        auto target = getTargetForNodeProperty(
            queryNodePos, propExpr->getPropertyName(), nodesIR, indexInfo,
            indexIR);
        if (!target.has_value()) {
          if (retainedNodeUniqueNames != nullptr &&
              retainedNodeUniqueNames->contains(varUniqueName)) {
            continue;
          }
          return false;
        }
        propertyRewriteTargets[prop.get()] = std::move(*target);
        expressionsToRewrite.insert({varUniqueName, prop});
      }
      continue;
    }
    if (auto relIt = nodesIR.relUniqueNameToPos.find(varUniqueName);
        relIt != nodesIR.relUniqueNameToPos.end()) {
      if (nodesIR.relsInfo[relIt->second].matchedIdx == UINT64_MAX) {
        continue;
      }
      auto target = getTargetForRelProperty(
          *propExpr, propExpr->getPropertyName(), nodesIR, indexInfo, indexIR);
      if (!target.has_value()) {
        if (retainedRelUniqueNames != nullptr &&
            retainedRelUniqueNames->contains(varUniqueName)) {
          continue;
        }
        return false;
      }
      propertyRewriteTargets[prop.get()] = std::move(*target);
      expressionsToRewrite.insert({varUniqueName, prop});
      continue;
    }
    // Not in main query graph (e.g. subquery) — ignore.
  }
  return true;
}

bool RewriterV4::validatePropertyCoverageForIndexRewrite(
    const expression_vector& exprs, PatternIR& nodesIR,
    catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR) {
  for (auto& expr : exprs) {
    if (!validatePropertyCoverageForIndexRewrite(expr, nodesIR, indexInfo,
                                                 indexIR)) {
      return false;
    }
  }
  return true;
}

// --- Graph rewrite operations ---

void RewriterV4::nodeIndexRewrite(NormalizedQueryPart& queryPart,
                                   BoundMatchClause& matchClause,
                                   PatternIR& nodesIR,
                                   catalog::IndexCatalogEntry& indexEntry,
                                   QueryGraph& matchedGraph) {
  auto& indexInfo = indexEntry.getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  auto& indexIR = indexInfo.getPatternIR();
  auto& indexNodesInfo = indexIR.nodesInfo;
  auto* patternGraph = &matchedGraph;
  // Find anchor node by table uniqName.
  auto anchorIdx = findAnchorInIndexIR(indexInfo);
  NEUG_ASSERT(anchorIdx < indexNodesInfo.size() && indexNodesInfo[anchorIdx].matchedIdx != UINT64_MAX);
  auto targetNode = patternGraph->getQueryNode(indexNodesInfo[anchorIdx].matchedIdx);

  std::unordered_set<std::string> matchedRelNames;
  for (const auto& rel : indexIR.relsInfo) {
    if (rel.matchedIdx != UINT64_MAX && rel.matchedIdx < nodesIR.relsInfo.size())
      matchedRelNames.insert(nodesIR.relsInfo[rel.matchedIdx].uniqName);
  }

  std::unordered_set<std::string> retainedBoundaryNodes;
  if (binder != nullptr) {
    auto fillTable = getFillTableContext(indexInfo);
    if (fillTable.has_value()) {
      for (auto& rel : patternGraph->getQueryRels()) {
        if (matchedRelNames.contains(rel->getUniqueName())) continue;
        for (const auto& sharedNode : {rel->getSrcNode(), rel->getDstNode()}) {
          const auto found = nodesIR.nodeUniqNameToPos.find(sharedNode->getUniqueName());
          if (found == nodesIR.nodeUniqNameToPos.end()) continue;
          const auto boundaryIndexPos = nodesIR.nodesInfo[found->second].matchedIdx;
          if (boundaryIndexPos == UINT64_MAX || boundaryIndexPos == anchorIdx) continue;
          auto mapping = findNodeBoundaryProperty(boundaryIndexPos, indexInfo, clientContext);
          if (!mapping.has_value()) continue;
          if (!retainedBoundaryNodes.insert(sharedNode->getUniqueName()).second) continue;
          auto filledProperty = makeFillPropertyExpression(
              *fillTable,
              PropertyRewriteTarget{targetNode->getUniqueName(),
                                    targetNode->getVariableName(),
                                    mapping->second});
          auto sharedProperty = sharedNode->getPropertyExpression(mapping->first);
          auto equality = binder->getExpressionBinder()->bindComparisonExpression(
              ExpressionType::EQUALS,
              expression_vector{std::move(filledProperty), std::move(sharedProperty)});
          matchClause.setPredicate(
              binder->getExpressionBinder()->combineBooleanExpressions(
                  ExpressionType::AND, matchClause.getPredicate(), std::move(equality)));
        }
      }
    }
  }

  QueryGraph graph;
  for (auto& rel : patternGraph->getQueryRels()) {
    if (!matchedRelNames.contains(rel->getUniqueName())) addRelAndEndpoints(graph, rel);
  }
  std::shared_ptr<NodeExpression> boundAnchorNode;
  for (uint32_t clauseIdx = 0;
       clauseIdx < queryPart.getNumReadingClause() && boundAnchorNode == nullptr;
       ++clauseIdx) {
    auto* clause = queryPart.getReadingClause(clauseIdx);
    if (clause == &matchClause) {
      break;
    }
    if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
      continue;
    }
    auto* collection =
        clause->cast<BoundMatchClause>().getQueryGraphCollectionUnsafe();
    for (uint32_t graphIdx = 0;
         graphIdx < collection->getNumQueryGraphs(); ++graphIdx) {
      for (const auto& node :
           collection->getQueryGraph(graphIdx)->getQueryNodes()) {
        if (node != nullptr &&
            node->getUniqueName() == targetNode->getUniqueName()) {
          boundAnchorNode = node;
          break;
        }
      }
    }
  }
  bool anchorAlreadyBound = boundAnchorNode != nullptr;
  bool anchorBoundByEarlierPart = false;
  if (currentQuery_ != nullptr) {
    for (uint32_t partIdx = 0; partIdx < currentQuery_->getNumQueryParts();
         ++partIdx) {
      if (anchorAlreadyBound) {
        break;
      }
      auto* part = currentQuery_->getQueryPartUnsafe(partIdx);
      if (part == &queryPart) {
        break;
      }
      if (!part->hasProjectionBody()) {
        continue;
      }
      for (const auto& projection :
           part->getProjectionBody()->getProjectionExpressions()) {
        if (projection != nullptr && ExpressionUtil::isNodePattern(*projection) &&
            projection->getUniqueName() == targetNode->getUniqueName()) {
          boundAnchorNode =
              std::static_pointer_cast<NodeExpression>(projection);
          anchorAlreadyBound = true;
          anchorBoundByEarlierPart = true;
          break;
        }
      }
      if (anchorAlreadyBound) {
        break;
      }
    }
  }
  if (boundAnchorNode != nullptr) {
    for (auto& [_, target] : propertyRewriteTargets) {
      if (target.newVarUniqueName == targetNode->getUniqueName()) {
        target.newVarUniqueName = boundAnchorNode->getUniqueName();
        target.newVarRawName = boundAnchorNode->getVariableName();
        target.targetNode = boundAnchorNode;
      }
    }
  }
  // Aggregate expressions are rewritten to properties on the FILL anchor. If
  // every matched relationship is eliminated, the anchor must still be scanned
  // so those properties and any key predicate remain bound in the plan. An
  // anchor carried by an earlier QueryPart is already in scope and must not be
  // scanned again, otherwise execution attempts to set the same column twice.
  // The binding can come from either an earlier QueryPart or an earlier MATCH
  // in this QueryPart. WITH aliases preserve the bound unique identity; a
  // reused spelling after WITH dropped the old node denotes a fresh binding.
  // Raw variable names therefore cannot establish that an anchor is in scope.
  // A set-only presence read also needs a MATCH boundary for its Boolean
  // predicate. Keeping the same bound node here is an identity join, not a
  // fresh binding. Otherwise removing an empty MATCH would discard the test
  // and admit input anchors whose source has no qualifying witness.
  // A mandatory MATCH over an anchor carried by WITH is a filter on that
  // input: its rewritten value predicates and presence test must stay on a
  // MATCH boundary. Without the anchor the clause becomes empty, is removed
  // by tryRewriteQueryPart, and silently drops those predicates (BI11:
  // WITH DISTINCT a, b MATCH (b)-[:ISLOCATEDIN]->()-[:ISPARTOF]->({name})).
  const bool anchorCarriesBoundFilter =
      anchorBoundByEarlierPart &&
      matchClause.getMatchClauseType() == MatchClauseType::MATCH &&
      (matchClause.hasPredicate() || !indexInfo.getPresenceColumns().empty());
  if (!anchorAlreadyBound || aggregateEndpointPresenceOnlyCandidate ||
      anchorCarriesBoundFilter) {
    graph.addQueryNode(targetNode);
    if (matchClause.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH &&
        !indexInfo.getRowCountColumn().empty()) {
      auto present = binder->getExpressionBinder()->bindComparisonExpression(
          ExpressionType::GREATER_THAN,
          expression_vector{makeFillPropertyExpression(*getFillTableContext(indexInfo),
              PropertyRewriteTarget{targetNode->getUniqueName(), targetNode->getVariableName(), indexInfo.getRowCountColumn()}),
              binder->getExpressionBinder()->createLiteralExpression(Value(int64_t{0}))});
      matchClause.setPredicate(binder->getExpressionBinder()->combineBooleanExpressions(
          ExpressionType::AND, matchClause.getPredicate(), present));
    }
  }

  for (auto& pr : patternGraph->queryNodeNameToPosMap) {
    visitedNodeUniqNames.insert(pr.first);
  }
  for (auto& pr : patternGraph->queryRelNameToPosMap) {
    visitedNodeUniqNames.insert(pr.first);
  }
  *patternGraph = std::move(graph);
}

void RewriterV4::stageSelectiveVariadicAnchorSplit(
    NormalizedQueryPart& queryPart, BoundMatchClause& matchClause,
    QueryGraph& queryGraph, const std::shared_ptr<RelExpression>& bridgeRel) {
  if (pendingPreQueryPart.has_value() || bridgeRel == nullptr ||
      queryPart.getNumReadingClause() != 1 || queryPart.hasProjectionBodyPredicate() ||
      matchClause.getMatchClauseType() != MatchClauseType::MATCH) {
    return;
  }

  auto srcNode = bridgeRel->getSrcNode();
  auto dstNode = bridgeRel->getDstNode();
  if (srcNode == nullptr || dstNode == nullptr) {
    return;
  }

  auto srcHasDistinctAggregate =
      queryPartHasDistinctAggregateOnVar(queryPart, srcNode->getUniqueName());
  auto dstHasDistinctAggregate =
      queryPartHasDistinctAggregateOnVar(queryPart, dstNode->getUniqueName());
  if (srcHasDistinctAggregate == dstHasDistinctAggregate) {
    return;
  }

  // Removing duplicate prefix paths is only safe when every aggregate is
  // duplicate-insensitive. Conservatively accept DISTINCT aggregates only.
  for (auto& expr : queryPart.getProjectionBody()->getAggregateExpressions()) {
    if (expr->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
        !expr->constCast<AggregateFunctionExpression>().isDistinct()) {
      return;
    }
  }

  auto anchorNode = srcHasDistinctAggregate ? srcNode : dstNode;
  auto restEndpoint = srcHasDistinctAggregate ? dstNode : srcNode;
  if (!queryGraph.containsQueryNode(anchorNode->getUniqueName()) ||
      !queryGraph.containsQueryNode(restEndpoint->getUniqueName())) {
    return;
  }

  std::unordered_map<std::string, std::vector<std::shared_ptr<RelExpression>>> adj;
  for (auto& rel : queryGraph.getQueryRels()) {
    if (rel == nullptr || rel->getUniqueName() == bridgeRel->getUniqueName()) {
      continue;
    }
    adj[rel->getSrcNodeName()].push_back(rel);
    adj[rel->getDstNodeName()].push_back(rel);
  }

  auto collectComponent = [&adj](const std::string& start) {
    std::unordered_set<std::string> nodes;
    std::queue<std::string> queue;
    nodes.insert(start);
    queue.push(start);
    while (!queue.empty()) {
      auto current = queue.front();
      queue.pop();
      for (auto& rel : adj[current]) {
        auto next = rel->getSrcNodeName() == current ? rel->getDstNodeName()
                                                     : rel->getSrcNodeName();
        if (nodes.insert(next).second) {
          queue.push(next);
        }
      }
    }
    return nodes;
  };

  auto prefixNodes = collectComponent(anchorNode->getUniqueName());
  if (prefixNodes.empty() || prefixNodes.contains(restEndpoint->getUniqueName())) {
    return;
  }

  auto projectionDeps = collectProjectionDependencies(queryPart);
  for (auto& nodeName : prefixNodes) {
    if (nodeName != anchorNode->getUniqueName() && projectionDeps.contains(nodeName)) {
      return;
    }
  }

  QueryGraph prefixGraph;
  QueryGraph restGraph;
  bool hasPrefixRel = false;
  bool hasRestRel = false;
  std::unordered_set<std::string> prefixRelNames;
  std::unordered_set<std::string> restRelNames;
  std::unordered_set<std::string> restNodeNames;
  std::vector<std::shared_ptr<RelExpression>> restRels;
  restRels.push_back(bridgeRel);
  restRelNames.insert(bridgeRel->getUniqueName());
  restNodeNames.insert(bridgeRel->getSrcNodeName());
  restNodeNames.insert(bridgeRel->getDstNodeName());
  for (auto& rel : queryGraph.getQueryRels()) {
    if (rel == nullptr) {
      continue;
    }
    const auto srcInPrefix = prefixNodes.contains(rel->getSrcNodeName());
    const auto dstInPrefix = prefixNodes.contains(rel->getDstNodeName());
    if (rel->getUniqueName() == bridgeRel->getUniqueName()) {
      hasRestRel = true;
      continue;
    }
    if (srcInPrefix && dstInPrefix) {
      if (projectionDeps.contains(rel->getUniqueName())) {
        return;
      }
      prefixRelNames.insert(rel->getUniqueName());
      addRelAndEndpoints(prefixGraph, rel);
      hasPrefixRel = true;
      continue;
    }
    if (srcInPrefix != dstInPrefix) {
      return;
    }
    restRels.push_back(rel);
    restRelNames.insert(rel->getUniqueName());
    restNodeNames.insert(rel->getSrcNodeName());
    restNodeNames.insert(rel->getDstNodeName());
    hasRestRel = true;
  }

  if (!hasPrefixRel || !hasRestRel) {
    return;
  }

  std::shared_ptr<Expression> prefixPredicate;
  std::shared_ptr<Expression> restPredicate;
  if (matchClause.hasPredicate()) {
    auto exprBinder = binder->getExpressionBinder();
    auto appendPredicate = [&](std::shared_ptr<Expression>& target,
                               const std::shared_ptr<Expression>& conjunct) {
      target = target == nullptr
                   ? conjunct
                   : exprBinder->combineBooleanExpressions(ExpressionType::AND,
                                                           target, conjunct);
    };
    for (auto& conjunct : matchClause.getConjunctivePredicates()) {
      auto deps = collectDependentVarNames(conjunct);
      bool hasPrefixOnlyDep = false;
      bool hasRestOnlyDep = false;
      bool hasSharedAnchorDep = false;
      for (auto& dep : deps) {
        const auto inPrefix = prefixNodes.contains(dep) || prefixRelNames.contains(dep);
        const auto inRest = restNodeNames.contains(dep) || restRelNames.contains(dep);
        if (inPrefix && inRest) {
          hasSharedAnchorDep = true;
        } else if (inPrefix) {
          hasPrefixOnlyDep = true;
        } else if (inRest) {
          hasRestOnlyDep = true;
        } else {
          return;
        }
      }
      if (hasPrefixOnlyDep && hasRestOnlyDep) {
        return;
      }
      if (hasPrefixOnlyDep || (!hasRestOnlyDep && hasSharedAnchorDep)) {
        appendPredicate(prefixPredicate, conjunct);
      } else {
        appendPredicate(restPredicate, conjunct);
      }
    }
  }

  binder->markVariadicAnchorSplitCandidate();
  if (!binder->isVariadicAnchorSplitEnabled()) {
    return;
  }

  QueryGraphCollection prefixCollection;
  prefixCollection.addAndMergeQueryGraphIfConnected(std::move(prefixGraph));
  prefixCollection.finalize();
  NormalizedQueryPart prePart;
  auto prefixMatch = std::make_unique<BoundMatchClause>(
      std::move(prefixCollection), matchClause.getMatchClauseType());
  prefixMatch->setPredicate(std::move(prefixPredicate));
  prePart.addReadingClause(std::move(prefixMatch));
  BoundProjectionBody projectionBody(true /* distinct */);
  expression_vector projectionExprs;
  projectionExprs.push_back(anchorNode);
  projectionBody.setProjectionExpressions(std::move(projectionExprs));
  prePart.setProjectionBody(std::move(projectionBody));

  for (auto& rel : restRels) {
    addRelAndEndpoints(restGraph, rel);
  }
  queryGraph = std::move(restGraph);
  matchClause.setPredicate(std::move(restPredicate));
  pendingPreQueryPart.emplace(std::move(prePart));
  LOG(INFO) << "Staged selective variadic anchor split on "
            << anchorNode->getVariableName();
}

void RewriterV4::variadicRelIndexRewrite(NormalizedQueryPart& queryPart,
                                         BoundMatchClause& matchClause,
                                         PatternIR& nodesIR,
                                         catalog::IndexCatalogEntry& indexEntry) {
  (void)queryPart;
  if (binder == nullptr) {
    THROW_INDEX_EXCEPTION(
        "Rewriter requires a Binder to rewrite variadic relationship indexes.");
  }

  auto& indexInfo =
      indexEntry.getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  auto& indexIR = indexInfo.getPatternIR();
  NEUG_ASSERT(indexIR.nodesInfo.size() == 2);
  NEUG_ASSERT(indexIR.nodesInfo[0].matchedIdx != UINT64_MAX &&
              indexIR.nodesInfo[1].matchedIdx != UINT64_MAX);

  auto queryGraph =
      matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  const auto& querySrcUnique =
      nodesIR.nodesInfo[indexIR.nodesInfo[0].matchedIdx].uniqName;
  const auto& queryDstUnique =
      nodesIR.nodesInfo[indexIR.nodesInfo[1].matchedIdx].uniqName;
  auto srcIt = queryGraph->queryNodeNameToPosMap.find(querySrcUnique);
  auto dstIt = queryGraph->queryNodeNameToPosMap.find(queryDstUnique);
  NEUG_ASSERT(srcIt != queryGraph->queryNodeNameToPosMap.end() &&
              dstIt != queryGraph->queryNodeNameToPosMap.end());

  auto querySrcNode = queryGraph->getQueryNode(srcIt->second);
  auto queryDstNode = queryGraph->getQueryNode(dstIt->second);
  uint32_t varLengthRelPos = UINT32_MAX;
  std::shared_ptr<RelExpression> varLengthRel;
  for (auto i = 0u; i < queryGraph->getNumQueryRels(); ++i) {
    auto rel = queryGraph->getQueryRel(i);
    if (((rel->getSrcNode() == querySrcNode &&
          rel->getDstNode() == queryDstNode) ||
         (rel->getSrcNode() == queryDstNode &&
          rel->getDstNode() == querySrcNode)) &&
        rel->getRecursiveInfo() != nullptr) {
      varLengthRelPos = i;
      varLengthRel = rel;
      break;
    }
  }
  NEUG_ASSERT(varLengthRel != nullptr);

  auto relEntries = binder->bindRelTableEntries({indexInfo.tableName});
  auto replacementSource = varLengthRel->getSrcNode();
  auto replacementDestination = varLengthRel->getDstNode();
  auto replacementDirection = varLengthRel->getDirectionType();
  if (!indexInfo.getSrcNodeUniqueName().empty()) {
    const auto coverage = orderedRecursiveCoverage(
        indexInfo, indexIR, *queryGraph, clientContext);
    NEUG_ASSERT(coverage.has_value());
    relEntries = coverage->tables;
    replacementSource = coverage->src;
    replacementDestination = coverage->dst;
    replacementDirection = RelDirectionType::SINGLE;
  }
  auto rewrittenRel = binder->createNonRecursiveQueryRel(
      varLengthRel->getVariableName(), relEntries, replacementSource,
      replacementDestination, replacementDirection);
  rewrittenRel->setLeftNode(replacementSource);
  rewrittenRel->setRightNode(replacementDestination);

  auto oldUniqueName = varLengthRel->getUniqueName();
  queryGraph->queryRels[varLengthRelPos] = rewrittenRel;
  queryGraph->queryRelNameToPosMap.erase(oldUniqueName);
  queryGraph->queryRelNameToPosMap[rewrittenRel->getUniqueName()] =
      varLengthRelPos;

  stageSelectiveVariadicAnchorSplit(queryPart, matchClause, *queryGraph,
                                    rewrittenRel);

  visitedNodeUniqNames.insert(oldUniqueName);
  visitedNodeUniqNames.insert(rewrittenRel->getUniqueName());
}

void RewriterV4::relIndexRewrite(NormalizedQueryPart& queryPart,
                                  BoundMatchClause& matchClause,
                                  PatternIR& nodesIR,
                                  catalog::IndexCatalogEntry& indexEntry) {
  (void)queryPart;
  (void)nodesIR;
  auto& indexInfo = indexEntry.getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  auto& indexIR = indexInfo.getPatternIR();
  auto indexNodesInfoCopy = indexIR.nodesInfo;
  auto& indexNodesInfo = indexNodesInfoCopy;
  auto patternGraph = matchClause.getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  std::vector<bool> markedDelete(patternGraph->getNumQueryRels(), false);
  // Consume the verified relationship mapping, including parallel edges and
  // incoming traversals. Matching only endpoint pairs can delete the wrong
  // edge or leave one of the materialized source edges dangling.
  for (const auto& indexRel : indexIR.relsInfo) {
    if (indexRel.matchedIdx == UINT64_MAX ||
        indexRel.matchedIdx >= nodesIR.relsInfo.size()) continue;
    const auto& name = nodesIR.relsInfo[indexRel.matchedIdx].uniqName;
    const auto local = patternGraph->queryRelNameToPosMap.find(name);
    if (local != patternGraph->queryRelNameToPosMap.end() &&
        local->second < markedDelete.size()) {
      markedDelete[local->second] = true;
    }
  }

  QueryGraph graph;
  for (size_t i = 0; i < patternGraph->getNumQueryRels(); i++) {
    if (markedDelete[i]) {
      continue;
    }
    auto rel = patternGraph->getQueryRel(i);
    addRelAndEndpoints(graph, rel);
  }

  auto* catalog = clientContext->getCatalog();
  auto* transaction = clientContext->getTransaction();
  auto endpointNodes = collectMatchedFillEndpointNodes(
      *patternGraph, indexInfo, indexNodesInfo);
  std::shared_ptr<RelExpression> targetRel;
  std::shared_ptr<RelExpression> aggregateTargetRel;
  const bool independentProjectionOccurrence =
      hasOrderedDerivedRelEndpoints(indexInfo, clientContext) &&
      indexInfo.getTargetColumns().empty();
  if (endpointNodes.size() == 2 && independentProjectionOccurrence) {
    // A row-preserving projection contains one relationship per source row.
    // Two disjoint source occurrences between the same retained endpoints
    // still form a bag join (m*m rows), so they need two independent scans.
    // Reusing an earlier scan by endpoint identity would silently turn this
    // into m rows. Aggregate carriers use their separate keyed-value rules.
    targetRel = synthesizeFillRelExpression(
        binder, clientContext, indexInfo, endpointNodes);
    if (targetRel != nullptr && !additionalFillTables.empty()) {
      auto entries = targetRel->getEntries();
      for (const auto& table : additionalFillTables)
        if (std::find(entries.begin(), entries.end(), table.entry) == entries.end())
          entries.push_back(table.entry);
      targetRel->setEntries(std::move(entries));
    }
    relExpressions.push_back(targetRel);
    addRelAndEndpoints(graph, targetRel);
  } else if (endpointNodes.size() == 2) {
    // Prefer the relationship from this MATCH clause. The same endpoint
    // variables can have another relationship of the same type in an earlier
    // clause; reusing that expression would leave this clause's alias dangling.
    targetRel = findExistingFillRelExpression(
        patternGraph->queryRels, indexInfo, endpointNodes, relLabels);

    // If the same filled relationship is already scanned by a mandatory
    // MATCH in this query part, bind the aggregate property to that scan. A
    // property slot introduced only in an OPTIONAL clause whose endpoints are
    // already bound is not materialized by the execution context.
    for (uint32_t clauseIdx = 0;
         clauseIdx < queryPart.getNumReadingClause() &&
         aggregateTargetRel == nullptr;
         ++clauseIdx) {
      auto* clause = queryPart.getReadingClause(clauseIdx);
      if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
        continue;
      }
      auto& otherMatch = clause->cast<BoundMatchClause>();
      if (&otherMatch == &matchClause ||
          otherMatch.getMatchClauseType() != MatchClauseType::MATCH) {
        continue;
      }
      auto* collection = otherMatch.getQueryGraphCollection();
      for (uint32_t graphIdx = 0;
           graphIdx < collection->getNumQueryGraphs() &&
           aggregateTargetRel == nullptr;
           ++graphIdx) {
        auto* otherGraph = collection->getQueryGraph(graphIdx);
        aggregateTargetRel = findExistingFillRelExpression(
            otherGraph->queryRels, indexInfo, endpointNodes, relLabels);
      }
    }
    if (aggregateTargetRel == nullptr) {
      aggregateTargetRel = findExistingFillRelExpression(
          relExpressions, indexInfo, endpointNodes, relLabels);
    }
    if (targetRel == nullptr) {
      targetRel = findExistingFillRelExpression(
          graph.queryRels, indexInfo, endpointNodes, relLabels);
    }
    if (targetRel == nullptr && aggregateTargetRel == nullptr) {
      targetRel = findExistingFillRelExpression(
          relExpressions, indexInfo, endpointNodes, relLabels);
      if (targetRel == nullptr || targetRel->getVariableName().empty()) {
        targetRel = synthesizeFillRelExpression(
            binder, clientContext, indexInfo, endpointNodes);
        if (targetRel != nullptr && !additionalFillTables.empty()) {
          auto entries = targetRel->getEntries();
          for (const auto& table : additionalFillTables) {
            if (std::find(entries.begin(), entries.end(), table.entry) == entries.end()) {
              entries.push_back(table.entry);
            }
          }
          targetRel->setEntries(std::move(entries));
        }
        relExpressions.push_back(targetRel);
      }
      addRelAndEndpoints(graph, targetRel);
    }
    // Reusing an expression does not preserve its scan. A local FILL target
    // may have been removed together with the materialized interaction graph.
    if (targetRel != nullptr &&
        !graph.queryRelNameToPosMap.contains(targetRel->getUniqueName())) {
      addRelAndEndpoints(graph, targetRel);
    }
  }
  if (aggregateTargetRel == nullptr) {
    aggregateTargetRel = targetRel;
  }
  if (aggregateTargetRel != nullptr) {
    for (auto& target : propertyRewriteTargets) {
      if (target.first->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        continue;
      }
      target.second.newVarUniqueName = aggregateTargetRel->getUniqueName();
      target.second.newVarRawName = aggregateTargetRel->getVariableName().empty()
          ? aggregateTargetRel->getUniqueName()
          : aggregateTargetRel->getVariableName();
      target.second.directionTargetRel = aggregateTargetRel;
    }
  }

  std::vector<std::shared_ptr<NodeExpression>> nodeTables;
  if (graph.queryNodes.empty() && aggregateTargetRel == nullptr) {
      NEUG_ASSERT(UINT32_MAX != indexInfo.srcTableId && UINT32_MAX != indexInfo.dstTableId);
      nodeTables =
          collectMatchedFillEndpointNodes(*patternGraph, indexInfo, indexNodesInfo);
      if (nodeTables.empty()) {
        for (auto& node : patternGraph->getQueryNodes()) {
            const auto& tableIDs = node->getTableIDs();
            if (!tableIDs.empty() &&
                (tableIDs[0] == indexInfo.srcTableId ||
                 tableIDs[0] == indexInfo.dstTableId)) {
                nodeTables.push_back(node);
            }
        }
      }
      NEUG_ASSERT(nodeTables.size() == 2);
      auto currentRels = patternGraph->getQueryRels();
      auto existingRel = findExistingFillRelExpression(
          currentRels, indexInfo, nodeTables, relLabels);
      if (existingRel != nullptr) {
          addRelAndEndpoints(graph, existingRel);
      } else {
          auto syntheticRel =
              synthesizeFillRelExpression(binder, clientContext, indexInfo,
                                          nodeTables);
          addRelAndEndpoints(graph, syntheticRel);
          for (auto & target : propertyRewriteTargets) {
            if (target.first->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
              NEUG_ASSERT(target.second.newVarRawName == "" && target.second.newVarUniqueName == "");
              target.second.newVarUniqueName = syntheticRel->getUniqueName();
              target.second.newVarRawName = syntheticRel->getVariableName();
            }
          }
          relExpressions.push_back(syntheticRel);
      }
  }
  for (auto &pr : patternGraph->queryNodeNameToPosMap) {
      visitedNodeUniqNames.insert(pr.first);
  }
  for (auto &pr : patternGraph->queryRelNameToPosMap) {
      visitedNodeUniqNames.insert(pr.first);
  }
  *patternGraph = std::move(graph);
}

// --- Expression rewriting ---

void RewriterV4::rewritePropertyExpression(catalog::IndexCatalogEntry* entry) {
  if (expressionsToRewrite.empty() || propertyRewriteTargets.empty()) {
    return;
  }

  auto& indexInfo = entry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  auto fillTable = getFillTableContext(indexInfo);
  if (!fillTable.has_value()) {
    return;
  }

  for (auto& iter : expressionsToRewrite) {
    auto& expr = iter.second;
    if (!expr || expr->expressionType != ExpressionType::PROPERTY) {
      continue;
    }
    auto* propExpr = expr->ptrCast<PropertyExpression>();
    if (propExpr == nullptr || isInternalPatternProperty(*propExpr)) {
      continue;
    }
    auto it = propertyRewriteTargets.find(expr.get());
    if (it == propertyRewriteTargets.end()) {
      continue;
    }
    const auto oldUniqueName = propExpr->getUniqueName();
    const auto oldAlias = propExpr->hasAlias() ? propExpr->getAlias() : "";
    const auto& target = it->second;

    propExpr->setUniqueVarName(target.newVarUniqueName);
    propExpr->setRawVariableName(target.newVarRawName);
    propExpr->setPropertyName(target.newPropertyName);

    propExpr->dataType =
        fillTable->entry->getProperty(target.newPropertyName).getType().copy();

    common::table_id_map_t<SingleLabelPropertyInfo> infos;
    infos.insert({fillTable->tableID,
                  SingleLabelPropertyInfo(true /* exists */,
                                          false /* isPrimaryKey */)});
    for (const auto& additional : additionalFillTables) {
      if (additional.tableID != fillTable->tableID &&
          additional.entry->containsProperty(target.newPropertyName) &&
          additional.entry->getProperty(target.newPropertyName).getType() ==
              propExpr->dataType) {
        infos.insert({additional.tableID,
                      SingleLabelPropertyInfo(true /* exists */,
                                              false /* isPrimaryKey */)});
      }
    }
    propExpr->setInfos(std::move(infos));
    // Keep the logical output identity (and therefore the unaliased wire
    // name), while the typed property still addresses the physical carrier.
    propExpr->setUniqueName(oldUniqueName);

    if (!oldAlias.empty()) {
      propExpr->setAlias(oldAlias);
    }

    uniqueNameRewriteMap[oldUniqueName] = expr;
    if (!oldAlias.empty()) {
      uniqueNameRewriteMap[oldAlias] = expr;
    }
  }
}

void RewriterV4::applyIndexExpressionRewrites(catalog::IndexCatalogEntry* entry,
                                              BoundMatchClause& matchClause,
                                              NormalizedQueryPart& queryPart) {
  projectionPredicateAnchorNodes.clear();
  if (propertyRewriteTargets.empty()) {
    return;
  }

  auto& indexInfo = entry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
  auto fillTable = getFillTableContext(indexInfo);
  if (!fillTable.has_value()) {
    return;
  }

  // A single ungrouped COUNT(*) is a linear total: absent prefix rows
  // contribute zero, and adding zero-weight suffix rows cannot create groups.
  // Keep the original guards for grouped, optional, or mixed aggregates.
  const auto* countBody = queryPart.hasProjectionBody()
      ? queryPart.getProjectionBody() : nullptr;
  bool linearGlobalCount = countBody != nullptr &&
      countBody->getGroupByExpressions().empty() &&
      countBody->getProjectionExpressions().size() == 1 &&
      countBody->getAggregateExpressions().size() == 1 &&
      !countBody->isDistinct() && !queryPart.hasUpdatingClause();
  if (linearGlobalCount) {
    const auto projection = countBody->getProjectionExpressions()[0];
    const auto aggregate = countBody->getAggregateExpressions()[0];
    linearGlobalCount = projection && aggregate &&
        projection->getUniqueName() == aggregate->getUniqueName() &&
        aggregate->expressionType == ExpressionType::AGGREGATE_FUNCTION &&
        aggregate->constPtrCast<AggregateFunctionExpression>()->getFunction().name == "COUNT_STAR" &&
        !aggregate->constPtrCast<AggregateFunctionExpression>()->isDistinct();
  }
  for (common::idx_t i = 0; linearGlobalCount && i < queryPart.getNumReadingClause(); ++i) {
    const auto* clause = queryPart.getReadingClause(i);
    linearGlobalCount = clause->getClauseType() == ClauseType::MATCH &&
        clause->constPtrCast<BoundMatchClause>()->getMatchClauseType() == MatchClauseType::MATCH;
  }

  std::unordered_set<std::string> erasedAggregateUniqueNames;
  std::vector<std::tuple<std::string, std::shared_ptr<NodeExpression>,
                         std::shared_ptr<Expression>>>
      nonzeroCountPreProjectionPredicates;
  expression_vector replacementAggregateExpressions;
  const ExpressionRewriteRule rewriteRule = [&](std::shared_ptr<Expression>& expr) -> bool {
    if (!expr) {
      return true;
    }
    if (expr->expressionType == ExpressionType::PROPERTY &&
        !propertyRewriteTargets.contains(expr.get())) {
      // GROUP BY and ORDER BY may hold copies rather than the exact property
      // object projected above. Rewrite by the preserved bound identity too.
      const auto replacement = uniqueNameRewriteMap.find(expr->getUniqueName());
      if (replacement != uniqueNameRewriteMap.end() &&
          replacement->second != expr) {
        expr = deepCopyShared(replacement->second);
        return true;
      }
    }

    if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
      auto it = propertyRewriteTargets.find(expr.get());
      if (it != propertyRewriteTargets.end()) {
        auto aggregateFunctionName =
            expr->constPtrCast<AggregateFunctionExpression>()
                ->getFunction()
                .name;
        const bool originalDistinct = expr->constPtrCast<AggregateFunctionExpression>()->isDistinct();
        std::transform(aggregateFunctionName.begin(),
                       aggregateFunctionName.end(),
                       aggregateFunctionName.begin(), [](unsigned char c) {
                         return static_cast<char>(std::toupper(c));
                       });
        const auto oldUniqueName = expr->getUniqueName();
        const auto oldAlias = expr->hasAlias() ? expr->getAlias() : std::string{};
        const auto oldParsedName = expr->getParsedName();
        const auto oldRawVars = expr->getRawNameVariables();
        erasedAggregateUniqueNames.insert(oldUniqueName);
        auto& target = it->second;
        std::shared_ptr<RelExpression> targetRel;
        if (target.newVarRawName.empty()) {
          bool found{false};
          for (size_t k = 0; k < matchClause.getQueryGraphCollection()->getNumQueryGraphs() && !found; ++k) {
            auto graph = matchClause.getQueryGraphCollection()->getQueryGraph(k);
            for (auto node : graph->queryNodes) {
              if (node->getSingleEntry()->containsProperty(target.newPropertyName)) {
                target.newVarRawName = node->getVariableName();
                target.newVarUniqueName = node->getUniqueName();
                found = true;
                break;
              }
            }
            if (found) {
              break;
            }
            for (auto rel : graph->queryRels) {
              if (rel->getSingleEntry()->containsProperty(target.newPropertyName)) {
                target.newVarRawName = rel->getVariableName();
                target.newVarUniqueName = rel->getUniqueName();
                targetRel = rel;
                found = true;
                break;
              }
            }
          }
        } else if (target.directionAwareSameLabelRel) {
          for (size_t k = 0;
               k < matchClause.getQueryGraphCollection()->getNumQueryGraphs() &&
               targetRel == nullptr;
               ++k) {
            auto graph = matchClause.getQueryGraphCollection()->getQueryGraph(k);
            for (auto rel : graph->queryRels) {
              if (rel != nullptr &&
                  rel->getUniqueName() == target.newVarUniqueName) {
                targetRel = rel;
                break;
              }
            }
          }
          if (targetRel == nullptr) {
            targetRel = target.directionTargetRel;
          }
        }
        if (target.newVarRawName.empty() || target.newVarUniqueName.empty()) {
          THROW_INDEX_EXCEPTION(
              "Cannot resolve rewrite target for aggregate expression: " +
              oldUniqueName);
        }
        std::shared_ptr<Expression> replacement;
        if (indexInfo.fillType == FillEntryType::NODE ||
            !additionalFillTables.empty()) {
          replacement = makeFillPropertyExpression(*fillTable, target);
        } else {
          replacement = makeDirectionAwareRelPropertyExpression(
              binder, fillTable->entry, fillTable->tableID,
              target.newVarUniqueName, target.newVarRawName,
              target.newPropertyName, target.forwardPropertyName,
              target.reversePropertyName, target.directionAwareSameLabelRel,
              targetRel, target.directionReferenceNodeUniqueName,
              target.directionReferenceNodeRawName);
        }
        auto replacementRawVars = target.newVarRawName.empty()
            ? oldRawVars
            : std::vector<std::string>{target.newVarRawName};
        const bool countBackedAggregate = target.countAggregateOperand != nullptr;
        const bool weightedSum = countBackedAggregate && aggregateFunctionName == "SUM" && !originalDistinct;
        const bool countNonNull = countBackedAggregate && aggregateFunctionName == "COUNT" && !originalDistinct;
        // Keep the unmultiplied count for the mandatory-presence predicate.
        // f may be zero or NULL even when the original region has real rows.
        auto preAggregateValue = deepCopyShared(replacement);
        const bool directGlobalCount = linearGlobalCount &&
            aggregateFunctionName == "COUNT_STAR" &&
            indexInfo.fillType == FillEntryType::NODE;
        if (globalCountProductProof && oldUniqueName == globalCountProductProof->aggregateUniqueName) {
          bool publicCount = false;
          for (size_t column = 0; column < indexInfo.getTargetColumns().size(); ++column)
            publicCount |= indexInfo.getTargetColumns()[column] == target.newPropertyName &&
                           isPublicRowCount(indexInfo, column);
          const bool first = globalCountProductProof->factors.empty() && directGlobalCount && !countBackedAggregate;
          const bool next = weightedSum &&
              matchesCountProduct(target.countAggregateOperand, globalCountProductProof->factors);
          if (indexInfo.fillType == FillEntryType::NODE && !indexInfo.usesWorkloadCompatibility() &&
              additionalFillTables.empty() && publicCount && !originalDistinct &&
              replacement->expressionType == ExpressionType::PROPERTY && (first || next)) {
            globalCountProductProof->factors.push_back(deepCopyShared(replacement));
          } else {
            globalCountProductProof.reset();
          }
        }
        if (countBackedAggregate) {
          if (weightedSum) {
            replacement = binder->getExpressionBinder()->bindScalarFunctionExpression(
                expression_vector{deepCopyShared(target.countAggregateOperand), std::move(replacement)}, "*");
          } else if (countNonNull) {
            auto isNull = binder->getExpressionBinder()->bindNullOperatorExpression(
                ExpressionType::IS_NULL, expression_vector{deepCopyShared(target.countAggregateOperand)});
            const auto unique = "idx_nonnull_count(" + target.countAggregateOperand->getUniqueName() +
                "," + replacement->getUniqueName() + ")";
            auto value = std::make_shared<CaseExpression>(replacement->getDataType().copy(),
                                                         std::move(replacement), unique);
            auto zero = Value(int64_t{0});
            value->addCaseAlternative(std::move(isNull),
                std::make_shared<LiteralExpression>(zero, zero.toString()));
            // The mandatory count-presence predicate makes this operand
            // non-NULL. SUM therefore preserves COUNT's empty-input zero.
            replacement = std::move(value);
          } else {
            // MIN/MAX and COUNT(DISTINCT) see the same value set when
            // duplicate rows collapse. Their count is used only for presence.
            replacement = deepCopyShared(target.countAggregateOperand);
          }
          for (const auto& name : oldRawVars)
            if (std::find(replacementRawVars.begin(), replacementRawVars.end(), name) == replacementRawVars.end())
              replacementRawVars.push_back(name);
        } else if (!directGlobalCount && (indexInfo.fillType == FillEntryType::NODE ||
            (indexInfo.fillType == FillEntryType::REL &&
             !target.directionAwareSameLabelRel))) {
          replacement = makeZeroIfNullExpression(binder, replacement);
        }
        if (directGlobalCount) {
          LOG(INFO) << "Simplified ungrouped node FILL COUNT to direct weighted SUM";
        }
        if (matchClause.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH &&
            matchClause.hasPredicate()) {
          const bool count = aggregateFunctionName == "COUNT" || aggregateFunctionName == "COUNT_STAR";
          auto missing = binder->getExpressionBinder()->createLiteralExpression(
              count ? Value(int64_t{0}) : Value::createNullValue(replacement->getDataType()));
          const auto& original = expr->constCast<AggregateFunctionExpression>();
          if (!count && original.getNumChildren() == 1 &&
              original.getChild(0)->expressionType == ExpressionType::CASE_ELSE)
            missing = deepCopyShared(original.getChild(0)->constCast<CaseExpression>().getElseExpression());
          auto guarded = std::make_shared<CaseExpression>(replacement->getDataType().copy(),
              missing, oldUniqueName + "_fill_optional_guard");
          guarded->addCaseAlternative(deepCopyShared(matchClause.getPredicate()), replacement);
          replacement = guarded;
        }
        replacement = makeValueAggregateExpression(
            clientContext, replacement, oldAlias, oldParsedName,
            replacementRawVars, countBackedAggregate && !countNonNull ? aggregateFunctionName :
                target.completePairDistinctCount ? "MAX" : "SUM",
            countBackedAggregate && originalDistinct);
        // WITH aliases can be referenced across several query parts. Keep
        // their bound identity for both node and relationship FILL aggregates.
        replacement->setUniqueName(oldUniqueName);
        if (!oldAlias.empty()) {
          replacement->setAlias(oldAlias);
          auto aliasRef = makeProjectionAliasReference(*replacement, oldAlias);
          uniqueNameRewriteMap[oldUniqueName] = aliasRef;
          uniqueNameRewriteMap[oldAlias] = aliasRef;
        }
        replacementAggregateExpressions.push_back(replacement);

        // A regular MATCH must not manufacture a zero-count group when the
        // eliminated pattern has no rows. OPTIONAL MATCH and SUM have
        // different zero-row semantics. A weighted SUM also requires source
        // presence, but keeps its operand NULL rather than replacing it by 0.
        // MIN/MAX also retain only anchors with at least one source row.
        const auto isCountAggregate = aggregateFunctionName == "COUNT" ||
                                      aggregateFunctionName == "COUNT_STAR";
        if (!directGlobalCount &&
            ((indexInfo.fillType == FillEntryType::NODE && (isCountAggregate || countBackedAggregate)) ||
             (indexInfo.fillType == FillEntryType::REL && countBackedAggregate)) &&
            matchClause.getMatchClauseType() == MatchClauseType::MATCH &&
            binder != nullptr) {
          auto zero = Value::createDefaultValue(preAggregateValue->getDataType());
          auto zeroExpr =
              std::make_shared<LiteralExpression>(zero, zero.toString());
          auto countValue = preAggregateValue;
          if (target.targetNode != nullptr) {
            countValue = target.targetNode->getPropertyExpression(
                target.newPropertyName);
          }
          nonzeroCountPreProjectionPredicates.emplace_back(
              target.newVarUniqueName, target.targetNode,
              binder->getExpressionBinder()->bindComparisonExpression(
                  ExpressionType::NOT_EQUALS,
                  expression_vector{std::move(countValue),
                                    std::move(zeroExpr)}));
          if (globalCountProductProof && oldUniqueName == globalCountProductProof->aggregateUniqueName)
            globalCountProductProof->introducedPresencePredicates.insert(
                std::get<2>(nonzeroCountPreProjectionPredicates.back())->getUniqueName());
        }

        expr = std::move(replacement);
        return true;
      }
    }

    auto it = propertyRewriteTargets.find(expr.get());
    if (it != propertyRewriteTargets.end()) {
      const auto oldUniqueName = expr->getUniqueName();
      const auto oldAlias = expr->hasAlias() ? expr->getAlias() : std::string{};
      collectAggregateUniqueNames(expr, erasedAggregateUniqueNames);
      auto dependentVarNames = collectDependentVarNames(expr);
      erasedAggregateUniqueNames.insert(dependentVarNames.begin(),
                                        dependentVarNames.end());
      auto& target = it->second;
      if (target.newVarUniqueName == "") {
        auto val = Value(0.0);
        auto replacement = std::make_shared<LiteralExpression>(std::move(val), val.toString());
        if (!oldAlias.empty()) {
          replacement->setAlias(oldAlias);
        }

        uniqueNameRewriteMap[oldUniqueName] = replacement;
        if (!oldAlias.empty()) {
          uniqueNameRewriteMap[oldAlias] = replacement;
        }

        expr = std::move(replacement);
        return true;
      }

      auto replacement = makeFillPropertyExpression(*fillTable, target);
      replacement->setUniqueName(oldUniqueName);
      if (!oldAlias.empty()) {
        replacement->setAlias(oldAlias);
      }

      uniqueNameRewriteMap[oldUniqueName] = replacement;
      if (!oldAlias.empty()) {
        uniqueNameRewriteMap[oldAlias] = replacement;
      }

      expr = std::move(replacement);
      return true;
    }

    return false;
  };

  if (matchClause.hasPredicate()) {
    auto pred = matchClause.getPredicate();
    rewriteExpressionTree(pred, rewriteRule);
    matchClause.setPredicate(std::move(pred));
  }
  if (queryPart.hasProjectionBodyPredicate()) {
    auto pred = queryPart.getProjectionBodyPredicate();
    rewriteExpressionTree(pred, rewriteRule);
    queryPart.setProjectionBodyPredicate(pred);
  }
  if (queryPart.hasProjectionBody()) {
    auto* body = queryPart.getProjectionBodyUnsafe();
    auto proj = body->getProjectionExpressions();
    for (auto& e : proj) {
      rewriteExpressionTree(e, rewriteRule);
    }
    body->setProjectionExpressions(std::move(proj));
    auto groupBy = body->getGroupByExpressions();
    for (auto& expression : groupBy) rewriteExpressionTree(expression, rewriteRule);
    body->setGroupByExpressions(std::move(groupBy));
    auto orderBy = body->getOrderByExpressions();
    for (auto& expression : orderBy) rewriteExpressionTree(expression, rewriteRule);
    body->setOrderByExpressions(std::move(orderBy), body->getSortingOrders());
    auto aggregateExprs = body->getAggregateExpressions();
    expression_vector rewrittenAggregateExprs =
        replacementAggregateExpressions;
    rewrittenAggregateExprs.reserve(rewrittenAggregateExprs.size() +
                                     aggregateExprs.size());
    for (auto& aggregateExpr : aggregateExprs) {
      const auto eraseAggregate =
          aggregateExpr != nullptr &&
          (erasedAggregateUniqueNames.contains(aggregateExpr->getUniqueName()) ||
           erasedAggregateUniqueNames.contains(aggregateExpr->rawName()) ||
           erasedAggregateUniqueNames.contains(aggregateExpr->getParsedName()) ||
           (aggregateExpr->hasAlias() &&
            erasedAggregateUniqueNames.contains(aggregateExpr->getAlias())));
      if (eraseAggregate) {
        continue;
      }
      rewriteExpressionTree(aggregateExpr, rewriteRule);
      if (aggregateExpr != nullptr &&
          aggregateExpr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
        rewrittenAggregateExprs.push_back(std::move(aggregateExpr));
      }
    }
    body->setAggregateExpressions(std::move(rewrittenAggregateExprs));
  }
  for (auto& [anchorUniqueName, anchorNode, predicate] :
       nonzeroCountPreProjectionPredicates) {
    bool currentClauseBindsAnchor = false;
    auto* graphCollection = matchClause.getQueryGraphCollectionUnsafe();
    for (uint32_t graphIdx = 0;
         graphIdx < graphCollection->getNumQueryGraphs(); ++graphIdx) {
      if (graphCollection->getQueryGraph(graphIdx)->containsQueryNode(anchorUniqueName) ||
          graphCollection->getQueryGraph(graphIdx)->containsQueryRel(anchorUniqueName)) {
        currentClauseBindsAnchor = true;
        break;
      }
    }
    if (currentClauseBindsAnchor) {
      matchClause.setPredicate(
          binder->getExpressionBinder()->combineBooleanExpressions(
              ExpressionType::AND, matchClause.getPredicate(),
              std::move(predicate)));
      continue;
    }

    BoundMatchClause* earlierAnchorMatch = nullptr;
    for (uint32_t clauseIdx = 0;
         clauseIdx < queryPart.getNumReadingClause(); ++clauseIdx) {
      auto* clause = queryPart.getReadingClause(clauseIdx);
      if (clause == &matchClause) {
        break;
      }
      if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
        continue;
      }
      auto& earlierMatch = clause->cast<BoundMatchClause>();
      auto* earlierCollection = earlierMatch.getQueryGraphCollectionUnsafe();
      for (uint32_t graphIdx = 0;
           graphIdx < earlierCollection->getNumQueryGraphs(); ++graphIdx) {
        if (earlierCollection->getQueryGraph(graphIdx)->containsQueryNode(
                anchorUniqueName)) {
          earlierAnchorMatch = &earlierMatch;
          break;
        }
      }
      if (earlierAnchorMatch != nullptr) {
        break;
      }
    }
    if (earlierAnchorMatch != nullptr) {
      earlierAnchorMatch->setPredicate(
          binder->getExpressionBinder()->combineBooleanExpressions(
              ExpressionType::AND, earlierAnchorMatch->getPredicate(),
              std::move(predicate)));
      continue;
    }

    uint32_t queryPartIndex = UINT32_MAX;
    if (currentQuery_ != nullptr) {
      for (uint32_t i = 0; i < currentQuery_->getNumQueryParts(); ++i) {
        if (currentQuery_->getQueryPartUnsafe(i) == &queryPart) {
          queryPartIndex = i;
          break;
        }
      }
    }
    auto projectsAnchor = [&](NormalizedQueryPart& part) {
      if (!part.hasProjectionBody()) {
        return false;
      }
      const auto projections =
          part.getProjectionBody()->getProjectionExpressions();
      return std::any_of(
          projections.begin(), projections.end(), [&](const auto& expression) {
            return expression != nullptr &&
                   expression->getUniqueName() == anchorUniqueName;
          });
    };
    NormalizedQueryPart* filterPart = nullptr;
    if (projectsAnchor(queryPart)) {
      filterPart = &queryPart;
      projectionPredicateAnchorNodes.insert(anchorUniqueName);
    } else if (queryPartIndex != UINT32_MAX && queryPartIndex > 0) {
      auto* previousPart = currentQuery_->getQueryPartUnsafe(queryPartIndex - 1);
      if (previousPart->hasProjectionBody() &&
          (projectsAnchor(*previousPart) || anchorNode != nullptr)) {
        filterPart = previousPart;
      }
    }
    if (filterPart != nullptr) {
      if (!projectsAnchor(*filterPart) && anchorNode != nullptr &&
          filterPart->hasProjectionBody()) {
        auto* body = filterPart->getProjectionBodyUnsafe();
        auto projections = body->getProjectionExpressions();
        projections.push_back(anchorNode);
        body->setProjectionExpressions(std::move(projections));
        auto groupBy = body->getGroupByExpressions();
        const auto groupsByAnchor = std::any_of(
            groupBy.begin(), groupBy.end(), [&](const auto& expression) {
              return expression != nullptr &&
                     expression->getUniqueName() == anchorUniqueName;
            });
        if (!groupsByAnchor) {
          groupBy.push_back(anchorNode);
          body->setGroupByExpressions(std::move(groupBy));
        }
      }
      filterPart->setProjectionBodyPredicate(
          binder->getExpressionBinder()->combineBooleanExpressions(
              ExpressionType::AND,
              filterPart->getProjectionBodyPredicate(),
              std::move(predicate)));
    } else {
      queryPart.setProjectionBodyPredicate(
          binder->getExpressionBinder()->combineBooleanExpressions(
              ExpressionType::AND, queryPart.getProjectionBodyPredicate(),
              std::move(predicate)));
    }
  }
}

void RewriterV4::rewriteReferencesFromPreviousQueryPart(
    NormalizedQueryPart& queryPart) {
  if ((uniqueNameRewriteMap.empty() && deferredPropertyRewriteMap.empty()) ||
      !queryPart.hasProjectionBody()) {
    return;
  }

  auto* body = queryPart.getProjectionBodyUnsafe();
  auto aggregateExprs = body->getAggregateExpressions();
  std::unordered_set<std::string> erasedAggregates;

  const ExpressionRewriteRule rewriteRule =
      [&](std::shared_ptr<Expression>& expr) -> bool {
    if (!expr) {
      return true;
    }

    if (expr->expressionType == ExpressionType::PROPERTY) {
      auto* property = expr->ptrCast<PropertyExpression>();
      auto pos = deferredPropertyRewriteMap.find(deferredPropertyKey(
          property->getVariableName(), property->getPropertyName()));
      if (pos == deferredPropertyRewriteMap.end()) {
        pos = deferredPropertyRewriteMap.find(deferredPropertyKey(
            property->getRawVariableName(), property->getPropertyName()));
      }
      if (pos != deferredPropertyRewriteMap.end()) {
        expr = deepCopyShared(pos->second);
        return true;
      }
    }

    if (auto pos = uniqueNameRewriteMap.find(expr->getUniqueName());
        pos != uniqueNameRewriteMap.end()) {
      const auto replacedUniqueName = expr->getUniqueName();
      for (auto& aggregateExpr : aggregateExprs) {
        if (!erasedAggregates.contains(aggregateExpr->getUniqueName()) &&
            (aggregateExpr->getUniqueName() == replacedUniqueName ||
             pos->second->getUniqueName().find(
                 aggregateExpr->getUniqueName()) != std::string::npos)) {
          erasedAggregates.insert(aggregateExpr->getUniqueName());
        }
      }
      expr = deepCopyShared(pos->second);
      return true;
    }

    return false;
  };

  auto projections = body->getProjectionExpressions();
  for (auto& projection : projections) {
    rewriteExpressionTree(projection, rewriteRule);
  }
  auto groupByExpressions = body->getGroupByExpressions();
  for (auto& groupBy : groupByExpressions) {
    rewriteExpressionTree(groupBy, rewriteRule);
  }
  if (queryPart.hasProjectionBodyPredicate()) {
    auto predicate = queryPart.getProjectionBodyPredicate();
    rewriteExpressionTree(predicate, rewriteRule);
    queryPart.setProjectionBodyPredicate(std::move(predicate));
  }
  for (auto& aggregateExpr : aggregateExprs) {
    rewriteExpressionTree(aggregateExpr, rewriteRule);
  }

  expression_vector newAggregateExprs;
  for (auto& aggregateExpr : aggregateExprs) {
    if (!erasedAggregates.contains(aggregateExpr->getUniqueName())) {
      newAggregateExprs.push_back(std::move(aggregateExpr));
    }
  }
  body->setProjectionExpressions(std::move(projections));
  body->setGroupByExpressions(std::move(groupByExpressions));
  body->setAggregateExpressions(std::move(newAggregateExprs));
  deferredPropertyRewriteMap.clear();
}

// --- Main rewrite orchestration ---

void RewriterV4::rewritePatternGraph(NormalizedQueryPart& queryPart,
                                     BoundMatchClause& matchClause,
                                     PatternIR& nodesIR,
                                     IndexRewriteCandidate& candidate) {
  if (candidate.weightedRecursiveRelCoverageOnly) {
    return;
  }
  // The candidate was rematched against nodesIR, so all positions are local to
  // the current QueryGraph. Copy them into the catalog IR used by the existing
  // graph rewrite routines.
  auto& storedIR = candidate.info->getPatternIR();
  for (size_t i = 0; i < storedIR.nodesInfo.size() && i < candidate.indexIR.nodesInfo.size(); ++i) {
    storedIR.nodesInfo[i].matchedIdx = candidate.indexIR.nodesInfo[i].matchedIdx;
  }
  for (size_t i = 0; i < storedIR.relsInfo.size() && i < candidate.indexIR.relsInfo.size(); ++i) {
    storedIR.relsInfo[i].matchedIdx = candidate.indexIR.relsInfo[i].matchedIdx;
  }
  switch (candidate.info->fillType) {
    case FillEntryType::NODE:
      nodeIndexRewrite(queryPart, matchClause, nodesIR, *candidate.entry,
                       *candidate.queryGraph);
      break;
    case FillEntryType::REL:
      relIndexRewrite(queryPart, matchClause, nodesIR, *candidate.entry);
      break;
    case FillEntryType::VARIADIC_REL:
      variadicRelIndexRewrite(queryPart, matchClause, nodesIR,
                              *candidate.entry);
      break;
    default:
      THROW_INDEX_EXCEPTION(
          "Unsupported fill type for rewrite: " +
          std::to_string(static_cast<int>(candidate.info->fillType)));
  }
}

bool RewriterV4::acceptRewriteChoice(const IndexRewriteCandidate& candidate,
                                     const PatternIR& current) {
  if (binder == nullptr || !binder->hasIndexRewriteReplay()) return true;
  // Include the actual binding identity: one FILL may cover either of two
  // disjoint, isomorphic regions, and those are different search branches.
  // Length prefixes make catalog and variable names unambiguous.
  std::string choice;
  const auto append = [&](const std::string& value) {
    choice += std::to_string(value.size()) + ":" + value;
  };
  append(candidate.entry->getName());
  for (const auto& node : candidate.indexIR.nodesInfo) {
    if (node.matchedIdx >= current.nodesInfo.size()) return false;
    append(current.nodesInfo[node.matchedIdx].uniqName);
  }
  choice += "/";
  for (const auto& rel : candidate.indexIR.relsInfo) {
    if (rel.matchedIdx >= current.relsInfo.size()) return false;
    append(current.relsInfo[rel.matchedIdx].uniqName);
  }
  return binder->acceptIndexRewriteChoice(choice);
}

bool RewriterV4::rewriteMatchClause(BoundMatchClause& matchClause,
                                    NormalizedQueryPart& queryPart,
                                    PatternIR& nodesIR,
                                    IndexRewriteCandidate& candidate) {
  BoundRewriteTraceStep traceStep(*candidate.entry, candidate.indexIR, nodesIR);
  auto pre = collectPreRewriteInfo(matchClause, queryPart);

  if (candidate.info->fillType == FillEntryType::NODE &&
      !pre.projectedPatternUniques.empty()) {
    auto fillTable = getFillTableContext(*candidate.info);
    if (fillTable.has_value()) {
      for (size_t nodePos = 0; nodePos < nodesIR.nodesInfo.size(); ++nodePos) {
        const auto& node = nodesIR.nodesInfo[nodePos];
        if (!pre.projectedPatternUniques.contains(node.uniqName)) {
          continue;
        }
        for (const auto& column : candidate.info->indexColumnInfos) {
          if (column->columnType != IndexColumnType::NODE_PROPERTY) {
            continue;
          }
          auto target = getTargetForNodeProperty(
              nodePos, column->propertyName, nodesIR, *candidate.info,
              candidate.indexIR);
          if (!target.has_value()) {
            continue;
          }
          auto replacement = makeFillPropertyExpression(*fillTable, *target);
          deferredPropertyRewriteMap[deferredPropertyKey(
              node.uniqName, column->propertyName)] = replacement;
          deferredPropertyRewriteMap[deferredPropertyKey(
              node.varName, column->propertyName)] = replacement;
        }
      }
    }
  }

  rewritePatternGraph(queryPart, matchClause, nodesIR, candidate);
  if (candidate.info->getFillType() == FillEntryType::NODE &&
      !candidate.info->getPresenceColumns().empty()) {
    const auto fillTable = getFillTableContext(*candidate.info);
    const auto anchor = findAnchorInIndexIR(*candidate.info);
    const auto& node = nodesIR.nodesInfo[candidate.indexIR.nodesInfo[anchor].matchedIdx];
    for (const auto& column : candidate.info->getPresenceColumns()) {
      auto predicate = makeFillPropertyExpression(
          *fillTable, PropertyRewriteTarget{node.uniqName, node.varName, column});
      matchClause.setPredicate(binder->getExpressionBinder()->combineBooleanExpressions(
          ExpressionType::AND, matchClause.getPredicate(), std::move(predicate)));
    }
  }
  rewritePropertyExpression(candidate.entry);
  prunePredicatesForRemovedPatterns(matchClause,
      computeRemovedPatternUniques(matchClause, pre.beforePatternUniques),
      candidate.info->getPredicateTemplates(), nodesIR, candidate.indexIR,
      binder == nullptr ? nullptr : binder->getExpressionBinder());
  applyIndexExpressionRewrites(candidate.entry, matchClause, queryPart);

  auto removed = computeRemovedPatternUniques(
      matchClause, pre.beforePatternUniques);
  // An OPTIONAL expansion may disappear completely while its FILL anchor
  // remains bound by an earlier query part. Keep that anchor in WITH output:
  // the rewritten aggregate and later query parts still read its properties.
  for (const auto& [expression, target] : propertyRewriteTargets) {
    removed.erase(target.newVarUniqueName);
  }
  for (uint32_t clauseIdx = 0;
       clauseIdx < queryPart.getNumReadingClause(); ++clauseIdx) {
    auto* clause = queryPart.getReadingClause(clauseIdx);
    if (clause == nullptr || clause == &matchClause ||
        clause->getClauseType() != ClauseType::MATCH) {
      continue;
    }
    auto* collection = clause->cast<BoundMatchClause>()
                           .getQueryGraphCollectionUnsafe();
    for (uint32_t graphIdx = 0;
         graphIdx < collection->getNumQueryGraphs(); ++graphIdx) {
      for (const auto& node : collection->getQueryGraph(graphIdx)
                                  ->getQueryNodes()) {
        removed.erase(node->getUniqueName());
      }
    }
  }
  if (!removed.empty()) {
    recordDeferredPredicatesFromMatch(matchClause, pre.uniqueToRawVar, removed,
                                      pre.projectedPatternUniques);
    prunePredicatesForRemovedPatterns(
        matchClause, removed, candidate.info->getPredicateTemplates(),
        nodesIR, candidate.indexIR,
        binder == nullptr ? nullptr : binder->getExpressionBinder());
    dropProjectionsForRemovedPatterns(queryPart, removed);
  }

  if (candidate.info->getFillType() == FillEntryType::REL) {
    std::set<table_id_t> tables;
    if (const auto fill = getFillTableContext(*candidate.info)) {
      tables.insert(fill->tableID);
    } else if (auto* catalog = clientContext->getCatalog();
               catalog->containsRelGroup(clientContext->getTransaction(),
                                         candidate.info->getTableName())) {
      for (const auto id : catalog->getRelGroupEntry(clientContext->getTransaction(),
               candidate.info->getTableName())->getRelTableIDs()) tables.insert(id);
    }
    if (!tables.empty()) {
      for (const auto& additional : additionalFillTables) tables.insert(additional.tableID);
      pruneGuaranteedDerivedEndpointPredicates(matchClause, *candidate.info, tables,
          *clientContext, *binder->getExpressionBinder());
    }
  }

  traceStep.commit();
  ++committedMatchRewrites;
  return true;
}

static void filterAllowedRewriteTargets(std::vector<IndexCatalogEntry*>& indexEntries) {
  // A staged COUNT/SUM materialization retains MATCH/OPTIONAL and grouping
  // boundaries in its source. Only the typed computation-prefix proof may
  // consume it; its merged PatternIR is not a canonical local row count.
  std::erase_if(indexEntries, [](const catalog::IndexCatalogEntry* entry) {
    return entry->getAuxInfo().cast<catalog::FillIndexAuxInfo>().isStagedPipeline();
  });
  // Optional per-compilation allowlist for experiments that must retain
  // other graph patterns online while keeping the shared catalog intact.
  // Tokens are table names (all target columns) or table.column names.
  if (const auto* allowedEnv = std::getenv("NEUG_INDEX_REWRITE_ALLOW_TARGETS");
      allowedEnv != nullptr && *allowedEnv != '\0') {
    std::unordered_set<std::string> allowedTargets;
    std::string_view remaining(allowedEnv);
    while (!remaining.empty()) {
      const auto comma = remaining.find(',');
      auto token = remaining.substr(0, comma);
      while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
        token.remove_prefix(1);
      }
      while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
        token.remove_suffix(1);
      }
      if (!token.empty()) {
        allowedTargets.emplace(token);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      remaining.remove_prefix(comma + 1);
    }
    indexEntries.erase(
        std::remove_if(indexEntries.begin(), indexEntries.end(),
                       [&](IndexCatalogEntry* entry) {
          const auto& info = entry->getAuxInfoUnsafe()
                                 .cast<catalog::FillIndexAuxInfo>();
          if (allowedTargets.contains(info.getTableName())) {
            return false;
          }
          return std::none_of(
              info.getTargetColumns().begin(), info.getTargetColumns().end(),
              [&](const auto& column) {
                return allowedTargets.contains(info.getTableName() + "." + column);
              });
        }), indexEntries.end());
  }
}

bool RewriterV4::tryRewriteIndependentNodePresence(
    BoundMatchClause& matchClause, NormalizedQueryPart& queryPart,
    const std::vector<IndexCatalogEntry*>& indexEntries) {
  // Keep the complete MATCH and query part visible to every semantic check.
  // Only the local graph used for topology matching/mutation changes. This
  // preserves predicates between components and already rewritten anchors.
  auto* collection = matchClause.getQueryGraphCollectionUnsafe();
  for (uint32_t graphIdx = 0; graphIdx < collection->getNumQueryGraphs();
       ++graphIdx) {
    auto* graph = collection->getQueryGraphUnsafe(graphIdx);
    if (graph->getNumQueryRels() == 0) continue;
    QueryGraphLabelAnalyzer analyzer(*clientContext, false /* throwOnViolate */);
    analyzer.pruneLabel(*graph);
    const auto graphIR = convertGraphPatternToIR(*graph);
    for (const auto& rel : graph->getQueryRels()) {
      if (std::find(relExpressions.begin(), relExpressions.end(), rel) ==
          relExpressions.end()) relExpressions.push_back(rel);
    }
    for (auto* entry : indexEntries) {
      auto& info = entry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
      if (!completeAggregateInputIsClosed ||
          !provesNodePresenceSetSourceCached(info)) continue;
      auto indexIR = info.getPatternIR();
      if (indexIR.relsInfo.empty() ||
          indexIR.nodesInfo.size() > graphIR.nodesInfo.size() ||
          indexIR.relsInfo.size() > graphIR.relsInfo.size()) continue;
      auto localIR = graphIR;
      std::optional<IndexRewriteCandidate> accepted;
      auto [matched, ignoredStart] = PatternIR::findMatch(
          localIR, indexIR, entry->getName(), [&]() {
            expressionsToRewrite.clear();
            propertyRewriteTargets.clear();
            additionalFillTables.clear();
            IndexRewriteCandidate candidate{entry, &info, indexIR, graph};
            if (!validateRewriteCandidate(matchClause, queryPart, localIR, candidate) ||
                !aggregateEndpointPresenceOnlyCandidate ||
                !acceptRewriteChoice(candidate, localIR)) return false;
            accepted = std::move(candidate);
            return true;
          });
      (void)ignoredStart;
      if (!matched || !accepted.has_value()) continue;
      if (!rewriteMatchClause(matchClause, queryPart, localIR, *accepted)) {
        THROW_INDEX_EXCEPTION("fail to rewrite independent NODE presence");
      }
      LOG(INFO) << "Successfully applied index rewrite with index: "
                << entry->getName();
      // Rebuild bindings and predicates before considering the next component.
      return true;
    }
  }
  return false;
}

void RewriterV4::tryRewriteMatchClause(BoundMatchClause& matchClause,
                                        NormalizedQueryPart& queryPart) {
  auto catalog = clientContext->getCatalog();
  auto indexEntries =
      catalog->getExtendedIndexEntries(clientContext->getTransaction());
  filterAllowedRewriteTargets(indexEntries);
  std::erase_if(indexEntries, [&](const auto* entry) {
    return catalog->isExtendedIndexBuilding(entry->getName());
  });
  std::stable_sort(
      indexEntries.begin(), indexEntries.end(),
      [](IndexCatalogEntry* lhs, IndexCatalogEntry* rhs) {
        const auto& lhsPattern =
            lhs->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>().getPatternIR();
        const auto& rhsPattern =
            rhs->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>().getPatternIR();
        if (lhsPattern.relsInfo.size() != rhsPattern.relsInfo.size()) {
          return lhsPattern.relsInfo.size() > rhsPattern.relsInfo.size();
        }
        return lhs->getName() < rhs->getName();
      });
  const auto maxRewritePasses =
      mergedQueryIR_.relsInfo.size() + indexEntries.size() + 1;
  for (size_t pass = 0; pass < maxRewritePasses; ++pass) {
    if (currentQuery_ != nullptr) {
      mergedQueryIR_ = buildMergedQueryIR(*currentQuery_);
    }
    auto queryGraphCollection = matchClause.getQueryGraphCollectionUnsafe();
    if (queryGraphCollection->getNumQueryGraphs() != 1) {
      if (queryGraphCollection->getNumQueryGraphs() > 1 &&
          tryRewriteIndependentNodePresence(matchClause, queryPart, indexEntries)) {
        continue;
      }
      return;
    }
    auto queryGraph = queryGraphCollection->getQueryGraphUnsafe(0);
    QueryGraphLabelAnalyzer labelAnalyzer(*clientContext,
                                          false /* throwOnViolate */);
    labelAnalyzer.pruneLabel(*queryGraph);
    // The merged gate IR must see this clause's pruned labels. Built before
    // pruning, an untyped union (e.g. HASTAG over FORUM/POST/COMMENT) never
    // equals the typed FILL source, so an OPTIONAL count whose anchor comes
    // from an earlier clause of the same part was never considered.
    if (currentQuery_ != nullptr) mergedQueryIR_ = buildMergedQueryIR(*currentQuery_);
    auto nodesIR = convertGraphPatternToIR(*queryGraph);
    for (auto& rel : queryGraph->queryRels) {
      if (std::find(relExpressions.begin(), relExpressions.end(), rel) ==
          relExpressions.end()) {
        relExpressions.push_back(rel);
      }
    }

    // A union-label node is scanned from several physical node tables. Group
    // otherwise identical aggregate FILL indexes so their table-specific
    // properties can cover the logical union node in one rewrite.
    const bool hasUnionNode = std::any_of(
        nodesIR.nodesInfo.begin(), nodesIR.nodesInfo.end(),
        [](const auto& node) { return node.nodeLabels.size() > 1; });
    if (hasUnionNode) {
    for (auto* rawPrimary : indexEntries) {
      auto* primaryEntry = rawPrimary->ptrCast<IndexCatalogEntry>();
      auto& primaryInfo = primaryEntry->getAuxInfoUnsafe()
                              .cast<catalog::FillIndexAuxInfo>();
      if ((primaryInfo.getFillType() != FillEntryType::NODE &&
           primaryInfo.getFillType() != FillEntryType::REL) ||
          (primaryInfo.getTargetColumns().empty() &&
           !hasOrderedDerivedRelEndpoints(primaryInfo, clientContext))) {
        continue;
      }
      const auto primaryAnchor = findAnchorInIndexIR(primaryInfo);
      const auto& primaryPattern = primaryInfo.getPatternIR();
      if (primaryPattern.nodesInfo.size() > nodesIR.nodesInfo.size() ||
          primaryPattern.relsInfo.size() > nodesIR.relsInfo.size()) {
        continue;
      }
      if (primaryAnchor >= primaryPattern.nodesInfo.size() ||
          primaryPattern.nodesInfo[primaryAnchor].nodeLabels.size() != 1) {
        continue;
      }
      const bool derivedFamily =
          hasOrderedDerivedRelEndpoints(primaryInfo, clientContext);
      std::set<size_t> partitionPositions{primaryAnchor};
      if (derivedFamily) {
        partitionPositions.insert(primaryPattern.nodeUniqNameToPos.at(
            primaryInfo.getDstNodeUniqueName()));
      }
      bool labelsPossible = true;
      for (size_t i = 0; i < primaryPattern.nodesInfo.size(); ++i) {
        if (partitionPositions.contains(i)) continue;
        if (std::none_of(nodesIR.nodesInfo.begin(), nodesIR.nodesInfo.end(),
                         [&](const auto& node) {
                           return PatternIR::labelsEqual(
                               node.nodeLabels, primaryPattern.nodesInfo[i].nodeLabels);
                         })) {
          labelsPossible = false;
          break;
        }
      }
      if (!labelsPossible) continue;

      auto [cachedGroup, inserted] = aggregateGroups.try_emplace(primaryEntry);
      if (inserted) {
        cachedGroup->second = [&]() -> std::optional<AggregateIndexGroup> {
      const auto primaryFill = getFillTableContext(primaryInfo);
      if (!primaryFill.has_value()) return std::nullopt;
      std::vector<FillTableContext> fillTables;
      std::vector<const PatternIR*> groupedPatterns;
      bool allAggregateEndpointSetSources = true;
      std::set<std::vector<table_id_t>> coveredPartitions;
      std::map<size_t, std::set<table_id_t>> partitionLabels;
      for (auto* rawGrouped : indexEntries) {
        auto* groupedEntry = rawGrouped->ptrCast<IndexCatalogEntry>();
        auto& groupedInfo = groupedEntry->getAuxInfoUnsafe()
                                .cast<catalog::FillIndexAuxInfo>();
        if (!sameNodeAggregateIndexExceptAnchorLabel(
                primaryInfo, groupedInfo, catalog,
                clientContext->getTransaction(), relLabels, clientContext)) {
          continue;
        }
        std::vector<table_id_t> partition;
        for (const auto pos : partitionPositions) {
          const auto& labels = groupedInfo.getPatternIR().nodesInfo[pos].nodeLabels;
          if (labels.size() != 1) break;
          partition.push_back(labels.front());
        }
        if (partition.size() != partitionPositions.size() ||
            coveredPartitions.contains(partition)) continue;
        auto fillTable = getFillTableContext(groupedInfo);
        if (!fillTable.has_value()) {
          continue;
        }
        const auto hasAllColumns = std::all_of(
            primaryInfo.getTargetColumns().begin(),
            primaryInfo.getTargetColumns().end(), [&](const auto& column) {
              return column.starts_with(
                     InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX) ||
                     (primaryFill->entry->containsProperty(column) &&
                      fillTable->entry->containsProperty(column) &&
                      primaryFill->entry->getProperty(column).getType() ==
                          fillTable->entry->getProperty(column).getType());
            });
        if (!hasAllColumns) {
          continue;
        }
        coveredPartitions.insert(partition);
        size_t labelPos = 0;
        for (const auto pos : partitionPositions)
          partitionLabels[pos].insert(partition[labelPos++]);
        fillTables.push_back(*fillTable);
        groupedPatterns.push_back(&groupedInfo.getPatternIR());
        allAggregateEndpointSetSources &=
            provesAggregateEndpointSetSource(groupedInfo, *clientContext) ||
            provesNodePresenceSetSourceCached(groupedInfo);
      }
      if (fillTables.size() < 2) {
        VLOG(1) << "Multi-label aggregate candidate "
                << primaryEntry->getName()
                << " covers fewer than two physical labels";
        return std::nullopt;
      }

      // A union of endpoint labels denotes every compatible endpoint pair.
      // Missing one materialized partition would silently narrow that domain.
      // Require the complete product; exact PatternIR matching below also
      // checks every source relationship-label union against the query.
      size_t expectedPartitions = 1;
      for (const auto& [pos, labels] : partitionLabels) {
        (void)pos;
        expectedPartitions *= labels.size();
      }
      if (coveredPartitions.size() != expectedPartitions) return std::nullopt;

      auto combinedIndexIR = primaryPattern;
      for (const auto& [pos, labels] : partitionLabels) {
        combinedIndexIR.nodesInfo[pos].nodeLabels.assign(labels.begin(), labels.end());
      }
      const auto appendUniqueLabels = [](auto& target, const auto& source) {
        for (const auto label : source) {
          if (std::find(target.begin(), target.end(), label) == target.end()) {
            target.push_back(label);
          }
        }
      };
      for (const auto* pattern : groupedPatterns) {
        for (size_t i = 0; i < combinedIndexIR.relsInfo.size(); ++i) {
          appendUniqueLabels(combinedIndexIR.relsInfo[i].relLabels,
                             pattern->relsInfo[i].relLabels);
        }
      }
      if (derivedFamily) {
        // Each branch must equal the combined source restricted to its typed
        // endpoint pair. Merely unioning R1 from one branch with R2 from
        // another is insufficient when both edge types fit both partitions.
        for (const auto* pattern : groupedPatterns) {
          for (size_t i = 0; i < combinedIndexIR.relsInfo.size(); ++i) {
            const auto& rel = pattern->relsInfo[i];
            if (rel.relType != QueryRelType::NON_RECURSIVE) {
              // Recursive labels describe the whole traversal domain, not a
              // single physical step between the projected endpoint labels.
              // Every branch has already proved the same rebound path
              // contract; never prune away a zero-hop endpoint partition.
              if (!hasRecursiveEndpointContract(primaryInfo) ||
                  !PatternIR::labelsEqual(rel.relLabels,
                      combinedIndexIR.relsInfo[i].relLabels)) return std::nullopt;
              continue;
            }
            const auto& srcLabels = pattern->nodesInfo[rel.srcNodeIdx].nodeLabels;
            const auto& dstLabels = pattern->nodesInfo[rel.dstNodeIdx].nodeLabels;
            const auto contains = [](const auto& labels, auto id) {
              return std::find(labels.begin(), labels.end(), id) != labels.end();
            };
            std::vector<common::oid_t> expectedLabels;
            for (const auto label : combinedIndexIR.relsInfo[i].relLabels) {
              const auto* table = catalog->getTableCatalogEntry(
                  clientContext->getTransaction(), label);
              if (table == nullptr || table->getType() != CatalogEntryType::REL_TABLE_ENTRY)
                return std::nullopt;
              const auto& edge = table->constCast<RelTableCatalogEntry>();
              const bool forward = contains(srcLabels, edge.getSrcTableID()) &&
                  contains(dstLabels, edge.getDstTableID());
              const bool reverse = rel.edgeType == EdgeType::UNDIRECTED &&
                  contains(dstLabels, edge.getSrcTableID()) &&
                  contains(srcLabels, edge.getDstTableID());
              if (forward || reverse) expectedLabels.push_back(label);
            }
            if (!PatternIR::labelsEqual(expectedLabels, rel.relLabels))
              return std::nullopt;
          }
        }
      }
      for (auto& node : combinedIndexIR.nodesInfo) {
        node.undirected.clear();
        node.outgoing.clear();
        node.incoming.clear();
      }
      for (const auto& rel : combinedIndexIR.relsInfo) {
        if (rel.edgeType == EdgeType::UNDIRECTED) {
          combinedIndexIR.nodesInfo[rel.srcNodeIdx].undirected.emplace_back(
              rel.dstNodeIdx, rel.relLabels);
          combinedIndexIR.nodesInfo[rel.dstNodeIdx].undirected.emplace_back(
              rel.srcNodeIdx, rel.relLabels);
        } else {
          combinedIndexIR.nodesInfo[rel.srcNodeIdx].outgoing.emplace_back(
              rel.dstNodeIdx, rel.relLabels);
          combinedIndexIR.nodesInfo[rel.dstNodeIdx].incoming.emplace_back(
              rel.srcNodeIdx, rel.relLabels);
        }
      }

          return AggregateIndexGroup{std::move(combinedIndexIR), std::move(fillTables),
                                     allAggregateEndpointSetSources};
        }();
      }
      if (!cachedGroup->second.has_value()) continue;
      const auto& combinedIndexIR = cachedGroup->second->pattern;
      const auto& fillTables = cachedGroup->second->fillTables;
      auto mergedIndexIR = combinedIndexIR;
      auto [gateMatch, _] = PatternIR::findMatch(
          mergedQueryIR_, mergedIndexIR, primaryEntry->getName());
      if (!gateMatch) {
        VLOG(1) << "Multi-label aggregate candidate "
                << primaryEntry->getName()
                << " failed merged-query PatternIR match";
        continue;
      }
      auto localIndexIR = combinedIndexIR;
      auto [localMatch, localStart] =
          PatternIR::findMatch(nodesIR, localIndexIR, primaryEntry->getName());
      (void)localStart;
      if (!localMatch) {
        VLOG(1) << "Multi-label aggregate candidate "
                << primaryEntry->getName()
                << " failed local PatternIR match";
        continue;
      }

      expressionsToRewrite.clear();
      propertyRewriteTargets.clear();
      IndexRewriteCandidate candidate{primaryEntry, &primaryInfo,
                                      std::move(localIndexIR), queryGraph};
      if (!validateRewriteCandidate(matchClause, queryPart, nodesIR,
                                    candidate)) {
        auto alternateIR = combinedIndexIR;
        auto [accepted, ignoredStart] = PatternIR::findMatch(
            nodesIR, alternateIR, primaryEntry->getName(), [&]() {
              expressionsToRewrite.clear();
              propertyRewriteTargets.clear();
              candidate.indexIR = alternateIR;
              return validateRewriteCandidate(matchClause, queryPart, nodesIR,
                                               candidate);
            });
        (void)ignoredStart;
        if (!accepted) continue;
      }
      const auto rewritesAggregate = std::any_of(
          propertyRewriteTargets.begin(), propertyRewriteTargets.end(),
          [](const auto& target) {
            return target.first != nullptr &&
                   target.first->expressionType ==
                       ExpressionType::AGGREGATE_FUNCTION;
          });
      const bool endpointProjectionFamily = derivedFamily &&
          primaryInfo.getTargetColumns().empty();
      // Matching templates and labels alone do not prove identical grouping
      // domains. Every branch must independently establish its endpoint set.
      if (aggregateEndpointPresenceOnlyCandidate &&
          !cachedGroup->second->allAggregateEndpointSetSources) continue;
      if (!rewritesAggregate && !endpointProjectionFamily &&
          !aggregateEndpointPresenceOnlyCandidate) {
        VLOG(1) << "Multi-label aggregate candidate "
                << primaryEntry->getName()
                << " did not register an aggregate rewrite target";
        continue;
      }

      additionalFillTables.clear();
      auto primaryFill = getFillTableContext(primaryInfo);
      for (const auto& fillTable : fillTables) {
        if (!primaryFill.has_value() ||
            fillTable.tableID != primaryFill->tableID) {
          additionalFillTables.push_back(fillTable);
        }
      }
      if (!acceptRewriteChoice(candidate, nodesIR)) continue;
      if (!rewriteMatchClause(matchClause, queryPart, nodesIR, candidate)) {
        THROW_INDEX_EXCEPTION("fail to rewrite multi-label node aggregate");
      }
      LOG(INFO) << "Successfully applied multi-label index rewrite with "
                << fillTables.size() << " physical indexes for: "
                << primaryEntry->getName();
      additionalFillTables.clear();
      return;
    }
    }

    bool appliedInPass = false;
    for (auto entry : indexEntries) {
      additionalFillTables.clear();
      auto indexEntry = entry->ptrCast<IndexCatalogEntry>();
      auto& indexInfo =
          indexEntry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
      // DISTINCT FILLs may only be consumed by the whole-fragment path below.
      if (hasDistinctFillShape(indexInfo)) continue;
      auto& fullIndexIR = indexInfo.getPatternIR();
      if (mergedQueryIR_.nodesInfo.empty() ||
          fullIndexIR.nodesInfo.size() > mergedQueryIR_.nodesInfo.size()) {
        continue;
      }
      // A split REL anchor removes one edge, never a node. Weighted recursive
      // property coverage is the only path that needs no complete local match.
      const auto minLocalRels = fullIndexIR.relsInfo.size() -
          (indexInfo.getFillType() == FillEntryType::REL &&
           !fullIndexIR.relsInfo.empty() ? 1 : 0);
      const auto missingRels = std::count_if(
          fullIndexIR.relsInfo.begin(), fullIndexIR.relsInfo.end(),
          [&](const auto& indexRel) {
            return std::none_of(nodesIR.relsInfo.begin(), nodesIR.relsInfo.end(),
                                [&](const auto& rel) {
                                  return rel.relType == indexRel.relType &&
                                      (rel.edgeType == indexRel.edgeType ||
                                       (rel.edgeType == EdgeType::UNDIRECTED &&
                                        indexRel.edgeType == EdgeType::OUTGOING)) &&
                                      PatternIR::labelsEqual(rel.relLabels, indexRel.relLabels);
                                });
          });
      const bool localShapePossible =
          missingRels <= (indexInfo.getFillType() == FillEntryType::REL ? 1 : 0) &&
          fullIndexIR.nodesInfo.size() <= nodesIR.nodesInfo.size() &&
          minLocalRels <= nodesIR.relsInfo.size() &&
          std::all_of(fullIndexIR.nodesInfo.begin(), fullIndexIR.nodesInfo.end(),
                      [&](const auto& indexNode) {
                        return std::any_of(
                            nodesIR.nodesInfo.begin(), nodesIR.nodesInfo.end(),
                            [&](const auto& node) {
                              return PatternIR::labelsEqual(
                                  node.nodeLabels, indexNode.nodeLabels);
                            });
                      });
      if (!localShapePossible && !matchesWeightedRecursiveRelPropertyIndex(
              IndexRewriteCandidate{indexEntry, &indexInfo, {}, queryGraph})) {
        continue;
      }
      auto indexInfoCopied = nodesIR;
      expressionsToRewrite.clear();
      propertyRewriteTargets.clear();
      auto candidate =
          makeRewriteCandidate(matchClause, indexInfoCopied, indexEntry, fullIndexIR);
      if (!candidate.has_value()) {
        continue;
      }
      // Structural REL rewrites consume only the verified local match. Other
      // FILL kinds retain their global gate; recursive coverage retains its IR.
      if (indexInfo.getFillType() != FillEntryType::REL ||
          candidate->weightedRecursiveRelCoverageOnly) {
        auto indexCopy = fullIndexIR;
        auto [gateMatch, _] = PatternIR::findMatch(
            mergedQueryIR_, indexCopy, indexEntry->getName());
        if (!gateMatch && indexInfo.getFillType() != FillEntryType::REL) {
          continue;
        }
        if (candidate->weightedRecursiveRelCoverageOnly) {
          candidate->indexIR = std::move(indexCopy);
        }
      }
      const auto acceptsCandidate = [&]() {
        return validateRewriteCandidate(matchClause, queryPart, indexInfoCopied,
                                         *candidate) &&
            acceptRewriteChoice(*candidate, indexInfoCopied);
      };
      if (!acceptsCandidate()) {
        // Symmetric patterns can map retained keys to the wrong endpoints.
        // Search other complete mappings under the same semantic guard.
        if ((indexInfo.getFillType() != FillEntryType::NODE &&
             indexInfo.getFillType() != FillEntryType::REL) ||
            candidate->splitRelAnchor ||
            candidate->weightedRecursiveRelCoverageOnly) continue;
        auto alternateIR = fullIndexIR;
        auto [accepted, ignoredStart] = PatternIR::findMatch(
            indexInfoCopied, alternateIR, indexEntry->getName(), [&]() {
              expressionsToRewrite.clear();
              propertyRewriteTargets.clear();
              candidate->indexIR = alternateIR;
              return acceptsCandidate();
            });
        (void)ignoredStart;
        if (!accepted) continue;
      }
      const auto rewritesAggregate = std::any_of(
          propertyRewriteTargets.begin(), propertyRewriteTargets.end(),
          [](const auto& target) {
            return target.first != nullptr &&
                   target.first->expressionType ==
                       ExpressionType::AGGREGATE_FUNCTION;
          });
      // A public count replacement produces a complete executable aggregate.
      // This closed rule family can continue into another remaining region;
      // the next pass rediscovers its proof from that expression and catalog.
      // Other aggregate transformations retain their original stop boundary.
      bool continueCountComposition = rewritesAggregate &&
          (indexInfo.getFillType() == FillEntryType::NODE || indexInfo.getFillType() == FillEntryType::REL) &&
          !indexInfo.usesWorkloadCompatibility() &&
          (indexInfo.getFillType() == FillEntryType::NODE || hasOrderedDerivedRelEndpoints(indexInfo, clientContext)) &&
          matchClause.getMatchClauseType() == MatchClauseType::MATCH &&
          !candidate->indexIR.relsInfo.empty() && queryPart.hasProjectionBody();
      if (continueCountComposition) {
        const auto countTable = getFillTableContext(indexInfo);
        for (const auto& aggregate : queryPart.getProjectionBody()->getAggregateExpressions()) {
          const auto target = propertyRewriteTargets.find(aggregate.get());
          if (aggregate == nullptr || aggregate->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
              target == propertyRewriteTargets.end()) {
            continueCountComposition = false;
            break;
          }
          const auto* function = aggregate->constPtrCast<AggregateFunctionExpression>();
          const auto& functionName = function->getFunction().name;
          const bool stringExtremum = target->second.countAggregateOperand &&
              (functionName == "MIN" || functionName == "MAX") &&
              aggregate->getDataType().getLogicalTypeID() == LogicalTypeID::STRING;
          if (aggregate->getDataType().getLogicalTypeID() != LogicalTypeID::INT64 && !stringExtremum) {
            continueCountComposition = false;
            break;
          }
          bool publicCount = false;
          for (size_t column = 0; column < indexInfo.getTargetColumns().size(); ++column)
            publicCount |= isPublicRowCount(indexInfo, column) &&
                indexInfo.getTargetColumns()[column] == target->second.newPropertyName &&
                countTable.has_value() && countTable->entry->containsProperty(target->second.newPropertyName) &&
                countTable->entry->getProperty(target->second.newPropertyName).getType().getLogicalTypeID() ==
                    LogicalTypeID::INT64;
          const bool countIdentity = function->getFunction().name == "COUNT" &&
              function->getNumChildren() == 1 && function->getChild(0) != nullptr &&
              function->getChild(0)->expressionType == ExpressionType::PATTERN;
          if (!publicCount || (function->isDistinct() && !target->second.countAggregateOperand) ||
              target->second.directionAwareSameLabelRel ||
              (!target->second.countAggregateOperand && function->getFunction().name != "COUNT_STAR" &&
               !countIdentity)) {
            continueCountComposition = false;
            break;
          }
        }
      }
      std::vector<std::string> consumedRelationships;
      if (continueCountComposition) {
        for (const auto& relationship : candidate->indexIR.relsInfo) {
          if (relationship.matchedIdx >= indexInfoCopied.relsInfo.size()) {
            continueCountComposition = false;
            break;
          }
          consumedRelationships.push_back(indexInfoCopied.relsInfo[relationship.matchedIdx].uniqName);
        }
      }
      if (!rewriteMatchClause(matchClause, queryPart, indexInfoCopied,
                              *candidate)) {
        THROW_INDEX_EXCEPTION("fail to rewrite");
      }
      LOG(INFO) << "Successfully applied index rewrite with index: "
                << indexEntry->getName();
      if (rewritesAggregate && !continueCountComposition) {
        return;
      }
      if (continueCountComposition) {
        const auto* remaining = matchClause.getQueryGraphCollection();
        bool sourceRelationshipSurvived = false;
        for (uint32_t graph = 0; graph < remaining->getNumQueryGraphs(); ++graph)
          for (const auto& name : consumedRelationships)
            sourceRelationshipSurvived |= remaining->getQueryGraph(graph)->containsQueryRel(name);
        // A derived relationship can replace a single source relationship:
        // graph size need not shrink, but every matched source binding must
        // have been consumed. The next pass matches only the remaining graph.
        if (pendingPreQueryPart.has_value() || sourceRelationshipSurvived || consumedRelationships.empty()) return;
      }
      if (candidate->weightedRecursiveRelCoverageOnly) {
        return;
      }
      appliedInPass = true;
      break;
    }
    if (!appliedInPass) {
      return;
    }
  }
}

bool RewriterV4::tryRewriteQueryPart(NormalizedQueryPart& queryPart) {
  if (queryPart.hasUpdatingClause()) {
    return false;
  }

  bool removedReadingClause = false;
  for (auto i = 0u; i < queryPart.getNumReadingClause();) {
    auto* clause = queryPart.getReadingClause(i);
    if (clause->getClauseType() == ClauseType::MATCH) {
      auto& match = clause->cast<BoundMatchClause>();
      tryRewriteMatchClause(match, queryPart);
      auto* graphCollection = match.getQueryGraphCollectionUnsafe();
      bool hasRemainingPattern = false;
      for (uint32_t graphIdx = 0;
           graphIdx < graphCollection->getNumQueryGraphs(); ++graphIdx) {
        const auto* graph = graphCollection->getQueryGraph(graphIdx);
        if (graph->getNumQueryNodes() != 0 || graph->getNumQueryRels() != 0) {
          hasRemainingPattern = true;
          break;
        }
      }
      if (!hasRemainingPattern) {
        queryPart.removeReadingClause(i);
        removedReadingClause = true;
        continue;
      }
    }
    if (i < queryPart.getNumReadingClause() &&
        queryPart.getReadingClause(i) == clause) {
      ++i;
    }
  }
  return removedReadingClause && queryPart.getNumReadingClause() == 0;
}

static bool containsAggregateExpression(
    const std::shared_ptr<Expression>& expression) {
  if (expression == nullptr) {
    return false;
  }
  if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    return true;
  }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto* caseExpr = expression->ptrCast<CaseExpression>();
    if (caseExpr != nullptr) {
      for (auto i = 0u; i < caseExpr->getNumCaseAlternatives(); ++i) {
        const auto* alternative = caseExpr->getCaseAlternative(i);
        if (alternative != nullptr &&
            (containsAggregateExpression(alternative->whenExpression) ||
             containsAggregateExpression(alternative->thenExpression))) {
          return true;
        }
      }
      return containsAggregateExpression(caseExpr->getElseExpression());
    }
  }
  for (auto i = 0u; i < expression->getNumChildren(); ++i) {
    if (containsAggregateExpression(expression->getChild(i))) {
      return true;
    }
  }
  return false;
}

static bool sameProjectedOutput(const std::shared_ptr<Expression>& lhs,
                                const std::shared_ptr<Expression>& rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return lhs == rhs;
  }
  if (lhs->getUniqueName() == rhs->getUniqueName()) {
    return true;
  }
  if (lhs->hasAlias() && rhs->hasAlias() &&
      lhs->getAlias() == rhs->getAlias()) {
    return true;
  }
  return lhs->rawName() == rhs->rawName();
}

static bool mergeRewriteCreatedAggregatePart(
    NormalizedQueryPart& previousPart, NormalizedQueryPart& currentPart) {
  if (!previousPart.hasProjectionBody() || !currentPart.hasProjectionBody() ||
      currentPart.hasProjectionBodyPredicate()) {
    return false;
  }
  auto* previous = previousPart.getProjectionBodyUnsafe();
  auto* current = currentPart.getProjectionBodyUnsafe();
  // Moving a rewritten aggregate before DISTINCT changes its input
  // multiplicity. ORDER BY/LIMIT and a WITH predicate are also boundaries.
  if (previous->isDistinct() || previous->hasOrderByExpressions() ||
      !previous->getAggregateExpressions().empty() ||
      previous->hasSkipOrLimit() || previousPart.hasProjectionBodyPredicate() ||
      current->isDistinct() || current->hasOrderByExpressions() ||
      current->hasSkipOrLimit()) {
    return false;
  }

  auto previousProjections = previous->getProjectionExpressions();
  auto currentProjections = current->getProjectionExpressions();
  expression_vector projectionsToAppend;
  for (const auto& projection : currentProjections) {
    const auto alreadyProjected = std::any_of(
        previousProjections.begin(), previousProjections.end(),
        [&](const auto& existing) {
          return sameProjectedOutput(existing, projection);
        });
    if (alreadyProjected) {
      continue;
    }
    if (!containsAggregateExpression(projection)) {
      return false;
    }
    projectionsToAppend.push_back(projection);
  }
  if (projectionsToAppend.empty()) {
    return false;
  }

  auto groupBy = previous->getGroupByExpressions();
  if (groupBy.empty()) {
    for (const auto& projection : previousProjections) {
      if (!containsAggregateExpression(projection)) {
        groupBy.push_back(projection);
      }
    }
  }
  previousProjections.insert(previousProjections.end(),
                             projectionsToAppend.begin(),
                             projectionsToAppend.end());

  auto previousAggregates = previous->getAggregateExpressions();
  for (const auto& aggregate : current->getAggregateExpressions()) {
    const auto alreadyPresent = std::any_of(
        previousAggregates.begin(), previousAggregates.end(),
        [&](const auto& existing) {
          return sameProjectedOutput(existing, aggregate);
        });
    if (!alreadyPresent) {
      previousAggregates.push_back(aggregate);
    }
  }
  previous->setProjectionExpressions(std::move(previousProjections));
  previous->setGroupByExpressions(std::move(groupBy));
  previous->setAggregateExpressions(std::move(previousAggregates));
  return true;
}

static void collectAggregateExpressions(
    const std::shared_ptr<Expression>& expression,
    expression_vector& aggregates,
    std::unordered_set<std::string>& seenUniqueNames) {
  if (expression == nullptr) {
    return;
  }
  if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    if (seenUniqueNames.insert(expression->getUniqueName()).second) {
      aggregates.push_back(expression);
    }
    return;
  }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto* caseExpr = expression->ptrCast<CaseExpression>();
    if (caseExpr != nullptr) {
      for (auto i = 0u; i < caseExpr->getNumCaseAlternatives(); ++i) {
        const auto* alternative = caseExpr->getCaseAlternative(i);
        if (alternative == nullptr) {
          continue;
        }
        collectAggregateExpressions(alternative->whenExpression, aggregates,
                                    seenUniqueNames);
        collectAggregateExpressions(alternative->thenExpression, aggregates,
                                    seenUniqueNames);
      }
      collectAggregateExpressions(caseExpr->getElseExpression(), aggregates,
                                  seenUniqueNames);
    }
    return;
  }
  for (auto i = 0u; i < expression->getNumChildren(); ++i) {
    collectAggregateExpressions(expression->getChild(i), aggregates,
                                seenUniqueNames);
  }
}

static bool referencesPropertyOutsideAggregate(
    const std::shared_ptr<Expression>& expression) {
  if (expression == nullptr ||
      expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    return false;
  }
  if (expression->expressionType == ExpressionType::PROPERTY) {
    return true;
  }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto* caseExpr = expression->ptrCast<CaseExpression>();
    if (caseExpr != nullptr) {
      for (auto i = 0u; i < caseExpr->getNumCaseAlternatives(); ++i) {
        const auto* alternative = caseExpr->getCaseAlternative(i);
        if (alternative != nullptr &&
            (referencesPropertyOutsideAggregate(
                 alternative->whenExpression) ||
             referencesPropertyOutsideAggregate(
                 alternative->thenExpression))) {
          return true;
        }
      }
      return referencesPropertyOutsideAggregate(caseExpr->getElseExpression());
    }
  }
  for (auto i = 0u; i < expression->getNumChildren(); ++i) {
    if (referencesPropertyOutsideAggregate(expression->getChild(i))) {
      return true;
    }
  }
  return false;
}

// SUM over a finer, already rewritten integer SUM can consume the original
// input directly. Keep the outer aggregate identity: ORDER BY and subsequent
// WITH/RETURN clauses may still refer to it through a bound alias.
static bool collapseIntegerSumRollup(
    main::ClientContext* clientContext, BoundProjectionBody& reading,
    const BoundProjectionBody& rollup) {
  if (reading.isDistinct() || reading.hasOrderByExpressions() ||
      reading.hasSkipOrLimit() || rollup.isDistinct()) {
    return false;
  }
  const auto innerGroups = reading.getGroupByExpressions();
  const auto outerGroups = rollup.getGroupByExpressions();
  if (innerGroups.empty() || outerGroups.empty()) {
    return false;
  }
  for (const auto& group : outerGroups) {
    if (std::none_of(innerGroups.begin(), innerGroups.end(),
                     [&](const auto& inner) {
                       return sameProjectedOutput(group, inner);
                     })) {
      return false;
    }
  }
  const auto isIntegerSum = [](const std::shared_ptr<Expression>& expr) {
    if (expr == nullptr ||
        expr->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
        expr->getNumChildren() != 1 ||
        expr->getDataType().getLogicalTypeID() != LogicalTypeID::INT64) {
      return false;
    }
    const auto* aggregate = expr->constPtrCast<AggregateFunctionExpression>();
    return aggregate->getFunction().name == "SUM" && !aggregate->isDistinct();
  };
  const auto innerAggregates = reading.getAggregateExpressions();
  const auto outerAggregates = rollup.getAggregateExpressions();
  if (outerAggregates.empty()) {
    return false;
  }
  std::unordered_map<std::string, std::shared_ptr<Expression>> replacements;
  expression_vector collapsedAggregates;
  for (const auto& outer : outerAggregates) {
    if (!isIntegerSum(outer)) {
      return false;
    }
    const auto child = outer->getChild(0);
    const auto inner = std::find_if(
        innerAggregates.begin(), innerAggregates.end(),
        [&](const auto& candidate) {
          return sameProjectedOutput(child, candidate);
        });
    if (inner == innerAggregates.end() || !isIntegerSum(*inner)) {
      return false;
    }
    auto collapsed = makeValueAggregateExpression(
        clientContext, (*inner)->getChild(0), outer->getAlias(),
        outer->getParsedName(), (*inner)->getRawNameVariables());
    collapsed->setUniqueName(outer->getUniqueName());
    replacements.emplace(outer->getUniqueName(), collapsed);
    collapsedAggregates.push_back(std::move(collapsed));
  }
  auto projections = rollup.getProjectionExpressions();
  for (auto& projection : projections) {
    const auto replacement = replacements.find(projection->getUniqueName());
    if (replacement != replacements.end()) {
      projection = replacement->second;
    } else if (std::none_of(outerGroups.begin(), outerGroups.end(),
                            [&](const auto& group) {
                              return sameProjectedOutput(projection, group);
                            })) {
      // Keep compound projections and aggregates with other semantics on the
      // existing conservative path rather than partially changing the body.
      return false;
    }
  }
  reading.setProjectionExpressions(std::move(projections));
  reading.setGroupByExpressions(outerGroups);
  reading.setAggregateExpressions(std::move(collapsedAggregates));
  reading.setOrderByExpressions(rollup.getOrderByExpressions(),
                                rollup.getSortingOrders());
  reading.setSkipNumber(rollup.getSkipNumber());
  reading.setLimitNumber(rollup.getLimitNumber());
  return true;
}

static bool collapseAggregateRollupIntoReadingPart(
    main::ClientContext* clientContext, NormalizedQueryPart& readingPart,
    NormalizedQueryPart& rollupPart) {
  if (!readingPart.hasReadingClause() || rollupPart.hasReadingClause() ||
      readingPart.hasUpdatingClause() || rollupPart.hasUpdatingClause() ||
      !readingPart.hasProjectionBody() || !rollupPart.hasProjectionBody() ||
      readingPart.hasProjectionBodyPredicate() ||
      rollupPart.hasProjectionBodyPredicate()) {
    return false;
  }
  auto* reading = readingPart.getProjectionBodyUnsafe();
  auto* rollup = rollupPart.getProjectionBodyUnsafe();
  if (collapseIntegerSumRollup(clientContext, *reading, *rollup)) {
    return true;
  }
  if (reading->isDistinct() || reading->hasOrderByExpressions() ||
      reading->hasSkipOrLimit() || rollup->isDistinct() ||
      rollup->hasOrderByExpressions() || rollup->hasSkipOrLimit()) {
    return false;
  }

  const auto readingProjections = reading->getProjectionExpressions();
  auto collapsedProjections = rollup->getProjectionExpressions();
  const auto allowNestedReplacement = std::none_of(
      collapsedProjections.begin(), collapsedProjections.end(),
      referencesPropertyOutsideAggregate);
  bool replacedAggregateRollup = false;
  for (auto& projection : collapsedProjections) {
    if (containsAggregateExpression(projection) && projection->hasAlias()) {
      const auto replacement = std::find_if(
          readingProjections.begin(), readingProjections.end(),
          [&](const auto& candidate) {
            return candidate != nullptr && candidate->hasAlias() &&
                   candidate->getAlias() == projection->getAlias() &&
                   containsAggregateExpression(candidate);
          });
      if (replacement != readingProjections.end()) {
        projection = *replacement;
        replacedAggregateRollup = true;
      }
    }
    if (!allowNestedReplacement) {
      continue;
    }
    const ExpressionRewriteRule replaceAggregateAlias =
        [&](std::shared_ptr<Expression>& expression) -> bool {
      if (expression == nullptr || !expression->hasAlias() ||
          (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION &&
           expression->expressionType != ExpressionType::VARIABLE)) {
        return false;
      }
      const auto replacement = std::find_if(
          readingProjections.begin(), readingProjections.end(),
          [&](const auto& candidate) {
            return candidate != nullptr && candidate->hasAlias() &&
                   candidate->getAlias() == expression->getAlias() &&
                   containsAggregateExpression(candidate);
          });
      if (replacement == readingProjections.end()) {
        return false;
      }
      expression = *replacement;
      replacedAggregateRollup = true;
      return true;
    };
    rewriteExpressionTree(projection, replaceAggregateAlias);
  }
  if (!replacedAggregateRollup) {
    return false;
  }

  auto collapsedGroupBy = rollup->getGroupByExpressions();
  for (auto& groupBy : collapsedGroupBy) {
    const auto replacement = std::find_if(
        readingProjections.begin(), readingProjections.end(),
        [&](const auto& candidate) {
          return sameProjectedOutput(candidate, groupBy) &&
                 !containsAggregateExpression(candidate);
        });
    if (replacement != readingProjections.end()) {
      groupBy = *replacement;
    }
  }

  expression_vector collapsedAggregates;
  std::unordered_set<std::string> seenAggregateUniqueNames;
  for (const auto& projection : collapsedProjections) {
    collectAggregateExpressions(projection, collapsedAggregates,
                                seenAggregateUniqueNames);
  }
  if (collapsedAggregates.empty()) {
    return false;
  }

  reading->setProjectionExpressions(std::move(collapsedProjections));
  reading->setGroupByExpressions(std::move(collapsedGroupBy));
  reading->setAggregateExpressions(std::move(collapsedAggregates));
  return true;
}

// Once a FILL removes the reading clause, grouping again by all keys of
// the immediately preceding aggregate produces singleton groups. An integer
// SUM of the non-null FILL value can therefore be projected directly. Keep
// the first grouping: duplicate prefix paths must still multiply its counts.
static bool projectSingletonFillSums(BoundProjectionBody* current) {
  const auto aggregates = current->getAggregateExpressions();
  if (aggregates.empty()) {
    return false;
  }
  std::unordered_map<std::string, std::shared_ptr<Expression>> replacements;
  for (const auto& expression : aggregates) {
    if (expression == nullptr ||
        expression->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
        expression->getNumChildren() != 1 ||
        expression->getDataType().getLogicalTypeID() != LogicalTypeID::INT64) {
      return false;
    }
    const auto* aggregate = expression->constPtrCast<AggregateFunctionExpression>();
    const auto child = expression->getChild(0);
    if (aggregate->getFunction().name != "SUM" || aggregate->isDistinct() ||
        child->expressionType != ExpressionType::CASE_ELSE ||
        !child->getUniqueName().starts_with("idx_coalesce_zero(")) {
      return false;
    }
    auto replacement = deepCopyShared(child);
    replacement->setUniqueName(expression->getUniqueName());
    replacement->setParsedName(expression->getParsedName());
    if (expression->hasAlias()) {
      replacement->setAlias(expression->getAlias());
    }
    replacements.emplace(expression->getUniqueName(), std::move(replacement));
  }
  const ExpressionRewriteRule rule =
      [&](std::shared_ptr<Expression>& expression) -> bool {
    if (expression == nullptr) {
      return true;
    }
    const auto replacement = replacements.find(expression->getUniqueName());
    if (replacement == replacements.end()) {
      return false;
    }
    expression = deepCopyShared(replacement->second);
    return true;
  };
  auto projections = current->getProjectionExpressions();
  for (auto& expression : projections) {
    rewriteExpressionTree(expression, rule);
  }
  auto orderBy = current->getOrderByExpressions();
  for (auto& expression : orderBy) {
    rewriteExpressionTree(expression, rule);
  }
  current->setProjectionExpressions(std::move(projections));
  current->setOrderByExpressions(std::move(orderBy), current->getSortingOrders());
  current->setGroupByExpressions({});
  current->setAggregateExpressions({});
  LOG(INFO) << "Projected proven-singleton FILL SUM aggregation";
  return true;
}

static bool removeSingletonFillSums(
    const NormalizedQueryPart& previousPart, NormalizedQueryPart& currentPart) {
  if (!previousPart.hasProjectionBody() || !currentPart.hasProjectionBody() ||
      previousPart.hasUpdatingClause() || currentPart.hasUpdatingClause() ||
      currentPart.hasReadingClause() ||
      currentPart.hasProjectionBodyPredicate()) {
    return false;
  }
  const auto* previous = previousPart.getProjectionBody();
  auto* current = currentPart.getProjectionBodyUnsafe();
  if (!previous->hasAggregateExpressions() || current->isDistinct()) {
    return false;
  }
  const auto keys = previous->getGroupByExpressions();
  const auto projected = previous->getProjectionExpressions();
  const auto groups = current->getGroupByExpressions();
  if (keys.empty()) {
    return false;
  }
  const auto containsIdentity = [](const expression_vector& expressions,
                                   const std::shared_ptr<Expression>& key) {
    return key != nullptr && std::any_of(
        expressions.begin(), expressions.end(), [&](const auto& expression) {
          return expression != nullptr &&
                 expression->getUniqueName() == key->getUniqueName();
        });
  };
  for (const auto& key : keys) {
    if (!containsIdentity(projected, key) || !containsIdentity(groups, key)) {
      return false;
    }
  }
  return projectSingletonFillSums(current);
}


// A PK equality to a scalar parameter/literal proves at most one row. This
// is a semantic bound, never an estimated cardinality. Do not infer it from
// OR predicates, non-key properties, relationship patterns or multiple labels.
static bool isSingleBoundNodeScan(const BoundReadingClause& clause) {
  if (clause.getClauseType() != ClauseType::MATCH) return false;
  const auto& match = clause.constCast<BoundMatchClause>();
  if (match.getMatchClauseType() != MatchClauseType::MATCH) return false;
  const auto* graphs = match.getQueryGraphCollection();
  if (graphs->getNumQueryGraphs() != 1) return false;
  const auto* graph = graphs->getQueryGraph(0);
  if (graph->getNumQueryNodes() != 1 || graph->getNumQueryRels() != 0) return false;
  const auto node = graph->getQueryNode(common::idx_t{0});
  if (node->getNumEntries() != 1) return false;
  for (const auto& predicate : clause.getConjunctivePredicates()) {
    if (predicate->expressionType != ExpressionType::EQUALS ||
        predicate->getNumChildren() != 2) continue;
    for (common::idx_t side = 0; side < 2; ++side) {
      const auto key = predicate->getChild(side);
      const auto value = predicate->getChild(1 - side);
      if (key->expressionType != ExpressionType::PROPERTY ||
          (value->expressionType != ExpressionType::PARAMETER &&
           value->expressionType != ExpressionType::LITERAL)) continue;
      const auto* property = key->constPtrCast<PropertyExpression>();
      if (property->getVariableName() == node->getUniqueName() &&
          property->isPrimaryKey()) return true;
    }
  }
  return false;
}

static void removeBoundSingletonFillSums(NormalizedSingleQuery& query) {
  bool atMostOne = true;  // The initial query input is one empty tuple.
  for (common::idx_t i = 0; i < query.getNumQueryParts(); ++i) {
    auto* part = query.getQueryPartUnsafe(i);
    if (part->hasUpdatingClause()) { atMostOne = false; continue; }
    for (common::idx_t j = 0; j < part->getNumReadingClause(); ++j) {
      atMostOne = atMostOne && isSingleBoundNodeScan(*part->getReadingClause(j));
    }
    if (!part->hasProjectionBody()) continue;
    auto* body = part->getProjectionBodyUnsafe();
    // Grouped empty input remains empty when projected. Global SUM must stay:
    // its empty-input row would otherwise disappear. Only rewritten integer
    // FILL sums are accepted by projectSingletonFillSums.
    if (atMostOne && !body->getGroupByExpressions().empty() &&
        !body->isDistinct() && !part->hasProjectionBodyPredicate() &&
        projectSingletonFillSums(body)) {
      LOG(INFO) << "Projected FILL SUM on proven at-most-one PK-bound input";
    }
    if (!body->getAggregateExpressions().empty() &&
        body->getGroupByExpressions().empty()) atMostOne = true;
  }
}

// A first mandatory scan of one node produces one row per node identity.
// After an OPTIONAL FILL is replaced by its non-null count, a grouping that
// retains that identity is singleton. Do not apply to relationships, multiple
// patterns, OPTIONAL scans, updates, or global aggregates (including empty input).
static bool removeFirstUniqueScanFillSums(NormalizedQueryPart& part) {
  if (!part.hasProjectionBody() || part.hasUpdatingClause() ||
      part.getNumReadingClause() != 1 || part.hasProjectionBodyPredicate()) {
    return false;
  }
  const auto* clause = part.getReadingClause(0);
  if (clause->getClauseType() != ClauseType::MATCH) return false;
  const auto* match = clause->constPtrCast<BoundMatchClause>();
  if (match->getMatchClauseType() != MatchClauseType::MATCH) return false;
  const auto* graphs = match->getQueryGraphCollection();
  if (graphs->getNumQueryGraphs() != 1) return false;
  const auto* graph = graphs->getQueryGraph(0);
  if (graph->getNumQueryNodes() != 1 || graph->getNumQueryRels() != 0) return false;
  auto* body = part.getProjectionBodyUnsafe();
  if (body->isDistinct()) return false;
  const auto groups = body->getGroupByExpressions();
  const auto node = graph->getQueryNode(common::idx_t{0});
  if (groups.size() != 1 || groups[0] == nullptr ||
      groups[0]->getUniqueName() != node->getUniqueName()) return false;
  return projectSingletonFillSums(body);
}

// The initial mandatory scan has proved one row per node. Inline only pure
// FILL null-to-zero projections into the final mandatory MATCH of that node.
// CASE WHEN coalesce(p,0)<>0 THEN coalesce(p,0) ELSE fallback END
// equals CASE WHEN p<>0 THEN p ELSE fallback END, including NULL p.
static bool simplifyNonzeroFillCase(std::shared_ptr<Expression>& e) {
  if (!e || e->expressionType!=ExpressionType::CASE_ELSE) return false;
  auto* outer=e->ptrCast<CaseExpression>();
  if (outer->getNumCaseAlternatives()!=1) return false;
  auto* alt=outer->getCaseAlternative(0);
  auto& predicate=alt->whenExpression;
  if (predicate->expressionType!=ExpressionType::NOT_EQUALS || predicate->getNumChildren()!=2) return false;
  const auto input=predicate->getChild(0);const auto zero=predicate->getChild(1);
  if(input->expressionType!=ExpressionType::CASE_ELSE || zero->expressionType!=ExpressionType::LITERAL ||
     zero->constPtrCast<LiteralExpression>()->isNull() || zero->constPtrCast<LiteralExpression>()->getValue().toString()!="0" ||
     input->getUniqueName()!=alt->thenExpression->getUniqueName()) return false;
  const auto* coalesce=input->constPtrCast<CaseExpression>();
  if(coalesce->getNumCaseAlternatives()!=1 || coalesce->getElseExpression()->expressionType!=ExpressionType::PROPERTY) return false;
  const auto* ca=coalesce->getCaseAlternative(0);
  if(ca->whenExpression->expressionType!=ExpressionType::IS_NULL || ca->whenExpression->getNumChildren()!=1 ||
     ca->whenExpression->getChild(0)->getUniqueName()!=coalesce->getElseExpression()->getUniqueName() ||
     ca->thenExpression->expressionType!=ExpressionType::LITERAL || ca->thenExpression->constPtrCast<LiteralExpression>()->isNull() ||
     ca->thenExpression->constPtrCast<LiteralExpression>()->getValue().toString()!="0") return false;
  predicate->setChild(0,deepCopyShared(coalesce->getElseExpression()));
  alt->thenExpression=deepCopyShared(coalesce->getElseExpression());
  LOG(INFO)<<"Simplified null-safe nonzero FILL count CASE";
  return true;
}

static bool inlineUniqueScanFillProjections(NormalizedSingleQuery& query) {
  const auto count=query.getNumQueryParts();
  if(count<2) return false;
  const auto* first=query.getQueryPart(0);
  if(first->getNumReadingClause()!=1 || first->getReadingClause(0)->hasPredicate()) return false;
  const auto* fm=first->getReadingClause(0)->constPtrCast<BoundMatchClause>();
  const auto anchor=fm->getQueryGraphCollection()->getQueryGraph(0)->getQueryNode(common::idx_t{0});
  auto* last=query.getQueryPartUnsafe(count-1);
  if(last->hasUpdatingClause() || last->hasProjectionBodyPredicate() || last->getNumReadingClause()!=1 || !last->hasProjectionBody()) return false;
  const auto* reading=last->getReadingClause(0);
  if(reading->getClauseType()!=ClauseType::MATCH || reading->hasPredicate()) return false;
  const auto* match=reading->constPtrCast<BoundMatchClause>();
  if(match->getMatchClauseType()!=MatchClauseType::MATCH) return false;
  const auto nodes=match->getQueryGraphCollection()->getQueryNodes();
  if(std::none_of(nodes.begin(),nodes.end(),[&](const auto& n){return n->getUniqueName()==anchor->getUniqueName();})) return false;
  std::unordered_map<std::string,std::shared_ptr<Expression>> replacements;
  const ExpressionRewriteRule rule=[&](std::shared_ptr<Expression>& e){
    if(!e) return true;
    const auto it=replacements.find(e->getUniqueName());
    if(it==replacements.end()) return false;
    e=deepCopyShared(it->second);return true;
  };
  for(common::idx_t i=0;i+1<count;++i){
    const auto* part=query.getQueryPart(i);
    if(!part->hasProjectionBody() || part->hasUpdatingClause() || part->hasProjectionBodyPredicate() || (i>0 && part->hasReadingClause())) return false;
    const auto* body=part->getProjectionBody();
    if(body->hasAggregateExpressions() || body->isDistinct() || body->hasOrderByExpressions() || body->hasSkipOrLimit()) return false;
    for(const auto& raw:body->getProjectionExpressions()){
      if(ExpressionUtil::isNodePattern(*raw)){
        if(raw->getUniqueName()!=anchor->getUniqueName()) return false;
        continue;
      }
      auto e=deepCopyShared(raw);rewriteExpressionTree(e,rule);
      if(e->expressionType!=ExpressionType::CASE_ELSE) return false;
      const auto* ce=e->constPtrCast<CaseExpression>();
      if(ce->getNumCaseAlternatives()!=1 || ce->getElseExpression()->expressionType!=ExpressionType::PROPERTY) return false;
      const auto* alt=ce->getCaseAlternative(0);
      if(alt->whenExpression->expressionType!=ExpressionType::IS_NULL || alt->whenExpression->getNumChildren()!=1 ||
         alt->whenExpression->getChild(0)->getUniqueName()!=ce->getElseExpression()->getUniqueName() ||
         alt->thenExpression->expressionType!=ExpressionType::LITERAL || alt->thenExpression->constPtrCast<LiteralExpression>()->isNull() ||
         alt->thenExpression->constPtrCast<LiteralExpression>()->getValue().toString()!="0") return false;
      replacements[raw->getUniqueName()]=std::move(e);
    }
  }
  auto* body=last->getProjectionBodyUnsafe();
  if(body->isDistinct() || body->hasSkipOrLimit() || body->hasOrderByExpressions() || !body->getGroupByExpressions().empty()) return false;
  auto projections=body->getProjectionExpressions();auto aggregates=body->getAggregateExpressions();
  for(auto& e:projections) rewriteExpressionTree(e,rule);
  for(auto& e:aggregates) rewriteExpressionTree(e,rule);
  for(auto& e:projections) rewriteExpressionTree(e,simplifyNonzeroFillCase);
  for(auto& e:aggregates) rewriteExpressionTree(e,simplifyNonzeroFillCase);
  body->setProjectionExpressions(std::move(projections));body->setAggregateExpressions(std::move(aggregates));
  for(common::idx_t i=0;i+1<count;++i) query.removeQueryPart(0);
  LOG(INFO)<<"Inlined deterministic FILL counts across unique-node projections";
  return true;
}

// A conditional outer SUM is linear in groupedWeight. Distribute the inner
// SUM over the following mandatory join. No FILL edge uniqueness is assumed.
static bool removeLinearConditionalFillRollup(NormalizedSingleQuery& query){
  if(query.getNumQueryParts()!=2) return false;
  auto* first=query.getQueryPartUnsafe(0);const auto* last=query.getQueryPart(1);
  for(const auto* part:{static_cast<const NormalizedQueryPart*>(first),last}){
    if(!part->hasProjectionBody() || part->hasUpdatingClause() || part->hasProjectionBodyPredicate() || part->getNumReadingClause()!=1) return false;
    const auto* body=part->getProjectionBody();const auto* clause=part->getReadingClause(0);
    if(body->isDistinct() || body->hasSkipOrLimit() || body->hasOrderByExpressions() || clause->hasPredicate() ||
       clause->getClauseType()!=ClauseType::MATCH || clause->constPtrCast<BoundMatchClause>()->getMatchClauseType()!=MatchClauseType::MATCH) return false;
  }
  const auto* gc=first->getReadingClause(0)->constPtrCast<BoundMatchClause>()->getQueryGraphCollection();
  if(gc->getNumQueryGraphs()!=1) return false;
  const auto* graph=gc->getQueryGraph(0);
  if(graph->getNumQueryNodes()!=2 || graph->getNumQueryRels()!=1) return false;
  auto* fb=first->getProjectionBodyUnsafe();const auto groups=fb->getGroupByExpressions();
  if(groups.size()!=2) return false;
  for(const auto& node:graph->getQueryNodes()){
    if(std::none_of(groups.begin(),groups.end(),[&](const auto& e){return e && e->getUniqueName()==node->getUniqueName();})) return false;
  }
  const auto weights=fb->getAggregateExpressions();const auto* finalBody=last->getProjectionBody();const auto finals=finalBody->getAggregateExpressions();
  if(weights.size()!=1 || finals.size()!=1 || !finalBody->getGroupByExpressions().empty() || finalBody->getProjectionExpressions().size()!=1) return false;
  const auto* sum=finals[0]->constPtrCast<AggregateFunctionExpression>();
  if(sum->getFunction().name!="SUM" || sum->isDistinct() || sum->getNumChildren()!=1) return false;
  const auto child=sum->getChild(0);
  if(child->expressionType!=ExpressionType::CASE_ELSE) return false;
  const auto* ce=child->constPtrCast<CaseExpression>();
  if(ce->getNumCaseAlternatives()!=1 || ce->getElseExpression()->expressionType!=ExpressionType::LITERAL) return false;
  const auto* zero=ce->getElseExpression()->constPtrCast<LiteralExpression>();
  if(zero->isNull() || zero->getValue().toString()!="0") return false;
  const auto* alt=ce->getCaseAlternative(0);
  if(alt->thenExpression->getUniqueName()!=weights[0]->getUniqueName() || alt->whenExpression->expressionType!=ExpressionType::NOT_EQUALS || alt->whenExpression->getNumChildren()!=2) return false;
  for(common::idx_t i=0;i<2;++i) if(!ExpressionUtil::isNodePattern(*alt->whenExpression->getChild(i)) && alt->whenExpression->getChild(i)->expressionType!=ExpressionType::PROPERTY) return false;
  if(!projectSingletonFillSums(fb)) return false;
  // Only the final linear SUM consumes this value. SUM ignores NULL, so
  // property NULL and zero contribute equally; avoid evaluating coalesce for
  // every suffix row. No predicate or non-linear use of weight is permitted.
  auto projections=fb->getProjectionExpressions();
  for(auto& e:projections) {
    if(e->getUniqueName()!=weights[0]->getUniqueName()) continue;
    const auto property=e->constPtrCast<CaseExpression>()->getElseExpression();
    auto replacement=deepCopyShared(property);
    replacement->setUniqueName(e->getUniqueName());replacement->setParsedName(e->getParsedName());
    if(e->hasAlias()) replacement->setAlias(e->getAlias());
    e=std::move(replacement);
  }
  fb->setProjectionExpressions(std::move(projections));
  LOG(INFO)<<"Distributed linear conditional FILL rollup across mandatory MATCH";
  return true;
}

// A complete-group materialization is a scalar lookup on one typed graph key.
// Discover it from the registered source and the original bound query; no
// generation-time Usage, binding map or rewritten query is supplied here.
bool RewriterV4::tryRewriteCompleteAggregate(NormalizedSingleQuery& query) {
  // This whole-query rule remains a candidate via the independently bound
  // legacy route; it does not participate in local-step replay.
  if (binder != nullptr && binder->hasIndexRewriteReplay()) return false;
  if (!completeAggregateInputIsClosed || query.getNumQueryParts() != 1) return false;
  auto* part = query.getQueryPartUnsafe(0);
  const auto closedGraph = [](const NormalizedQueryPart& p) -> const QueryGraph* {
    if (p.hasUpdatingClause() || !p.hasProjectionBody() || p.getNumReadingClause() != 1)
      return nullptr;
    const auto* clause = p.getReadingClause(0);
    if (clause->getClauseType() != ClauseType::MATCH) return nullptr;
    const auto* match = clause->constPtrCast<BoundMatchClause>();
    if (match->getMatchClauseType() != MatchClauseType::MATCH || match->hasHint() ||
        match->getQueryGraphCollection()->getNumQueryGraphs() != 1) return nullptr;
    const auto* graph = match->getQueryGraphCollection()->getQueryGraph(0);
    if (graph->getNumQueryRels() == 0) return nullptr;
    for (const auto& rel : graph->getQueryRels()) if (rel->isRecursive()) return nullptr;
    return graph;
  };
  const auto* graph = closedGraph(*part);
  if (graph == nullptr || !part->getProjectionBody()->hasAggregateExpressions()) return false;
  const auto queryShape = index::extractStandaloneQueryPart(*part, {});
  if (!queryShape.unsupportedReason.empty()) return false;
  auto queryIR = convertGraphPatternToIR(*graph);
  auto* catalog = clientContext->getCatalog();
  auto entries = catalog->getExtendedIndexEntries(clientContext->getTransaction());
  filterAllowedRewriteTargets(entries);
  std::sort(entries.begin(), entries.end(), [](const auto* a, const auto* b) {
    return a->getName() < b->getName();
  });
  for (auto* entry : entries) {
    const auto& info = entry->getAuxInfo().cast<FillIndexAuxInfo>();
    if (catalog->isExtendedIndexBuilding(entry->getName()) ||
        info.getFillType() != FillEntryType::NODE || !info.usesWorkloadCompatibility() ||
        info.getSourceQuery().empty()) continue;
    // Unsupported or stale source metadata may only decline a candidate.
    std::unique_ptr<BoundRegularQuery> source;
    try {
      const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
      if (parsed.size() != 1) continue;
      Binder sourceBinder(clientContext);
      // Reuse the same MATCH normalization, label pruning and type solving as
      // the original query and FILL registration, without index substitution.
      sourceBinder.setAllowIndexRewrite(false);
      auto bound = sourceBinder.bind(*parsed[0]);
      if (bound->getStatementType() != StatementType::QUERY) continue;
      source.reset(static_cast<BoundRegularQuery*>(bound.release()));
    } catch (const std::exception&) { continue; }
    if (!source->getPreQueryPart().empty() || source->getPostSingleQuery() != nullptr ||
        source->getNumSingleQueries() != 1 || source->getSingleQuery(0)->getNumQueryParts() != 1) continue;
    const auto* sourcePart = source->getSingleQuery(0)->getQueryPart(0);
    const auto* sourceGraph = closedGraph(*sourcePart);
    if (sourceGraph == nullptr) continue;
    const auto* sourceBody = sourcePart->getProjectionBody();
    if (sourcePart->hasProjectionBodyPredicate() ||
        sourceBody->hasSkipOrLimit() || sourceBody->hasOrderByExpressions() ||
        sourceBody->isDistinct()) continue;
    const auto sourceShape = index::extractStandaloneQueryPart(*sourcePart, {});
    if (!sourceShape.unsupportedReason.empty() || sourceShape.outputs.size() != info.getTargetColumns().size() + 1 ||
        sourceShape.nodes.size() != queryShape.nodes.size() ||
        sourceShape.relationships.size() != queryShape.relationships.size()) continue;
    std::function<bool(const index::Expression&)> stable = [&](const auto& e) {
      return e.deterministic && e.safeToPrecompute && e.kind != index::ExpressionKind::Parameter &&
          e.kind != index::ExpressionKind::ResidualPredicate &&
          std::all_of(e.children.begin(), e.children.end(), stable);
    };
    if (!std::all_of(sourceShape.outputs.begin(), sourceShape.outputs.end(), stable) ||
        !std::all_of(sourceShape.predicates.begin(), sourceShape.predicates.end(), stable)) continue;
    const auto& key = sourceShape.outputs[0];
    const auto anchor = std::find_if(sourceShape.nodes.begin(), sourceShape.nodes.end(), [&](const auto& n) {
      return key.kind == index::ExpressionKind::Property && key.binding == n.binding &&
          n.labels == std::vector<std::string>{info.getTableName()} &&
          !n.primaryKey.empty() && key.name == n.primaryKey && key.type == n.primaryKeyType;
    });
    if (anchor == sourceShape.nodes.end()) continue;
    const auto exactGrouping = [](const index::QueryPart& p, const std::string& binding) {
      const auto node = std::find_if(p.nodes.begin(), p.nodes.end(), [&](const auto& n) { return n.binding == binding; });
      if (node == p.nodes.end() || node->labels.size() != 1) return false;
      bool identity = false;
      for (const auto& e : p.groupBy) {
        if (e.kind == index::ExpressionKind::Literal) continue;
        if (e.binding != binding ||
            !(e.kind == index::ExpressionKind::Variable ||
              (e.kind == index::ExpressionKind::Property && !node->primaryKey.empty() &&
               e.name == node->primaryKey && e.type == node->primaryKeyType))) return false;
        identity = true;
      }
      return identity;
    };
    if (!exactGrouping(sourceShape, anchor->binding)) continue;
    auto* table = tableByName(catalog, clientContext->getTransaction(), info.getTableName());
    if (table == nullptr) continue;
    const auto sourceOutputs = sourceBody->getProjectionExpressions();
    std::optional<size_t> presence;
    bool typesMatch = sourceOutputs.size() == sourceShape.outputs.size();
    for (size_t i = 0; typesMatch && i < info.getTargetColumns().size(); ++i) {
      const auto& name = info.getTargetColumns()[i];
      const auto& expression = sourceOutputs[i + 1];
      typesMatch = table->containsProperty(name) && table->getProperty(name).getType() == expression->getDataType();
      if (expression->expressionType == ExpressionType::LITERAL &&
          expression->getDataType().getLogicalTypeID() == LogicalTypeID::BOOL) {
        const auto& literal = expression->constCast<LiteralExpression>();
        if (!literal.isNull() && literal.getValue().getValue<bool>()) presence = i;
      }
    }
    if (!typesMatch || !presence.has_value()) continue;
    auto sourceIR = convertGraphPatternToIR(*sourceGraph);
    std::shared_ptr<NodeExpression> queryAnchor;
    std::unordered_map<std::string, std::shared_ptr<Expression>> replacements;
    auto [matched, ignored] = PatternIR::findMatch(queryIR, sourceIR, entry->getName(), [&]() {
      std::unordered_map<std::string, std::string> names;
      for (const auto& n : sourceIR.nodesInfo) {
        if (n.matchedIdx >= queryIR.nodesInfo.size()) return false;
        names.emplace(queryIR.nodesInfo[n.matchedIdx].uniqName, n.uniqName);
      }
      for (const auto& r : sourceIR.relsInfo) {
        if (r.matchedIdx >= queryIR.relsInfo.size()) return false;
        // General subgraph matching allows an undirected query to use a
        // directed edge. A complete group must cover precisely the same rows.
        if (queryIR.relsInfo[r.matchedIdx].edgeType != r.edgeType) return false;
        names.emplace(queryIR.relsInfo[r.matchedIdx].uniqName, r.uniqName);
      }
      queryAnchor.reset();
      for (const auto& n : graph->getQueryNodes()) {
        const auto found = names.find(n->getUniqueName());
        if (found != names.end() && found->second == anchor->binding) queryAnchor = n;
      }
      if (queryAnchor == nullptr || !exactGrouping(queryShape, queryAnchor->getUniqueName())) return false;
      std::function<index::Expression(index::Expression)> rename = [&](auto e) {
        if (const auto found = names.find(e.binding); found != names.end()) e.binding = found->second;
        for (auto& child : e.children) child = rename(std::move(child));
        return e;
      };
      const auto predicateKeys = [&](const std::vector<index::Expression>& expressions, bool mapped) {
        std::set<std::string> keys;
        std::function<void(const index::Expression&)> add = [&](const auto& e) {
          if (e.kind == index::ExpressionKind::Call && e.name == "AND") {
            for (const auto& child : e.children) add(child);
          } else { keys.insert((mapped ? rename(e) : e).structuralKey()); }
        };
        for (const auto& e : expressions) add(e);
        return keys;
      };
      if (predicateKeys(queryShape.predicates, true) != predicateKeys(sourceShape.predicates, false)) return false;
      const auto aggregates = part->getProjectionBody()->getAggregateExpressions();
      if (aggregates.size() != queryShape.aggregates.size()) return false;
      replacements.clear();
      for (size_t i = 0; i < aggregates.size(); ++i) {
        const auto signature = rename(queryShape.aggregates[i]).structuralKey();
        size_t column = 1;
        for (; column < sourceShape.outputs.size(); ++column)
          if (sourceShape.outputs[column].kind == index::ExpressionKind::Aggregate &&
              sourceShape.outputs[column].structuralKey() == signature) break;
        if (column == sourceShape.outputs.size()) return false;
        auto value = makeFillPropertyExpressionImpl(table, table->getTableID(), queryAnchor->getUniqueName(),
            queryAnchor->getVariableName(), info.getTargetColumns()[column - 1]);
        value->setUniqueName(aggregates[i]->getUniqueName());
        value->setParsedName(aggregates[i]->getParsedName());
        if (aggregates[i]->hasAlias()) value->setAlias(aggregates[i]->getAlias());
        replacements.emplace(aggregates[i]->getUniqueName(), std::move(value));
      }
      std::function<bool(const index::Expression&)> retained = [&](const auto& e) {
        if (e.kind == index::ExpressionKind::Aggregate) return true;
        if (!e.binding.empty() && e.binding != queryAnchor->getUniqueName()) return false;
        return std::all_of(e.children.begin(), e.children.end(), retained);
      };
      return std::all_of(queryShape.outputs.begin(), queryShape.outputs.end(), retained) &&
          std::all_of(queryShape.postExpressions.begin(), queryShape.postExpressions.end(), retained);
    });
    (void)ignored;
    if (!matched) continue;
    // Commit only after every shape, type, grouping and presence check passed.
    auto body = part->getProjectionBody()->copy();
    const ExpressionRewriteRule substitute = [&](auto& e) {
      const auto found = replacements.find(e->getUniqueName());
      if (found == replacements.end()) return false;
      e = deepCopyShared(found->second);
      return true;
    };
    const auto replace = [&](expression_vector values) {
      // Bound node and aggregate identities are shared with result metadata;
      // they do not implement Expression::copy(). Mutate references only after
      // the full proof, as in the other production expression rewrites.
      for (auto& value : values) rewriteExpressionTree(value, substitute);
      return values;
    };
    body.setProjectionExpressions(replace(body.getProjectionExpressions()));
    body.setOrderByExpressions(replace(body.getOrderByExpressions()), body.getSortingOrders());
    body.setAggregateExpressions({});
    body.setGroupByExpressions({});
    auto postPredicate = part->getProjectionBodyPredicate();
    rewriteExpressionTree(postPredicate, substitute);
    QueryGraph carrier;
    carrier.addQueryNode(queryAnchor);
    QueryGraphCollection collection;
    collection.addAndMergeQueryGraphIfConnected(std::move(carrier));
    auto match = std::make_unique<BoundMatchClause>(std::move(collection), MatchClauseType::MATCH);
    match->setPredicate(makeFillPropertyExpressionImpl(table, table->getTableID(), queryAnchor->getUniqueName(),
        queryAnchor->getVariableName(), info.getTargetColumns()[*presence]));
    part->removeReadingClause(0);
    part->addReadingClause(std::move(match));
    part->setProjectionBody(std::move(body));
    part->setProjectionBodyPredicate(std::move(postPredicate));
    LOG(INFO) << "Successfully applied complete aggregate index read: " << entry->getName();
    return true;
  }
  return false;
}

// --- Entry point ---

void RewriterV4::visitRegularQueryUnsafe(BoundStatement& statement) {
  nodePresenceSetSourceProofs.clear();
  endpointSetSourceProofs.clear();
  const auto& query = statement.cast<BoundRegularQuery>();
  // A CALL/UNION wrapper can feed rows into an apparently standalone branch.
  // Until that input is represented in the proof, do not treat it as closed.
  completeAggregateInputIsClosed = query.getPreQueryPart().empty() &&
                                  query.getPostSingleQuery() == nullptr;
  BoundStatementVisitor::visitRegularQueryUnsafe(statement);
  completeAggregateInputIsClosed = false;
}

void RewriterV4::enumerateGlobalCountPresenceVariants(NormalizedSingleQuery& query) {
  if (!globalCountProductProof || globalCountProductProof->factors.empty() ||
      query.getNumQueryParts() != 1 || binder == nullptr) return;
  auto* part = query.getQueryPartUnsafe(0);
  if (!part->hasProjectionBody() || part->hasProjectionBodyPredicate()) return;
  auto* body = part->getProjectionBodyUnsafe();
  if (body->isDistinct() || body->hasOrderByExpressions() || body->hasSkipOrLimit() ||
      !body->getGroupByExpressions().empty() || body->getProjectionExpressions().size() != 1 ||
      body->getAggregateExpressions().size() != 1) return;
  auto aggregate = body->getAggregateExpressions()[0];
  auto projection = body->getProjectionExpressions()[0];
  if (!aggregate || !projection || aggregate->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
      aggregate->getUniqueName() != globalCountProductProof->aggregateUniqueName ||
      projection->getUniqueName() != aggregate->getUniqueName() ||
      aggregate->constCast<AggregateFunctionExpression>().getFunction().name != "SUM" ||
      aggregate->constCast<AggregateFunctionExpression>().isDistinct() || aggregate->getNumChildren() != 1 ||
      !matchesCountProduct(aggregate->getChild(0), globalCountProductProof->factors)) return;

  // The origin is COUNT(*), so even a completely empty carrier must produce
  // zero. Keep this output guard in every physical alternative, including the
  // original presence subset; do not trade correctness for its small cost.
  // Both references address the same aggregate accumulator. Aggregate bound
  // expressions deliberately have no generic copy(), unlike property leaves.
  auto isNull = binder->getExpressionBinder()->bindNullOperatorExpression(
      ExpressionType::IS_NULL, expression_vector{aggregate});
  auto guarded = std::make_shared<CaseExpression>(aggregate->getDataType().copy(),
      aggregate, "idx_coalesce_zero(" + aggregate->getUniqueName() + ")");
  guarded->addCaseAlternative(std::move(isNull),
      std::make_shared<LiteralExpression>(Value(int64_t{0}), "0"));
  guarded->setParsedName(projection->getParsedName());
  guarded->setRawNameVariables(projection->getRawNameVariables());
  if (projection->hasAlias()) guarded->setAlias(projection->getAlias());
  // The output alias belongs to the CASE, not its child accumulator. Sharing
  // it would let GExprConverter resolve this new CASE as an already computed
  // child column by alias and silently bypass the null-to-zero expression.
  // Keep the aggregate unique identity, which both CASE references address.
  aggregate->setAlias("");
  body->setProjectionExpressions(expression_vector{std::move(guarded)});

  // Three factors can overflow an intermediate product before a later zero.
  // No presence-subset alternatives are admitted without that extra proof.
  if (globalCountProductProof->factors.size() > 2 || !binder->hasIndexRewriteReplay()) return;
  expression_vector factors;
  for (const auto& factor : globalCountProductProof->factors)
    if (std::none_of(factors.begin(), factors.end(), [&](const auto& existing) {
          return sameCountProperty(existing, factor);
        })) factors.push_back(factor);
  std::vector<BoundMatchClause*> matches, anchors(factors.size(), nullptr);
  for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
    auto* clause = part->getReadingClause(ci);
    if (clause->getClauseType() != ClauseType::MATCH) return;
    auto* match = &clause->cast<BoundMatchClause>();
    if (match->getMatchClauseType() != MatchClauseType::MATCH) return;
    matches.push_back(match);
    for (size_t i = 0; i < factors.size(); ++i) {
      const auto& owner = factors[i]->constCast<PropertyExpression>().getVariableName();
      const auto* collection = match->getQueryGraphCollection();
      for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi)
        if (collection->getQueryGraph(gi)->containsQueryNode(owner)) anchors[i] = match;
    }
  }
  if (std::any_of(anchors.begin(), anchors.end(), [](auto* anchor) { return anchor == nullptr; })) return;
  uint32_t existingMask = 0;
  std::function<void(const std::shared_ptr<Expression>&)> inspect = [&](const auto& predicate) {
    if (!predicate) return;
    if (globalCountProductProof->introducedPresencePredicates.contains(predicate->getUniqueName()) &&
        predicate->expressionType == ExpressionType::NOT_EQUALS && predicate->getNumChildren() == 2) {
      for (size_t i = 0; i < factors.size(); ++i)
        if (sameCountProperty(predicate->getChild(0), factors[i]) ||
            sameCountProperty(predicate->getChild(1), factors[i])) existingMask |= 1u << i;
    }
    if (predicate->expressionType == ExpressionType::AND)
      for (const auto& child : predicate->getChildren()) inspect(child);
  };
  for (const auto* match : matches) inspect(match->getPredicate());
  std::string identity = "global_count_presence/" + globalCountProductProof->aggregateUniqueName;
  for (const auto& factor : factors)
    identity += "/" + std::to_string(factor->getUniqueName().size()) + ":" + factor->getUniqueName();
  std::optional<uint32_t> selectedMask;
  for (uint32_t mask = 0; mask < (1u << factors.size()); ++mask) {
    if (mask == existingMask) continue;
    if (binder->acceptIndexRewriteChoice(identity + "/" + std::to_string(mask))) {
      selectedMask = mask;
      break;
    }
  }
  if (!selectedMask) return;
  std::function<std::shared_ptr<Expression>(const std::shared_ptr<Expression>&)> removeIntroduced =
      [&](const auto& predicate) -> std::shared_ptr<Expression> {
    if (!predicate || globalCountProductProof->introducedPresencePredicates.contains(predicate->getUniqueName()))
      return nullptr;
    if (predicate->expressionType != ExpressionType::AND) return predicate;
    std::shared_ptr<Expression> result;
    for (const auto& child : predicate->getChildren())
      result = binder->getExpressionBinder()->combineBooleanExpressions(
          ExpressionType::AND, result, removeIntroduced(child));
    return result;
  };
  for (auto* match : matches) match->setPredicate(removeIntroduced(match->getPredicate()));
  for (size_t i = 0; i < factors.size(); ++i) {
    if (!(*selectedMask & (1u << i))) continue;
    auto nonzero = binder->getExpressionBinder()->bindComparisonExpression(ExpressionType::NOT_EQUALS,
        expression_vector{deepCopyShared(factors[i]), std::make_shared<LiteralExpression>(Value(int64_t{0}), "0")});
    anchors[i]->setPredicate(binder->getExpressionBinder()->combineBooleanExpressions(
        ExpressionType::AND, anchors[i]->getPredicate(), std::move(nonzero)));
  }
}

void RewriterV4::visitSingleQueryUnsafe(NormalizedSingleQuery& query) {
  committedMatchRewrites = 0;
  globalCountProductProof.reset();
  aggregateGroups.clear();
  relLabels.clear();
  if (binder == nullptr) {
    return;
  }
  if (!binder->allowIndexRewrite()) {
    return;
  }
  if (clientContext->getCatalog()
          ->getExtendedIndexEntries(clientContext->getTransaction())
          .empty()) {
    return;
  }
  for (auto i = 0u; i < query.getNumQueryParts(); i++) {
    if (query.getQueryPartUnsafe(i)->hasUpdatingClause()) {
      return;
    }
  }

  if (completeAggregateInputIsClosed && query.getNumQueryParts() == 1) {
    const auto* part = query.getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    bool eligible = body && !part->hasProjectionBodyPredicate() && !body->isDistinct() &&
        !body->hasOrderByExpressions() && !body->hasSkipOrLimit() &&
        body->getGroupByExpressions().empty() && body->getProjectionExpressions().size() == 1 &&
        body->getAggregateExpressions().size() == 1;
    for (uint32_t ci = 0; eligible && ci < part->getNumReadingClause(); ++ci) {
      const auto* clause = part->getReadingClause(ci);
      eligible = clause->getClauseType() == ClauseType::MATCH &&
          clause->constCast<BoundMatchClause>().getMatchClauseType() == MatchClauseType::MATCH &&
          !clause->constCast<BoundMatchClause>().hasPredicate();
    }
    if (eligible) {
      const auto aggregate = body->getAggregateExpressions()[0];
      const auto projection = body->getProjectionExpressions()[0];
      if (aggregate && projection && aggregate->expressionType == ExpressionType::AGGREGATE_FUNCTION &&
          aggregate->constCast<AggregateFunctionExpression>().getFunction().name == "COUNT_STAR" &&
          !aggregate->constCast<AggregateFunctionExpression>().isDistinct() &&
          aggregate->getUniqueName() == projection->getUniqueName())
        globalCountProductProof = GlobalCountProductProof{aggregate->getUniqueName(), {}, {}};
    }
  }

  if (tryRewriteCompleteAggregate(query)) return;

  // Resolve physical relationship labels once within the timed visit.
  for (auto* group : clientContext->getCatalog()->getRelGroupEntries(
           clientContext->getTransaction())) {
    for (const auto tableID : group->getRelTableIDs()) {
      relLabels.emplace(tableID, group->getName());
    }
  }

  // A merged PatternIR must not erase DISTINCT boundaries independently.
  // Match a complete DISTINCT/aggregate prefix, including recursion bounds
  // and recursive predicates, and commit only after the normal projection
  // and predicate coverage checks accept its FILL replacement.
  auto distinctEntries = clientContext->getCatalog()->getExtendedIndexEntries(
      clientContext->getTransaction());
  filterAllowedRewriteTargets(distinctEntries);
  std::erase_if(distinctEntries, [&](const auto* entry) {
    return clientContext->getCatalog()->isExtendedIndexBuilding(entry->getName());
  });
  for (auto* entry : distinctEntries) {
    if (binder->hasIndexRewriteReplay()) break;
    auto& info = entry->getAuxInfoUnsafe().cast<catalog::FillIndexAuxInfo>();
    if (info.getFillType() != FillEntryType::NODE || !hasDistinctFillShape(info)) continue;
    for (uint32_t count = 2; count <= query.getNumQueryParts(); ++count) {
      const auto* last = query.getQueryPart(count - 1);
      if (!last->hasProjectionBody() || last->hasReadingClause() ||
          !last->getProjectionBody()->hasAggregateExpressions()) continue;
      QueryGraph graph;
      std::shared_ptr<Expression> predicate;
      bool eligible = true;
      for (uint32_t pi = 0; pi + 1 < count && eligible; ++pi) {
        const auto* part = query.getQueryPart(pi);
        if (part->getNumReadingClause() != 1 ||
            part->getReadingClause(0)->getClauseType() != ClauseType::MATCH) {
          eligible = false;
          break;
        }
        const auto* match = part->getReadingClause(0)->constPtrCast<BoundMatchClause>();
        const auto* collection = match->getQueryGraphCollection();
        for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi) {
          const auto* source = collection->getQueryGraph(gi);
          for (const auto& node : source->getQueryNodes()) {
            if (!graph.containsQueryNode(node->getUniqueName())) graph.addQueryNode(node);
          }
          for (const auto& rel : source->getQueryRels()) {
            if (!graph.containsQueryRel(rel->getUniqueName())) graph.addQueryRel(rel);
          }
        }
        if (match->hasPredicate()) {
          predicate = binder->getExpressionBinder()->combineBooleanExpressions(
              ExpressionType::AND, predicate, deepCopyShared(match->getPredicate()));
        }
      }
      if (!eligible) continue;
      auto nodesIR = convertGraphPatternToIR(graph);
      auto indexIR = info.getPatternIR();
      if (nodesIR.nodesInfo.size() != indexIR.nodesInfo.size() ||
          nodesIR.relsInfo.size() != indexIR.relsInfo.size()) continue;
      auto [matched, start] = PatternIR::findMatch(nodesIR, indexIR, entry->getName());
      (void)start;
      if (!matched) continue;
      auto shape = distinctFillShape(query, count, nodesIR, true);
      if (!shape.has_value() || std::find(info.getPredicateTemplates().begin(),
          info.getPredicateTemplates().end(), *shape) == info.getPredicateTemplates().end()) continue;
      QueryGraphCollection collection;
      collection.addAndMergeQueryGraphIfConnected(std::move(graph));
      auto match = std::make_unique<BoundMatchClause>(std::move(collection), MatchClauseType::MATCH);
      match->setPredicate(std::move(predicate));
      auto* matchPtr = match.get();
      NormalizedQueryPart stagedPart;
      stagedPart.addReadingClause(std::move(match));
      stagedPart.setProjectionBody(last->getProjectionBody()->copy());
      NormalizedSingleQuery stagedQuery;
      stagedQuery.appendQueryPart(std::move(stagedPart));
      auto* part = stagedQuery.getQueryPartUnsafe(0);
      currentQuery_ = &stagedQuery;
      mergedQueryIR_ = nodesIR;
      additionalFillTables.clear();
      propertyRewriteTargets.clear();
      expressionsToRewrite.clear();
      auto candidate = makeRewriteCandidate(*matchPtr, nodesIR, entry, indexIR);
      if (!candidate.has_value() || !validateRewriteCandidate(*matchPtr, *part, nodesIR, *candidate)) {
        currentQuery_ = nullptr;
        continue;
      }
      if (!rewriteMatchClause(*matchPtr, *part, nodesIR, *candidate)) {
        currentQuery_ = nullptr;
        continue;
      }
      for (uint32_t pi = count; pi < query.getNumQueryParts(); ++pi)
        rewriteReferencesFromPreviousQueryPart(*query.getQueryPartUnsafe(pi));
      uniqueNameRewriteMap.clear();
      for (uint32_t pi = 0; pi < count; ++pi) query.removeQueryPart(0);
      query.insertQueryPart(0, std::move(*part));
      currentQuery_ = nullptr;
      LOG(INFO) << "Successfully applied DISTINCT aggregate index rewrite with index: " << entry->getName();
      break;
    }
  }

  currentQuery_ = &query;
  mergedQueryIR_ = buildMergedQueryIR(query);

  bool collapseNextProjectionPart = false;
  for (auto i = 0u; i < query.getNumQueryParts();) {
    auto queryPart = query.getQueryPartUnsafe(i);
    if (collapseNextProjectionPart) {
      collapseNextProjectionPart = false;
      // References in a projection-only part still point at the aggregate
      // expressions produced by the preceding reading part. Rewrite them
      // before collapsing the two parts; otherwise the collapse removes the
      // boundary that makes those aliases resolvable and leaves stale
      // variables nested in expressions such as CASE.
      rewriteReferencesFromPreviousQueryPart(*queryPart);
      uniqueNameRewriteMap.clear();
      if (i > 0 && collapseAggregateRollupIntoReadingPart(
                       clientContext, *query.getQueryPartUnsafe(i - 1),
                       *queryPart)) {
        query.removeQueryPart(i);
        deferredPropertyRewriteMap.clear();
        continue;
      }
    }
    rewriteReferencesFromPreviousQueryPart(*queryPart);
    uniqueNameRewriteMap.clear();
    applyDeferredRebindPredicates(*queryPart);
    if (query.getQueryPart(i)->getNumReadingClause() == 0) {
      ++i;
      continue;
    }
    const auto rewriteCreatedProjectionOnlyPart =
        tryRewriteQueryPart(*queryPart);
    const auto insertedPreQueryPart = pendingPreQueryPart.has_value();
    if (insertedPreQueryPart) {
      query.insertQueryPart(i, std::move(*pendingPreQueryPart));
      pendingPreQueryPart.reset();
      ++i;
    }
    if (!insertedPreQueryPart && rewriteCreatedProjectionOnlyPart && i > 0) {
      removeSingletonFillSums(*query.getQueryPartUnsafe(i - 1),
                              *query.getQueryPartUnsafe(i));
    }
    if (!insertedPreQueryPart && rewriteCreatedProjectionOnlyPart && i > 0 &&
        mergeRewriteCreatedAggregatePart(
            *query.getQueryPartUnsafe(i - 1),
            *query.getQueryPartUnsafe(i))) {
      const auto previousHasPredicate =
          query.getQueryPartUnsafe(i - 1)->hasProjectionBodyPredicate();
      query.removeQueryPart(i);
      collapseNextProjectionPart = !previousHasPredicate;
      continue;
    }
    ++i;
  }
  // Cleanup is conditional on an actual committed substitution in this
  // branch, including specialized passes which precede V4. Merely inspecting
  // an unrelated READY index must preserve the original graph and join-order
  // ties; otherwise an index that is never read receives fictitious savings.
  if (committedMatchRewrites != 0 || binder->hasIndexRewriteApplied(query)) {
    // Run after both FILL substitutions so the existing following-aggregate
    // proof can first remove the second grouping.
    if (query.getNumQueryParts() > 0) {
      if (removeFirstUniqueScanFillSums(*query.getQueryPartUnsafe(0))) {
        inlineUniqueScanFillProjections(query);
      }
    }
    removeLinearConditionalFillRollup(query);
    removeBoundSingletonFillSums(query);
    restoreConnectedQueryGraphs(query);
  }
  enumerateGlobalCountPresenceVariants(query);
  currentQuery_ = nullptr;
}

void RewriterV4::dropProjectionsForRemovedPatterns(
    NormalizedQueryPart& queryPart,
    const std::unordered_set<std::string>& removedNodeUniqueNames) {
  if (removedNodeUniqueNames.empty() || !queryPart.hasProjectionBody()) {
    return;
  }
  auto* body = queryPart.getProjectionBodyUnsafe();
  auto projExprs = body->getProjectionExpressions();
  expression_vector filtered;
  filtered.reserve(projExprs.size());
  for (auto& expr : projExprs) {
    if (expr != nullptr && ExpressionUtil::isNodePattern(*expr)) {
      auto* node = expr->ptrCast<NodeExpression>();
      if (node != nullptr &&
          removedNodeUniqueNames.contains(node->getUniqueName()) &&
          !projectionPredicateAnchorNodes.contains(node->getUniqueName())) {
        continue;
      }
    }
    filtered.push_back(expr);
  }
  body->setProjectionExpressions(std::move(filtered));
}

void RewriterV4::recordDeferredPredicatesFromMatch(
    const BoundMatchClause& matchClause,
    const std::unordered_map<std::string, std::string>& uniqueToRawVar,
    const std::unordered_set<std::string>& removedNodeUniqueNames,
    const std::unordered_set<std::string>& projectedPatternUniqueNames) {
  if (removedNodeUniqueNames.empty() || !matchClause.hasPredicate()) {
    return;
  }
  auto conjuncts = matchClause.getConjunctivePredicates();
  if (conjuncts.empty()) {
    return;
  }
  for (const auto& removedUnique : removedNodeUniqueNames) {
    if (!projectedPatternUniqueNames.contains(removedUnique)) {
      continue;
    }
    auto itRaw = uniqueToRawVar.find(removedUnique);
    if (itRaw == uniqueToRawVar.end()) {
      continue;
    }
    DeferredRebindPredicate deferred;
    deferred.oldUniqueName = removedUnique;
    deferred.rawVarName = itRaw->second;
    for (auto& conj : conjuncts) {
      if (conj == nullptr) {
        continue;
      }
      auto referencedVars = collectPropertyVarUniqueNames(conj);
      if (referencedVars.size() == 1 && referencedVars.contains(removedUnique)) {
        deferred.conjuncts.push_back(deepCopyShared(conj));
      }
    }
    if (!deferred.conjuncts.empty()) {
      deferredRebindPredicates.push_back(std::move(deferred));
    }
  }
}

void RewriterV4::applyDeferredRebindPredicates(NormalizedQueryPart& queryPart) {
  if (deferredRebindPredicates.empty()) {
    return;
  }
  if (binder == nullptr) {
    deferredRebindPredicates.clear();
    return;
  }
  auto* exprBinder = binder->getExpressionBinder();
  if (exprBinder == nullptr) {
    deferredRebindPredicates.clear();
    return;
  }

  std::vector<DeferredRebindPredicate> remaining;
  remaining.reserve(deferredRebindPredicates.size());

  for (auto& deferred : deferredRebindPredicates) {
    bool applied = false;
    for (auto i = 0u; i < queryPart.getNumReadingClause() && !applied; ++i) {
      auto* clause = queryPart.getReadingClause(i);
      if (clause == nullptr || clause->getClauseType() != ClauseType::MATCH) {
        continue;
      }
      auto& matchClause = clause->cast<BoundMatchClause>();
      auto* collection = matchClause.getQueryGraphCollectionUnsafe();
      if (collection == nullptr || collection->getNumQueryGraphs() == 0) {
        continue;
      }
      auto* qg = collection->getQueryGraphUnsafe(0);
      if (qg == nullptr || !qg->queryNodeNameToPosMap.contains(deferred.oldUniqueName)) {
        continue;
      }
      auto predicate = matchClause.hasPredicate() ? matchClause.getPredicate() : nullptr;
      for (auto& conj : deferred.conjuncts) {
        if (conj == nullptr) {
          continue;
        }
        predicate = exprBinder->combineBooleanExpressions(ExpressionType::AND,
                                                          conj, predicate);
      }
      matchClause.setPredicate(std::move(predicate));
      applied = true;
    }

    if (!applied) {
      remaining.push_back(std::move(deferred));
    }
  }

  deferredRebindPredicates = std::move(remaining);
}

}  // namespace binder
}  // namespace neug

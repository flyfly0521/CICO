#include "neug/compiler/binder/rewriter/partial_count_rewriter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "glog/logging.h"
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/function/built_in_function_utils.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/parser.h"

namespace neug::binder {
namespace {
using namespace common;
using namespace catalog;
using Expr = std::shared_ptr<Expression>;
using Replacements = std::unordered_map<std::string, Expr>;

struct SourceCount {
  table_id_t table;
  std::string column;
  bool anchorIsSource;
  table_id_set_t neighborLabels;
  table_id_set_t relationLabels;
};

bool allowed(const FillIndexAuxInfo& info, const std::string& column) {
  const auto* value = std::getenv("NEUG_INDEX_REWRITE_ALLOW_TARGETS");
  if (value == nullptr || *value == '\0') return true;
  std::istringstream input(value);
  for (std::string token; std::getline(input, token, ',');) {
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) continue;
    token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
    if (token == info.getTableName() || token == info.getTableName() + "." + column)
      return true;
  }
  return false;
}

std::optional<SourceCount> proveSource(const FillIndexAuxInfo& info,
                                      main::ClientContext& context) {
  if (info.getFillType() != FillEntryType::NODE || info.getSourceQuery().empty() ||
      !info.getPresenceColumns().empty() || !info.getPredicateTemplates().empty() ||
      !info.getExistsPredicates().empty()) return std::nullopt;
  auto* table =
      // A FILL may target a REL group, which is not a table entry (lookup throws).
      context.getCatalog()->containsTable(context.getTransaction(), info.getTableName(), context.useInternalCatalogEntry()) ?
      context.getCatalog()->getTableCatalogEntry(context.getTransaction(), info.getTableName(), context.useInternalCatalogEntry()) : nullptr;
  if (table == nullptr || table->getType() != CatalogEntryType::NODE_TABLE_ENTRY)
    return std::nullopt;
  std::string countColumn;
  const auto& targets = info.getTargetColumns();
  const auto& columns = info.getIndexColumnInfos();
  if (targets.size() != columns.size()) return std::nullopt;
  for (size_t i = 0; i < targets.size(); ++i) {
    if (targets[i].starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
        targets[i].starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) continue;
    if (!countColumn.empty() || columns[i]->columnType != IndexColumnType::NODE_AGGREGATE ||
        !table->containsProperty(targets[i]) ||
        table->getProperty(targets[i]).getType().getLogicalTypeID() != LogicalTypeID::INT64)
      return std::nullopt;
    auto spelling = static_cast<const NodeAggregationIndexColnumInfo&>(*columns[i]).nameTemplate;
    std::erase_if(spelling, [](unsigned char c) { return std::isspace(c); });
    std::transform(spelling.begin(), spelling.end(), spelling.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    if (spelling != "COUNT(*)") return std::nullopt;
    countColumn = targets[i];
  }
  if (countColumn.empty() || !allowed(info, countColumn)) return std::nullopt;
  const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
  if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
    return std::nullopt;
  Binder sourceBinder(&context);
  sourceBinder.setAllowIndexRewrite(false);
  auto statement = sourceBinder.bind(*parsed[0]);
  const auto& regular = statement->constCast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
      regular.getPostSingleQuery()) return std::nullopt;
  const auto& single = *regular.getSingleQuery(0);
  if (single.getNumQueryParts() != 1) return std::nullopt;
  const auto& part = *single.getQueryPart(0);
  if (part.hasUpdatingClause() || part.getNumReadingClause() != 1 ||
      !part.hasProjectionBody() || part.hasProjectionBodyPredicate()) return std::nullopt;
  const auto* clause = part.getReadingClause(0);
  if (clause->getClauseType() != ClauseType::MATCH) return std::nullopt;
  const auto& match = clause->constCast<BoundMatchClause>();
  if (match.getMatchClauseType() != MatchClauseType::MATCH ||
      match.hasPredicate() || match.hasHint()) return std::nullopt;
  const auto* graphs = match.getQueryGraphCollection();
  if (graphs->getNumQueryGraphs() != 1) return std::nullopt;
  const auto* graph = graphs->getQueryGraph(0);
  if (graph->getNumQueryNodes() != 2 || graph->getNumQueryRels() != 1)
    return std::nullopt;
  const auto edge = graph->getQueryRel(0);
  if (edge->isRecursive() || edge->getDirectionType() != RelDirectionType::SINGLE)
    return std::nullopt;
  const auto& body = *part.getProjectionBody();
  const auto values = body.getProjectionExpressions();
  const auto groups = body.getGroupByExpressions();
  if (body.isDistinct() || body.hasSkipOrLimit() || body.hasOrderByExpressions() ||
      values.size() != 2 || groups.size() != 1 ||
      values[0]->expressionType != ExpressionType::PROPERTY ||
      values[1]->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
      groups[0]->getUniqueName() != values[0]->getUniqueName()) return std::nullopt;
  const auto& key = values[0]->constCast<PropertyExpression>();
  if (!key.isPrimaryKey(table->getTableID())) return std::nullopt;
  const auto& aggregate = values[1]->constCast<AggregateFunctionExpression>();
  if (aggregate.isDistinct() || aggregate.getNumChildren() != 0 ||
      (aggregate.getFunction().name != "COUNT_STAR" && aggregate.getFunction().name != "COUNT"))
    return std::nullopt;
  const auto anchorIsSource = edge->getSrcNode()->getUniqueName() == key.getVariableName();
  const auto anchor = anchorIsSource ? edge->getSrcNode() : edge->getDstNode();
  const auto neighbor = anchorIsSource ? edge->getDstNode() : edge->getSrcNode();
  if (anchor->getUniqueName() != key.getVariableName() ||
      anchor->getTableIDsSet() != table_id_set_t{table->getTableID()}) return std::nullopt;
  return SourceCount{table->getTableID(), std::move(countColumn), anchorIsSource,
                     neighbor->getTableIDsSet(), edge->getTableIDsSet()};
}

void substitute(Expr& expression, const Replacements& replacements) {
  if (!expression) return;
  if (const auto found = replacements.find(expression->getUniqueName());
      found != replacements.end()) { expression = found->second; return; }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto& c = expression->cast<CaseExpression>();
    for (size_t i = 0; i < c.getNumCaseAlternatives(); ++i) {
      substitute(c.getCaseAlternative(i)->whenExpression, replacements);
      substitute(c.getCaseAlternative(i)->thenExpression, replacements);
    }
    auto other = c.getElseExpression();
    substitute(other, replacements);
    c.setElseExpression(std::move(other));
  } else {
    for (size_t i = 0; i < expression->getNumChildren(); ++i) {
      auto child = expression->getChild(i);
      substitute(child, replacements);
      expression->setChild(i, std::move(child));
    }
  }
}

void rewritePart(NormalizedQueryPart& part, const Replacements& replacements) {
  if (part.hasProjectionBody()) {
    auto* body = part.getProjectionBodyUnsafe();
    const auto rewrite = [&](expression_vector values) {
      for (auto& value : values) substitute(value, replacements);
      return values;
    };
    body->setProjectionExpressions(rewrite(body->getProjectionExpressions()));
    body->setAggregateExpressions(rewrite(body->getAggregateExpressions()));
    body->setGroupByExpressions(rewrite(body->getGroupByExpressions()));
    body->setOrderByExpressions(rewrite(body->getOrderByExpressions()), body->getSortingOrders());
  }
  auto predicate = part.getProjectionBodyPredicate();
  substitute(predicate, replacements);
  part.setProjectionBodyPredicate(predicate);
  for (size_t i = 0; i < part.getNumReadingClause(); ++i) {
    auto* clause = part.getReadingClause(i);
    predicate = clause->getPredicate();
    substitute(predicate, replacements);
    clause->setPredicate(predicate);
  }
}

Expr makeSum(Expr input, const Expr& original, main::ClientContext& context) {
  expression_vector children{std::move(input)};
  std::vector<LogicalType> types;
  types.push_back(children[0]->getDataType().copy());
  auto* entry = context.getCatalog()->getFunctionEntry(context.getTransaction(), "SUM");
  auto function = function::BuiltInFunctionsUtils::matchAggregateFunction(
      "SUM", types, false, entry->ptrCast<FunctionCatalogEntry>())->copy();
  std::unique_ptr<function::FunctionBindData> data;
  if (function.bindFunc) {
    auto bindInput = function::ScalarBindFuncInput{children, &function, &context, {}};
    data = function.bindFunc(bindInput);
  } else data = std::make_unique<function::FunctionBindData>(LogicalType(function.returnTypeID));
  auto result = std::make_shared<AggregateFunctionExpression>(
      std::move(function), std::move(data), std::move(children), original->getUniqueName());
  result->setParsedName(original->getParsedName());
  result->setRawNameVariables(original->getRawNameVariables());
  if (original->hasAlias()) result->setAlias(original->getAlias());
  return result;
}

bool rewriteOne(NormalizedSingleQuery& query, size_t position,
                const SourceCount& source, main::ClientContext& context, Binder& binder) {
  auto* part = query.getQueryPartUnsafe(position);
  if (!part->hasProjectionBody() || part->hasProjectionBodyPredicate() ||
      part->getNumReadingClause() == 0) return false;
  const auto clausePosition = part->getNumReadingClause() - 1;
  auto* clause = part->getReadingClause(clausePosition);
  if (clause->getClauseType() != ClauseType::MATCH) return false;
  auto& match = clause->cast<BoundMatchClause>();
  if (match.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH ||
      match.hasPredicate() || match.hasHint()) return false;
  const auto* graphs = match.getQueryGraphCollection();
  if (graphs->getNumQueryGraphs() != 1) return false;
  const auto* graph = graphs->getQueryGraph(0);
  if (graph->getNumQueryNodes() != 2 || graph->getNumQueryRels() != 1) return false;
  const auto edge = graph->getQueryRel(0);
  if (edge->isRecursive() || edge->getDirectionType() != RelDirectionType::SINGLE) return false;
  const auto anchor = source.anchorIsSource ? edge->getSrcNode() : edge->getDstNode();
  const auto fresh = source.anchorIsSource ? edge->getDstNode() : edge->getSrcNode();
  const auto anchorLabels = anchor->getTableIDsSet();
  if (anchorLabels.size() < 2 || !anchorLabels.contains(source.table) ||
      fresh->getTableIDsSet() != source.neighborLabels) return false;
  // Compare the actual directed relationship tables in the covered label
  // branch. Similar printed group names do not establish the same graph.
  table_id_set_t coveredEdges;
  for (const auto* entry : edge->getEntries()) {
    if (entry->getType() != CatalogEntryType::REL_TABLE_ENTRY) return false;
    const auto& relation = entry->constCast<RelTableCatalogEntry>();
    const auto anchorTable = source.anchorIsSource ? relation.getSrcTableID() : relation.getDstTableID();
    const auto neighborTable = source.anchorIsSource ? relation.getDstTableID() : relation.getSrcTableID();
    if (anchorTable == source.table && source.neighborLabels.contains(neighborTable))
      coveredEdges.insert(entry->getTableID());
  }
  if (coveredEdges != source.relationLabels) return false;
  bool anchorBound = false;
  for (size_t p = 0; p <= position; ++p) {
    const auto* previous = query.getQueryPart(p);
    const auto limit = p == position ? clausePosition : previous->getNumReadingClause();
    for (size_t c = 0; c < limit; ++c) {
      const auto* reading = previous->getReadingClause(c);
      if (reading->getClauseType() != ClauseType::MATCH) return false;
      const auto* collection = reading->constCast<BoundMatchClause>().getQueryGraphCollection();
      for (size_t g = 0; g < collection->getNumQueryGraphs(); ++g) {
        const auto* prior = collection->getQueryGraph(g);
        anchorBound |= prior->containsQueryNode(anchor->getUniqueName());
        if (prior->containsQueryNode(fresh->getUniqueName()) ||
            prior->containsQueryRel(edge->getUniqueName())) return false;
      }
    }
  }
  if (!anchorBound) return false;
  auto* body = part->getProjectionBodyUnsafe();
  // A bare numeric PK is not a union-label identity: equal IDs from two
  // physical node tables must not be merged before the guarded aggregate.
  const auto groups = body->getGroupByExpressions();
  if (std::none_of(groups.begin(), groups.end(),
        [&](const auto& group) { return group->expressionType == ExpressionType::PATTERN &&
            group->getUniqueName() == anchor->getUniqueName(); })) return false;
  const auto aggregates = body->getAggregateExpressions();
  if (aggregates.empty()) return false;
  std::unordered_set<std::string> aggregateNames;
  for (const auto& expression : aggregates) {
    if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
    const auto& aggregate = expression->constCast<AggregateFunctionExpression>();
    if (aggregate.getFunction().name != "COUNT" || aggregate.isDistinct() ||
        aggregate.getNumChildren() != 1 ||
        aggregate.getChild(0)->expressionType != ExpressionType::PATTERN) return false;
    const auto name = aggregate.getChild(0)->getUniqueName();
    if (name != fresh->getUniqueName() && name != edge->getUniqueName()) return false;
    aggregateNames.insert(expression->getUniqueName());
  }
  // A previous WITH's projected scalar is an input value in this part, even
  // when its bound expression object still describes the earlier aggregate.
  // Only that immediately preceding projection's complete expression object
  // establishes the boundary. Aggregate unique names can be structural, so a
  // newly written COUNT with the same spelling must not acquire this proof.
  std::unordered_set<const Expression*> incomingScalars;
  std::function<bool(const Expr&)> dependsOnFresh = [&](const Expr& expression) {
    if (!expression) return false;
    if (expression->expressionType == ExpressionType::SUBQUERY) return true;
    auto name = expression->getUniqueName();
    if (expression->expressionType == ExpressionType::PROPERTY)
      name = expression->constCast<PropertyExpression>().getVariableName();
    if (name == fresh->getUniqueName() || name == edge->getUniqueName()) return true;
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      if (dependsOnFresh(child)) return true;
    return false;
  };
  if (position > 0) {
    const auto* previous = query.getQueryPart(position - 1);
    if (previous->hasProjectionBody()) {
      for (const auto& input : previous->getProjectionBody()->getProjectionExpressions()) {
        if (input->expressionType != ExpressionType::PATTERN &&
            input->getDataType().getLogicalTypeID() != LogicalTypeID::ANY &&
            !LogicalTypeUtils::isNested(input->getDataType()) && !dependsOnFresh(input))
          incomingScalars.insert(input.get());
      }
    }
  }
  std::function<bool(const Expr&)> safe = [&](const Expr& expression) {
    if (!expression || aggregateNames.contains(expression->getUniqueName()) ||
        incomingScalars.contains(expression.get())) return true;
    if (expression->expressionType == ExpressionType::SUBQUERY ||
        expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) return false;
    auto name = expression->getUniqueName();
    if (expression->expressionType == ExpressionType::PROPERTY)
      name = expression->constCast<PropertyExpression>().getVariableName();
    if (name == fresh->getUniqueName() || name == edge->getUniqueName()) return false;
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      if (!safe(child)) return false;
    return true;
  };
  for (const auto& expressions : {body->getProjectionExpressions(),
                                  body->getGroupByExpressions(), body->getOrderByExpressions()})
    for (const auto& expression : expressions) if (!safe(expression)) return false;

  auto* expressionBinder = binder.getExpressionBinder();
  auto* table = context.getCatalog()->getTableCatalogEntry(context.getTransaction(), source.table);
  auto covered = expressionBinder->createEqualityComparisonExpression(
      anchor->getLabelExpression(), expressionBinder->createLiteralExpression(
          table->getLabel(context.getCatalog(), context.getTransaction())));
  auto guard = expressionBinder->bindBooleanExpression(ExpressionType::NOT, {covered});
  table_id_map_t<SingleLabelPropertyInfo> infos;
  for (const auto label : anchorLabels)
    infos.emplace(label, SingleLabelPropertyInfo(label == source.table, false));
  auto saved = std::make_shared<PropertyExpression>(LogicalType::INT64(), source.column,
      anchor->getUniqueName(), anchor->getVariableName(), std::move(infos));
  auto zero = expressionBinder->createLiteralExpression(Value(int64_t{0}));
  auto one = expressionBinder->createLiteralExpression(Value(int64_t{1}));
  auto savedOrZero = std::make_shared<CaseExpression>(LogicalType::INT64(), saved,
      "partial_count_saved_" + saved->getUniqueName());
  savedOrZero->addCaseAlternative(expressionBinder->bindNullOperatorExpression(
      ExpressionType::IS_NULL, {saved}), zero);
  auto online = std::make_shared<CaseExpression>(LogicalType::INT64(), one,
      "partial_count_online_" + fresh->getUniqueName());
  online->addCaseAlternative(expressionBinder->bindNullOperatorExpression(
      ExpressionType::IS_NULL, {fresh->getInternalID()}), zero);
  auto contribution = std::make_shared<CaseExpression>(LogicalType::INT64(), online,
      "partial_count_guarded_" + saved->getUniqueName());
  contribution->addCaseAlternative(covered, savedOrZero);
  Replacements current, downstream;
  for (const auto& aggregate : aggregates) {
    auto replacement = makeSum(contribution, aggregate, context);
    if (replacement->getDataType() != aggregate->getDataType()) return false;
    current.emplace(aggregate->getUniqueName(), replacement);
    auto variable = std::make_shared<VariableExpression>(aggregate->getDataType().copy(),
        aggregate->getUniqueName(), aggregate->toString());
    variable->setParsedName(aggregate->getParsedName());
    if (aggregate->hasAlias()) variable->setAlias(aggregate->getAlias());
    downstream.emplace(aggregate->getUniqueName(), variable);
  }
  // Mutate only after the source, label branch, null-extension and complete
  // output boundary have all been proved. Keep every original graph edge.
  match.setPredicate(guard);
  rewritePart(*part, current);
  for (size_t p = position + 1; p < query.getNumQueryParts(); ++p)
    rewritePart(*query.getQueryPartUnsafe(p), downstream);
  return true;
}
}  // namespace

void rewritePartialCounts(BoundStatement& statement,
                          main::ClientContext& context, Binder* binder) {
  if (binder == nullptr || !binder->allowIndexRewrite() ||
      statement.getStatementType() != StatementType::QUERY) return;
  auto& regular = statement.cast<BoundRegularQuery>();
  if (!regular.getPreQueryPart().empty() || regular.getPostSingleQuery()) return;
  auto entries = context.getCatalog()->getExtendedIndexEntries(context.getTransaction());
  std::sort(entries.begin(), entries.end(), [](const auto* a, const auto* b) {
    return a->getName() < b->getName();
  });
  for (const auto* entry : entries) {
    if (!entry->isLoaded() || context.getCatalog()->isExtendedIndexBuilding(entry->getName())) continue;
    const auto* info = dynamic_cast<const FillIndexAuxInfo*>(&entry->getAuxInfo());
    if (info == nullptr) continue;
    try {
      const auto source = proveSource(*info, context);
      if (!source) continue;
      for (size_t q = 0; q < regular.getNumSingleQueries(); ++q) {
        auto& query = *regular.getSingleQueryUnsafe(q);
        bool readOnly = true;
        for (size_t p = 0; p < query.getNumQueryParts(); ++p)
          readOnly &= !query.getQueryPart(p)->hasUpdatingClause();
        if (!readOnly) continue;
        for (size_t p = 0; p < query.getNumQueryParts(); ++p)
          if (rewriteOne(query, p, *source, context, *binder)) {
            binder->markIndexRewriteApplied(query);
            LOG(INFO) << "Successfully applied partial COUNT index: " << entry->getName();
          }
      }
    } catch (const std::exception& error) {
      LOG(INFO) << "Partial COUNT not applicable: " << entry->getName() << ": " << error.what();
    }
  }
}
}  // namespace neug::binder

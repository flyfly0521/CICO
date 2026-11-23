#include "neug/compiler/binder/rewriter/distinct_count_partition_normalizer.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "glog/logging.h"
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/query/reading_clause/bound_unwind_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_functional_assumption.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/function/built_in_function_utils.h"
#include "neug/compiler/main/client_context.h"

namespace neug::binder {
namespace {
using namespace common;
using Expr = std::shared_ptr<Expression>;
using catalog::DeclaredRelationshipMultiplicity;

struct Partition {
  NormalizedSingleQuery* query = nullptr;
  size_t position = 0;
  std::vector<const BoundReadingClause*> clauses;
  std::optional<BoundProjectionBody> originalBody;
  Expr originalPredicate;
  const Expression* installedCount = nullptr;  // in the split part
  const Expression* installedSum = nullptr;    // in the inserted tail part
};

bool containsName(const Expr& expression, const std::string& name) {
  if (!expression) return false;
  if (expression->getUniqueName() == name) return true;
  for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
    if (containsName(child, name)) return true;
  return false;
}

// Every leaf below the aggregate boundary must be a grouping key or the
// aggregate itself: the tail part only sees the split part's outputs.
bool coveredByKeys(const Expr& expression, const std::unordered_set<std::string>& keys,
                   const std::string& aggregate) {
  if (!expression) return true;
  const auto& name = expression->getUniqueName();
  if (keys.contains(name) || name == aggregate) return true;
  switch (expression->expressionType) {
  case ExpressionType::LITERAL:
  case ExpressionType::PARAMETER:
    return true;
  case ExpressionType::AGGREGATE_FUNCTION:
  case ExpressionType::SUBQUERY:
  case ExpressionType::PROPERTY:
  case ExpressionType::PATTERN:
  case ExpressionType::VARIABLE:
  case ExpressionType::PATH:
    return false;
  default:
    break;
  }
  for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
    if (!coveredByKeys(child, keys, aggregate)) return false;
  return true;
}

std::vector<DeclaredRelationshipMultiplicity> declarationsFor(table_id_t anchorTable,
                                                             main::ClientContext& context) {
  std::vector<DeclaredRelationshipMultiplicity> result;
  auto* catalog = context.getCatalog();
  for (const auto* entry : catalog->getExtendedIndexEntries(context.getTransaction())) {
    if (!entry->isLoaded() || catalog->isExtendedIndexBuilding(entry->getName())) continue;
    const auto* info = dynamic_cast<const catalog::FillIndexAuxInfo*>(&entry->getAuxInfo());
    if (info == nullptr || info->getFillType() != catalog::FillEntryType::NODE ||
        info->getSourceQuery().empty())
      continue;
    const auto* table =
      // A FILL may target a REL group, which is not a table entry (lookup throws).
      catalog->containsTable(context.getTransaction(), info->getTableName(), context.useInternalCatalogEntry()) ?
      catalog->getTableCatalogEntry(context.getTransaction(), info->getTableName(), context.useInternalCatalogEntry()) : nullptr;
    if (table == nullptr || table->getTableID() != anchorTable) continue;
    if (auto parsed = catalog::parseFillRelationshipDeclarations(info->getSourceQuery(), context))
      for (auto& declared : *parsed) result.push_back(std::move(declared));
  }
  return result;
}

// Nodes functionally determined by the counted object inside one MATCH
// clause, using only edge endpoints and declared at-most-one bounds that cover
// a relationship's complete type union.
std::unordered_set<std::string> determinedNodes(
    const QueryGraphCollection& graphs, const Expr& counted,
    const std::vector<DeclaredRelationshipMultiplicity>& declarations) {
  std::unordered_set<std::string> known;
  std::vector<std::shared_ptr<RelExpression>> rels;
  for (size_t g = 0; g < graphs.getNumQueryGraphs(); ++g) {
    for (const auto& rel : graphs.getQueryGraph(g)->getQueryRels()) {
      rels.push_back(rel);
      if (rel->getUniqueName() == counted->getUniqueName()) {
        if (rel->isRecursive() || rel->getDirectionType() != RelDirectionType::SINGLE) return {};
        known.insert(rel->getSrcNodeName());
        known.insert(rel->getDstNodeName());
      }
    }
    for (const auto& node : graphs.getQueryGraph(g)->getQueryNodes())
      if (node->getUniqueName() == counted->getUniqueName()) known.insert(node->getUniqueName());
  }
  if (known.empty()) return {};
  bool progress = true;
  while (progress) {
    progress = false;
    for (const auto& rel : rels) {
      if (rel->isRecursive() || rel->getDirectionType() != RelDirectionType::SINGLE) continue;
      const auto tables = rel->getTableIDsSet();
      for (const auto& declared : declarations) {
        if (!std::all_of(tables.begin(), tables.end(),
                         [&](auto id) { return declared.tables.contains(id); }))
          continue;
        const auto src = rel->getSrcNodeName(), dst = rel->getDstNodeName();
        if (declared.atMostOneOutgoing && known.contains(src) && !known.contains(dst)) {
          known.insert(dst);
          progress = true;
        }
        if (declared.atMostOneIncoming && known.contains(dst) && !known.contains(src)) {
          known.insert(src);
          progress = true;
        }
      }
    }
  }
  return known;
}

Expr makeAggregate(const std::string& functionName, bool distinct, Expr input,
                   std::string uniqueName, main::ClientContext& context) {
  expression_vector children{std::move(input)};
  std::vector<LogicalType> types;
  types.push_back(children[0]->getDataType().copy());
  auto* entry = context.getCatalog()->getFunctionEntry(context.getTransaction(), functionName);
  auto function = function::BuiltInFunctionsUtils::matchAggregateFunction(
      functionName, types, distinct, entry->ptrCast<catalog::FunctionCatalogEntry>())->copy();
  std::unique_ptr<function::FunctionBindData> data;
  if (function.bindFunc) {
    auto bindInput = function::ScalarBindFuncInput{children, &function, &context, {}};
    data = function.bindFunc(bindInput);
  } else {
    data = std::make_unique<function::FunctionBindData>(LogicalType(function.returnTypeID));
  }
  return std::make_shared<AggregateFunctionExpression>(std::move(function), std::move(data),
                                                       std::move(children), std::move(uniqueName));
}

std::string displayName(const Expr& expression) {
  return expression->hasAlias() ? expression->getAlias() : expression->toString();
}

std::optional<Partition> normalizePart(NormalizedSingleQuery& query, size_t position,
                                       main::ClientContext& context) {
  auto* part = query.getQueryPartUnsafe(position);
  if (!part->hasProjectionBody() || part->hasUpdatingClause() || part->getNumReadingClause() < 2)
    return std::nullopt;
  const auto* body = part->getProjectionBody();
  if (body->isDistinct()) return std::nullopt;
  const auto aggregates = body->getAggregateExpressions();
  if (aggregates.size() != 1 || aggregates[0]->expressionType != ExpressionType::AGGREGATE_FUNCTION)
    return std::nullopt;
  const auto aggregate = aggregates[0];
  const auto& count = aggregate->constCast<AggregateFunctionExpression>();
  if (count.getFunction().name != "COUNT" || !count.isDistinct() || count.getNumChildren() != 1)
    return std::nullopt;
  const auto counted = count.getChild(0);
  if (counted->expressionType != ExpressionType::PATTERN) return std::nullopt;
  const auto& aggregateName = aggregate->getUniqueName();

  // The aggregate may appear only as a whole projection/order item, so the
  // tail part can reference it by name without mutating shared expressions.
  const auto groupBy = body->getGroupByExpressions();
  std::unordered_set<std::string> keys;
  for (const auto& key : groupBy) {
    if (containsName(key, counted->getUniqueName()) || containsName(key, aggregateName))
      return std::nullopt;
    keys.insert(key->getUniqueName());
  }
  for (const auto& values : {body->getProjectionExpressions(), body->getOrderByExpressions()})
    for (const auto& value : values)
      if (value->getUniqueName() != aggregateName &&
          (containsName(value, aggregateName) || !coveredByKeys(value, keys, aggregateName)))
        return std::nullopt;
  if (part->hasProjectionBodyPredicate() &&
      (containsName(part->getProjectionBodyPredicate(), aggregateName) ||
       !coveredByKeys(part->getProjectionBodyPredicate(), keys, aggregateName)))
    return std::nullopt;

  // Locate the clause introducing the counted object, and the identities bound
  // before it (candidate partition anchors must survive that clause unchanged).
  std::unordered_set<std::string> bound;
  for (size_t p = 0; p < position; ++p)
    if (query.getQueryPart(p)->hasProjectionBody())
      for (const auto& value : query.getQueryPart(p)->getProjectionBody()->getProjectionExpressions())
        bound.insert(value->getUniqueName());
  const BoundMatchClause* owner = nullptr;
  std::unordered_map<std::string, Expr> boundNodes;
  for (size_t c = 0; c < part->getNumReadingClause(); ++c) {
    const auto* reading = part->getReadingClause(c);
    if (reading->getClauseType() == ClauseType::UNWIND) {
      bound.insert(reading->constCast<BoundUnwindClause>().getOutExpr()->getUniqueName());
      continue;
    }
    if (reading->getClauseType() != ClauseType::MATCH) return std::nullopt;
    const auto& match = reading->constCast<BoundMatchClause>();
    if (match.hasHint()) return std::nullopt;
    const auto* graphs = match.getQueryGraphCollection();
    bool introduces = false;
    for (size_t g = 0; g < graphs->getNumQueryGraphs(); ++g) {
      for (const auto& rel : graphs->getQueryGraph(g)->getQueryRels())
        introduces |= rel->getUniqueName() == counted->getUniqueName();
      for (const auto& node : graphs->getQueryGraph(g)->getQueryNodes())
        introduces |= node->getUniqueName() == counted->getUniqueName();
    }
    if (introduces) {
      if (owner != nullptr || bound.contains(counted->getUniqueName())) return std::nullopt;
      owner = &match;
      for (size_t g = 0; g < graphs->getNumQueryGraphs(); ++g)
        for (const auto& node : graphs->getQueryGraph(g)->getQueryNodes())
          if (bound.contains(node->getUniqueName())) boundNodes.emplace(node->getUniqueName(), node);
      break;
    }
    for (size_t g = 0; g < graphs->getNumQueryGraphs(); ++g) {
      for (const auto& node : graphs->getQueryGraph(g)->getQueryNodes()) bound.insert(node->getUniqueName());
      for (const auto& rel : graphs->getQueryGraph(g)->getQueryRels()) bound.insert(rel->getUniqueName());
    }
  }
  if (owner == nullptr || boundNodes.empty()) return std::nullopt;

  // Choose the unique previously bound anchor that the counted object
  // determines under declarations carried by FILLs on that anchor's label.
  Expr anchor;
  std::vector<std::string> names;
  for (const auto& [name, node] : boundNodes) names.push_back(name);
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const auto& node = boundNodes.at(name);
    if (keys.contains(name)) return std::nullopt;  // already grouped by the anchor
    const auto labels = node->constCast<NodeExpression>().getTableIDs();
    if (labels.size() != 1) continue;
    const auto declarations = declarationsFor(labels.front(), context);
    if (declarations.empty()) continue;
    const auto known = determinedNodes(*owner->getQueryGraphCollection(), counted, declarations);
    if (!known.contains(name)) continue;
    if (anchor) return std::nullopt;  // ambiguous partition
    anchor = node;
  }
  if (!anchor) return std::nullopt;

  // Split: (keys, anchor, COUNT(DISTINCT w)) then SUM per original key. A
  // counted object determines its anchor, so the per-anchor sets are disjoint
  // and their sizes add up to the original distinct count (zero included).
  const auto partitionName = "__distinct_partition_" + aggregateName;
  // Same function, operand and template identity as the original COUNT, so
  // FILL template matching sees exactly the query's aggregate; only the
  // output name differs from the final SUM.
  auto partitionCount = std::make_shared<AggregateFunctionExpression>(
      count.getFunction().copy(), count.getBindData()->copy(), expression_vector{counted},
      partitionName);
  partitionCount->setParsedName(aggregate->getParsedName());
  partitionCount->setRawNameVariables(aggregate->getRawNameVariables());
  partitionCount->setAlias(partitionName);
  auto input = std::make_shared<VariableExpression>(aggregate->getDataType().copy(),
                                                    partitionName, partitionName);
  auto sum = makeAggregate("SUM", false, input, aggregateName, context);
  if (sum->getDataType() != aggregate->getDataType()) return std::nullopt;
  sum->setParsedName(aggregate->getParsedName());
  sum->setRawNameVariables(aggregate->getRawNameVariables());
  if (aggregate->hasAlias()) sum->setAlias(aggregate->getAlias());

  Partition result;
  result.query = &query;
  result.position = position;
  for (size_t c = 0; c < part->getNumReadingClause(); ++c)
    result.clauses.push_back(part->getReadingClause(c));
  result.originalBody = body->copy();
  result.originalPredicate = part->getProjectionBodyPredicate();
  result.installedCount = partitionCount.get();
  result.installedSum = sum.get();

  auto tailBody = body->copy();
  const auto swap = [&](expression_vector values) {
    for (auto& value : values)
      if (value->getUniqueName() == aggregateName) value = sum;
    return values;
  };
  tailBody.setProjectionExpressions(swap(tailBody.getProjectionExpressions()));
  tailBody.setAggregateExpressions({sum});
  tailBody.setOrderByExpressions(swap(tailBody.getOrderByExpressions()), tailBody.getSortingOrders());

  BoundProjectionBody splitBody(false);
  expression_vector outputs = groupBy, groups = groupBy;
  outputs.push_back(anchor);
  groups.push_back(anchor);
  outputs.push_back(partitionCount);
  std::vector<std::string> outputNames;
  for (const auto& output : outputs) outputNames.push_back(displayName(output));
  splitBody.setProjectionExpressions(outputs);
  splitBody.setProjectionNames(std::move(outputNames));
  splitBody.setGroupByExpressions(std::move(groups));
  splitBody.setAggregateExpressions({partitionCount});

  NormalizedQueryPart tail;
  tail.setProjectionBody(std::move(tailBody));
  tail.setProjectionBodyPredicate(result.originalPredicate);
  part->setProjectionBody(std::move(splitBody));
  part->setProjectionBodyPredicate(nullptr);
  query.insertQueryPart(position + 1, std::move(tail));
  return result;
}

bool aggregatesContain(const NormalizedQueryPart& part, const Expression* target) {
  if (!part.hasProjectionBody()) return false;
  for (const auto& value : part.getProjectionBody()->getAggregateExpressions())
    if (value.get() == target) return true;
  return false;
}
}  // namespace

struct DistinctCountPartitions {
  std::vector<Partition> applied;
};

std::shared_ptr<DistinctCountPartitions> normalizeDistinctCountPartitions(
    BoundStatement& statement, main::ClientContext& context, Binder* binder) {
  auto result = std::make_shared<DistinctCountPartitions>();
  if (binder == nullptr || !binder->allowIndexRewrite() || binder->hasIndexRewriteReplay() ||
      statement.getStatementType() != StatementType::QUERY)
    return result;
  if (context.getCatalog()->getExtendedIndexEntries(context.getTransaction()).empty()) return result;
  auto& regular = statement.cast<BoundRegularQuery>();
  if (!regular.getPreQueryPart().empty() || regular.getPostSingleQuery()) return result;
  for (size_t q = 0; q < regular.getNumSingleQueries(); ++q) {
    auto& query = *regular.getSingleQueryUnsafe(q);
    bool readOnly = true;
    for (size_t p = 0; p < query.getNumQueryParts(); ++p)
      readOnly &= !query.getQueryPart(p)->hasUpdatingClause();
    if (!readOnly) continue;
    // Insertions shift later parts; walk backwards so earlier positions hold.
    for (size_t p = query.getNumQueryParts(); p-- > 0;) {
      try {
        if (auto applied = normalizePart(query, p, context))
          result->applied.push_back(std::move(*applied));
      } catch (const std::exception& error) {
        VLOG(1) << "Distinct COUNT partition skipped: " << error.what();
      }
    }
  }
  return result;
}

void restoreUnusedDistinctCountPartitions(const std::shared_ptr<DistinctCountPartitions>& state) {
  if (!state) return;
  // Applied back-to-front per query, so restore front-to-back (LIFO).
  for (auto it = state->applied.rbegin(); it != state->applied.rend(); ++it) {
    auto& n = *it;
    if (n.position + 1 >= n.query->getNumQueryParts()) continue;
    auto* part = n.query->getQueryPartUnsafe(n.position);
    const auto* tail = n.query->getQueryPart(n.position + 1);
    // Keep any split that a rewrite consumed or otherwise touched: the split
    // itself is semantics-preserving under the declaration it relied on.
    if (!aggregatesContain(*part, n.installedCount) || !aggregatesContain(*tail, n.installedSum) ||
        tail->getNumReadingClause() != 0 || part->getNumReadingClause() != n.clauses.size())
      continue;
    bool same = true;
    for (size_t c = 0; c < n.clauses.size(); ++c) same &= part->getReadingClause(c) == n.clauses[c];
    if (!same) continue;
    n.query->removeQueryPart(n.position + 1);
    part = n.query->getQueryPartUnsafe(n.position);
    part->setProjectionBody(n.originalBody->copy());
    part->setProjectionBodyPredicate(n.originalPredicate);
  }
}

}  // namespace neug::binder

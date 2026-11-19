#include "neug/compiler/binder/rewriter/filtered_count_normalizer.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "glog/logging.h"
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/query/reading_clause/bound_unwind_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/function/built_in_function_utils.h"
#include "neug/compiler/main/client_context.h"

namespace neug::binder {
namespace {
using namespace common;
using Expr = std::shared_ptr<Expression>;

// A replacement made by this pass, identified by object identity so that a
// later rewrite which consumed (and therefore replaced) it is detectable.
struct Replacement {
  const Expression* installed;
  Expr original;
};

struct Normalization {
  BoundMatchClause* clause = nullptr;
  Expr originalPredicate;
  Expr installedPredicate;
  std::vector<Replacement> aggregates;  // in the owning part
  size_t part = 0;
  NormalizedSingleQuery* query = nullptr;
  std::vector<Replacement> downstream;  // later parts
};

std::string variableOf(const Expression& expression) {
  if (expression.expressionType == ExpressionType::PROPERTY)
    return expression.constCast<PropertyExpression>().getVariableName();
  return expression.getUniqueName();
}

void dependencies(const Expr& expression, std::unordered_set<std::string>& names) {
  if (!expression) return;
  if (expression->expressionType == ExpressionType::PROPERTY ||
      expression->expressionType == ExpressionType::PATTERN ||
      expression->expressionType == ExpressionType::VARIABLE) {
    names.insert(variableOf(*expression));
    if (expression->expressionType != ExpressionType::VARIABLE) return;
  }
  for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
    dependencies(child, names);
}

bool isIntLiteral(const Expr& expression, int64_t value) {
  if (!expression || expression->expressionType != ExpressionType::LITERAL) return false;
  const auto literal = expression->constCast<LiteralExpression>().getValue();
  return !literal.isNull() && literal.getDataType().getLogicalTypeID() == LogicalTypeID::INT64 &&
         literal.getValue<int64_t>() == value;
}

void conjuncts(const Expr& expression, expression_vector& out) {
  if (expression && expression->expressionType == ExpressionType::AND) {
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      conjuncts(child, out);
  } else if (expression) {
    out.push_back(expression);
  }
}

// SUM(CASE WHEN c THEN 1 ELSE 0 END), c = AND of IS NOT NULL(region identity)
// and null-propagating comparisons over the region only. Returns the filter
// (without the IS NOT NULL tests) and a counted region node.
struct FilteredCount {
  expression_vector filters;
  std::string key;
  Expr counted;
};

std::optional<FilteredCount> filteredCount(const Expr& expression,
                                           const std::unordered_set<std::string>& region,
                                           const std::unordered_map<std::string, Expr>& regionNodes) {
  if (!expression || expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return std::nullopt;
  const auto& aggregate = expression->constCast<AggregateFunctionExpression>();
  if (aggregate.getFunction().name != "SUM" || aggregate.isDistinct() || aggregate.getNumChildren() != 1)
    return std::nullopt;
  const auto body = aggregate.getChild(0);
  if (body->expressionType != ExpressionType::CASE_ELSE) return std::nullopt;
  const auto& caseExpression = body->constCast<CaseExpression>();
  if (caseExpression.getNumCaseAlternatives() != 1 ||
      !isIntLiteral(caseExpression.getCaseAlternative(0)->thenExpression, 1) ||
      !isIntLiteral(caseExpression.getElseExpression(), 0)) return std::nullopt;
  expression_vector parts;
  conjuncts(caseExpression.getCaseAlternative(0)->whenExpression, parts);
  FilteredCount result;
  for (const auto& part : parts) {
    if (part->expressionType == ExpressionType::IS_NOT_NULL) {
      const auto child = part->getChild(0);
      if (child->expressionType != ExpressionType::PATTERN || !region.contains(child->getUniqueName()))
        return std::nullopt;
      if (!result.counted && regionNodes.contains(child->getUniqueName()))
        result.counted = regionNodes.at(child->getUniqueName());
      continue;
    }
    if (!ExpressionTypeUtil::isComparison(part->expressionType)) return std::nullopt;
    std::unordered_set<std::string> names;
    dependencies(part, names);
    // A comparison over a padded NULL region value is NULL, hence ELSE 0.
    bool touchesRegion = false;
    for (const auto& name : names) {
      if (!region.contains(name)) return std::nullopt;
      touchesRegion = true;
    }
    if (!touchesRegion) return std::nullopt;
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*part))
      if (child->expressionType == ExpressionType::AGGREGATE_FUNCTION ||
          child->expressionType == ExpressionType::SUBQUERY) return std::nullopt;
    result.filters.push_back(part);
    result.key += part->getUniqueName() + "&";
  }
  if (result.filters.empty()) return std::nullopt;
  if (!result.counted) {
    // Any region node is non-NULL exactly on matched OPTIONAL rows.
    for (const auto& [name, node] : regionNodes) {
      if (!result.counted || name < result.counted->getUniqueName()) result.counted = node;
    }
  }
  if (!result.counted) return std::nullopt;
  return result;
}

Expr makeCount(const Expr& counted, const Expr& original, main::ClientContext& context) {
  expression_vector children{counted};
  std::vector<LogicalType> types;
  types.push_back(counted->getDataType().copy());
  auto* entry = context.getCatalog()->getFunctionEntry(context.getTransaction(), "COUNT");
  auto function = function::BuiltInFunctionsUtils::matchAggregateFunction(
      "COUNT", types, false, entry->ptrCast<catalog::FunctionCatalogEntry>())->copy();
  std::unique_ptr<function::FunctionBindData> data;
  if (function.bindFunc) {
    auto input = function::ScalarBindFuncInput{children, &function, &context, {}};
    data = function.bindFunc(input);
  } else {
    data = std::make_unique<function::FunctionBindData>(LogicalType(function.returnTypeID));
  }
  auto result = std::make_shared<AggregateFunctionExpression>(
      std::move(function), std::move(data), std::move(children), original->getUniqueName());
  result->setParsedName(original->getParsedName());
  result->setRawNameVariables(original->getRawNameVariables());
  if (original->hasAlias()) result->setAlias(original->getAlias());
  return result;
}

using ByName = std::unordered_map<std::string, Expr>;

void substitute(Expr& expression, const std::function<Expr(const Expr&)>& find) {
  if (!expression) return;
  if (auto found = find(expression)) { expression = found; return; }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto& c = expression->cast<CaseExpression>();
    for (size_t i = 0; i < c.getNumCaseAlternatives(); ++i) {
      substitute(c.getCaseAlternative(i)->whenExpression, find);
      substitute(c.getCaseAlternative(i)->thenExpression, find);
    }
    auto other = c.getElseExpression();
    substitute(other, find);
    c.setElseExpression(std::move(other));
    return;
  }
  for (size_t i = 0; i < expression->getNumChildren(); ++i) {
    auto child = expression->getChild(i);
    substitute(child, find);
    expression->setChild(i, std::move(child));
  }
}

void rewriteBody(NormalizedQueryPart& part, const std::function<Expr(const Expr&)>& find) {
  if (part.hasProjectionBody()) {
    auto* body = part.getProjectionBodyUnsafe();
    const auto rewrite = [&](expression_vector values) {
      for (auto& value : values) substitute(value, find);
      return values;
    };
    body->setProjectionExpressions(rewrite(body->getProjectionExpressions()));
    body->setAggregateExpressions(rewrite(body->getAggregateExpressions()));
    body->setGroupByExpressions(rewrite(body->getGroupByExpressions()));
    body->setOrderByExpressions(rewrite(body->getOrderByExpressions()), body->getSortingOrders());
  }
  auto predicate = part.getProjectionBodyPredicate();
  substitute(predicate, find);
  part.setProjectionBodyPredicate(predicate);
  for (size_t i = 0; i < part.getNumReadingClause(); ++i) {
    auto* clause = part.getReadingClause(i);
    predicate = clause->getPredicate();
    substitute(predicate, find);
    clause->setPredicate(predicate);
  }
}

bool containsObject(const Expr& expression, const Expression* target) {
  if (!expression) return false;
  if (expression.get() == target) return true;
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    const auto& c = expression->constCast<CaseExpression>();
    for (size_t i = 0; i < c.getNumCaseAlternatives(); ++i)
      if (containsObject(c.getCaseAlternative(i)->whenExpression, target) ||
          containsObject(c.getCaseAlternative(i)->thenExpression, target)) return true;
    return containsObject(c.getElseExpression(), target);
  }
  for (size_t i = 0; i < expression->getNumChildren(); ++i)
    if (containsObject(expression->getChild(i), target)) return true;
  return false;
}

bool partContains(const NormalizedQueryPart& part, const Expression* target) {
  if (part.hasProjectionBody()) {
    const auto* body = part.getProjectionBody();
    for (const auto& values : {body->getProjectionExpressions(), body->getAggregateExpressions(),
                               body->getGroupByExpressions(), body->getOrderByExpressions()})
      for (const auto& value : values) if (containsObject(value, target)) return true;
  }
  if (containsObject(part.getProjectionBodyPredicate(), target)) return true;
  for (size_t i = 0; i < part.getNumReadingClause(); ++i)
    if (containsObject(part.getReadingClause(i)->getPredicate(), target)) return true;
  return false;
}

std::optional<Normalization> normalizePart(NormalizedSingleQuery& query, size_t position,
                                           main::ClientContext& context, Binder& binder) {
  auto* part = query.getQueryPartUnsafe(position);
  if (!part->hasProjectionBody() || part->hasUpdatingClause() || part->getNumReadingClause() == 0)
    return std::nullopt;
  const auto clausePosition = part->getNumReadingClause() - 1;
  auto* clause = part->getReadingClause(clausePosition);
  if (clause->getClauseType() != ClauseType::MATCH) return std::nullopt;
  auto& match = clause->cast<BoundMatchClause>();
  if (match.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH || match.hasHint())
    return std::nullopt;
  // Identities already bound before this OPTIONAL clause.
  std::unordered_set<std::string> bound;
  for (size_t p = 0; p <= position; ++p) {
    const auto* previous = query.getQueryPart(p);
    const auto limit = p == position ? clausePosition : previous->getNumReadingClause();
    for (size_t c = 0; c < limit; ++c) {
      const auto* reading = previous->getReadingClause(c);
      if (reading->getClauseType() == ClauseType::MATCH) {
        const auto* graphs = reading->constCast<BoundMatchClause>().getQueryGraphCollection();
        for (size_t g = 0; g < graphs->getNumQueryGraphs(); ++g) {
          for (const auto& node : graphs->getQueryGraph(g)->getQueryNodes()) bound.insert(node->getUniqueName());
          for (const auto& rel : graphs->getQueryGraph(g)->getQueryRels()) bound.insert(rel->getUniqueName());
        }
      } else if (reading->getClauseType() == ClauseType::UNWIND) {
        bound.insert(reading->constCast<BoundUnwindClause>().getOutExpr()->getUniqueName());
      } else {
        return std::nullopt;
      }
    }
    if (p < position && previous->hasProjectionBody())
      for (const auto& value : previous->getProjectionBody()->getProjectionExpressions())
        bound.insert(value->getUniqueName());
  }
  std::unordered_set<std::string> region;
  std::unordered_map<std::string, Expr> regionNodes;
  const auto* graphs = match.getQueryGraphCollection();
  for (size_t g = 0; g < graphs->getNumQueryGraphs(); ++g) {
    for (const auto& node : graphs->getQueryGraph(g)->getQueryNodes())
      if (!bound.contains(node->getUniqueName())) {
        region.insert(node->getUniqueName());
        regionNodes.emplace(node->getUniqueName(), node);
      }
    for (const auto& rel : graphs->getQueryGraph(g)->getQueryRels())
      if (!bound.contains(rel->getUniqueName())) region.insert(rel->getUniqueName());
  }
  if (regionNodes.empty()) return std::nullopt;
  auto* body = part->getProjectionBodyUnsafe();
  const auto aggregates = body->getAggregateExpressions();
  if (aggregates.empty()) return std::nullopt;
  std::optional<std::string> key;
  std::vector<std::pair<Expr, FilteredCount>> found;
  for (const auto& aggregate : aggregates) {
    auto count = filteredCount(aggregate, region, regionNodes);
    // Every aggregate must see the same filtered region, otherwise moving
    // the filter into OPTIONAL changes the rows another aggregate observes.
    if (!count || (key && *key != count->key)) return std::nullopt;
    key = count->key;
    found.emplace_back(aggregate, std::move(*count));
  }
  // Grouping keys, other outputs and the HAVING-style predicate must not read
  // the region: the rewritten OPTIONAL emits a different number of rows.
  std::unordered_set<std::string> aggregateNames;
  for (const auto& aggregate : aggregates) aggregateNames.insert(aggregate->getUniqueName());
  // A previous WITH output is an input column here even when its bound object
  // still describes that earlier aggregate (e.g. zombieLikeCount in BI13).
  std::unordered_set<std::string> incoming;
  if (position > 0 && query.getQueryPart(position - 1)->hasProjectionBody())
    for (const auto& value : query.getQueryPart(position - 1)->getProjectionBody()->getProjectionExpressions())
      if (!region.contains(value->getUniqueName())) incoming.insert(value->getUniqueName());
  std::function<bool(const Expr&)> independent = [&](const Expr& expression) {
    if (!expression || aggregateNames.contains(expression->getUniqueName()) ||
        incoming.contains(expression->getUniqueName())) return true;
    if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION ||
        expression->expressionType == ExpressionType::SUBQUERY) return false;
    if (expression->expressionType == ExpressionType::PROPERTY ||
        expression->expressionType == ExpressionType::PATTERN) {
      if (region.contains(variableOf(*expression))) return false;
      if (expression->expressionType == ExpressionType::PROPERTY) return true;
    }
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      if (!independent(child)) return false;
    return true;
  };
  for (const auto& values : {body->getProjectionExpressions(), body->getGroupByExpressions(),
                             body->getOrderByExpressions()})
    for (const auto& value : values) if (!independent(value)) return std::nullopt;
  if (!independent(part->getProjectionBodyPredicate())) return std::nullopt;
  if (body->isDistinct()) return std::nullopt;

  Normalization result;
  result.clause = &match;
  result.part = position;
  result.query = &query;
  result.originalPredicate = match.getPredicate();
  Expr filter = result.originalPredicate;
  for (const auto& conjunct : found.front().second.filters)
    filter = binder.getExpressionBinder()->combineBooleanExpressions(ExpressionType::AND, filter, conjunct);
  result.installedPredicate = filter;
  ByName current, later;
  for (const auto& [aggregate, count] : found) {
    auto replacement = makeCount(count.counted, aggregate, context);
    if (replacement->getDataType() != aggregate->getDataType()) return std::nullopt;
    current.emplace(aggregate->getUniqueName(), replacement);
    result.aggregates.push_back({replacement.get(), aggregate});
    auto variable = std::make_shared<VariableExpression>(aggregate->getDataType().copy(),
        aggregate->getUniqueName(), aggregate->toString());
    variable->setParsedName(aggregate->getParsedName());
    if (aggregate->hasAlias()) variable->setAlias(aggregate->getAlias());
    later.emplace(aggregate->getUniqueName(), variable);
  }
  match.setPredicate(filter);
  rewriteBody(*part, [&](const Expr& e) -> Expr {
    const auto it = current.find(e->getUniqueName());
    return it == current.end() ? nullptr : it->second;
  });
  for (size_t p = position + 1; p < query.getNumQueryParts(); ++p) {
    rewriteBody(*query.getQueryPartUnsafe(p), [&](const Expr& e) -> Expr {
      const auto it = later.find(e->getUniqueName());
      if (it == later.end() || e->expressionType == ExpressionType::AGGREGATE_FUNCTION) return nullptr;
      result.downstream.push_back({it->second.get(), e});
      return it->second;
    });
  }
  return result;
}
}  // namespace

struct FilteredCountNormalizations {
  std::vector<Normalization> applied;
};

std::shared_ptr<FilteredCountNormalizations> normalizeFilteredCounts(
    BoundStatement& statement, main::ClientContext& context, Binder* binder) {
  auto result = std::make_shared<FilteredCountNormalizations>();
  if (binder == nullptr || !binder->allowIndexRewrite() || binder->hasIndexRewriteReplay() ||
      statement.getStatementType() != StatementType::QUERY) return result;
  if (context.getCatalog()->getExtendedIndexEntries(context.getTransaction()).empty()) return result;
  auto& regular = statement.cast<BoundRegularQuery>();
  if (!regular.getPreQueryPart().empty() || regular.getPostSingleQuery()) return result;
  for (size_t q = 0; q < regular.getNumSingleQueries(); ++q) {
    auto& query = *regular.getSingleQueryUnsafe(q);
    bool readOnly = true;
    for (size_t p = 0; p < query.getNumQueryParts(); ++p)
      readOnly &= !query.getQueryPart(p)->hasUpdatingClause();
    if (!readOnly) continue;
    for (size_t p = 0; p < query.getNumQueryParts(); ++p) {
      try {
        if (auto applied = normalizePart(query, p, context, *binder))
          result->applied.push_back(std::move(*applied));
      } catch (const std::exception& error) {
        VLOG(1) << "Filtered COUNT normalization skipped: " << error.what();
      }
    }
  }
  return result;
}

void restoreUnusedFilteredCounts(const std::shared_ptr<FilteredCountNormalizations>& state) {
  if (!state) return;
  // Undo in reverse so a later normalization never sees an earlier restore.
  for (auto it = state->applied.rbegin(); it != state->applied.rend(); ++it) {
    auto& n = *it;
    if (n.part >= n.query->getNumQueryParts()) continue;
    auto* part = n.query->getQueryPartUnsafe(n.part);
    bool clausePresent = false;
    for (size_t c = 0; c < part->getNumReadingClause(); ++c)
      clausePresent |= part->getReadingClause(c) == n.clause;
    // Any consumption by an index rewrite replaces at least one installed
    // object. Keep those (semantics-preserving) normalizations.
    if (!clausePresent || n.clause->getPredicate() != n.installedPredicate) continue;
    bool untouched = true;
    for (const auto& replacement : n.aggregates)
      untouched &= partContains(*part, replacement.installed);
    if (!untouched) continue;
    n.clause->setPredicate(n.originalPredicate);
    for (const auto& replacement : n.aggregates) {
      rewriteBody(*part, [&](const Expr& e) -> Expr {
        return e.get() == replacement.installed ? replacement.original : nullptr;
      });
    }
    for (const auto& replacement : n.downstream) {
      for (size_t p = n.part + 1; p < n.query->getNumQueryParts(); ++p)
        rewriteBody(*n.query->getQueryPartUnsafe(p), [&](const Expr& e) -> Expr {
          return e.get() == replacement.installed ? replacement.original : nullptr;
        });
    }
  }
}

}  // namespace neug::binder

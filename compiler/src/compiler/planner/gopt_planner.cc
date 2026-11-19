/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "neug/compiler/planner/gopt_planner.h"
#include <execinfo.h>
#include <yaml-cpp/node/emit.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>
#include <variant>
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/parameter_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/visitor/property_collector.h"
#include "neug/compiler/binder/bound_statement_result.h"
#include "neug/compiler/common/case_insensitive_map.h"
#include "neug/compiler/common/types/value/nested.h"
#include "neug/compiler/gopt/g_alias_manager.h"
#include "neug/compiler/function/gds/rec_joins.h"
#include "neug/compiler/gopt/g_catalog.h"
#include "neug/compiler/gopt/g_physical_convertor.h"
#include "neug/compiler/gopt/g_result_schema.h"
#include "neug/compiler/optimizer/optimizer.h"
#include "neug/compiler/parser/parser.h"
#include "neug/compiler/planner/planner.h"
#include "neug/utils/exception/exception.h"

namespace neug {

namespace {

using binder::BoundMatchClause;
using binder::NormalizedQueryPart;

bool isRegularMatchOnlyPart(const NormalizedQueryPart& part) {
  if (part.hasUpdatingClause() || part.getNumReadingClause() == 0) {
    return false;
  }
  for (auto i = 0u; i < part.getNumReadingClause(); ++i) {
    const auto* clause = part.getReadingClause(i);
    if (clause->getClauseType() != common::ClauseType::MATCH) {
      return false;
    }
    const auto& match = clause->constCast<BoundMatchClause>();
    if (match.getMatchClauseType() != common::MatchClauseType::MATCH ||
        match.hasHint()) {
      return false;
    }
  }
  return true;
}

// A WITH is transparent to a MATCH region when it preserves bag cardinality
// and only carries bound pattern objects forward. In particular, a scalar
// expression cannot be elided here: a later predicate may refer to the
// projected variable and would otherwise require planning that projection.
bool hasTransparentWithBoundary(const NormalizedQueryPart& part) {
  if (!part.hasProjectionBody() || part.hasProjectionBodyPredicate() ||
      part.hasUpdatingClause()) {
    return false;
  }
  const auto* projection = part.getProjectionBody();
  if (projection->isDistinct() || projection->hasAggregateExpressions() ||
      projection->hasOrderByExpressions() || projection->hasSkipOrLimit()) {
    return false;
  }
  const auto expressions = projection->getProjectionExpressions();
  if (expressions.empty()) {
    return false;
  }
  for (const auto& expression : expressions) {
    if (expression->expressionType != common::ExpressionType::PATTERN) {
      return false;
    }
  }
  return true;
}

// Fold a deliberately small, label-only expression language. Unsupported
// expressions stay on the source pipeline; no workload names or constants
// participate in this proof. Every branch is checked, including dead ones.
using LabelConstant = std::variant<bool, double, std::string>;
using LabelBindings = std::unordered_map<std::string, common::table_id_t>;
std::optional<LabelConstant> foldLabelConstant(
    const binder::Expression& expression, const LabelBindings& labels) {
  using E = common::ExpressionType;
  if (expression.expressionType == E::LITERAL) {
    const auto value = expression.constCast<binder::LiteralExpression>().getValue();
    if (value.isNull()) return {};
    switch (value.getDataType().getLogicalTypeID()) {
    case common::LogicalTypeID::BOOL: return value.getValue<bool>();
    case common::LogicalTypeID::DOUBLE: return value.getValue<double>();
    case common::LogicalTypeID::STRING: return value.getValue<std::string>();
    default: return {};
    }
  }
  if (expression.expressionType == E::FUNCTION) {
    const auto& function = expression.constCast<binder::ScalarFunctionExpression>();
    if (function.getFunction().name != "LABEL" || expression.getNumChildren() != 2 ||
        expression.getChild(0)->expressionType != E::PROPERTY ||
        expression.getChild(1)->expressionType != E::LITERAL) return {};
    const auto& property = expression.getChild(0)->constCast<binder::PropertyExpression>();
    if (property.getPropertyName() != common::InternalKeyword::ID) return {};
    const auto found = labels.find(property.getVariableName());
    const auto list = expression.getChild(1)->constCast<binder::LiteralExpression>().getValue();
    if (found == labels.end() || list.isNull() ||
        list.getDataType().getLogicalTypeID() != common::LogicalTypeID::LIST ||
        found->second >= common::NestedVal::getChildrenSize(&list)) return {};
    const auto* value = common::NestedVal::getChildVal(&list, found->second);
    if (value->isNull() || value->getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING)
      return {};
    return value->getValue<std::string>();
  }
  if ((expression.expressionType == E::IS_NULL || expression.expressionType == E::IS_NOT_NULL) &&
      expression.getNumChildren() == 1 &&
      expression.getChild(0)->expressionType == E::PATTERN &&
      labels.contains(expression.getChild(0)->getUniqueName()))
    return expression.expressionType == E::IS_NOT_NULL;
  if (expression.expressionType == E::CASE_ELSE) {
    const auto& branch = expression.constCast<binder::CaseExpression>();
    if (!branch.getElseExpression()) return {};
    auto result = foldLabelConstant(*branch.getElseExpression(), labels);
    if (!result) return {};
    for (size_t i = branch.getNumCaseAlternatives(); i-- > 0;) {
      const auto* alternative = branch.getCaseAlternative(i);
      const auto condition = foldLabelConstant(*alternative->whenExpression, labels);
      const auto value = foldLabelConstant(*alternative->thenExpression, labels);
      if (!condition || !std::holds_alternative<bool>(*condition) || !value ||
          value->index() != result->index()) return {};
      if (std::get<bool>(*condition)) result = value;
    }
    return result;
  }
  if (expression.getNumChildren() != 2) return {};
  const auto left = foldLabelConstant(*expression.getChild(0), labels);
  const auto right = foldLabelConstant(*expression.getChild(1), labels);
  if (!left || !right || left->index() != right->index()) return {};
  switch (expression.expressionType) {
  case E::EQUALS: return *left == *right;
  case E::NOT_EQUALS: return *left != *right;
  case E::OR:
  case E::AND:
    if (!std::holds_alternative<bool>(*left)) return {};
    return expression.expressionType == E::OR
        ? std::get<bool>(*left) || std::get<bool>(*right)
        : std::get<bool>(*left) && std::get<bool>(*right);
  default: return {};
  }
}

bool proveLabelWeights(const binder::Expression& expression,
                       GOptPlanner::SourceDependencies& result) {
  // The runtime supports paths with up to four nodes. Bound the Cartesian
  // label domain so planning never grows with data size.
  size_t combinations = 1;
  for (const auto& node : result.pattern.nodesInfo) {
    if (node.nodeLabels.empty() || node.nodeLabels.size() > 256 / combinations) return false;
    combinations *= node.nodeLabels.size();
  }
  if (result.pattern.nodesInfo.size() > 4) return false;
  std::vector<double> values;
  std::vector<GOptPlanner::SourceDependencies::LabelWeight> weights;
  LabelBindings labels;
  std::vector<common::table_id_t> binding;
  auto visit = [&](auto&& self, size_t position) -> bool {
    if (position == result.pattern.nodesInfo.size()) {
      const auto value = foldLabelConstant(expression, labels);
      if (!value || !std::holds_alternative<double>(*value)) return false;
      const auto number = std::get<double>(*value);
      // Strict positivity makes group presence equivalent to a nonzero sum.
      if (!std::isfinite(number) || number <= 0) return false;
      values.push_back(number);
      weights.push_back({binding, 0});
      return true;
    }
    const auto& node = result.pattern.nodesInfo[position];
    for (const auto label : node.nodeLabels) {
      labels[node.uniqName] = label;
      binding.push_back(label);
      if (!self(self, position + 1)) return false;
      binding.pop_back();
    }
    return true;
  };
  if (!visit(visit, 0)) return false;
  for (int exponent = 0; exponent <= 20; ++exponent) {
    bool exact = true;
    for (size_t i = 0; i < values.size(); ++i) {
      const double scaled = std::ldexp(values[i], exponent);
      if (scaled > double(uint64_t{1} << 52) || scaled != std::floor(scaled)) { exact = false; break; }
      weights[i].units = int64_t(scaled);
    }
    if (exact) {
      result.carrierWeights = std::move(weights);
      result.carrierWeightUnit = std::ldexp(1.0, -exponent);
      return true;
    }
  }
  return false;
}

bool partContainsAnchor(const NormalizedQueryPart& part,
                        const std::string& anchorRelUniqueName) {
  for (auto i = 0u; i < part.getNumReadingClause(); ++i) {
    const auto* clause = part.getReadingClause(i);
    if (clause->getClauseType() != common::ClauseType::MATCH) {
      continue;
    }
    const auto& match = clause->constCast<BoundMatchClause>();
    if (match.getQueryGraphCollection()->contains(anchorRelUniqueName)) {
      for (const auto& rel :
           match.getQueryGraphCollection()->getQueryRels()) {
        if (rel->getUniqueName() == anchorRelUniqueName) {
          return true;
        }
      }
    }
  }
  return false;
}

void mergeMatchPart(const NormalizedQueryPart& part,
                    binder::QueryGraphCollection& collection,
                    binder::expression_vector& predicates) {
  for (auto i = 0u; i < part.getNumReadingClause(); ++i) {
    const auto& match =
        part.getReadingClause(i)->constCast<BoundMatchClause>();
    collection.merge(*match.getQueryGraphCollection());
    auto conjuncts = match.getConjunctivePredicates();
    predicates.insert(predicates.end(), conjuncts.begin(), conjuncts.end());
  }
}

}  // namespace

result<std::pair<physical::PhysicalPlan, std::string>> GOptPlanner::compilePlan(
    const std::string& query) {
  main::ScopedMetadata metadata_scope(database.get());
  VLOG(1) << "[GOptPlanner] compilePlan called with query: " << query;
  // read access to the planner

  if (database->getCatalog() == nullptr) {
    RETURN_ERROR(
        Status(StatusCode::ERR_INVALID_SCHEMA, "Catalog is not initialized"));
  }

  try {
    // Prepare and compile query
    auto statement = ctx->prepare(query);
    if (!statement->success) {
      RETURN_ERROR(Status(StatusCode::ERR_QUERY_SYNTAX, statement->errMsg));
    }

    VLOG(1) << "Logical Plan: " << std::endl
            << statement->logicalPlan->toString() << std::endl;

    if (statement->logicalPlan->emptyResult(
            statement->logicalPlan->getLastOperator())) {
      // If the logical plan results in an empty result,
      // return an empty physical plan.
      return std::make_pair(physical::PhysicalPlan(), std::string(""));
    }

    auto aliasManager =
        std::make_shared<neug::gopt::GAliasManager>(*statement->logicalPlan);
    neug::gopt::GPhysicalConvertor converter(aliasManager,
                                             database->getCatalog());
    auto physicalPlan = converter.convert(*statement->logicalPlan);
    physicalPlan->set_origin_query(query);

    VLOG(10) << "got plan: " << physicalPlan->DebugString();

    // set result schema
    auto resultYaml = gopt::GResultSchema::infer(
        *statement->logicalPlan, aliasManager, database->getCatalog(),
        statement->getStatementResult()->getColumnNames());
    return std::make_pair(std::move(*physicalPlan), YAML::Dump(resultYaml));
  } catch (const neug::exception::InvalidArgumentException& e) {
    // return Status(StatusCode::ERR_INVALID_ARGUMENT, e.what());
    RETURN_ERROR(Status(StatusCode::ERR_INVALID_ARGUMENT, e.what()));
  } catch (const neug::exception::BinderException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_COMPILATION, e.what()));
  } catch (const neug::exception::CatalogException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_INVALID_SCHEMA, e.what()));
  } catch (const neug::exception::ConversionException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_TYPE_CONVERSION, e.what()));
  } catch (const neug::exception::InternalException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_INTERNAL_ERROR, e.what()));
  } catch (const neug::exception::NotImplementedException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_NOT_IMPLEMENTED, e.what()));
  } catch (const neug::exception::NotSupportedException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_NOT_SUPPORTED, e.what()));
  } catch (const neug::exception::RuntimeError& e) {
    RETURN_ERROR(Status(StatusCode::ERR_INTERNAL_ERROR, e.what()));
  } catch (const neug::exception::TransactionManagerException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_INTERNAL_ERROR, e.what()));
  } catch (const neug::exception::OverflowException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_TYPE_OVERFLOW, e.what()));
  } catch (const neug::exception::PropertyNotFoundException& e) {
    RETURN_ERROR(Status(StatusCode::ERR_PROPERTY_NOT_FOUND, e.what()));
  } catch (const neug::exception::Exception& e) {
    RETURN_ERROR(Status(StatusCode::ERR_COMPILATION, e.what()));
  } catch (const std::exception& e) {
    RETURN_ERROR(Status(StatusCode::ERR_COMPILATION, e.what()));
  } catch (...) {
    RETURN_ERROR(Status(StatusCode::ERR_UNKNOWN,
                        "Unknown error during plan "
                        "compilation"));
  }
}

GOptPlanner::SourceDependencies GOptPlanner::materializedSourceDependencies(
    const std::string& query, size_t keyCount) {
  main::ScopedMetadata metadata_scope(database.get());
  auto statements = parser::Parser::parseQuery(query);
  auto binder = binder::Binder(ctx.get());
  binder.setAllowIndexRewrite(false);
  auto bound = binder.bind(*statements.at(0));
  auto& regular = bound->cast<binder::BoundRegularQuery>();
  std::vector<common::PatternIR> positive, negative;
  std::unordered_set<std::string> properties;
  std::function<void(std::shared_ptr<binder::Expression>)> collectProperties;
  collectProperties = [&](std::shared_ptr<binder::Expression> expression) {
    if (!expression)
      return;
    if (expression->expressionType == common::ExpressionType::PROPERTY) {
      auto name =
          expression->constCast<binder::PropertyExpression>().getPropertyName();
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      properties.insert(std::move(name));
      return;
    }
    // Carrying a node or counting it reads its identity, not every payload.
    if (expression->expressionType == common::ExpressionType::PATTERN)
      return;
    if (expression->expressionType == common::ExpressionType::SUBQUERY) {
      const auto& sub = expression->constCast<binder::SubqueryExpression>();
      if (sub.hasWhereExpression())
        collectProperties(sub.getWhereExpression());
      return;
    }
    for (const auto& child :
         binder::ExpressionChildrenCollector::collectChildren(*expression))
      collectProperties(child);
  };
  auto append = [&](const binder::QueryGraphCollection& graphs, auto& output) {
    for (size_t i = 0; i < graphs.getNumQueryGraphs(); ++i)
      output.push_back(
          common::convertGraphPatternToIR(*graphs.getQueryGraph(i)));
    for (const auto& rel : graphs.getQueryRels())
      if (rel->isRecursive()) {
        collectProperties(rel->getRecursiveInfo()->nodePredicate);
        collectProperties(rel->getRecursiveInfo()->relPredicate);
      }
  };
  std::function<void(std::shared_ptr<binder::Expression>)> nested;
  nested = [&](std::shared_ptr<binder::Expression> expression) {
    if (!expression)
      return;
    collectProperties(expression);
    binder::SubqueryExprCollector visitor;
    visitor.visit(expression);
    for (const auto& item : visitor.getSubqueryExprs()) {
      const auto& sub = item->constCast<binder::SubqueryExpression>();
      append(*sub.getQueryGraphCollection(), negative);
      if (sub.hasWhereExpression())
        nested(sub.getWhereExpression());
    }
  };
  for (uint32_t qi = 0; qi < regular.getNumSingleQueries(); ++qi) {
    const auto* single = regular.getSingleQuery(qi);
    for (uint32_t pi = 0; pi < single->getNumQueryParts(); ++pi) {
      const auto* part = single->getQueryPart(pi);
      for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
        const auto* clause = part->getReadingClause(ci);
        if (clause->getClauseType() == common::ClauseType::MATCH)
          append(*clause->constCast<binder::BoundMatchClause>()
                      .getQueryGraphCollection(),
                 positive);
        if (clause->hasPredicate())
          nested(clause->getPredicate());
      }
      if (part->hasProjectionBody())
        for (const auto& expression :
             part->getProjectionBody()->getProjectionExpressions())
          nested(expression);
      if (part->hasProjectionBodyPredicate())
        nested(part->getProjectionBodyPredicate());
    }
  }
  std::vector<std::vector<uint32_t>> remaps;
  SourceDependencies result;
  result.singleQueryPart = regular.getNumSingleQueries() == 1 &&
      regular.getSingleQuery(0)->getNumQueryParts() == 1;
  result.pattern = common::PatternIR::merge(positive, remaps);
  result.positiveNodes = result.pattern.nodesInfo.size();
  result.positiveRels = result.pattern.relsInfo.size();
  positive.insert(positive.end(), negative.begin(), negative.end());
  remaps.clear();
  result.pattern = common::PatternIR::merge(positive, remaps);
  auto columns = regular.getStatementResult()->getColumns();
  for (size_t key = 0; key < keyCount; ++key) {
    auto owner = columns.at(key)
                     ->constCast<binder::PropertyExpression>()
                     .getVariableName();
    result.keys.push_back(result.pattern.nodeUniqNameToPos.at(owner));
  }
  result.properties = std::move(properties);

  // A complete structural proof, independent of query/table/property names.
  // More complex predicates, DISTINCT, extra grouping and optional matches
  // retain source-query recomputation.
  if (keyCount == 1 && result.singleQueryPart && negative.empty() &&
      result.pattern.nodesInfo.size() == 2 && result.pattern.relsInfo.size() == 1 &&
      columns.size() == 2) {
    const auto* part = regular.getSingleQuery(0)->getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    const auto& rel = result.pattern.relsInfo.front();
    bool plain = isRegularMatchOnlyPart(*part) && part->getNumReadingClause() == 1 && body &&
        !part->hasProjectionBodyPredicate() && !body->isDistinct() &&
        !body->hasSkipOrLimit() && !body->hasOrderByExpressions() &&
        rel.relType == common::QueryRelType::NON_RECURSIVE &&
        rel.edgeType != common::EdgeType::UNDIRECTED && rel.srcNodeIdx != rel.dstNodeIdx &&
        columns[0]->constCast<binder::PropertyExpression>().isPrimaryKey() &&
        columns[1]->expressionType == common::ExpressionType::AGGREGATE_FUNCTION;
    if (plain) {
      const auto& aggregate = columns[1]->constCast<binder::AggregateFunctionExpression>();
      plain = !aggregate.isDistinct() && aggregate.getFunction().name == "COUNT" &&
          aggregate.getNumChildren() == 1 &&
          aggregate.getChild(0)->expressionType == common::ExpressionType::PATTERN &&
          result.pattern.nodeUniqNameToPos.contains(aggregate.getChild(0)->getUniqueName());
      if (plain && part->getReadingClause(0)->hasPredicate()) {
        const auto predicate = part->getReadingClause(0)->getPredicate();
        if (predicate->expressionType == common::ExpressionType::LESS_THAN &&
            predicate->getNumChildren() == 2 &&
            predicate->getChild(0)->expressionType == common::ExpressionType::PROPERTY) {
          const auto& property = predicate->getChild(0)->constCast<binder::PropertyExpression>();
          const auto owner = result.pattern.nodeUniqNameToPos.find(property.getVariableName());
          std::optional<std::string> before;
          const auto& cutoff = predicate->getChild(1);
          if (cutoff->expressionType == common::ExpressionType::FUNCTION) {
            const auto& literal = cutoff->constCast<binder::ScalarFunctionExpression>();
            if (literal.getFunction().name == "TIMESTAMP" && literal.getNumChildren() == 1 &&
                literal.getChild(0)->expressionType == common::ExpressionType::LITERAL) {
              const auto value = literal.getChild(0)->constCast<binder::LiteralExpression>().getValue();
              if (!value.isNull() && value.getDataType().getLogicalTypeID() == common::LogicalTypeID::STRING)
                before = value.getValue<std::string>();
            }
          } else if (cutoff->expressionType == common::ExpressionType::LITERAL) {
            const auto value = cutoff->constCast<binder::LiteralExpression>().getValue();
            if (!value.isNull() &&
                (value.getDataType().getLogicalTypeID() == common::LogicalTypeID::TIMESTAMP_MS ||
                 (value.getDataType().getLogicalTypeID() == common::LogicalTypeID::TIMESTAMP &&
                  value.getValue<common::timestamp_t>().value % 1000 == 0)))
              before = value.toString();
          }
          if (owner != result.pattern.nodeUniqNameToPos.end() &&
              owner->second != result.keys.front() && before)
              result.filteredNodeCount = SourceDependencies::FilteredNodeCount{
                  owner->second, property.getPropertyName(), *before};
        }
      }
    }
    if (std::getenv("NEUG_FILL_MAINTENANCE_PROFILE")) {
      LOG(INFO) << "FilteredCountProof eligible=" << bool(result.filteredNodeCount)
                << " columns=" << columns.size() << " plain=" << plain;
      if (part->getNumReadingClause() == 1 && part->getReadingClause(0)->hasPredicate()) {
        const auto p = part->getReadingClause(0)->getPredicate();
        LOG(INFO) << "FilteredCountPredicate " << p->toString()
                  << " type=" << int(p->expressionType)
                  << " rhs_type=" << (p->getNumChildren()==2 ? int(p->getChild(1)->expressionType) : -1);
      }
    }
  }

  // Reject subqueries and arbitrary scalar functions (including random
  // values). Filtering by ordinary property/literal comparisons is local.
  std::function<bool(const std::shared_ptr<binder::Expression>&)> localPredicate;
  localPredicate = [&](const std::shared_ptr<binder::Expression>& expression) {
    const auto type = expression->expressionType;
    if (type == common::ExpressionType::PROPERTY || type == common::ExpressionType::LITERAL) return true;
    if (type == common::ExpressionType::FUNCTION) {
      const auto& function = expression->constCast<binder::ScalarFunctionExpression>();
      if (function.getFunction().name == "*" && function.getNumChildren() == 2)
        return localPredicate(function.getChild(0)) && localPredicate(function.getChild(1));
      return function.getFunction().name == "TIMESTAMP" && function.getNumChildren() == 1 &&
             function.getChild(0)->expressionType == common::ExpressionType::LITERAL;
    }
    if (!common::ExpressionTypeUtil::isBoolean(type) &&
        !common::ExpressionTypeUtil::isComparison(type) &&
        !common::ExpressionTypeUtil::isNullOperator(type)) return false;
    for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(*expression))
      if (!localPredicate(child)) return false;
    return true;
  };
  if (keyCount == 1 && result.singleQueryPart && negative.empty() &&
      result.pattern.nodesInfo.size() == 2 && result.pattern.relsInfo.size() == 1) {
    const auto* part = regular.getSingleQuery(0)->getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    const auto& rel = result.pattern.relsInfo.front();
    bool plain = isRegularMatchOnlyPart(*part) && part->getNumReadingClause() == 1 && body &&
        !part->hasProjectionBodyPredicate() && !body->isDistinct() &&
        !body->hasSkipOrLimit() && !body->hasOrderByExpressions() &&
        rel.relType == common::QueryRelType::NON_RECURSIVE &&
        rel.edgeType != common::EdgeType::UNDIRECTED && columns.size() > 1;
    bool floatingSum = false;
    for (size_t i = 1; plain && i < columns.size(); ++i) {
      if (columns[i]->expressionType != common::ExpressionType::AGGREGATE_FUNCTION) {
        plain = false;
        break;
      }
      const auto& aggregate = columns[i]->constCast<binder::AggregateFunctionExpression>();
      plain = !aggregate.isDistinct() && aggregate.getNumChildren() == 1;
      if (!plain) break;
      const auto& argument = aggregate.getChild(0);
      if (aggregate.getFunction().name == "COUNT") {
        plain = argument->expressionType == common::ExpressionType::PATTERN &&
                argument->getUniqueName() == rel.uniqName;
      } else if (aggregate.getFunction().name == "SUM") {
        plain = argument->expressionType == common::ExpressionType::PROPERTY &&
                argument->constCast<binder::PropertyExpression>().getVariableName() == rel.uniqName &&
                aggregate.getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE;
        floatingSum |= plain;
      } else plain = false;
    }
    if (plain && part->getReadingClause(0)->hasPredicate())
      plain = localPredicate(part->getReadingClause(0)->getPredicate());
    result.partitionedNodeAggregate = plain && floatingSum;
  }
  // A two-edge cycle still owns every joined row by its projected endpoint.
  // Both parallel-edge multiplicities and cross-edge comparisons stay inside
  // one worker, using the original source pipeline.
  if (keyCount == 1 && result.singleQueryPart && negative.empty() &&
      result.pattern.nodesInfo.size() == 2 && result.pattern.relsInfo.size() == 2) {
    const auto* part = regular.getSingleQuery(0)->getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    bool plain = isRegularMatchOnlyPart(*part) && part->getNumReadingClause() == 1 && body &&
        !part->hasProjectionBodyPredicate() && !body->isDistinct() &&
        !body->hasSkipOrLimit() && !body->hasOrderByExpressions() && columns.size() > 1;
    for (const auto& rel : result.pattern.relsInfo)
      plain &= rel.relType == common::QueryRelType::NON_RECURSIVE &&
               rel.edgeType != common::EdgeType::UNDIRECTED && rel.srcNodeIdx != rel.dstNodeIdx;
    bool floatingSum = false;
    for (size_t i = 1; plain && i < columns.size(); ++i) {
      if (columns[i]->expressionType != common::ExpressionType::AGGREGATE_FUNCTION) { plain = false; break; }
      const auto& aggregate = columns[i]->constCast<binder::AggregateFunctionExpression>();
      plain = !aggregate.isDistinct() && aggregate.getNumChildren() == 1;
      if (!plain) break;
      const auto& argument = aggregate.getChild(0);
      if (aggregate.getFunction().name == "COUNT")
        plain = argument->expressionType == common::ExpressionType::PATTERN &&
                result.pattern.relUniqueNameToPos.contains(argument->getUniqueName());
      else if (aggregate.getFunction().name == "SUM") {
        plain = argument->expressionType == common::ExpressionType::PROPERTY &&
                result.pattern.relUniqueNameToPos.contains(
                    argument->constCast<binder::PropertyExpression>().getVariableName()) &&
                aggregate.getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE;
        floatingSum |= plain;
      } else plain = false;
    }
    if (plain && part->getReadingClause(0)->hasPredicate())
      plain = localPredicate(part->getReadingClause(0)->getPredicate());
    result.partitionedNodeAggregate = plain && floatingSum;
  }
  // DISTINCT neighbor counts and two-hop derived aggregates are also local
  // to the first projected key. Keep each complete group in one worker;
  // neither floating partial sums nor DISTINCT sets cross partitions.
  if (result.singleQueryPart && negative.empty() &&
      ((keyCount == 1 && result.pattern.nodesInfo.size() == 2 &&
        result.pattern.relsInfo.size() == 1) ||
       (keyCount == 2 && result.pattern.nodesInfo.size() == 3 &&
        result.pattern.relsInfo.size() == 2))) {
    const auto* part = regular.getSingleQuery(0)->getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    bool plain = isRegularMatchOnlyPart(*part) && part->getNumReadingClause() == 1 &&
        body && !part->hasProjectionBodyPredicate() && !body->isDistinct() &&
        !body->hasSkipOrLimit() && !body->hasOrderByExpressions() &&
        columns.size() == keyCount + 1;
    std::unordered_set<size_t> connected;
    for (const auto& rel : result.pattern.relsInfo) {
      plain &= rel.relType == common::QueryRelType::NON_RECURSIVE &&
               rel.edgeType != common::EdgeType::UNDIRECTED &&
               rel.srcNodeIdx != rel.dstNodeIdx;
      connected.insert(rel.srcNodeIdx);
      connected.insert(rel.dstNodeIdx);
    }
    plain &= connected.size() == result.pattern.nodesInfo.size();
    if (plain && part->getReadingClause(0)->hasPredicate())
      plain = localPredicate(part->getReadingClause(0)->getPredicate());
    if (plain && columns.back()->expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      const auto& aggregate = columns.back()->constCast<binder::AggregateFunctionExpression>();
      plain = aggregate.getNumChildren() == 1;
      if (plain) {
        const auto& argument = aggregate.getChild(0);
        if (aggregate.getFunction().name == "COUNT")
          plain = aggregate.isDistinct() &&
                  argument->expressionType == common::ExpressionType::PATTERN &&
                  result.pattern.nodeUniqNameToPos.contains(argument->getUniqueName());
        else if (keyCount == 2 && aggregate.getFunction().name == "SUM")
          plain = !aggregate.isDistinct() &&
                  argument->expressionType == common::ExpressionType::PROPERTY &&
                  aggregate.getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE;
        else plain = false;
      }
    } else plain = false;
    if (keyCount == 1) result.partitionedNodeAggregate |= plain;
    else result.partitionedPairAggregate = plain;
  }
  // DISTINCT WITH boundaries remain separable if every boundary carries
  // the source node. Each worker then owns all deduplication and aggregation
  // for its source keys, including a bounded recursive prefix.
  if (keyCount == 1 && !result.singleQueryPart && regular.getNumSingleQueries() == 1 &&
      negative.empty() && result.pattern.nodesInfo.size() == 3 && result.pattern.relsInfo.size() == 2 &&
      std::count_if(result.pattern.relsInfo.begin(), result.pattern.relsInfo.end(),
          [](const auto& rel) { return rel.relType != common::QueryRelType::NON_RECURSIVE; }) == 1) {
    const auto* single = regular.getSingleQuery(0);
    const auto& source = result.pattern.nodesInfo.at(result.keys.front()).uniqName;
    bool plain = columns.size() > 1, floatingSum = false;
    for (uint32_t p = 0; plain && p < single->getNumQueryParts(); ++p) {
      const auto* part = single->getQueryPart(p);
      const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
      plain = !part->hasUpdatingClause() && body && !body->hasSkipOrLimit() &&
              !body->hasOrderByExpressions() && !part->hasProjectionBodyPredicate() &&
              (!part->getNumReadingClause() || isRegularMatchOnlyPart(*part));
      if (!plain) break;
      if (p + 1 < single->getNumQueryParts()) {
        bool carriesSource = false;
        for (const auto& expression : body->getProjectionExpressions()) {
          plain &= expression->expressionType == common::ExpressionType::PATTERN;
          carriesSource |= expression->getUniqueName() == source;
        }
        plain &= carriesSource && !body->hasAggregateExpressions();
      } else plain &= !body->isDistinct();
      for (uint32_t c = 0; plain && c < part->getNumReadingClause(); ++c) {
        const auto* clause = part->getReadingClause(c);
        if (clause->hasPredicate()) plain &= localPredicate(clause->getPredicate());
        const auto* graphs = clause->constCast<binder::BoundMatchClause>().getQueryGraphCollection();
        for (const auto& rel : graphs->getQueryRels()) if (rel->isRecursive()) {
          const auto info = rel->getRecursiveInfo();
          if (info->nodePredicate) plain &= localPredicate(info->nodePredicate);
          if (info->relPredicate) plain &= localPredicate(info->relPredicate);
        }
      }
    }
    for (size_t i = 1; plain && i < columns.size(); ++i) {
      if (columns[i]->expressionType != common::ExpressionType::AGGREGATE_FUNCTION) { plain = false; break; }
      const auto& aggregate = columns[i]->constCast<binder::AggregateFunctionExpression>();
      plain = !aggregate.isDistinct() && aggregate.getNumChildren() == 1;
      if (!plain) break;
      const auto& argument = aggregate.getChild(0);
      if (aggregate.getFunction().name == "COUNT")
        plain = argument->expressionType == common::ExpressionType::PATTERN;
      else if (aggregate.getFunction().name == "SUM") {
        plain = argument->expressionType == common::ExpressionType::PROPERTY &&
                aggregate.getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE;
        floatingSum |= plain;
      } else plain = false;
    }
    result.partitionedNodeAggregate = plain && floatingSum;
  }
  if (keyCount == 2 && result.singleQueryPart && columns.size() == 3 && negative.empty()) {
    const auto* part = regular.getSingleQuery(0)->getQueryPart(0);
    const auto* body = part->hasProjectionBody() ? part->getProjectionBody() : nullptr;
    bool plain = isRegularMatchOnlyPart(*part) && part->getNumReadingClause() == 1 &&
        body && !part->getReadingClause(0)->hasPredicate() && !part->hasProjectionBodyPredicate() &&
        !body->isDistinct() && !body->hasSkipOrLimit() && !body->hasOrderByExpressions() &&
        columns[2]->expressionType == common::ExpressionType::AGGREGATE_FUNCTION;
    if (plain) {
      const auto& count = columns[2]->constCast<binder::AggregateFunctionExpression>();
      plain = count.getFunction().name == "COUNT" && count.isDistinct() && count.getNumChildren() == 1 &&
          count.getChild(0)->expressionType == common::ExpressionType::PATTERN &&
          result.pattern.nodeUniqNameToPos.contains(count.getChild(0)->getUniqueName());
      if (plain) {
        const auto* graphs = part->getReadingClause(0)->constCast<binder::BoundMatchClause>().getQueryGraphCollection();
        size_t recursive = 0;
        std::optional<SourceDependencies::RecursiveDistinctCount> shape;
        for (const auto& rel : graphs->getQueryRels()) {
          if (!rel->isRecursive()) continue;
          ++recursive;
          const auto info = rel->getRecursiveInfo();
          const auto type = result.pattern.relsInfo[result.pattern.relUniqueNameToPos.at(rel->getUniqueName())].relType;
          if (!info->nodePredicate && !info->relPredicate && info->bindData->lowerBound == 0 &&
              (type == common::QueryRelType::VARIABLE_LENGTH_WALK ||
               type == common::QueryRelType::VARIABLE_LENGTH_TRAIL ||
               type == common::QueryRelType::VARIABLE_LENGTH_ACYCLIC))
            shape = SourceDependencies::RecursiveDistinctCount{
                result.pattern.nodeUniqNameToPos.at(count.getChild(0)->getUniqueName()),
                result.pattern.relUniqueNameToPos.at(rel->getUniqueName()), info->bindData->upperBound};
        }
        if (recursive == 1) result.recursiveDistinctCount = shape;
      }
    }
  }
  if (keyCount == 2 && regular.getNumSingleQueries() == 1 &&
      regular.getSingleQuery(0)->getNumQueryParts() == 2 &&
      columns.size() == 3 && negative.empty()) {
    const auto* single = regular.getSingleQuery(0);
    const auto* first = single->getQueryPart(0);
    const auto* last = single->getQueryPart(1);
    bool plain = hasTransparentWithBoundary(*first) &&
                 isRegularMatchOnlyPart(*first) &&
                 isRegularMatchOnlyPart(*last) &&
                 first->getNumReadingClause() == 1 &&
                 last->getNumReadingClause() == 1 &&
                 last->hasProjectionBody() &&
                 !last->hasProjectionBodyPredicate();
    for (const auto* part : {first, last}) {
      if (part->getNumReadingClause() != 1 || !part->hasProjectionBody()) {
        plain = false;
        continue;
      }
      const auto* projection = part->getProjectionBody();
      plain &= !part->getReadingClause(0)->hasPredicate() &&
               !projection->isDistinct() && !projection->hasSkipOrLimit() &&
               !projection->hasOrderByExpressions();
    }
    if (plain) {
      const auto* graph = first->getReadingClause(0)
                              ->constCast<binder::BoundMatchClause>()
                              .getQueryGraphCollection();
      const auto carry = first->getProjectionBody()->getProjectionExpressions();
      const auto& aggregate = columns[2];
      plain = graph->getQueryRels().size() == 1 &&
              graph->getQueryNodes().size() == 2 && carry.size() == 2 &&
              aggregate->expressionType == common::ExpressionType::AGGREGATE_FUNCTION;
      if (plain) {
        const auto& count = aggregate->constCast<binder::AggregateFunctionExpression>();
        plain = !count.isDistinct() && count.getNumChildren() == 1;
        if (plain) {
          plain = (count.getFunction().name == "COUNT" &&
                   count.getChild(0)->expressionType == common::ExpressionType::PATTERN &&
                   result.pattern.nodeUniqNameToPos.contains(count.getChild(0)->getUniqueName())) ||
                  (count.getFunction().name == "SUM" && proveLabelWeights(*count.getChild(0), result));
        }
        std::set<std::string> carried;
        for (const auto& expression : carry) carried.insert(expression->getUniqueName());
        plain &= carried == std::set<std::string>{
            result.pattern.nodesInfo[result.keys[0]].uniqName,
            result.pattern.nodesInfo[result.keys[1]].uniqName};
        if (plain) {
          const auto name = graph->getQueryRels().front()->getUniqueName();
          for (size_t i = 0; i < result.pattern.relsInfo.size(); ++i)
            if (result.pattern.relsInfo[i].uniqName == name)
              result.carrierCountRel = i;
        }
      }
    }
    // A path COUNT grouped by the two endpoints before joining the carrier.
    // Preserve that boundary: duplicate carrier rows repeat the result, they
    // do not multiply the already computed count.
    bool grouped = !result.carrierCountRel && isRegularMatchOnlyPart(*first) &&
        isRegularMatchOnlyPart(*last) && first->getNumReadingClause() == 1 &&
        last->getNumReadingClause() == 1 && first->hasProjectionBody() &&
        last->hasProjectionBody() && !first->hasProjectionBodyPredicate() &&
        !last->hasProjectionBodyPredicate();
    for (const auto* part : {first, last}) {
      if (part->getNumReadingClause() != 1 || !part->hasProjectionBody()) { grouped = false; continue; }
      const auto* body = part->getProjectionBody();
      grouped &= !part->getReadingClause(0)->hasPredicate() && !body->isDistinct() &&
                 !body->hasSkipOrLimit() && !body->hasOrderByExpressions();
    }
    if (grouped) {
      const auto carry = first->getProjectionBody()->getProjectionExpressions();
      const auto* graph = last->getReadingClause(0)->constCast<binder::BoundMatchClause>().getQueryGraphCollection();
      grouped = carry.size() == 3 && graph->getQueryRels().size() == 1 &&
          graph->getQueryNodes().size() == 2 && !last->getProjectionBody()->hasAggregateExpressions();
      std::set<std::string> keys;
      size_t aggregates = 0;
      for (const auto& expression : carry) {
        if (expression->expressionType == common::ExpressionType::PATTERN)
          keys.insert(expression->getUniqueName());
        else if (expression->expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
          ++aggregates;
          const auto& count = expression->constCast<binder::AggregateFunctionExpression>();
          grouped &= count.getFunction().name == "COUNT" && !count.isDistinct() &&
              count.getNumChildren() == 1 &&
              count.getChild(0)->expressionType == common::ExpressionType::PATTERN &&
              result.pattern.nodeUniqNameToPos.contains(count.getChild(0)->getUniqueName()) &&
              columns[2]->getUniqueName() == expression->getUniqueName();
        } else grouped = false;
      }
      grouped &= aggregates == 1 && keys == std::set<std::string>{
          result.pattern.nodesInfo[result.keys[0]].uniqName,
          result.pattern.nodesInfo[result.keys[1]].uniqName};
      if (grouped) {
        result.carrierCountRel = result.pattern.relUniqueNameToPos.at(graph->getQueryRels().front()->getUniqueName());
        result.carrierAfterAggregate = true;
        result.carrierWeights.clear();
      }
    }
  }
  return result;
}

result<physical::PhysicalPlan> GOptPlanner::compileMaterializedSource(
    const std::string& query, bool bagProof,
    const std::vector<std::string>& keyLabels) {
  return compileSourceImpl(query, bagProof, keyLabels, false);
}

result<physical::PhysicalPlan> GOptPlanner::compileKeyedMaterializedSource(
    const std::string& query, bool bagProof,
    const std::vector<std::string>& keyLabels) {
  return compileSourceImpl(query, bagProof, keyLabels, true);
}

result<physical::PhysicalPlan> GOptPlanner::compileSourceImpl(
    const std::string& query, bool bagProof,
    const std::vector<std::string>& keyLabels, bool keyed) {
  main::ScopedMetadata metadata_scope(database.get());
  try {
    auto parsed = parser::Parser::parseQuery(query);
    if (parsed.size() != 1 ||
        parsed[0]->getStatementType() != common::StatementType::QUERY)
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "A FILL source must be a single read query");
    auto binder = binder::Binder(ctx.get());
    binder.setAllowIndexRewrite(false);
    auto bound = binder.bind(*parsed[0]);
    if (!keyLabels.empty()) {
      auto& regular = bound->cast<binder::BoundRegularQuery>();
      const auto columns = regular.getStatementResult()->getColumns();
      if (columns.size() < keyLabels.size())
        THROW_INVALID_ARGUMENT_EXCEPTION("FILL key projection is incomplete");
      for (size_t key = 0; key < keyLabels.size(); ++key) {
        if (columns[key]->expressionType != common::ExpressionType::PROPERTY)
          THROW_INVALID_ARGUMENT_EXCEPTION(
              "FILL keys must retain node lineage");
        const auto owner = columns[key]
                               ->constCast<binder::PropertyExpression>()
                               .getVariableName();
        auto* target = ctx->getCatalog()->getTableCatalogEntry(
            ctx->getTransaction(), keyLabels[key]);
        bool found = false;
        for (uint32_t si = 0; si < regular.getNumSingleQueries(); ++si) {
          auto* single = regular.getSingleQueryUnsafe(si);
          for (uint32_t pi = 0; pi < single->getNumQueryParts(); ++pi) {
            auto* part = single->getQueryPartUnsafe(pi);
            for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
              auto* clause = part->getReadingClause(ci);
              if (clause->getClauseType() != common::ClauseType::MATCH)
                continue;
              auto* graphs = clause->cast<binder::BoundMatchClause>()
                                 .getQueryGraphCollectionUnsafe();
              for (const auto& node : graphs->getQueryNodes())
                if (node->getUniqueName() == owner) {
                  if (std::none_of(
                          node->getEntries().begin(), node->getEntries().end(),
                          [&](const auto* e) {
                            return e->getTableID() == target->getTableID();
                          }))
                    THROW_INVALID_ARGUMENT_EXCEPTION(
                        "FILL endpoint label does not belong to its source "
                        "key");
                  node->setEntries({target});
                  if (keyed && key == 0 && !found) {
                    if (pi != 0)
                      THROW_INVALID_ARGUMENT_EXCEPTION(
                          "Keyed source requires the carrier in its initial "
                          "MATCH region");
                    auto parameter =
                        std::make_shared<binder::ParameterExpression>(
                            "__neug_source_keys",
                            common::Value::createNullValue(
                                common::LogicalType::LIST(
                                    columns[0]->getDataType().copy())));
                    // Predicate pushdown replaces property aliases with the
                    // current vertex. Do not mutate the RETURN expression.
                    auto keyExpression =
                        std::shared_ptr<binder::Expression>(columns[0]->copy());
                    auto filter =
                        binder.getExpressionBinder()
                            ->bindScalarFunctionExpression(
                                {parameter, keyExpression}, "LIST_CONTAINS");
                    if (clause->hasPredicate())
                      filter =
                          binder.getExpressionBinder()->bindBooleanExpression(
                              common::ExpressionType::AND,
                              {clause->getPredicate(), filter});
                    clause->setPredicate(filter);
                    auto& match = clause->cast<binder::BoundMatchClause>();
                    auto hint =
                        std::make_shared<binder::BoundJoinHintNode>(node);
                    std::unordered_set<std::string> boundNodes{
                        node->getUniqueName()},
                        usedRels;
                    auto relationships = graphs->getQueryRels();
                    while (usedRels.size() < relationships.size()) {
                      bool extended = false;
                      for (const auto& rel : relationships) {
                        if (usedRels.count(rel->getUniqueName()))
                          continue;
                        bool source = boundNodes.count(rel->getSrcNodeName()),
                             destination =
                                 boundNodes.count(rel->getDstNodeName());
                        if (!source && !destination)
                          continue;
                        if (source && destination) {
                          auto sourceNode =
                              std::make_shared<binder::BoundJoinHintNode>(
                                  rel->getSrcNode());
                          auto edge =
                              std::make_shared<binder::BoundJoinHintNode>(rel);
                          edge->expandE = true;
                          edge->addChild(sourceNode);
                          auto destinationNode =
                              std::make_shared<binder::BoundJoinHintNode>(
                                  rel->getDstNode());
                          destinationNode->getV = true;
                          destinationNode->addChild(edge);
                          auto join =
                              std::make_shared<binder::BoundJoinHintNode>();
                          join->addChild(hint);
                          join->addChild(destinationNode);
                          hint = join;
                        } else {
                          auto edge =
                              std::make_shared<binder::BoundJoinHintNode>(rel);
                          edge->expandE = true;
                          edge->addChild(hint);
                          auto neighbor =
                              source ? rel->getDstNode() : rel->getSrcNode();
                          auto vertex =
                              std::make_shared<binder::BoundJoinHintNode>(
                                  neighbor);
                          vertex->getV = true;
                          vertex->addChild(edge);
                          hint = vertex;
                          boundNodes.insert(neighbor->getUniqueName());
                        }
                        usedRels.insert(rel->getUniqueName());
                        extended = true;
                      }
                      if (!extended)
                        THROW_RUNTIME_ERROR("Disconnected keyed MATCH source");
                    }
                    hint->carrierKeyAnchor = true;
                    match.setHint(hint);
                  }
                  found = true;
                }
            }
          }
        }
        if (!found)
          THROW_INVALID_ARGUMENT_EXCEPTION(
              "Cannot resolve FILL source key lineage");
      }
    }
    if (bagProof) {
      auto& regular = bound->cast<binder::BoundRegularQuery>();
      if (regular.getNumSingleQueries() != 1)
        THROW_INVALID_ARGUMENT_EXCEPTION("FILL bag proof requires one query");
      auto* single = regular.getSingleQueryUnsafe(0);
      if (single->getNumQueryParts() != 1)
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "FILL edge bag proof requires one query part");
      auto* part = single->getQueryPartUnsafe(0);
      auto* old = part->getProjectionBody();
      if (old->hasAggregateExpressions() || old->hasSkipOrLimit())
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "FILL edge bag proof requires a plain projection");
      auto replacement = binder::BoundProjectionBody(false);
      replacement.setProjectionExpressions(old->getProjectionExpressions());
      replacement.setOrderByExpressions(old->getOrderByExpressions(),
                                        old->getSortingOrders());
      part->setProjectionBody(std::move(replacement));
    }
    auto planner = planner::Planner(ctx.get());
    auto plan = planner.getBestPlan(*bound);
    optimizer::Optimizer::optimize(plan.get(), ctx.get(),
                                   planner.getCardinalityEstimator());
    auto aliases = std::make_shared<gopt::GAliasManager>(*plan);
    auto converter = gopt::GPhysicalConvertor(aliases, database->getCatalog());
    auto physical = converter.convert(*plan);
    physical->set_origin_query(query);
    if (keyed) {
      std::function<bool(const google::protobuf::Message&)> hasLookup;
      hasLookup = [&](const google::protobuf::Message& message) {
        if (const auto* scan = dynamic_cast<const physical::Scan*>(&message))
          if (scan->has_idx_predicate())
            for (const auto& group : scan->idx_predicate().or_predicates())
              for (const auto& predicate : group.predicates())
                if (predicate.has_param() &&
                    predicate.param().name() == "__neug_source_keys")
                  return true;
        const auto* reflection = message.GetReflection();
        std::vector<const google::protobuf::FieldDescriptor*> fields;
        reflection->ListFields(message, &fields);
        for (const auto* field : fields)
          if (field->cpp_type() ==
              google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            if (field->is_repeated()) {
              for (int i = 0; i < reflection->FieldSize(message, field); ++i)
                if (hasLookup(
                        reflection->GetRepeatedMessage(message, field, i)))
                  return true;
            } else if (hasLookup(reflection->GetMessage(message, field)))
              return true;
          }
        return false;
      };
      if (!hasLookup(*physical)) {
        LOG(ERROR) << physical->DebugString();
        THROW_RUNTIME_ERROR(
            "Keyed FILL plan lost its carrier primary-key lookup");
      }
    }
    return std::move(*physical);
  } catch (const std::exception& error) {
    RETURN_ERROR(Status(StatusCode::ERR_COMPILATION, error.what()));
  }
}

result<AnchoredMatchPlan> GOptPlanner::compileAnchoredMatchPlan(
    const std::string& sourceQuery,
    const std::string& anchorRelUniqueName,
    uint64_t anchorCardinality) {
  main::ScopedMetadata metadata_scope(database.get());
  if (database->getCatalog() == nullptr) {
    RETURN_ERROR(
        Status(StatusCode::ERR_INVALID_SCHEMA, "Catalog is not initialized"));
  }
  if (sourceQuery.empty()) {
    RETURN_ERROR(Status(StatusCode::ERR_INVALID_ARGUMENT,
                        "FILL source query is empty (legacy metadata cannot "
                        "be compiled as an anchored MATCH plan)"));
  }
  if (anchorRelUniqueName.empty()) {
    RETURN_ERROR(Status(StatusCode::ERR_INVALID_ARGUMENT,
                        "Anchored relationship unique name is empty"));
  }

  const char* stage = "init";
  try {
    auto parsedStatements = parser::Parser::parseQuery(sourceQuery);
    if (parsedStatements.size() != 1 ||
        parsedStatements[0]->getStatementType() !=
            common::StatementType::QUERY) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Saved FILL source must contain exactly one read query");
    }

    // Bind the exact saved source while explicitly suppressing index
    // substitution. Semantic normalization (including merging MATCH clauses
    // within one QueryPart) remains enabled.
    stage = "bind";
    auto binder = binder::Binder(ctx.get());
    binder.setAllowIndexRewrite(false);
    auto boundStatement = binder.bind(*parsedStatements[0]);
    auto& regularQuery = boundStatement->cast<binder::BoundRegularQuery>();
    if (!regularQuery.getPreQueryPart().empty() ||
        regularQuery.getPostSingleQuery() != nullptr ||
        regularQuery.getNumSingleQueries() != 1) {
      THROW_NOT_SUPPORTED_EXCEPTION(
          "Anchored FILL planning currently requires one non-UNION query");
    }

    const auto* singleQuery = regularQuery.getSingleQuery(0);
    const auto numParts = singleQuery->getNumQueryParts();
    uint32_t anchorPart = std::numeric_limits<uint32_t>::max();
    for (auto i = 0u; i < numParts; ++i) {
      if (!partContainsAnchor(*singleQuery->getQueryPart(i),
                              anchorRelUniqueName)) {
        continue;
      }
      if (anchorPart != std::numeric_limits<uint32_t>::max()) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Anchored relationship occurs in more than one QueryPart: " +
            anchorRelUniqueName);
      }
      anchorPart = i;
    }
    if (anchorPart == std::numeric_limits<uint32_t>::max()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Anchored relationship was not found after re-binding saved FILL "
          "source: " +
          anchorRelUniqueName);
    }
    if (!isRegularMatchOnlyPart(*singleQuery->getQueryPart(anchorPart))) {
      THROW_NOT_SUPPORTED_EXCEPTION(
          "The QueryPart containing the anchor is not a regular MATCH-only "
          "region");
    }

    // Grow to the maximal region through transparent WITH boundaries. A
    // boundary belongs to the preceding QueryPart.
    auto firstPart = anchorPart;
    while (firstPart > 0 &&
           isRegularMatchOnlyPart(
               *singleQuery->getQueryPart(firstPart - 1)) &&
           hasTransparentWithBoundary(
               *singleQuery->getQueryPart(firstPart - 1))) {
      --firstPart;
    }
    auto lastPart = anchorPart;
    while (lastPart + 1 < numParts &&
           hasTransparentWithBoundary(
               *singleQuery->getQueryPart(lastPart)) &&
           isRegularMatchOnlyPart(
               *singleQuery->getQueryPart(lastPart + 1))) {
      ++lastPart;
    }

    auto collection = binder::QueryGraphCollection();
    auto predicates = binder::expression_vector();
    for (auto i = firstPart; i <= lastPart; ++i) {
      mergeMatchPart(*singleQuery->getQueryPart(i), collection, predicates);
    }
    const auto anchor = [&]() -> std::shared_ptr<binder::RelExpression> {
      for (const auto& rel : collection.getQueryRels()) {
        if (rel->getUniqueName() == anchorRelUniqueName) {
          return rel;
        }
      }
      return nullptr;
    }();
    NEUG_ASSERT(anchor != nullptr);
    if (anchor->isRecursive()) {
      THROW_NOT_SUPPORTED_EXCEPTION(
          "A recursive relationship cannot currently be used as an anchored "
          "event occurrence");
    }
    if (anchor->getDirectionType() == binder::RelDirectionType::BOTH) {
      THROW_NOT_SUPPORTED_EXCEPTION(
          "An undirected relationship requires two oriented anchored plans "
          "and is not supported yet");
    }

    // Collect property dependencies from the complete source query, including
    // terminal contribution expressions. Keep only properties owned by this
    // region's pattern occurrences. This makes the detached region plan expose
    // the same property data that later maintenance/contribution stages need,
    // without executing the source RETURN/aggregation here.
    auto propertyCollector = binder::PropertyCollector();
    propertyCollector.visitSingleQuery(*singleQuery);
    auto requiredProperties = binder::expression_vector();
    for (const auto& property : propertyCollector.getProperties()) {
      const auto& propertyExpression =
          property->constCast<binder::PropertyExpression>();
      if (collection.contains(propertyExpression.getVariableName())) {
        requiredProperties.push_back(property);
      }
    }

    stage = "plan";
    auto logicalPlanner = planner::Planner(ctx.get());
    std::unique_ptr<planner::LogicalPlan> logicalPlan;
    try {
      logicalPlan = logicalPlanner.planAnchoredQueryGraphCollection(
          collection, predicates, anchorRelUniqueName, anchorCardinality,
          requiredProperties);
    } catch (const std::exception& e) {
      void* frames[24];
      const int n = backtrace(frames, 24);
      auto symbols = backtrace_symbols(frames, n);
      std::ostringstream stack;
      for (int i = 0; i < n && symbols != nullptr; ++i) {
        stack << "\n  " << symbols[i];
      }
      free(symbols);
      LOG(INFO) << "Anchored PLAN-stage exception: " << e.what() << stack.str();
      throw;
    }
    // Keep the pattern identities: the anchored plan must expose every
    // relationship occurrence for ownership (repeated labels), so skip the
    // expand-chain fusion rules that drop intermediate columns.
    stage = "optimize";
    optimizer::Optimizer::optimize(logicalPlan.get(), ctx.get(),
                                   logicalPlanner.getCardinalityEstimator(),
                                   /*preservePatternIdentities=*/true);

    stage = "alias";
    // The anchored complete-binding projection references every pattern
    // element; keep their aliases so the WCO intersect can expose the
    // build-side relationships as edge columns for ownership.
    auto aliasManager =
        std::make_shared<gopt::GAliasManager>(*logicalPlan,
                                              /*preservePatternAliases=*/true);

    stage = "convert";
    auto converter =
        gopt::GPhysicalConvertor(aliasManager, database->getCatalog());
    auto physicalPlan = converter.convert(*logicalPlan);

    // Temporary diagnostic (NEUG_ANCHORED_DIAGNOSTIC=1): log both the
    // full-build-style order for the same source query and the anchored
    // order, so the two execution orders can be compared side by side.
    if (std::getenv("NEUG_ANCHORED_DIAGNOSTIC") != nullptr) {
      auto diagPlanner = planner::Planner(ctx.get());
      auto normalPlans = diagPlanner.planQuery(regularQuery);
      auto normalPlan = diagPlanner.getBestPlan(std::move(normalPlans));
      VLOG(1) << "[AnchoredDiagnostic] full-build logical plan:\n"
              << normalPlan->toString();
      VLOG(1) << "[AnchoredDiagnostic] anchored logical plan:\n"
              << logicalPlan->toString();
    }

    AnchoredMatchPlan result;
    result.firstQueryPart = firstPart;
    result.lastQueryPart = lastPart;
    for (const auto& node : collection.getQueryNodes()) {
      if (aliasManager->hasValidAliasId(node->getUniqueName())) {
        result.nodeAliasIds.emplace(node->getUniqueName(),
                                    aliasManager->getAliasId(
                                        node->getUniqueName()));
      }
    }
    for (const auto& rel : collection.getQueryRels()) {
      if (aliasManager->hasValidAliasId(rel->getUniqueName())) {
        result.relAliasIds.emplace(rel->getUniqueName(),
                                   aliasManager->getAliasId(
                                       rel->getUniqueName()));
      }
    }
    result.physicalPlan = std::move(*physicalPlan);
    result.logicalPlan = std::move(logicalPlan);
    return result;
  } catch (const std::exception& e) {
    {
      void* frames[24];
      const int n = backtrace(frames, 24);
      auto symbols = backtrace_symbols(frames, n);
      std::ostringstream stack;
      for (int i = 0; i < n && symbols != nullptr; ++i) {
        stack << "\n  " << symbols[i];
      }
      free(symbols);
      LOG(INFO) << "Anchored compile exception at stage " << stage
               << ": " << e.what() << stack.str();
    }
    RETURN_ERROR(Status(StatusCode::ERR_COMPILATION, e.what()));
  } catch (...) {
    RETURN_ERROR(Status(StatusCode::ERR_UNKNOWN,
                        "Unknown error during anchored MATCH compilation"));
  }
}

void GOptPlanner::update_meta(const YAML::Node& schema_yaml_node) {
  main::ScopedMetadata metadata_scope(database.get());
  VLOG(1) << "[GOptPlanner] update_meta called";
  if (schema_yaml_node.IsNull()) {
    LOG(ERROR) << "Schema YAML node is null";
    return;
  }
  if (!schema_yaml_node.IsMap()) {
    LOG(ERROR) << "Schema YAML node is not a map";
    return;
  }
  database->updateSchema(schema_yaml_node);
}

void GOptPlanner::update_statistics(const std::string& graph_statistic_json) {
  main::ScopedMetadata metadata_scope(database.get());
  VLOG(1) << "[GOptPlanner] update_statistics called";
  if (graph_statistic_json.empty()) {
    LOG(ERROR) << "Graph statistics JSON is empty";
    return;
  }
  database->updateStats(graph_statistic_json);
}

bool isTokenEnd(char ch) {
  return ch == ' ' || ch == ';' || ch == '\n' || ch == '\t' || ch == '{' ||
         ch == '(';
}

AccessMode GOptPlanner::analyzeMode(const std::string& query) const {
  size_t i = 0;
  const size_t n = query.size();

  while (i < n) {
    while (i < n && isTokenEnd(query[i]))
      ++i;
    if (i >= n)
      break;

    // mark the start pos of current token
    size_t token_start = i;
    bool invalid_token = false;

    // scan the token until a non-alphabetic character or an end character
    while (i < n) {
      char c = query[i];
      if (std::isalpha(static_cast<unsigned char>(c))) {
        ++i;
      } else if (isTokenEnd(c)) {
        break;
      } else {
        invalid_token = true;  // non-alphabetic character found, we need to
                               // skip the current token
        break;
      }
    }

    // if the token is invalid, skip to the next valid token
    if (invalid_token) {
      while (i < n && !isTokenEnd(query[i]))
        ++i;
      ++i;
      continue;
    }

    std::string token(query.data() + token_start, i - token_start);

    if (getSchemaOpTokens().contains(token)) {
      return AccessMode::kSchema;
    }

    if (getUpdateOpTokens().contains(token)) {
      return AccessMode::kUpdate;
    }

    ++i;
  }

  return AccessMode::kRead;
}

const common::case_insensitve_set_t& GOptPlanner::getUpdateOpTokens() const {
  static common::case_insensitve_set_t updateOps = {
      "set", "copy", "checkpoint", "load", "install", "uninstall", "call"};
  return updateOps;
}

const common::case_insensitve_set_t& GOptPlanner::getSchemaOpTokens() const {
  static common::case_insensitve_set_t schemaOps = {"create", "delete", "drop",
                                                    "alter", "rename"};
  return schemaOps;
}
}  // namespace neug

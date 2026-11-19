/**
 * Copyright 2026 Alibaba Group Holding Limited.
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
#include "neug/compiler/index/bound_index_candidate_adapter.h"
#include "neug/compiler/index/computation_prefix_enumerator.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/parameter_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/common/types/value/nested.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/parser.h"

namespace neug::binder {
std::unique_ptr<BoundRegularQuery> Binder::bindForIndexEnumeration(
    const parser::Statement& statement) {
  if (statement.getStatementType() != common::StatementType::QUERY)
    throw std::invalid_argument(
        "Index enumeration accepts query statements only");
  return bindQuery(statement);
}
}  // namespace neug::binder

namespace neug::index {
namespace {
std::string upper(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::vector<std::string> labels(const binder::NodeOrRelExpression& e) {
  std::vector<std::string> out;
  for (auto* entry : e.getEntries())
    out.push_back(entry->getName());
  std::sort(out.begin(), out.end());
  return out;
}
struct Convert {
  const Constraints& constraints;
  const std::set<std::string>& inputs;
  QueryPart& part;

  Expression expression(const std::shared_ptr<binder::Expression>& e) {
    using common::ExpressionType;
    if (!e)
      throw std::invalid_argument("Missing bound expression");
    const auto type = e->getDataType().toString();
    // A previous projection is an input value, not a local computation to
    // expand again. Node identities remain available for property access.
    if (inputs.contains(e->getUniqueName()) &&
        e->expressionType != ExpressionType::PATTERN)
      return Expression::variable(e->getUniqueName(), type);
    switch (e->expressionType) {
    case ExpressionType::LITERAL: {
      const auto* lit = e->constPtrCast<binder::LiteralExpression>();
      const auto value = lit->getValue();
      if (!lit->isNull() && e->getDataType().getLogicalTypeID() == common::LogicalTypeID::LIST) {
        std::vector<Expression> elements;
        for (uint32_t i = 0; i < value.getChildrenSize(); ++i) {
          auto child = std::make_shared<binder::LiteralExpression>(
              *common::NestedVal::getChildVal(&value, i), "index-list-literal");
          elements.push_back(expression(child));
        }
        return Expression::call("LIST_CREATION", std::move(elements), type);
      }
      std::string text = lit->isNull() ? "NULL" : value.toString();
      if (!lit->isNull() && e->getDataType().getLogicalTypeID() ==
                                common::LogicalTypeID::DOUBLE) {
        std::ostringstream os;
        os << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value.getValue<double>();
        text = os.str();
      } else if (!lit->isNull() && e->getDataType().getLogicalTypeID() ==
                                       common::LogicalTypeID::FLOAT) {
        std::ostringstream os;
        os << std::setprecision(std::numeric_limits<float>::max_digits10)
           << value.getValue<float>();
        text = os.str();
      }
      auto out = Expression::literal(text, type);
      out.nullable = lit->isNull();
      return out;
    }
    case ExpressionType::PARAMETER:
      return Expression::parameter(
          e->constPtrCast<binder::ParameterExpression>()->getName(), type);
    case ExpressionType::PROPERTY: {
      const auto* p = e->constPtrCast<binder::PropertyExpression>();
      return Expression::property(p->getVariableName(), p->getPropertyName(),
                                  type,
                                  !(p->isPrimaryKey() || p->isInternalID()));
    }
    case ExpressionType::VARIABLE:
    case ExpressionType::PATTERN:
      return Expression::variable(e->getUniqueName(), type);
    case ExpressionType::AGGREGATE_FUNCTION: {
      const auto* a = e->constPtrCast<binder::AggregateFunctionExpression>();
      std::vector<Expression> args;
      for (const auto& c : a->getChildren())
        args.push_back(expression(c));
      auto name = upper(a->getFunction().name);
      if (name == "COUNT_STAR") {
        name = "COUNT";
        args.clear();
      }
      return Expression::aggregate(name, std::move(args), type,
                                   a->isDistinct());
    }
    case ExpressionType::CASE_ELSE: {
      const auto* c = e->constPtrCast<binder::CaseExpression>();
      std::vector<Expression> children;
      for (size_t i = 0; i < c->getNumCaseAlternatives(); ++i) {
        children.push_back(
            expression(c->getCaseAlternative(i)->whenExpression));
        children.push_back(
            expression(c->getCaseAlternative(i)->thenExpression));
      }
      children.push_back(expression(c->getElseExpression()));
      return Expression::call("CASE", std::move(children), type);
    }
    case ExpressionType::SUBQUERY: {
      const auto* sub = e->constPtrCast<binder::SubqueryExpression>();
      const auto* graph = sub->getQueryGraphCollection();
      bool supported = sub->getSubqueryType() == common::SubqueryType::EXISTS &&
                       !sub->hasWhereExpression();
      for (const auto& rel : graph->getQueryRels())
        supported &= !rel->isRecursive();
      if (!supported)
        break;
      const auto correlated = [&](const auto& binding) {
        return inputs.contains(binding) || std::any_of(part.nodes.begin(), part.nodes.end(),
            [&](const auto& node) { return node.binding == binding; });
      };
      const auto rels = graph->getQueryRels();
      const auto subNodes = graph->getQueryNodes();
      if (rels.size() == 1 && !rels[0]->getEntries().empty() &&
          correlated(rels[0]->getSrcNodeName()) && correlated(rels[0]->getDstNodeName()) &&
          std::all_of(subNodes.begin(), subNodes.end(),
              [&](const auto& node) { return node->getUniqueName() == rels[0]->getSrcNodeName() ||
                                            node->getUniqueName() == rels[0]->getDstNodeName(); })) {
        Expression exists;
        exists.kind = ExpressionKind::PatternExists;
        exists.type = "BOOL";
        exists.nullable = false;
        exists.name = rels[0]->getDirectionType() == binder::RelDirectionType::SINGLE
                          ? "DIRECTED" : "UNDIRECTED";
        for (const auto& binding : {rels[0]->getSrcNodeName(), rels[0]->getDstNodeName()}) {
          std::vector<Expression> fields{Expression::variable(binding)};
          for (const auto& node : subNodes)
            if (node->getUniqueName() == binding)
              for (const auto& label : labels(*node))
                fields.push_back(Expression::literal(label, "STRING"));
          exists.children.push_back(Expression::call("PATTERN_NODE", std::move(fields), "NODE"));
        }
        for (const auto& type : labels(*rels[0]))
          exists.children.push_back(Expression::literal(type, "STRING"));
        return exists;
      }
      Expression out;
      out.kind = ExpressionKind::ResidualPredicate;
      out.name = sub->toString();
      out.type = type;
      out.safeToPrecompute = false;
      for (const auto& node : graph->getQueryNodes()) {
        const auto& binding = node->getUniqueName();
        if (inputs.contains(binding) ||
            std::any_of(part.nodes.begin(), part.nodes.end(),
                [&](const auto& n) { return n.binding == binding; }))
          out.children.push_back(Expression::variable(binding));
      }
      return out;
    }
    default:
      break;
    }
    const bool scalarFunction =
        e->expressionType == ExpressionType::FUNCTION ||
        common::ExpressionTypeUtil::isBoolean(e->expressionType) ||
        common::ExpressionTypeUtil::isComparison(e->expressionType) ||
        common::ExpressionTypeUtil::isNullOperator(e->expressionType);
    if (!scalarFunction) {
      part.unsupportedReason =
          "Unsupported bound expression kind: " +
          common::ExpressionTypeUtil::toString(e->expressionType);
      auto out = Expression::literal("NULL", type);
      out.deterministic = false;
      return out;
    }
    const auto* f = e->constPtrCast<binder::ScalarFunctionExpression>();
    auto name = upper(f->getFunction().name);
    if (!f->getFunction().isReadOnly) {
      part.unsupportedReason = "Scalar function has side effects: " + name;
      auto out = Expression::literal("NULL", type);
      out.deterministic = false;
      return out;
    }
    // The binder lowers LABEL(n) to LABEL(n._ID, catalog-label-array).
    // Restore its graph binding instead of treating catalog metadata as a
    // user list literal, which cannot be emitted as a scalar index column.
    if (name == "LABEL" && e->getNumChildren() == 2 &&
        e->getChild(0)->expressionType == ExpressionType::PROPERTY &&
        e->getChild(1)->expressionType == ExpressionType::LITERAL) {
      const auto* id =
          e->getChild(0)->constPtrCast<binder::PropertyExpression>();
      if (id->isInternalID() &&
          e->getChild(1)->getDataType().getLogicalTypeID() ==
              common::LogicalTypeID::LIST) {
        const auto& binding = id->getVariableName();
        for (const auto& node : part.nodes)
          if (node.binding == binding)
            return Expression::call(
                name, {Expression::variable(binding, "NODE")}, type);
        for (const auto& rel : part.relationships)
          if (rel.binding == binding)
            return Expression::call(
                name, {Expression::variable(binding, "REL")}, type);
      }
    }
    const std::map<std::string, std::string> operators = {
        {"EQUALS", "="},        {"NOT_EQUALS", "<>"},
        {"GREATER_THAN", ">"},  {"GREATER_THAN_EQUALS", ">="},
        {"LESS_THAN", "<"},     {"LESS_THAN_EQUALS", "<="},
        {"IS_NULL", "IS NULL"}, {"IS_NOT_NULL", "IS NOT NULL"}};
    if (auto it = operators.find(name); it != operators.end())
      name = it->second;
    const std::set<std::string> builtins = {
        "AND",         "OR",        "XOR",     "NOT",   "=",      "<>",
        "!=",          "<",         ">",       "<=",    ">=",     "IS NULL",
        "IS NOT NULL", "COALESCE",  "LOWER",   "UPPER", "LENGTH", "CHAR_LENGTH",
        "STARTS_WITH", "ENDS_WITH", "CONTAINS", "LIST_CONTAINS",
        "LIST_CREATION"};
    const std::set<std::string> arithmetic = {"+", "-", "*", "NEGATE", "ABS"};
    const std::set<std::string> partial = {"/", "%", "CAST", "DATE_PART",
                                           "LIST_EXTRACT"};
    const bool known = builtins.contains(name) || arithmetic.contains(name) ||
                       partial.contains(name);
    const bool deterministic =
        known || constraints.deterministicFunctions.contains(name);
    bool total =
        builtins.contains(name) ||
        (arithmetic.contains(name) && constraints.exactArithmetic) ||
        constraints.totalFunctions.contains(name);
    std::vector<Expression> children;
    for (const auto& child : e->getChildren())
      children.push_back(expression(child));
    if (name == "CAST" && !children.empty()) {
      const std::map<std::string, int> signedWidth = {
          {"INT8", 8}, {"INT16", 16}, {"INT32", 32}, {"INT64", 64}, {"INT128", 128}};
      const auto from = upper(children[0].type), to = upper(type);
      total |= from == to ||
          (signedWidth.contains(from) && signedWidth.contains(to) &&
           signedWidth.at(from) <= signedWidth.at(to)) ||
          (to == "DOUBLE" && (signedWidth.contains(from) || from == "FLOAT"));
      // The binder omits the target-type literal for implicit casts.
      if (children.size() == 1)
        children.push_back(Expression::literal(type, "STRING"));
    }
    // DATE_PART can reject a dynamic field name. These constant fields are
    // valid for the bound temporal overloads used by BI queries.
    if (name == "DATE_PART" && children.size() == 2 &&
        children[0].kind == ExpressionKind::Literal && !children[0].nullable &&
        (upper(children[0].name) == "YEAR" || upper(children[0].name) == "MONTH"))
      total = true;
    auto out = Expression::call(name, std::move(children), type, total);
    out.deterministic = deterministic;
    if (!deterministic)
      part.unsupportedReason =
          "Unknown or non-deterministic scalar function: " + name;
    return out;
  }
};
std::set<std::string> inputColumns(const binder::NormalizedQueryPart& part) {
  std::set<std::string> out;
  if (part.hasProjectionBody())
    for (const auto& e : part.getProjectionBody()->getProjectionExpressions())
      out.insert(e->getUniqueName());
  return out;
}
std::map<std::string, std::string> projectedNames(
    const binder::NormalizedQueryPart& part) {
  std::map<std::string, std::string> out;
  if (!part.hasProjectionBody())
    return out;
  const auto* projection = part.getProjectionBody();
  const auto expressions = projection->getProjectionExpressions();
  const auto& names = projection->getProjectionNames();
  if (names.size() != expressions.size())
    return out;
  for (size_t i = 0; i < expressions.size(); ++i)
    if (!names[i].empty())
      out.try_emplace(expressions[i]->getUniqueName(), names[i]);
  return out;
}
QueryPart extractPart(const binder::NormalizedQueryPart& bound,
                      const Constraints& constraints,
                      const std::set<std::string>& inputs, bool singleton,
                      const std::string& source,
                      const std::vector<CarrierDomain>& domains = {},
                      const std::map<std::string, std::string>& inputNames = {}) {
  QueryPart out;
  out.source = source;
  // Ordinary NeuG MATCH uses homomorphic edge bindings. TRAIL uniqueness is
  // an explicit recursive-path option, not an implicit pairwise constraint
  // between non-recursive edge occurrences. Keep all occurrences in an
  // induced region, but do not invent edge-inequality predicates on reads.
  out.forbidEdgeReuse = false;
  out.inputBindings = inputs;
  out.singletonEmptyInput = singleton;
  out.carrierDomains = domains;
  Convert converter{constraints, inputs, out};
  if (bound.hasUpdatingClause())
    out.unsupportedReason = "Updating QueryParts are not enumerated";
  std::map<std::string, size_t> nodes;
  std::set<std::string> relationships;
  auto boundNames = inputs;
  for (size_t clause = 0; clause < bound.getNumReadingClause(); ++clause) {
    const auto* reading = bound.getReadingClause(clause);
    if (reading->getClauseType() != common::ClauseType::MATCH) {
      out.unsupportedReason = "Only MATCH reading clauses are supported";
      continue;
    }
    const auto* match = reading->constPtrCast<binder::BoundMatchClause>();
    MatchClause block;
    block.position = clause;
    block.optional =
        match->getMatchClauseType() != common::MatchClauseType::MATCH;
    block.boundBefore = boundNames;
    const auto* collection = match->getQueryGraphCollection();
    for (const auto& n : collection->getQueryNodes()) {
      const auto name = n->getUniqueName();
      out.bindingNames[name] = n->getVariableName();
      if (!block.boundBefore.contains(name))
        block.introduced.insert(name);
      boundNames.insert(name);
      // Binding an unlabelled occurrence before a labelled one can leave its
      // node expression with every table in the catalog. A typed incident
      // relationship still constrains the endpoint domain. Intersect catalog
      // endpoint domains, without assuming any data-dependent cardinality.
      auto entries = n->getEntries();
      for (const auto& r : collection->getQueryRels()) {
        if (r->isRecursive() ||
            (r->getSrcNodeName() != name && r->getDstNodeName() != name))
          continue;
        std::set<common::table_id_t> allowed;
        for (auto* entry : r->getEntries()) {
          const auto* rel = entry->constPtrCast<catalog::RelTableCatalogEntry>();
          if (r->getSrcNodeName() == name ||
              r->getDirectionType() != binder::RelDirectionType::SINGLE)
            allowed.insert(rel->getSrcTableID());
          if (r->getDstNodeName() == name ||
              r->getDirectionType() != binder::RelDirectionType::SINGLE)
            allowed.insert(rel->getDstTableID());
        }
        std::erase_if(entries, [&](const auto* entry) {
          return !allowed.contains(entry->getTableID());
        });
      }
      Node node;
      node.binding = n->getUniqueName();
      for (const auto* entry : entries)
        node.labels.push_back(entry->getName());
      std::sort(node.labels.begin(), node.labels.end());
      if (entries.size() == 1) {
        const auto primary =
            n->getPrimaryKey(entries.front()->getTableID());
        if (primary &&
            primary->expressionType == common::ExpressionType::PROPERTY) {
          node.primaryKey = primary->constPtrCast<binder::PropertyExpression>()
                                ->getPropertyName();
          node.primaryKeyType = primary->getDataType().toString();
        }
      }
      if (entries.size() != 1) {
        node.primaryKey.clear();
        node.primaryKeyType.clear();
      }
      if (nodes.contains(name)) {
        const auto index = nodes.at(name);
        block.nodes.push_back(index);
        // An OPTIONAL constraint cannot narrow an already bound outer node.
        if (!block.optional && node.labels.size() == 1 &&
            std::find(out.nodes[index].labels.begin(),
                      out.nodes[index].labels.end(), node.labels.front()) !=
                out.nodes[index].labels.end())
          out.nodes[index] = std::move(node);
        continue;
      }
      nodes[node.binding] = out.nodes.size();
      block.nodes.push_back(out.nodes.size());
      out.nodes.push_back(std::move(node));
    }
    for (const auto& r : collection->getQueryRels()) {
      if (!relationships.insert(r->getUniqueName()).second) {
        out.unsupportedReason =
            "Repeated relationship bindings are not supported";
        continue;
      }
      Relationship rel;
      rel.binding = r->getUniqueName();
      out.bindingNames[rel.binding] = r->getVariableName();
      rel.types = labels(*r);
      rel.source = nodes.at(r->getSrcNodeName());
      rel.destination = nodes.at(r->getDstNodeName());
      rel.directed = r->getDirectionType() == binder::RelDirectionType::SINGLE;
      rel.matchGroup = clause;
      rel.recursive = r->isRecursive();
      if (!rel.recursive) {
        // The binder keeps all tables of a relationship label even when the
        // endpoint labels exclude some of them. Source cardinality contracts
        // apply to these possible tables, not to unrelated endpoint domains.
        const auto endpointTables = [&](const auto& node, size_t position) {
          std::set<common::table_id_t> tables;
          for (const auto* entry : node->getEntries())
            if (std::find(out.nodes[position].labels.begin(),
                          out.nodes[position].labels.end(), entry->getName()) !=
                out.nodes[position].labels.end())
              tables.insert(entry->getTableID());
          return tables;
        };
        const auto sources = endpointTables(r->getSrcNode(), rel.source);
        const auto destinations =
            endpointTables(r->getDstNode(), rel.destination);
        rel.types.clear();
        for (const auto* entry : r->getEntries()) {
          const auto* typed =
              entry->constPtrCast<catalog::RelTableCatalogEntry>();
          const bool forward = sources.contains(typed->getSrcTableID()) &&
                               destinations.contains(typed->getDstTableID());
          const bool reverse = !rel.directed &&
                               sources.contains(typed->getDstTableID()) &&
                               destinations.contains(typed->getSrcTableID());
          if (forward || reverse)
            rel.types.push_back(entry->getName());
        }
        std::sort(rel.types.begin(), rel.types.end());
        if (rel.types.empty())
          out.unsupportedReason =
              "Relationship endpoint labels have no compatible table";
      }
      if (rel.recursive) {
        const auto* info = r->getRecursiveInfo();
        if (!info || !info->bindData || info->nodePredicate ||
            info->relPredicate ||
            r->getRelType() != common::QueryRelType::VARIABLE_LENGTH_WALK) {
          out.unsupportedReason =
              "R8 requires a WALK recursion without internal predicates";
        } else {
          rel.lowerBound = info->bindData->lowerBound;
          rel.upperBound = info->bindData->upperBound;
          if (rel.upperBound == std::numeric_limits<uint32_t>::max())
            rel.upperBound = std::numeric_limits<uint64_t>::max();
        }
      }
      if (!block.boundBefore.contains(rel.binding))
        block.introduced.insert(rel.binding);
      boundNames.insert(rel.binding);
      block.relationships.push_back(out.relationships.size());
      out.relationships.push_back(std::move(rel));
    }
    if (reading->hasPredicate()) {
      auto predicate = converter.expression(reading->getPredicate());
      // Keep correlated OPTIONAL conditions on their match boundary. The
      // planner preserves outer-only filters through ID scans and pattern
      // reuse; the core still rejects predicates requiring hidden variables.
      // Runtime parameters can filter a complete local summary online, but
      // must never specialize the materialized source.
      block.predicates.push_back(predicate);
      if (!block.optional)
        out.predicates.push_back(std::move(predicate));
    }
    out.clauses.push_back(std::move(block));
  }
  if (!bound.hasProjectionBody()) {
    out.unsupportedReason = "QueryPart has no projection body";
    return out;
  }
  const auto* projection = bound.getProjectionBody();
  out.distinct = projection->isDistinct();
  for (const auto& e : projection->getGroupByExpressions())
    out.groupBy.push_back(converter.expression(e));
  for (const auto& e : projection->getAggregateExpressions())
    out.aggregates.push_back(converter.expression(e));
  for (const auto& e : projection->getProjectionExpressions())
    out.outputs.push_back(converter.expression(e));
  for (const auto& e : projection->getOrderByExpressions())
    out.postExpressions.push_back(converter.expression(e));
  if (projection->hasSkip())
    out.postExpressions.push_back(
        converter.expression(projection->getSkipNumber()));
  if (projection->hasLimit())
    out.postExpressions.push_back(
        converter.expression(projection->getLimitNumber()));
  if (bound.hasProjectionBodyPredicate())
    out.postExpressions.push_back(
        converter.expression(bound.getProjectionBodyPredicate()));
  // A carried graph identity keeps the alias from the immediately preceding
  // projection. Its original variable spelling may now name a different node.
  for (const auto& [binding, name] : inputNames)
    out.bindingNames[binding] = name;
  return out;
}
std::vector<CarrierDomain> retainedDomains(
    const QueryPart& part, const std::set<std::string>& outputs) {
  auto domains = part.carrierDomains;
  for (const auto& clause : part.clauses) {
    if (clause.optional)
      continue;
    for (auto pos : clause.relationships) {
      const auto& r = part.relationships[pos];
      if (!r.recursive && r.directed)
        domains.push_back({part.nodes[r.source].binding,
                           part.nodes[r.destination].binding, r.types, true});
    }
  }
  std::erase_if(domains, [&](const auto& d) {
    return !outputs.contains(d.source) || !outputs.contains(d.destination);
  });
  for (auto& domain : domains) {
    std::sort(domain.types.begin(), domain.types.end());
    domain.types.erase(std::unique(domain.types.begin(), domain.types.end()),
                       domain.types.end());
  }
  const auto key = [](const auto& d) {
    return std::tie(d.source, d.destination, d.types, d.directed);
  };
  std::sort(domains.begin(), domains.end(),
            [&](const auto& a, const auto& b) { return key(a) < key(b); });
  domains.erase(std::unique(domains.begin(), domains.end(),
                            [&](const auto& a, const auto& b) {
                              return key(a) == key(b);
                            }),
                domains.end());
  return domains;
}
}  // namespace

QueryPart extractStandaloneQueryPart(const binder::NormalizedQueryPart& part,
                                     const Constraints& constraints) {
  return extractPart(part, constraints, {}, true, "standalone/0/0");
}

std::vector<QueryPart> extractClosedQueryParts(
    const binder::NormalizedSingleQuery& query, const Constraints& constraints) {
  std::vector<QueryPart> result;
  std::set<std::string> inputs;
  std::map<std::string, std::string> names;
  std::vector<CarrierDomain> domains;
  for (size_t i = 0; i < query.getNumQueryParts(); ++i) {
    const auto& part = *query.getQueryPart(i);
    result.push_back(extractPart(part, constraints, inputs, i == 0,
                                "closed/0/" + std::to_string(i), domains, names));
    inputs = inputColumns(part);
    names = projectedNames(part);
    domains = retainedDomains(result.back(), inputs);
  }
  return result;
}

std::vector<QueryPart> extractQueryParts(const binder::BoundRegularQuery& query,
                                         const Constraints& constraints,
                                         const std::string& source) {
  std::vector<QueryPart> parts;
  std::set<std::string> commonInputs;
  std::map<std::string, std::string> commonNames;
  std::vector<CarrierDomain> commonDomains;
  size_t prefix = 0;
  for (const auto& part : query.getPreQueryPart()) {
    parts.push_back(extractPart(part, constraints, commonInputs, prefix == 0,
                                source + "/pre/" + std::to_string(prefix),
                                commonDomains, commonNames));
    ++prefix;
    commonInputs = inputColumns(part);
    commonNames = projectedNames(part);
    commonDomains = retainedDomains(parts.back(), commonInputs);
  }
  for (size_t branch = 0; branch < query.getNumSingleQueries(); ++branch) {
    const auto* single = query.getSingleQuery(branch);
    auto inputs = commonInputs;
    auto names = commonNames;
    auto domains = commonDomains;
    for (size_t pos = 0; pos < single->getNumQueryParts(); ++pos) {
      const auto* part = single->getQueryPart(pos);
      parts.push_back(extractPart(
          *part, constraints, inputs, prefix == 0 && pos == 0,
          source + "/" + std::to_string(branch) + "/" + std::to_string(pos),
          domains, names));
      inputs = inputColumns(*part);
      names = projectedNames(*part);
      domains = retainedDomains(parts.back(), inputs);
    }
  }
  // Post-UNION stages have a different binding interface. Do not infer it.
  if (query.getPostSingleQuery()) {
    QueryPart unsupported;
    unsupported.source = source + "/post";
    unsupported.unsupportedReason = "Post-UNION stages are not enumerated";
    parts.push_back(std::move(unsupported));
  }
  return parts;
}
}  // namespace neug::index

namespace neug::main {
index::EnumerationResult ClientContext::enumerateIndexCandidates(
    const std::vector<std::string>& queries,
    const index::Constraints& constraints,
    const std::vector<index::Carrier>& carriers, const index::Limits& limits,
    bool includeUses) {
  std::unique_lock lock{mtx};
  std::vector<index::QueryPart> parts;
  std::vector<std::unique_ptr<binder::BoundRegularQuery>> boundQueries;
  for (size_t i = 0; i < queries.size(); ++i) {
    const auto parsed = parser::Parser::parseQuery(queries[i]);
    if (parsed.size() != 1 ||
        parsed[0]->getStatementType() != common::StatementType::QUERY)
      throw std::invalid_argument(
          "Each history entry must contain exactly one read query");
    binder::Binder binder(this);
    auto bound = binder.bindForIndexEnumeration(*parsed[0]);
    auto extracted = index::extractQueryParts(*bound, constraints,
                                              "query" + std::to_string(i));
    parts.insert(parts.end(), std::make_move_iterator(extracted.begin()),
                 std::make_move_iterator(extracted.end()));
    if (limits.prefixes)
      boundQueries.push_back(std::move(bound));
  }
  auto result = index::enumerateCandidates(parts, constraints, carriers, limits,
                                           includeUses);
  if (!limits.prefixes)
    return result;
  std::map<std::string, index::Candidate> definitions;
  for (auto& candidate : result.candidates)
    definitions.try_emplace(candidate.definitionKey, std::move(candidate));
  result.candidates.clear();
  for (size_t i = 0; i < boundQueries.size(); ++i) {
    // Zero means unlimited at the lower-level APIs, so an exhausted finite
    // budget must stop before forming the remaining-budget Limits value.
    if ((limits.maxGenerated && result.generated >= limits.maxGenerated) ||
        (limits.maxCombinations &&
         result.combinations >= limits.maxCombinations)) {
      result.truncated = true;
      break;
    }
    auto remaining = limits;
    if (limits.maxGenerated)
      remaining.maxGenerated = limits.maxGenerated - result.generated;
    if (limits.maxCombinations)
      remaining.maxCombinations = limits.maxCombinations - result.combinations;
    auto prefix = index::enumerateComputationPrefixes(
        queries[i], *boundQueries[i], carriers, remaining,
        constraints.uniqueProperties, "query" + std::to_string(i), constraints);
    result.generated += prefix.generated;
    result.combinations += prefix.combinations;
    result.truncated |= prefix.truncated;
    for (auto& candidate : prefix.candidates)
      definitions.try_emplace(candidate.definitionKey, std::move(candidate));
    if (includeUses)
      result.uses.insert(result.uses.end(),
                         std::make_move_iterator(prefix.uses.begin()),
                         std::make_move_iterator(prefix.uses.end()));
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(prefix.diagnostics.begin()),
        std::make_move_iterator(prefix.diagnostics.end()));
    for (const auto& [reason, count] : prefix.rejections)
      result.rejections[reason] += count;
    for (const auto& [reason, count] : prefix.pruned)
      result.pruned[reason] += count;
  }
  for (auto& [key, candidate] : definitions)
    result.candidates.push_back(std::move(candidate));
  return result;
}
}  // namespace neug::main

#pragma once
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_expr_converter.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/generated/proto/plan/expr.pb.h"

namespace neug::binder {
inline constexpr std::string_view FILL_PREDICATE_SIGNATURE_PREFIX =
    "@predicate-ir-v1:";

inline constexpr std::string_view FILL_RECURSIVE_SIGNATURE_PREFIX =
    "@recursive-fill-v1:";
inline std::optional<std::string> fillRecursiveSignature(
    const RelExpression& relation) {
  const auto* info = relation.getRecursiveInfo();
  if (!info || !info->bindData)
    return std::nullopt;
  std::string result =
      std::string(FILL_RECURSIVE_SIGNATURE_PREFIX) +
      std::to_string(static_cast<unsigned>(relation.getRelType())) + ":" +
      std::to_string(info->bindData->lowerBound) + ":" +
      std::to_string(info->bindData->upperBound) + ":" +
      std::to_string(static_cast<unsigned>(info->bindData->semantic));
  auto resolve = [&](const std::string& name) -> std::optional<uint32_t> {
    if (info->rel && (name == info->rel->getUniqueName() ||
                      name == info->rel->getVariableName()))
      return 0;
    if (info->node && (name == info->node->getUniqueName() ||
                       name == info->node->getVariableName()))
      return 1;
    if (info->nodeCopy && (name == info->nodeCopy->getUniqueName() ||
                           name == info->nodeCopy->getVariableName()))
      return 1;
    return std::nullopt;
  };
  try {
    for (const auto& predicate : {info->relPredicate, info->nodePredicate}) {
      const auto text =
          predicate ? catalog::ConvertFillIndexExprToAst(*predicate, resolve)
                          ->SerializeAsString()
                    : std::string{};
      result += "/" + std::to_string(text.size()) + ":" + text;
    }
  } catch (const std::exception&) { return std::nullopt; }
  return result;
}

// Exact alpha-equivalence for predicates outside the scalar AST grammar.
// EXISTS support is deliberately limited to one fixed relationship between
// already-bound outer nodes. Local variables, hints and recursive subqueries
// require separate proofs and cannot acquire a signature here.
inline std::optional<std::string> fillPredicateSignature(
    const Expression& root, const common::PatternIR& outer, bool matched,
    catalog::Catalog* catalog, const transaction::Transaction* transaction) {
  auto pack = [](std::string value) {
    return std::to_string(value.size()) + ":" + value;
  };
  auto variable = [&](const std::string& name) -> std::optional<std::string> {
    if (auto it = outer.nodeUniqNameToPos.find(name);
        it != outer.nodeUniqNameToPos.end()) {
      const auto pos =
          matched ? outer.nodesInfo[it->second].matchedIdx : it->second;
      if (pos != UINT64_MAX)
        return "n" + std::to_string(pos);
    }
    if (auto it = outer.relUniqueNameToPos.find(name);
        it != outer.relUniqueNameToPos.end()) {
      const auto pos =
          matched ? outer.relsInfo[it->second].matchedIdx : it->second;
      if (pos != UINT64_MAX)
        return "r" + std::to_string(pos);
    }
    return std::nullopt;
  };
  std::function<std::optional<std::string>(const Expression&)> visit;
  visit = [&](const Expression& expr) -> std::optional<std::string> {
    std::string value =
        std::to_string(static_cast<unsigned>(expr.expressionType)) + "/";
    if (expr.expressionType == common::ExpressionType::LITERAL) {
      const auto literal = expr.constCast<LiteralExpression>().getValue();
      return value + pack(expr.getDataType().toString()) +
             pack(literal.toString());
    }
    if (expr.expressionType == common::ExpressionType::PROPERTY) {
      const auto& property = expr.constCast<PropertyExpression>();
      auto owner = variable(property.getVariableName());
      if (!owner)
        return std::nullopt;
      return value + pack(*owner) + pack(property.getPropertyName());
    }
    if (expr.expressionType == common::ExpressionType::PATTERN) {
      auto owner = variable(expr.getUniqueName());
      return owner ? std::optional<std::string>(value + pack(*owner))
                   : std::nullopt;
    }
    if (expr.expressionType == common::ExpressionType::SUBQUERY) {
      const auto& sub = expr.constCast<SubqueryExpression>();
      const auto* graphs = sub.getQueryGraphCollection();
      if (sub.getHint() || graphs->getNumQueryGraphs() != 1)
        return std::nullopt;
      const auto* graph = graphs->getQueryGraph(0);
      if (graph->getNumQueryNodes() != 2 || graph->getNumQueryRels() != 1)
        return std::nullopt;
      std::vector<std::string> nodes;
      for (const auto& node : graph->getQueryNodes()) {
        auto name = variable(node->getUniqueName());
        if (!name)
          return std::nullopt;
        std::vector<std::string> labels;
        for (auto* entry : node->getEntries())
          labels.push_back(entry->getName());
        std::sort(labels.begin(), labels.end());
        std::string key = pack(*name);
        for (const auto& label : labels)
          key += pack(label);
        nodes.push_back(key);
      }
      std::sort(nodes.begin(), nodes.end());
      value +=
          pack(std::to_string(static_cast<unsigned>(sub.getSubqueryType())));
      for (const auto& node : nodes)
        value += pack(node);
      const auto rel = graph->getQueryRel(0);
      if (rel->isRecursive() ||
          outer.relUniqueNameToPos.contains(rel->getUniqueName()))
        return std::nullopt;
      const auto src = variable(rel->getSrcNode()->getUniqueName()),
                 dst = variable(rel->getDstNode()->getUniqueName());
      if (!src || !dst)
        return std::nullopt;
      value +=
          pack(*src) + pack(*dst) +
          pack(std::to_string(static_cast<unsigned>(rel->getDirectionType())));
      std::vector<std::string> labels;
      for (auto* entry : rel->getEntries())
        labels.push_back(entry->getLabel(catalog, transaction));
      std::sort(labels.begin(), labels.end());
      for (const auto& label : labels)
        value += pack(label);
      if (sub.hasWhereExpression()) {
        auto where = visit(*sub.getWhereExpression());
        if (!where)
          return std::nullopt;
        value += pack(*where);
      }
      return value;
    }
    if (expr.expressionType == common::ExpressionType::FUNCTION)
      value +=
          pack(expr.constCast<ScalarFunctionExpression>().getFunction().name);
    const auto children = ExpressionChildrenCollector::collectChildren(expr);
    if (children.empty())
      return std::nullopt;
    for (const auto& child : children) {
      auto signature = visit(*child);
      if (!signature)
        return std::nullopt;
      value += pack(*signature);
    }
    return value;
  };
  auto result = visit(root);
  if (result)
    *result = std::string(FILL_PREDICATE_SIGNATURE_PREFIX) + *result;
  return result;
}
}  // namespace neug::binder

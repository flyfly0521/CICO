#include "neug/generated/proto/plan/expr.pb.h"
#include "neug/compiler/binder/expression/index_rewrite_template_matcher.h"

#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/string_utils.h"

namespace neug {
namespace binder {

using common::ExpressionType;

namespace {

static std::string normalizeTemplateForComparison(std::string_view s) {
  // Normalize case for keywords/identifiers outside single-quoted string
  // literals, while preserving string literals exactly.
  std::string out;
  out.reserve(s.size());
  bool inString = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\'') {
      out.push_back(c);
      if (inString) {
        if (i + 1 < s.size() && s[i + 1] == '\'') {
          out.push_back('\'');
          ++i;
          continue;
        }
        inString = false;
      } else {
        inString = true;
      }
      continue;
    }
    if (inString) {
      out.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      // Layout is not part of an expression: a template spelled over several
      // lines (e.g. a multi-line CASE in a FILL source) equals its one-line
      // form. Collapse whitespace runs; drop them next to ( ) , entirely.
      if (!out.empty() && out.back() != ' ' && out.back() != '(' && out.back() != ',')
        out.push_back(' ');
    } else {
      if ((c == ')' || c == ',' || c == '(') && !out.empty() && out.back() == ' ')
        out.pop_back();
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

static void collectAggregateFunctionExprs(
    const std::shared_ptr<Expression>& expr,
    std::vector<const AggregateFunctionExpression*>& out) {
  if (!expr) {
    return;
  }
  if (expr->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    out.push_back(expr->constPtrCast<AggregateFunctionExpression>());
  }
  for (auto& child : ExpressionChildrenCollector::collectChildren(*expr)) {
    collectAggregateFunctionExprs(child, out);
  }
}

// Match aggregate template variables by their unique names (uniqName)
// instead of fragile per-pattern position indices. Each raw variable can
// identify a node or a relationship.
static bool matchesAggregateVariablesByUniqName(
    const std::vector<std::string>& rawVarNames,
    const std::vector<std::string>& indexNodeUniqueNames,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR) {
  if (rawVarNames.size() != indexNodeUniqueNames.size()) {
    return false;
  }
  for (size_t i = 0; i < rawVarNames.size(); ++i) {
    // Find the query node or relationship by raw variable name.
    size_t qpos = UINT64_MAX;
    for (size_t ni = 0; ni < queryIR.nodesInfo.size(); ++ni) {
      if (queryIR.nodesInfo[ni].varName == rawVarNames[i]) {
        qpos = ni;
        break;
      }
    }
    if (qpos != UINT64_MAX) {
      const auto matchedIndexPos = queryIR.nodesInfo[qpos].matchedIdx;
      if (matchedIndexPos == UINT64_MAX ||
          matchedIndexPos >= indexIR.nodesInfo.size() ||
          indexIR.nodesInfo[matchedIndexPos].uniqName !=
              indexNodeUniqueNames[i]) {
        return false;
      }
      continue;
    }

    for (size_t ri = 0; ri < queryIR.relsInfo.size(); ++ri) {
      if (queryIR.relsInfo[ri].varName == rawVarNames[i]) {
        qpos = ri;
        break;
      }
    }
    if (qpos == UINT64_MAX) {
      return false;
    }
    const auto matchedIndexPos = queryIR.relsInfo[qpos].matchedIdx;
    if (matchedIndexPos == UINT64_MAX ||
        matchedIndexPos >= indexIR.relsInfo.size() ||
        indexIR.relsInfo[matchedIndexPos].uniqName != indexNodeUniqueNames[i]) {
      return false;
    }
  }
  return true;
}

static bool matchesRelAggregateTemplate(
    const AggregateFunctionExpression& agg,
    const catalog::RelIndexColnumInfo& info,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR) {
  // Template string must match exactly, with COUNT(*) wildcard support.
  const auto queryTemplate = normalizeTemplateForComparison(agg.getParsedName());
  const auto indexTemplate = normalizeTemplateForComparison(info.nameTemplate);
  if (queryTemplate == "COUNT(*)" && indexTemplate != queryTemplate &&
      indexTemplate.rfind("COUNT(", 0) == 0) {
    // COUNT(*) can reuse COUNT(entity) in a mandatory source region. A
    // property, CASE, or external nullable value requires its own argument
    // proof; matching the function prefix alone loses NULL semantics.
    if (!info.aggregateArgExpr || info.aggregateArgExpr->operators_size() != 1)
      return false;
    const auto& argument = info.aggregateArgExpr->operators(0);
    return argument.has_var() && argument.var().has_tag() &&
           !argument.var().has_property() &&
           argument.var().tag().id() >= 0 &&
           static_cast<size_t>(argument.var().tag().id()) <
               indexIR.nodesInfo.size() + indexIR.relsInfo.size();
  }
  if (queryTemplate != indexTemplate) {
    return false;
  }
  return matchesAggregateVariablesByUniqName(
      agg.getRawNameVariables(), info.nodeUniqueNames, queryIR, indexIR);
}

static bool matchesNodeAggregateTemplate(
    const AggregateFunctionExpression& agg,
    const catalog::NodeAggregationIndexColnumInfo& info,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR) {
  auto aggFuncName = common::StringUtils::getUpper(agg.getFunction().name);
  auto indexFuncName = common::StringUtils::getUpper(info.aggregateFuncName);
  // The regular FILL metadata builder canonicalizes COUNT_STAR to COUNT.
  // Keep matching bound COUNT(*) queries against that metadata; the template
  // and argument checks below still distinguish COUNT(*) from COUNT(expr).
  if (aggFuncName == "COUNT_STAR") aggFuncName = "COUNT";
  if (indexFuncName == "COUNT_STAR") indexFuncName = "COUNT";
  if (aggFuncName != indexFuncName) {
    return false;
  }

  // Early match: if name template and variables match directly, succeed
  // without requiring child property extraction.
  if (normalizeTemplateForComparison(agg.getParsedName()) ==
          normalizeTemplateForComparison(info.nameTemplate) &&
      matchesAggregateVariablesByUniqName(
          agg.getRawNameVariables(), info.nodeUniqueNames, queryIR, indexIR)) {
    return true;
  }

  // A direct relationship scan emits each relationship once. For that
  // single-edge pattern, COUNT(DISTINCT rel) is equivalent to the materialized
  // COUNT(rel). Keep this deliberately narrow: with multiple relationships a
  // join can duplicate the counted edge and DISTINCT is no longer redundant.
  if (aggFuncName == "COUNT" && agg.isDistinct() &&
      indexIR.relsInfo.size() == 1 && agg.getNumChildren() == 1) {
    auto child = agg.getChild(0);
    if (child != nullptr &&
        queryIR.relUniqueNameToPos.contains(child->getUniqueName())) {
      auto queryTemplate = normalizeTemplateForComparison(agg.getParsedName());
      auto indexTemplate = normalizeTemplateForComparison(info.nameTemplate);
      const std::string distinctPrefix = "COUNT(DISTINCT ";
      if (queryTemplate.rfind(distinctPrefix, 0) == 0) {
        queryTemplate.erase(6, 9);  // Remove "DISTINCT ".
      }
      if (queryTemplate == indexTemplate &&
          matchesAggregateVariablesByUniqName(
              agg.getRawNameVariables(), info.nodeUniqueNames, queryIR,
              indexIR)) {
        return true;
      }
    }
  }

  // Node identity alone does not prove equal multiplicity: parallel edges
  // can repeat the counted node. Preserve DISTINCT unless the narrow
  // relationship-identity case above has already established equivalence.
  const auto indexTemplate = normalizeTemplateForComparison(info.nameTemplate);
  const auto indexIsDistinct =
      indexTemplate.find("(DISTINCT ") != std::string::npos;
  if (agg.isDistinct() != indexIsDistinct) {
    return false;
  }

  // Extract the (uniqueVar, property) target from the aggregate's argument.
  if (agg.getNumChildren() != 1) {
    return false;
  }
  auto child = agg.getChild(0);
  if (!child) {
    return false;
  }

  std::string queryUniqueVar;
  std::string queryPropName;

  if (child->expressionType == ExpressionType::PROPERTY) {
    auto* prop = child->ptrCast<PropertyExpression>();
    if (!prop || prop->isInternalID()) {
      return false;
    }
    queryUniqueVar = prop->getVariableName();
    queryPropName = prop->getPropertyName();
  } else if (child->expressionType == ExpressionType::PATTERN &&
             child->dataType.getLogicalTypeID() == common::LogicalTypeID::NODE) {
    queryUniqueVar = child->getUniqueName();
    queryPropName = "";
  } else {
    // Fallback: match by name template + variable unique names.
    return normalizeTemplateForComparison(agg.getParsedName()) ==
               normalizeTemplateForComparison(info.nameTemplate) &&
           matchesAggregateVariablesByUniqName(
               agg.getRawNameVariables(), info.nodeUniqueNames, queryIR, indexIR);
  }

  // Map the query-side unique var to the index-side node position.
  const auto it = queryIR.nodeUniqNameToPos.find(queryUniqueVar);
  if (it == queryIR.nodeUniqNameToPos.end()) {
    return false;
  }
  const auto qpos = it->second;
  const auto matchedIndexPos = queryIR.nodesInfo[qpos].matchedIdx;
  if (matchedIndexPos == UINT64_MAX || matchedIndexPos >= indexIR.nodesInfo.size()) {
    return false;
  }

  // Index-side node unique must match.
  if (indexIR.nodesInfo[matchedIndexPos].uniqName != info.nodeUnique) {
    return false;
  }

  // If the index aggregate was defined over a property, require same property.
  if (!info.propertyName.empty() && queryPropName != info.propertyName) {
    return false;
  }

  return true;
}

static bool matchesAnyIndexAggregateTemplate(
    const AggregateFunctionExpression& agg,
    const catalog::FillIndexAuxInfo& indexInfo,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR,
    size_t& outIndexColumnPos) {
  const auto& infos = indexInfo.getIndexColumnInfos();
  const auto& targets = indexInfo.getTargetColumns();
  for (size_t idx = 0; idx < infos.size(); ++idx) {
    // Private group-existence/finalizer state is not a public aggregate
    // replacement. In particular, a nullable COUNT(expr) must not advertise
    // its hidden row count as a matching COUNT(*) output.
    if (idx >= targets.size() ||
        targets[idx].starts_with(common::InternalKeyword::FILL_AGGREGATE_STATE_PREFIX)) {
      continue;
    }
    const auto& base = infos[idx];
    if (!base) {
      continue;
    }
    switch (base->columnType) {
    case catalog::IndexColumnType::NODE_AGGREGATE: {
      auto* nodeInfo = dynamic_cast<catalog::NodeAggregationIndexColnumInfo*>(base.get());
      if (nodeInfo && matchesNodeAggregateTemplate(agg, *nodeInfo, queryIR, indexIR)) {
        outIndexColumnPos = idx;
        return true;
      }
    } break;
    case catalog::IndexColumnType::REL_AGGREGATE: {
      auto* relInfo = dynamic_cast<catalog::RelIndexColnumInfo*>(base.get());
      if (relInfo && matchesRelAggregateTemplate(agg, *relInfo, queryIR, indexIR)) {
        outIndexColumnPos = idx;
        return true;
      }
    } break;
    default:
      break;
    }
  }
  return false;
}

static bool matchesRelComputationWithAggregateTemplate(
    const ScalarFunctionExpression& expr,
    const catalog::RelComputationWithAggreColumnInfo& info,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR) {
  if (normalizeTemplateForComparison(expr.getParsedName()) !=
      normalizeTemplateForComparison(info.nameTemplate)) {
    return false;
  }
  return matchesAggregateVariablesByUniqName(
      expr.getRawNameVariables(), info.nodeUniqueNames, queryIR, indexIR);
}

}  // namespace

bool IndexRewriteTemplateMatcher::validateAggregateTemplatesForIndexRewrite(
    const std::shared_ptr<Expression>& rootExpr,
    const catalog::FillIndexAuxInfo& indexInfo,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR,
    AggRewriteTargetMap& outRewriteTargets) {
  if (!rootExpr) {
    return true;
  }

  std::vector<const AggregateFunctionExpression*> aggs;
  collectAggregateFunctionExprs(rootExpr, aggs);
  if (aggs.empty()) {
    return true;
  }

  bool indexHasAggregate = false;
  for (const auto& base : indexInfo.getIndexColumnInfos()) {
    if (!base) continue;
    if (base->columnType == catalog::IndexColumnType::NODE_AGGREGATE ||
        base->columnType == catalog::IndexColumnType::REL_AGGREGATE) {
      indexHasAggregate = true;
      break;
    }
  }
  if (!indexHasAggregate) {
    return false;
  }

  for (const auto* agg : aggs) {
    if (!agg) continue;
    size_t indexColPos = 0;
    if (!matchesAnyIndexAggregateTemplate(*agg, indexInfo, queryIR, indexIR,
                                          indexColPos)) {
      return false;
    }
    outRewriteTargets.emplace(agg, indexColPos);
  }
  return true;
}

bool IndexRewriteTemplateMatcher::validateTemplatesForIndexRewrite(
    const binder::ScalarFunctionExpression* rootExpr,
    const catalog::FillIndexAuxInfo& indexInfo,
    const common::PatternIR& queryIR,
    const common::PatternIR& indexIR,
    AggRewriteTargetMap& outRewriteTargets) {
  if (!rootExpr) {
    return true;
  }

  size_t indexColPos = 0;
  const auto& infos = indexInfo.getIndexColumnInfos();
  for (size_t idx = 0; idx < infos.size(); ++idx) {
    const auto& base = infos[idx];
    if (!base) continue;
    switch (base->columnType) {
      case catalog::IndexColumnType::REL_COMPUATION_WITH_AGGREGATION: {
        auto relCompuWithAggCol =
            dynamic_cast<catalog::RelComputationWithAggreColumnInfo*>(infos[idx].get());
        if (!matchesRelComputationWithAggregateTemplate(
                *rootExpr, *relCompuWithAggCol, queryIR, indexIR)) {
          return false;
        }
        outRewriteTargets.emplace(rootExpr, indexColPos);
      } break;
    }
  }
  return true;
}

}  // namespace binder
}  // namespace neug

#include "neug/compiler/binder/rewriter/complete_key_group_rewriter.h"

#include "glog/logging.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/index_rewrite_template_matcher.h"
#include "neug/compiler/binder/expression/index_predicate_key.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/query/reading_clause/bound_unwind_clause.h"
#include "neug/compiler/binder/rewriter/endpoint_projection_contract.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/parser.h"

namespace neug::binder {
namespace {
using namespace common;
using namespace catalog;

std::string identity(const std::shared_ptr<Expression>& expression) {
  if (expression->expressionType == ExpressionType::PATTERN &&
      expression->dataType.getLogicalTypeID() == LogicalTypeID::NODE)
    return expression->getUniqueName();
  if (expression->expressionType == ExpressionType::PROPERTY) {
    const auto& property = expression->constCast<PropertyExpression>();
    // Equal scalar keys from different vertex tables are different graph
    // identities. A multi-label PK alone can merge materialized groups.
    if (property.isSingleLabel() && property.isPrimaryKey())
      return property.getVariableName();
  }
  return {};
}

// PatternIR omits bounds and path semantics of a recursive edge. A complete
// group count over a recursive source equals the query's only when the matched
// recursive edges are identical: same type, direction, bounds, semantics and
// label domains, and neither side filters inside the recursion.
struct RecursiveEdgeContract {
  QueryRelType type;
  RelDirectionType direction;
  uint64_t lowerBound, upperBound;
  PathSemantic semantic;
  std::vector<table_id_t> tables, stepTables, nodeTables, nodeCopyTables;
  bool operator==(const RecursiveEdgeContract&) const = default;
};

std::optional<RecursiveEdgeContract> recursiveEdgeContract(const RelExpression& edge) {
  if (edge.getRelType() != QueryRelType::VARIABLE_LENGTH_WALK &&
      edge.getRelType() != QueryRelType::VARIABLE_LENGTH_TRAIL &&
      edge.getRelType() != QueryRelType::VARIABLE_LENGTH_ACYCLIC) return std::nullopt;
  const auto* recursive = edge.getRecursiveInfo();
  if (recursive == nullptr || recursive->bindData == nullptr || recursive->node == nullptr ||
      recursive->nodeCopy == nullptr || recursive->rel == nullptr ||
      recursive->nodePredicate != nullptr || recursive->relPredicate != nullptr ||
      recursive->bindData->weightPropertyExpr != nullptr) return std::nullopt;
  const auto sorted = [](auto values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
  };
  return RecursiveEdgeContract{edge.getRelType(), edge.getDirectionType(),
      recursive->bindData->lowerBound, recursive->bindData->upperBound,
      recursive->bindData->semantic, sorted(edge.getTableIDs()),
      sorted(recursive->rel->getTableIDs()), sorted(recursive->node->getTableIDs()),
      sorted(recursive->nodeCopy->getTableIDs())};
}

bool distinctCount(const std::shared_ptr<Expression>& expression) {
  if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION)
    return false;
  const auto& aggregate = expression->constCast<AggregateFunctionExpression>();
  if (!aggregate.isDistinct() || aggregate.getFunction().name != "COUNT" ||
      aggregate.getNumChildren() != 1) return false;
  const auto type = aggregate.getChild(0)->expressionType;
  return type == ExpressionType::PATTERN || type == ExpressionType::PROPERTY;
}

// A complete key group (theory.md, Definition 6 with Ext = the group itself):
// when the query group is exactly one materialized source group, the stored
// value is the whole group's aggregate, whatever its algebra. COUNT(DISTINCT),
// MIN and MAX are therefore read as stored (lowered with MAX over the
// duplicated rows of one group), and so is a scalar finalizer over them.
bool completeGroupAggregate(const std::shared_ptr<Expression>& expression) {
  if (distinctCount(expression)) return true;
  if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION) return false;
  const auto& aggregate = expression->constCast<AggregateFunctionExpression>();
  const auto& name = aggregate.getFunction().name;
  // SUM and COUNT of one complete group are its stored values as well (a FILL
  // with MIN/MAX columns admits only complete-group reads for all columns).
  if (!aggregate.isDistinct() && name == "COUNT_STAR" && aggregate.getNumChildren() == 0)
    return true;
  return !aggregate.isDistinct() && aggregate.getNumChildren() == 1 &&
      (name == "MIN" || name == "MAX" || name == "SUM" || name == "COUNT");
}

bool collectCompleteGroupAggregates(const std::shared_ptr<Expression>& expression,
                                    std::vector<std::shared_ptr<Expression>>& out) {
  if (!expression) return true;
  if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    if (!completeGroupAggregate(expression)) return false;
    out.push_back(expression);
    return true;
  }
  if (expression->expressionType == ExpressionType::SUBQUERY) return false;
  for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
    if (!collectCompleteGroupAggregates(child, out)) return false;
  return true;
}

// A complete-group aggregate value, or a deterministic scalar over such values.
bool completeGroupValue(const std::shared_ptr<Expression>& expression) {
  std::vector<std::shared_ptr<Expression>> aggregates;
  return collectCompleteGroupAggregates(expression, aggregates) && !aggregates.empty();
}
}  // namespace

std::vector<std::shared_ptr<Expression>> guaranteedIncomingNodePredicates(
    const BoundMatchClause& match, const NormalizedQueryPart& part,
    const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<std::string>& retainedNodes) {
  if (wholeQuery == nullptr || retainedNodes.empty()) return {};
  std::unordered_map<std::string, const NodeExpression*> nodes;
  for (const auto& node : match.getQueryGraphCollection()->getQueryNodes()) {
    if (retainedNodes.contains(node->getUniqueName()))
      nodes.emplace(node->getUniqueName(), node.get());
  }
  struct Fact {
    std::shared_ptr<Expression> predicate;
    std::unordered_set<std::string> dependencies;
  };
  std::vector<Fact> facts;
  const auto collect = [&](const std::shared_ptr<Expression>& predicate) {
    if (!predicate) return;
    for (const auto& atom : predicate->splitOnAND()) {
      Fact fact{atom, {}};
      std::function<bool(const std::shared_ptr<Expression>&)> safe =
          [&](const auto& expression) {
        switch (expression->expressionType) {
        case ExpressionType::LITERAL:
        case ExpressionType::PARAMETER:
          return true;
        case ExpressionType::PROPERTY: {
          const auto& property = expression->template constCast<PropertyExpression>();
          const auto found = nodes.find(property.getVariableName());
          if (found == nodes.end()) return false;
          // A scalar property must have the same complete typed domain as
          // the retained node, not an equal key from a different label.
          const auto labels = found->second->getTableIDs();
          if (labels.size() != 1 || !property.isSingleLabel() ||
              property.getSingleTableID() != labels.front()) return false;
          fact.dependencies.insert(found->first);
          return true;
        }
        case ExpressionType::FUNCTION: {
          static const std::set<std::string> deterministic = {
              "CAST_TO_INT64", "CAST_TO_DOUBLE", "CAST_TO_STRING", "CAST_TO_DATE",
              "CAST_TO_TIMESTAMP"};
          if (!deterministic.contains(expression->template constCast<ScalarFunctionExpression>()
                                          .getFunction().name)) return false;
          break;
        }
        default:
          if (!ExpressionTypeUtil::isBoolean(expression->expressionType) &&
              !ExpressionTypeUtil::isComparison(expression->expressionType) &&
              !ExpressionTypeUtil::isNullOperator(expression->expressionType)) return false;
        }
        for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
          if (!safe(child)) return false;
        return true;
      };
      if (safe(atom) && !fact.dependencies.empty()) facts.push_back(std::move(fact));
    }
  };
  for (uint32_t pi = 0; pi < wholeQuery->getNumQueryParts(); ++pi) {
    const auto* current = wholeQuery->getQueryPart(pi);
    if (current->hasUpdatingClause()) return {};
    for (uint32_t ci = 0; ci < current->getNumReadingClause(); ++ci) {
      const auto* clause = current->getReadingClause(ci);
      if (clause->getClauseType() != ClauseType::MATCH) return {};
      const auto& previous = clause->constCast<BoundMatchClause>();
      if (&previous == &match) {
        if (current != &part) return {};
        std::vector<std::shared_ptr<Expression>> result;
        for (const auto& fact : facts) result.push_back(fact.predicate);
        return result;
      }
      if (previous.getMatchClauseType() == MatchClauseType::MATCH)
        collect(previous.getPredicate());
      // OPTIONAL can preserve a proven input fact, but its own WHERE may
      // fail and produce a NULL-extended row, so it establishes no new fact.
    }
    if (current == &part || !current->hasProjectionBody()) return {};
    const auto* body = current->getProjectionBody();
    const auto directlyRetains = [&](const auto& expressions, const std::string& name) {
      return std::any_of(expressions.begin(), expressions.end(), [&](const auto& value) {
        return value.get() == nodes.at(name) &&
            value->expressionType == ExpressionType::PATTERN &&
            value->getDataType().getLogicalTypeID() == LogicalTypeID::NODE &&
            value->getUniqueName() == name;
      });
    };
    const auto survives = [&](const std::string& name) {
      return directlyRetains(body->getProjectionExpressions(), name) &&
          (body->getAggregateExpressions().empty() ||
           directlyRetains(body->getGroupByExpressions(), name));
    };
    std::erase_if(facts, [&](const auto& fact) {
      return std::any_of(fact.dependencies.begin(), fact.dependencies.end(),
                         [&](const auto& name) { return !survives(name); });
    });
    // This WHERE is after this WITH; it can only justify later MATCHes.
    const auto count = facts.size();
    collect(current->getProjectionBodyPredicate());
    for (size_t i = facts.size(); i > count; --i) {
      if (std::any_of(facts[i - 1].dependencies.begin(), facts[i - 1].dependencies.end(),
                      [&](const auto& name) { return !survives(name); }))
        facts.erase(facts.begin() + i - 1);
    }
  }
  return {};
}

bool provesResidualMatchExistsInputsSurvive(
    const Expression& expression, const FillIndexAuxInfo& info,
    const PatternIR& matched, const PatternIR& query,
    const BoundMatchClause& match, bool provedSingleStageEndpointSetSource) {
  // A predicate in an OPTIONAL clause or a different WITH/MATCH scope needs
  // a separate null-extension and input-availability proof.
  const auto& templates = info.getPredicateTemplates();
  const bool endpointSetCompatibility = provedSingleStageEndpointSetSource &&
      info.getTargetColumns().empty() && !info.getSourceQuery().empty() &&
      std::find(templates.begin(), templates.end(), ENDPOINT_SET_CONTRACT) != templates.end();
  if (info.getFillType() != FillEntryType::REL ||
      (info.usesWorkloadCompatibility() && !endpointSetCompatibility) ||
      match.getMatchClauseType() != MatchClauseType::MATCH) return false;
  const auto* collection = match.getQueryGraphCollection();
  if (collection == nullptr || collection->getNumQueryGraphs() != 1) return false;
  const auto* graph = collection->getQueryGraph(0);
  if (graph == nullptr || graph->getNumQueryNodes() != query.nodesInfo.size() ||
      graph->getNumQueryRels() != query.relsInfo.size()) return false;
  const auto sameDomain = [](const auto& lhs, const auto& rhs) {
    return std::set<table_id_t>(lhs.begin(), lhs.end()) ==
           std::set<table_id_t>(rhs.begin(), rhs.end());
  };
  // Query IR positions must refer to this exact current graph, not a merged
  // query or another MATCH having the same labels/raw variable spellings.
  for (const auto& node : query.nodesInfo) {
    if (!graph->containsQueryNode(node.uniqName)) return false;
    const auto actual = graph->getQueryNode(node.uniqName);
    if (actual == nullptr || actual->getDataType().getLogicalTypeID() != LogicalTypeID::NODE ||
        !sameDomain(actual->getTableIDs(), node.nodeLabels)) return false;
  }
  for (const auto& rel : query.relsInfo) {
    if (rel.srcNodeIdx >= query.nodesInfo.size() ||
        rel.dstNodeIdx >= query.nodesInfo.size() ||
        rel.relType != QueryRelType::NON_RECURSIVE ||
        !graph->containsQueryRel(rel.uniqName)) return false;
    const auto actual = graph->getQueryRel(rel.uniqName);
    if (actual == nullptr || actual->isRecursive() ||
        !sameDomain(actual->getTableIDs(), rel.relLabels) ||
        actual->getSrcNode()->getUniqueName() != query.nodesInfo[rel.srcNodeIdx].uniqName ||
        actual->getDstNode()->getUniqueName() != query.nodesInfo[rel.dstNodeIdx].uniqName)
      return false;
  }
  std::unordered_set<std::string> matchedNodes, deletedRels, keys;
  for (const auto& node : matched.nodesInfo) {
    if (node.matchedIdx >= query.nodesInfo.size() ||
        !matchedNodes.insert(query.nodesInfo[node.matchedIdx].uniqName).second)
      return false;
  }
  for (const auto& rel : matched.relsInfo) {
    if (rel.matchedIdx >= query.relsInfo.size() ||
        !deletedRels.insert(query.relsInfo[rel.matchedIdx].uniqName).second)
      return false;
  }
  if (deletedRels.empty()) return false;
  for (const auto& name : {info.getSrcNodeUniqueName(), info.getDstNodeUniqueName()}) {
    const auto found = matched.nodeUniqNameToPos.find(name);
    if (name.empty() || found == matched.nodeUniqNameToPos.end() ||
        found->second >= matched.nodesInfo.size()) return false;
    const auto position = matched.nodesInfo[found->second].matchedIdx;
    if (position >= query.nodesInfo.size()) return false;
    keys.insert(query.nodesInfo[position].uniqName);
  }
  if (keys.size() != 2) return false;
  auto survives = keys;
  for (const auto& rel : graph->getQueryRels()) {
    if (deletedRels.contains(rel->getUniqueName())) continue;
    // Do not resurrect a hidden materialized-source vertex just because a
    // residual edge uses it: the FILL has already summed away that identity.
    for (const auto& node : {rel->getSrcNode(), rel->getDstNode()}) {
      const auto& name = node->getUniqueName();
      if (matchedNodes.contains(name) && !keys.contains(name)) return false;
      survives.insert(name);
    }
  }
  try {
    const auto exists = extractFillExistsPredicate(expression, query);
    if (!exists || exists->srcNodePos >= query.nodesInfo.size() ||
        exists->dstNodePos >= query.nodesInfo.size()) return false;
    const auto& src = query.nodesInfo[exists->srcNodePos];
    const auto& dst = query.nodesInfo[exists->dstNodePos];
    return survives.contains(src.uniqName) && survives.contains(dst.uniqName) &&
           sameDomain(exists->srcTableIDs, src.nodeLabels) &&
           sameDomain(exists->dstTableIDs, dst.nodeLabels);
  } catch (const std::exception&) {
    return false;
  }
}

bool completeKeyGroupInputsAreClosed(
    const FillIndexAuxInfo& info, const PatternIR& matched,
    const PatternIR& query, const BoundMatchClause& match,
    const NormalizedQueryPart& part, const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<std::string>* retainedNodes,
    const std::unordered_set<std::string>* retainedRels,
    const std::unordered_set<const Expression*>* coveredExpressions,
    bool provedSingleStageEndpointSetSource) {
  if (wholeQuery == nullptr || !part.hasProjectionBody()) return false;
  std::unordered_set<std::string> removed, retained, aggregates;
  IndexPredicateBindings bindings;
  for (size_t i = 0; i < matched.nodesInfo.size(); ++i) {
    const auto& node = matched.nodesInfo[i];
    if (node.matchedIdx >= query.nodesInfo.size()) return false;
    const auto& name = query.nodesInfo[node.matchedIdx].uniqName;
    bindings.emplace(name, "node:" + std::to_string(i));
    // General aggregate rewrites supply their actual retained identities;
    // the per-pair callers retain the two ordered source keys by default.
    const bool keepsNode = retainedNodes != nullptr
        ? retainedNodes->contains(name)
        : node.uniqName == info.getSrcNodeUniqueName() ||
              node.uniqName == info.getDstNodeUniqueName();
    if (keepsNode) retained.insert(name);
    else removed.insert(name);
  }
  for (size_t i = 0; i < matched.relsInfo.size(); ++i) {
    const auto& rel = matched.relsInfo[i];
    if (rel.matchedIdx >= query.relsInfo.size()) return false;
    const auto& name = query.relsInfo[rel.matchedIdx].uniqName;
    bindings.emplace(name, "rel:" + std::to_string(i));
    if (retainedRels == nullptr || !retainedRels->contains(name))
      removed.insert(name);
  }
  for (const auto& aggregate : part.getProjectionBody()->getAggregateExpressions())
    aggregates.insert(aggregate->getUniqueName());
  const auto sourceCovers = [&](const std::shared_ptr<Expression>& predicate) {
    std::optional<std::string> key;
    try {
      if (auto exists = extractFillExistsPredicate(*predicate, query)) {
        const auto mapped = [&](uint64_t pos) -> uint64_t {
          for (size_t i = 0; i < matched.nodesInfo.size(); ++i)
            if (matched.nodesInfo[i].matchedIdx == pos) return i;
          return UINT64_MAX;
        };
        exists->srcNodePos = mapped(exists->srcNodePos);
        exists->dstNodePos = mapped(exists->dstNodePos);
        if (exists->srcNodePos == UINT64_MAX || exists->dstNodePos == UINT64_MAX)
          return false;
        key = fillExistsPredicateKey(std::move(*exists));
      } else key = makeIndexPredicateKey(*predicate, bindings);
    } catch (const std::exception&) { return false; }
    return key && std::find(info.getPredicateTemplates().begin(),
                            info.getPredicateTemplates().end(), *key) !=
                      info.getPredicateTemplates().end();
  };
  std::function<bool(const std::shared_ptr<Expression>&, bool)> independent =
      [&](const auto& expression, bool currentMatchPredicate) {
    if (!expression) return true;
    // General bag/projection consumers may keep an aggregate unchanged, but
    // every argument then has to survive. Only actual proved substitutions
    // can hide a removed input. The per-pair aggregate callers independently
    // prove their whole aggregate templates and use the default mode below.
    if (coveredExpressions != nullptr && coveredExpressions->contains(expression.get()))
      return true;
    switch (expression->expressionType) {
    case ExpressionType::AGGREGATE_FUNCTION:
      // The caller separately proves these exact aggregate templates. Their
      // hidden arguments are supplied by the stored complete group.
      if (coveredExpressions == nullptr)
        return aggregates.contains(expression->getUniqueName());
      break;
    case ExpressionType::PROPERTY:
      return !removed.contains(
          expression->template constCast<PropertyExpression>().getVariableName());
    case ExpressionType::PATTERN:
    case ExpressionType::VARIABLE:
      return !removed.contains(expression->getUniqueName());
    case ExpressionType::LITERAL:
    case ExpressionType::PARAMETER:
      return true;
    case ExpressionType::SUBQUERY:
      if (currentMatchPredicate && retainedNodes != nullptr &&
          coveredExpressions != nullptr && provesResidualMatchExistsInputsSurvive(
              *expression, info, matched, query, match,
              provedSingleStageEndpointSetSource)) return true;
      // A strict one-hop existence test on the retained pair selects all or
      // none of that complete group; it remains online after substitution.
      try {
        if (const auto exists = extractFillExistsPredicate(*expression, query)) {
          return exists->srcNodePos < query.nodesInfo.size() &&
              exists->dstNodePos < query.nodesInfo.size() &&
              retained.contains(query.nodesInfo[exists->srcNodePos].uniqName) &&
              retained.contains(query.nodesInfo[exists->dstNodePos].uniqName);
        }
      } catch (const std::exception&) {}
      return false;
    case ExpressionType::FUNCTION: {
      static const std::set<std::string> deterministic = {
          "+", "-", "*", "/", "%", "LABEL", "STRUCT_EXTRACT", "TIMESTAMP",
          "CAST_TO_INT64", "CAST_TO_DOUBLE", "CAST_TO_STRING", "CAST_TO_DATE",
          "CAST_TO_TIMESTAMP", "CAST_TO_FLOAT", "ROUND", "ABS", "FLOOR", "CEIL",
          "CEILING", "SIGN"};
      if (!deterministic.contains(expression->template constCast<ScalarFunctionExpression>()
                                      .getFunction().name)) return false;
      break;
    }
    case ExpressionType::CASE_ELSE:
      break;
    default:
      if (!ExpressionTypeUtil::isBoolean(expression->expressionType) &&
          !ExpressionTypeUtil::isComparison(expression->expressionType) &&
          !ExpressionTypeUtil::isNullOperator(expression->expressionType)) return false;
    }
    for (const auto& child : ExpressionChildrenCollector::collectChildren(*expression))
      if (!independent(child, currentMatchPredicate)) return false;
    return true;
  };
  bool found = false;
  for (uint32_t i = 0; i < wholeQuery->getNumQueryParts(); ++i) {
    const auto* prior = wholeQuery->getQueryPart(i);
    if (prior == &part) { found = true; break; }
    if (!prior->hasProjectionBody()) continue;
    for (const auto& expression : prior->getProjectionBody()->getProjectionExpressions()) {
      DependentVarNameCollector collector;
      collector.visit(expression);
      for (const auto& name : collector.getVarNames())
        if (removed.contains(name)) return false;
      if (removed.contains(expression->getUniqueName())) return false;
    }
  }
  if (!found) return false;
  for (uint32_t i = 0; i < part.getNumReadingClause(); ++i) {
    const auto* clause = part.getReadingClause(i);
    if (clause->getClauseType() == ClauseType::UNWIND) {
      const auto& unwind = clause->constCast<BoundUnwindClause>();
      if (!independent(unwind.getInExpr(), false) ||
          removed.contains(unwind.getOutExpr()->getUniqueName())) return false;
      continue;
    }
    if (clause->getClauseType() != ClauseType::MATCH) return false;
    const auto& other = clause->constCast<BoundMatchClause>();
    const auto* graphs = other.getQueryGraphCollection();
    if (&other != &match) {
      for (const auto& node : graphs->getQueryNodes())
        if (removed.contains(node->getUniqueName())) return false;
      for (const auto& rel : graphs->getQueryRels())
        if (removed.contains(rel->getUniqueName())) return false;
    }
    for (const auto& predicate : other.getConjunctivePredicates()) {
      if (&other == &match && sourceCovers(predicate)) continue;
      if (!independent(predicate, &other == &match)) return false;
    }
    for (const auto& rel : graphs->getQueryRels()) {
      if (!rel->isRecursive()) continue;
      const auto* recursive = rel->getRecursiveInfo();
      if (!independent(recursive->nodePredicate, false) ||
          !independent(recursive->relPredicate, false)) return false;
    }
  }
  const auto* body = part.getProjectionBody();
  for (const auto& expressions : {body->getProjectionExpressions(),
                                  body->getGroupByExpressions(), body->getOrderByExpressions()})
    for (const auto& expression : expressions)
      if (!independent(expression, false)) return false;
  return independent(part.getProjectionBodyPredicate(), false);
}

bool provesCompleteKeyGroupDistinctCounts(
    const catalog::FillIndexAuxInfo& info, const common::PatternIR& matched,
    const common::PatternIR& query, const BoundMatchClause& match,
    const NormalizedQueryPart& part, main::ClientContext& context,
    const NormalizedSingleQuery* wholeQuery,
    const std::unordered_set<const Expression*>& incomingExpressions) {
  using namespace common;
  using namespace catalog;
  // A NODE FILL grouped by its single anchor is the one-key form of the
  // same contract: COUNT(DISTINCT w) over the anchor's complete source group
  // (LDBC BI16: OPTIONAL MATCH (p)<-[:HASCREATOR]-(m)-[:HASTAG]->(t) WHERE
  // <window/tag> WITH p, count(DISTINCT m)). Duplicated anchor rows see the
  // same value set, so the caller lowers it with MAX rather than SUM.
  const bool nodeFill = info.getFillType() == FillEntryType::NODE;
  // A property carrier on an existing relationship (e.g. KNOWS(bi20_direct))
  // stores the group value on every carrier edge of the key pair. A complete
  // group read sees that one value on all its rows; under MATCH the caller
  // adds the group-presence test (rewriter_v4.cpp, REL presence predicate).
  const bool edgeCarrier = !nodeFill && info.getFillType() == FillEntryType::REL &&
      !info.getTableUniqName().empty();
  if (edgeCarrier && match.getMatchClauseType() != MatchClauseType::MATCH) return false;
  if ((!nodeFill && (info.getFillType() != FillEntryType::REL || info.isFillUndirectedRel())) ||
      info.getSourceQuery().empty() || !part.hasProjectionBody() ||
      (match.getMatchClauseType() != MatchClauseType::MATCH &&
       match.getMatchClauseType() != MatchClauseType::OPTIONAL_MATCH))
    return false;
  auto* carrier =
      // A FILL may target a REL group, which is not a table entry (lookup throws).
      context.getCatalog()->containsTable(context.getTransaction(), info.getTableName(), context.useInternalCatalogEntry()) ?
      context.getCatalog()->getTableCatalogEntry(context.getTransaction(), info.getTableName(), context.useInternalCatalogEntry()) : nullptr;
  if (carrier == nullptr || carrier->getType() != (nodeFill ? CatalogEntryType::NODE_TABLE_ENTRY
                                                            : CatalogEntryType::REL_TABLE_ENTRY))
    return false;
  if (nodeFill && (!info.getPresenceColumns().empty() || !info.getExistsPredicates().empty()))
    return false;
  // Recursive positions are verified against the rebound source below.
  std::vector<std::pair<size_t, RecursiveEdgeContract>> recursiveEdges;
  size_t carrierEdges = 0;
  for (size_t i = 0; i < matched.relsInfo.size(); ++i) {
    const auto& rel = matched.relsInfo[i];
    const bool carrierLabel = !nodeFill &&
        std::find(rel.relLabels.begin(), rel.relLabels.end(), carrier->getTableID()) != rel.relLabels.end();
    if (rel.matchedIdx >= query.relsInfo.size()) return false;
    if (carrierLabel) {
      // Only the single non-recursive carrier edge of an edge-carrier FILL.
      if (!edgeCarrier || rel.relLabels.size() != 1 ||
          rel.relType != QueryRelType::NON_RECURSIVE || ++carrierEdges > 1) return false;
      continue;
    }
    if (rel.relType == QueryRelType::NON_RECURSIVE) continue;
    // Recursive positions of NODE and REL FILLs are both checked against the
    // rebound source contract below (bounds, semantics and predicates).
    if (query.relsInfo[rel.matchedIdx].relType != rel.relType) return false;
    const auto& name = query.relsInfo[rel.matchedIdx].uniqName;
    const auto edges = match.getQueryGraphCollection()->getQueryRels();
    const auto actual = std::find_if(edges.begin(), edges.end(),
        [&](const auto& edge) { return edge->getUniqueName() == name; });
    if (actual == edges.end()) return false;
    auto contract = recursiveEdgeContract(**actual);
    if (!contract) return false;
    recursiveEdges.emplace_back(i, std::move(*contract));
  }
  std::set<std::string> keys;
  std::vector<std::string> keyNames;
  if (nodeFill) keyNames.push_back(info.getTableUniqName());
  else keyNames = {info.getSrcNodeUniqueName(), info.getDstNodeUniqueName()};
  for (const auto& name : keyNames) {
    const auto position = matched.nodeUniqNameToPos.find(name);
    if (position == matched.nodeUniqNameToPos.end() ||
        matched.nodesInfo[position->second].matchedIdx >= query.nodesInfo.size())
      return false;
    if (nodeFill && (matched.nodesInfo[position->second].nodeLabels.size() != 1 ||
                     matched.nodesInfo[position->second].nodeLabels[0] != carrier->getTableID()))
      return false;
    keys.insert(query.nodesInfo[matched.nodesInfo[position->second].matchedIdx].uniqName);
  }
  if (keys.size() != keyNames.size() || (edgeCarrier && carrierEdges != 1)) return false;
  std::set<std::string> groupedKeys;
  for (const auto& group : part.getProjectionBody()->getGroupByExpressions()) {
    const auto name = identity(group);
    if (keys.contains(name)) groupedKeys.insert(name);
  }
  if (groupedKeys != keys) return false;
  const auto aggregates = part.getProjectionBody()->getAggregateExpressions();
  if (aggregates.empty()) return false;
  // Projected scalars over the aggregates that match a stored finalizer
  // column; the whole scalar is replaced by that column.
  std::vector<const Expression*> finalizers;
  for (const auto& aggregate : aggregates) {
    if (!completeGroupAggregate(aggregate)) return false;
    // MIN/MAX/SUM of an empty OPTIONAL group is NULL (COUNT is 0) rather than
    // a stored default. A NODE FILL with a row count gets the OPTIONAL guard
    // CASE WHEN <rows> > 0 THEN value ELSE NULL/0 (rewriter_v4.cpp); without
    // one only COUNT(DISTINCT) is safe.
    if (!distinctCount(aggregate) && match.getMatchClauseType() != MatchClauseType::MATCH &&
        !(nodeFill && !info.getRowCountColumn().empty()))
      return false;
    if (match.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH &&
        aggregate->constCast<AggregateFunctionExpression>().getNumChildren() == 0)
      return false;   // COUNT(*) counts the padded row
    if (match.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH) {
      // A prebound endpoint can remain non-NULL when OPTIONAL has no match.
      // Only a removed witness is guaranteed to contribute zero on that row.
      const auto& operand = aggregate->constCast<AggregateFunctionExpression>().getChild(0);
      const auto name = operand->expressionType == ExpressionType::PROPERTY
          ? operand->constCast<PropertyExpression>().getVariableName()
          : operand->getUniqueName();
      bool hiddenWitness = false;
      for (const auto& node : matched.nodesInfo)
        hiddenWitness |= node.matchedIdx < query.nodesInfo.size() &&
            query.nodesInfo[node.matchedIdx].uniqName == name && !keys.contains(name);
      for (const auto& rel : matched.relsInfo)
        hiddenWitness |= rel.matchedIdx < query.relsInfo.size() &&
            query.relsInfo[rel.matchedIdx].uniqName == name;
      if (!hiddenWitness) return false;
    }
    IndexRewriteTemplateMatcher::AggRewriteTargetMap targets;
    if (!IndexRewriteTemplateMatcher::validateAggregateTemplatesForIndexRewrite(
            aggregate, info, query, matched, targets) || targets.size() != 1) {
      // A stored finalizer (e.g. min(...) + 1): the projected scalar holding
      // this aggregate must match a computation-with-aggregate column.
      targets.clear();
      bool finalizer = false;
      for (const auto& projected : part.getProjectionBody()->getProjectionExpressions()) {
        if (projected == nullptr || projected->expressionType == ExpressionType::AGGREGATE_FUNCTION)
          continue;
        std::vector<std::shared_ptr<Expression>> inner;
        if (!collectCompleteGroupAggregates(projected, inner) ||
            std::find(inner.begin(), inner.end(), aggregate) == inner.end()) continue;
        auto* scalar = projected->ptrCast<ScalarFunctionExpression>();
        if (scalar == nullptr ||
            !IndexRewriteTemplateMatcher::validateTemplatesForIndexRewrite(
                scalar, info, query, matched, targets) || targets.size() != 1) {
          targets.clear();
          continue;
        }
        finalizer = true;
        finalizers.push_back(projected.get());
        break;
      }
      if (!finalizer) return false;
    }
    const auto position = targets.begin()->second;
    if (position >= info.getTargetColumns().size() ||
        info.getTargetColumns()[position].starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) ||
        info.getTargetColumns()[position].starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX))
      return false;
  }
  // Earlier WITH values are input columns even when their bound objects still
  // contain an aggregate tree. Only the caller's exact incoming objects and
  // the complete aggregates proved above may hide their original arguments.
  auto covered = incomingExpressions;
  for (const auto& aggregate : aggregates) covered.insert(aggregate.get());
  covered.insert(finalizers.begin(), finalizers.end());
  // The per-pair default retains both ordered endpoints; a NODE group keeps
  // exactly its anchor identity and removes every other source binding.
  const std::unordered_set<std::string> retainedKeys(keys.begin(), keys.end());
  if (!completeKeyGroupInputsAreClosed(info, matched, query, match, part, wholeQuery,
                                      nodeFill ? &retainedKeys : nullptr, nullptr, &covered))
    return false;
  // The merged graph alone does not establish the materialized grouping.
  // Rebind the saved source without rewriting and require one closed mandatory
  // stage, with precisely its two returned node identities as grouping keys.
  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return false;
    Binder sourceBinder(&context);
    sourceBinder.setAllowIndexRewrite(false);
    auto statement = sourceBinder.bind(*parsed[0]);
    const auto& source = statement->constCast<BoundRegularQuery>();
    if (source.getNumSingleQueries() != 1 || !source.getPreQueryPart().empty() ||
        source.getPostSingleQuery()) return false;
    const auto& single = *source.getSingleQuery(0);
    if (single.getNumQueryParts() != 1) return false;
    const auto& sourcePart = *single.getQueryPart(0);
    const bool totalNodeCount = nodeFill && sourcePart.getNumReadingClause() == 2;
    if (sourcePart.hasUpdatingClause() ||
        (sourcePart.getNumReadingClause() != 1 && !totalNodeCount) ||
        !sourcePart.hasProjectionBody() || sourcePart.hasProjectionBodyPredicate())
      return false;
    if (totalNodeCount) {
      // MATCH(anchor) OPTIONAL MATCH(witness pattern) is a total per-anchor
      // DISTINCT count. The padded row contributes zero only for a fresh
      // witness, never for the outer anchor. Preserve the complete-group proof.
      const auto* prefix = sourcePart.getReadingClause(0);
      if (prefix->getClauseType() != ClauseType::MATCH) return false;
      const auto& scan = prefix->constCast<BoundMatchClause>();
      const auto* graphs = scan.getQueryGraphCollection();
      if (scan.getMatchClauseType() != MatchClauseType::MATCH || scan.hasHint() ||
          scan.hasPredicate() || graphs->getNumQueryGraphs() != 1) return false;
      const auto* graph = graphs->getQueryGraph(0);
      const auto values = sourcePart.getProjectionBody()->getProjectionExpressions();
      if (graph->getNumQueryNodes() != 1 || graph->getNumQueryRels() != 0 || values.empty() ||
          identity(values[0]) != graph->getQueryNode(0)->getUniqueName()) return false;
      const auto labels = graph->getQueryNode(0)->getEntries();
      if (labels.size() != 1 || labels[0]->getTableID() != carrier->getTableID()) return false;
      for (size_t i = 1; i < values.size(); ++i) {
        if (!distinctCount(values[i])) return false;
        const auto operand = values[i]->constCast<AggregateFunctionExpression>().getChild(0);
        // Nullable properties cannot establish mandatory group presence from
        // a zero value. This extension proves fresh graph identities only.
        if (operand->expressionType != ExpressionType::PATTERN) return false;
        if (operand->getUniqueName() == graph->getQueryNode(0)->getUniqueName()) return false;
      }
    }
    const auto* clause = sourcePart.getReadingClause(totalNodeCount ? 1 : 0);
    if (clause->getClauseType() != ClauseType::MATCH) return false;
    const auto& sourceMatch = clause->constCast<BoundMatchClause>();
    if (sourceMatch.getMatchClauseType() != (totalNodeCount ? MatchClauseType::OPTIONAL_MATCH
                                                          : MatchClauseType::MATCH) || sourceMatch.hasHint())
      return false;
    const auto sourceRels = sourceMatch.getQueryGraphCollection()->getQueryRels();
    size_t sourceRecursive = 0;
    for (const auto& rel : sourceRels) sourceRecursive += rel->isRecursive();
    if (sourceRecursive != recursiveEdges.size()) return false;
    if (!recursiveEdges.empty()) {
      const auto* graphs = sourceMatch.getQueryGraphCollection();
      if (graphs->getNumQueryGraphs() != 1) return false;
      const auto* graph = graphs->getQueryGraph(0);
      const auto source = convertGraphPatternToIR(*graph);
      const auto& saved = info.getPatternIR();
      if (source.relsInfo.size() != saved.relsInfo.size()) return false;
      const auto edges = graph->getQueryRels();
      for (const auto& [position, expected] : recursiveEdges) {
        if (position >= source.relsInfo.size() || position >= edges.size() ||
            source.relsInfo[position].varName != saved.relsInfo[position].varName ||
            source.relsInfo[position].relType != saved.relsInfo[position].relType) return false;
        const auto observed = recursiveEdgeContract(*edges[position]);
        if (!observed || !(*observed == expected)) return false;
      }
    }
    const auto& body = *sourcePart.getProjectionBody();
    if (body.isDistinct() || body.hasSkipOrLimit() || body.hasOrderByExpressions())
      return false;
    const auto values = body.getProjectionExpressions();
    const size_t keyCount = keyNames.size();
    if (values.size() < keyCount + 1) return false;
    std::set<std::string> sourceKeys;
    for (size_t i = 0; i < keyCount; ++i) sourceKeys.insert(identity(values[i]));
    if (sourceKeys.size() != keyCount || sourceKeys.contains("")) return false;
    std::set<std::string> sourceGroups;
    for (const auto& group : body.getGroupByExpressions()) {
      const auto name = identity(group);
      if (name.empty()) return false;
      sourceGroups.insert(name);
    }
    if (sourceGroups != sourceKeys) return false;
    for (size_t i = keyCount; i < values.size(); ++i)
      if (!completeGroupValue(values[i])) return false;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}
}  // namespace neug::binder

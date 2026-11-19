
#include "neug/compiler/binder/bound_scan_source.h"
#include "neug/compiler/binder/rewriter/computation_prefix_rewriter.h"
#include "neug/compiler/binder/rewriter/distinct_fill_shape.h"
#include "neug/compiler/binder/rewriter/endpoint_projection_contract.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/index_predicate_key.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/normalized_single_query.h"
#include "neug/compiler/binder/query/query_graph_label_analyzer.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"

#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_expr_converter.h"
#include "neug/compiler/catalog/catalog_entry/fill_functional_assumption.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"

#include "neug/compiler/common/assert.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/common/enums/scan_source_type.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/common/serializer/buffered_reader.h"
#include "neug/compiler/common/serializer/buffered_serializer.h"
#include "neug/compiler/common/serializer/deserializer.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/compiler/common/string_format.h"

#include "neug/compiler/main/client_context.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neug {
namespace catalog {

namespace {

bool isTrueLiteral(const binder::Expression& expression) {
  return expression.expressionType == common::ExpressionType::LITERAL &&
         expression.dataType.getLogicalTypeID() == common::LogicalTypeID::BOOL &&
         !expression.constCast<binder::LiteralExpression>().isNull() &&
         expression.constCast<binder::LiteralExpression>().getValue().getValue<bool>();
}

std::shared_ptr<::common::Expression> workloadSumArgument(
    const binder::AggregateFunctionExpression& aggregate,
    const binder::NormalizedSingleQuery& source,
    const common::PatternIR& pattern) {
  if (aggregate.getFunction().name != "SUM") return nullptr;
  // Fresh workload definitions need the same exact numeric and NULL
  // contracts as canonical FILLs. Name templates are not executable ASTs:
  // interpreting CASE constants through the old double-valued fallback
  // loses both the bound expression's type and its variable identity.
  validateCanonicalFillProjection(aggregate);
  if (aggregate.getNumChildren() != 1 ||
      !isFillSourceExpressionNonNull(*aggregate.getChild(0), source))
    THROW_BINDER_EXCEPTION("Workload FILL SUM requires a provably non-NULL source value.");
  const auto resolve = [&](const std::string& name) -> std::optional<uint32_t> {
    for (size_t i = 0; i < pattern.nodesInfo.size(); ++i)
      if (pattern.nodesInfo[i].uniqName == name)
        return static_cast<uint32_t>(i);
    for (size_t i = 0; i < pattern.relsInfo.size(); ++i)
      if (pattern.relsInfo[i].uniqName == name)
        return static_cast<uint32_t>(pattern.nodesInfo.size() + i);
    // The converter retries the bound unique name if a printable name was
    // rebound in another query part. Never pick an arbitrary earlier scope.
    std::optional<uint32_t> raw;
    for (size_t i = 0; i < pattern.nodesInfo.size(); ++i)
      if (pattern.nodesInfo[i].varName == name) {
        if (raw) return std::nullopt;
        raw = static_cast<uint32_t>(i);
      }
    for (size_t i = 0; i < pattern.relsInfo.size(); ++i)
      if (pattern.relsInfo[i].varName == name) {
        if (raw) return std::nullopt;
        raw = static_cast<uint32_t>(pattern.nodesInfo.size() + i);
      }
    return raw;
  };
  return ConvertFillIndexExprToAst(*aggregate.getChild(0), resolve);
}

template<typename T>
void appendVectorSignature(std::ostringstream& out, const std::vector<T>& values) {
  out << values.size() << '[';
  for (const auto& value : values) {
    out << value << ',';
  }
  out << ']';
}

void appendAdjSignature(std::ostringstream& out,
                        const std::vector<common::AdjacentInfo>& adjs) {
  out << adjs.size() << '[';
  for (const auto& adj : adjs) {
    out << adj.dstNodeIdx << ':';
    appendVectorSignature(out, adj.edgeLabels);
    out << ';';
  }
  out << ']';
}

void appendPatternSignature(std::ostringstream& out,
                            const common::PatternIR& patternIR) {
  out << "nodes=" << patternIR.nodesInfo.size() << '[';
  for (const auto& node : patternIR.nodesInfo) {
    out << node.varName << '|' << node.uniqName << '|';
    appendVectorSignature(out, node.nodeLabels);
    appendAdjSignature(out, node.undirected);
    appendAdjSignature(out, node.outgoing);
    appendAdjSignature(out, node.incoming);
    out << ';';
  }
  out << ']';
  out << "rels=" << patternIR.relsInfo.size() << '[';
  for (const auto& rel : patternIR.relsInfo) {
    out << rel.varName << '|' << rel.uniqName << '|' << rel.srcNodeIdx
        << "->" << rel.dstNodeIdx << '|'
        << static_cast<uint32_t>(rel.edgeType) << '|'
        << static_cast<uint32_t>(rel.relType) << '|';
    appendVectorSignature(out, rel.relLabels);
    out << ';';
  }
  out << ']';
}

void appendIndexColumnSignature(
    std::ostringstream& out,
    const std::vector<std::shared_ptr<IndexColnumInfo>>& indexColumnInfos) {
  out << "indexColumns=" << indexColumnInfos.size() << '{';
  for (const auto& info : indexColumnInfos) {
    if (info == nullptr) {
      out << "null;";
      continue;
    }
    out << static_cast<uint32_t>(info->columnType) << '|' << info->nodeUnique
        << '|' << info->propertyName << '|';
    if (auto* nodeAgg = dynamic_cast<NodeAggregationIndexColnumInfo*>(info.get())) {
      out << nodeAgg->aggregateFuncName << '|' << nodeAgg->nameTemplate << '|';
      appendVectorSignature(out, nodeAgg->nodeUniqueNames);
    } else if (auto* rel = dynamic_cast<RelIndexColnumInfo*>(info.get())) {
      out << rel->nameTemplate << '|';
      appendVectorSignature(out, rel->nodeUniqueNames);
    } else if (auto* relAgg =
                   dynamic_cast<RelComputationWithAggreColumnInfo*>(info.get())) {
      out << relAgg->nameTemplate << '|';
      appendVectorSignature(out, relAgg->nodeUniqueNames);
    }
    out << ';';
  }
  out << '}';
}

std::string makeFillIndexBaseName(
    const std::string& tableName, const std::vector<std::string>& columnNames,
    FillEntryType fillType, const common::PatternIR& patternIR,
    const std::vector<std::shared_ptr<IndexColnumInfo>>& indexColumnInfos,
    const std::string& extraSignature = "") {
  std::string baseName = "fill_" + tableName;
  for (const auto& columnName : columnNames) {
    baseName += "_" + columnName;
  }
  if (columnNames.empty()) {
    baseName += "_no_columns";
  }

  std::ostringstream signature;
  signature << tableName << '|' << static_cast<uint32_t>(fillType) << '|';
  appendVectorSignature(signature, columnNames);
  appendPatternSignature(signature, patternIR);
  appendIndexColumnSignature(signature, indexColumnInfos);
  signature << extraSignature;

  std::ostringstream hash;
  hash << std::hex << std::setw(16) << std::setfill('0')
       << std::hash<std::string>{}(signature.str());
  return baseName + "_" + hash.str().substr(0, 12);
}

std::vector<std::string> collectPredicateTemplates(
    const binder::NormalizedSingleQuery& singleQuery,
    const common::PatternIR& patternIR) {
  binder::IndexPredicateBindings bindings;
  for (size_t i = 0; i < patternIR.nodesInfo.size(); ++i) {
    bindings.emplace(patternIR.nodesInfo[i].uniqName, "node:" + std::to_string(i));
  }
  for (size_t i = 0; i < patternIR.relsInfo.size(); ++i) {
    bindings.emplace(patternIR.relsInfo[i].uniqName, "rel:" + std::to_string(i));
  }
  std::vector<std::string> templates;
  for (auto partIdx = 0u; partIdx < singleQuery.getNumQueryParts(); ++partIdx) {
    const auto* queryPart = singleQuery.getQueryPart(partIdx);
    for (auto clauseIdx = 0u; clauseIdx < queryPart->getNumReadingClause();
         ++clauseIdx) {
      const auto* clause = queryPart->getReadingClause(clauseIdx);
      if (clause == nullptr ||
          clause->getClauseType() != common::ClauseType::MATCH) {
        continue;
      }
      const auto* matchClause =
          clause->constPtrCast<binder::BoundMatchClause>();
      for (const auto& predicate : matchClause->getConjunctivePredicates()) {
        if (predicate != nullptr) {
          if (auto exists = extractFillExistsPredicate(*predicate, patternIR)) {
            templates.push_back(fillExistsPredicateKey(std::move(*exists)));
            continue;
          }
          // A workload source may contain scalar bindings from an earlier
          // stage. Keep its catalog entry available to complete-stage matching,
          // but never claim that a local graph rewrite covers that predicate.
          templates.push_back(binder::makeIndexPredicateKey(*predicate, bindings)
                                  .value_or("@unsupported-index-predicate"));
        }
      }
    }
  }
  std::sort(templates.begin(), templates.end());
  templates.erase(std::unique(templates.begin(), templates.end()),
                  templates.end());
  return templates;
}

std::string makePredicateSignature(
    const std::vector<std::string>& predicateTemplates) {
  std::ostringstream signature;
  signature << "|predicates=";
  appendVectorSignature(signature, predicateTemplates);
  return signature.str();
}

std::string makeUniqueExtendedIndexName(
    Catalog* catalog, transaction::Transaction* transaction,
    const std::string& baseName) {
  auto indexName = baseName;
  uint64_t suffix = 1;
  while (catalog->containsExtendedIndex(transaction, indexName)) {
    indexName = baseName + "_" + std::to_string(suffix++);
  }
  return indexName;
}

}  // namespace

// --- IndexColnumInfo serialization ---

using binder::BoundQueryScanSource;
using binder::BoundRegularQuery;
using binder::NormalizedSingleQuery;

std::vector<std::string> collectWorkloadFillPredicates(
    const NormalizedSingleQuery& query, const common::PatternIR& pattern) {
  return collectPredicateTemplates(query, pattern);
}

bool normalizeFillAggregateProjection(
    std::unique_ptr<binder::BoundBaseScanSource>& source,
    TableCatalogEntry* table, main::ClientContext* context) {
  using common::ExpressionType;
  using common::LogicalTypeID;
  if (source->type != common::ScanSourceType::QUERY) return false;
  auto& regular = source->cast<BoundQueryScanSource>()
                      .statement->cast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1 ||
      !regular.getPreQueryPart().empty() || regular.getPostSingleQuery())
    return false;
  auto* query = regular.getSingleQueryUnsafe(0);
  if (query->getNumQueryParts() != 2) return false;
  auto* input = query->getQueryPartUnsafe(0);
  const auto* output = query->getQueryPart(1);
  if (input->hasUpdatingClause() || output->hasUpdatingClause() ||
      !input->hasProjectionBody() || !output->hasProjectionBody() ||
      output->hasReadingClause() || output->hasProjectionBodyPredicate())
    return false;
  const auto* body = input->getProjectionBody();
  const auto* tail = output->getProjectionBody();
  const bool distinctPresence =
      table->getType() == CatalogEntryType::NODE_TABLE_ENTRY &&
      body->isDistinct() && !body->hasAggregateExpressions();
  if ((!distinctPresence &&
       (!body->hasAggregateExpressions() || body->isDistinct())) ||
      body->hasOrderByExpressions() || body->hasSkipOrLimit() ||
      tail->isDistinct() || tail->hasAggregateExpressions() ||
      tail->hasOrderByExpressions() || tail->hasSkipOrLimit()) return false;

  std::unordered_map<std::string, std::shared_ptr<binder::NodeExpression>> nodes;
  std::vector<binder::QueryGraph*> graphs;
  for (uint32_t ci = 0; ci < input->getNumReadingClause(); ++ci) {
    auto* clause = input->getReadingClause(ci);
    if (clause->getClauseType() != common::ClauseType::MATCH) return false;
    auto* match = &clause->cast<binder::BoundMatchClause>();
    if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
        match->hasHint()) return false;
    auto* collection = match->getQueryGraphCollectionUnsafe();
    for (const auto& node : collection->getQueryNodes())
      nodes.emplace(node->getUniqueName(), node);
    for (const auto& rel : collection->getQueryRels())
      if (rel->isRecursive()) return false;
    for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi)
      graphs.push_back(collection->getQueryGraphUnsafe(gi));
  }
  if (graphs.empty()) return false;
  const bool isNode = table->getType() == CatalogEntryType::NODE_TABLE_ENTRY;
  const auto keyCount = isNode ? 1u : 2u;
  const auto columns = source->getColumns();
  if (columns.size() <= keyCount) return false;
  std::unordered_map<std::string, TableCatalogEntry*> keys;
  for (uint32_t i = 0; i < keyCount; ++i) {
    if (columns[i]->expressionType != ExpressionType::PROPERTY) return false;
    const auto& property = columns[i]->constCast<binder::PropertyExpression>();
    const auto tableID = isNode ? table->getTableID()
        : i == 0 ? table->constCast<RelTableCatalogEntry>().getSrcTableID()
                 : table->constCast<RelTableCatalogEntry>().getDstTableID();
    const auto found = nodes.find(property.getVariableName());
    if (found == nodes.end() || !property.isPrimaryKey(tableID)) return false;
    auto* target = context->getCatalog()->getTableCatalogEntry(
        context->getTransaction(), tableID);
    const auto [it, inserted] = keys.emplace(found->first, target);
    if (!inserted && it->second->getTableID() != tableID) return false;
    const auto ids = found->second->getTableIDsSet();
    if (!ids.contains(tableID)) return false;
  }
  // Grouping must retain exactly the carrier graph identities. Keeping these
  // identities until after label dispatch prevents equal numeric primary keys
  // from different tables from being combined into one aggregate group.
  std::unordered_set<std::string> grouped;
  const auto identityExpressions = distinctPresence
      ? body->getProjectionExpressions() : body->getGroupByExpressions();
  for (const auto& group : identityExpressions) {
    if (group->expressionType != ExpressionType::PATTERN ||
        group->getDataType().getLogicalTypeID() != LogicalTypeID::NODE ||
        !keys.contains(group->getUniqueName())) return false;
    grouped.insert(group->getUniqueName());
  }
  if (grouped.size() != keys.size()) return false;
  const auto aggregates = body->getAggregateExpressions();
  const auto isAggregate = [&](const auto& expression) {
    return std::any_of(aggregates.begin(), aggregates.end(),
        [&](const auto& aggregate) { return expression.get() == aggregate.get(); });
  };
  for (const auto& expression : body->getProjectionExpressions()) {
    if (!isAggregate(expression) && !grouped.contains(expression->getUniqueName()))
      return false;
  }
  for (size_t i = keyCount; i < columns.size(); ++i)
    if (distinctPresence ? !isTrueLiteral(*columns[i]) : !isAggregate(columns[i]))
      return false;
  for (const auto& aggregate : aggregates)
    if (std::none_of(columns.begin() + keyCount, columns.end(),
        [&](const auto& column) { return column.get() == aggregate.get(); }))
      return false;

  const auto stringLiteral = [](const binder::Expression& expression)
      -> std::optional<std::string> {
    if (expression.expressionType != ExpressionType::LITERAL ||
        expression.getDataType().getLogicalTypeID() != LogicalTypeID::STRING)
      return std::nullopt;
    const auto& literal = expression.constCast<binder::LiteralExpression>();
    if (literal.isNull()) return std::nullopt;
    return literal.getValue().getValue<std::string>();
  };
  std::unordered_set<std::string> dispatched;
  if (input->hasProjectionBodyPredicate()) {
    for (const auto& predicate : input->getProjectionBodyPredicate()->splitOnAND()) {
      if (predicate->expressionType == ExpressionType::LITERAL &&
          predicate->getDataType().getLogicalTypeID() == LogicalTypeID::BOOL &&
          !predicate->constCast<binder::LiteralExpression>().isNull() &&
          predicate->constCast<binder::LiteralExpression>().getValue().getValue<bool>())
        continue;
      if (predicate->expressionType != ExpressionType::EQUALS ||
          predicate->getNumChildren() != 2) return false;
      auto label = predicate->getChild(0);
      auto value = stringLiteral(*predicate->getChild(1));
      if (!value) {
        value = stringLiteral(*label);
        label = predicate->getChild(1);
      }
      if (!value) return false;
      // LABEL on a single-label node is already a literal after binding.
      if (auto constant = stringLiteral(*label)) {
        if (*constant != *value) return false;
        continue;
      }
      if (label->expressionType != ExpressionType::FUNCTION ||
          label->constCast<binder::ScalarFunctionExpression>().getFunction().name != "LABEL" ||
          label->getNumChildren() != 2 ||
          label->getChild(0)->expressionType != ExpressionType::PROPERTY ||
          label->getChild(1)->expressionType != ExpressionType::LITERAL)
        return false;
      const auto& id = label->getChild(0)->constCast<binder::PropertyExpression>();
      const auto key = keys.find(id.getVariableName());
      if (!id.isInternalID() || key == keys.end() ||
          key->second->getLabel(context->getCatalog(), context->getTransaction()) != *value)
        return false;
      dispatched.insert(key->first);
    }
  }
  for (const auto& [name, target] : keys)
    if (nodes.at(name)->isMultiLabeled() && !dispatched.contains(name)) return false;

  // All proofs precede mutation. The label predicate is now exactly enforced
  // by the carrier's table domain. Original MATCH predicates, including their
  // EXISTS ASTs, remain intact for canonical dependency validation.
  for (auto* graph : graphs)
    for (uint32_t ni = 0; ni < graph->getNumQueryNodes(); ++ni) {
      auto node = graph->getQueryNode(ni);
      if (const auto key = keys.find(node->getUniqueName()); key != keys.end())
        node->setEntries({key->second});
    }
  binder::QueryGraphLabelAnalyzer analyzer(*context, true);
  for (auto* graph : graphs) analyzer.pruneLabel(*graph);
  for (uint32_t i = 0; i < keyCount; ++i) {
    auto& property = columns[i]->cast<binder::PropertyExpression>();
    common::table_id_map_t<binder::SingleLabelPropertyInfo> infos;
    infos.emplace(keys.at(property.getVariableName())->getTableID(),
                  binder::SingleLabelPropertyInfo(true, true));
    property.setInfos(std::move(infos));
  }
  auto merged = body->copy();
  merged.setProjectionExpressions(tail->getProjectionExpressions());
  merged.setProjectionNames(tail->getProjectionNames());
  // Each key now has one table and its declared primary key, so entity
  // identity and the emitted key identify exactly the same aggregate groups.
  merged.setGroupByExpressions(distinctPresence ? binder::expression_vector{}
      : binder::expression_vector(columns.begin(), columns.begin() + keyCount));
  input->setProjectionBody(std::move(merged));
  input->setProjectionBodyPredicate(nullptr);
  query->removeQueryPart(1);
  return true;
}

bool requiresWorkloadFillCompatibility(
    const std::unique_ptr<binder::BoundBaseScanSource>& source,
    const TableCatalogEntry* table, const std::vector<std::string>& columns) {
  if (source->type != common::ScanSourceType::QUERY) return false;
  const bool isNode = table->getType() == CatalogEntryType::NODE_TABLE_ENTRY;
  // Canonical NODE aggregates and derived REL aggregates need the regular
  // binder's argument/predicate ASTs and independent row-count state. Neither
  // a NODE carrier nor MANY_MANY multiplicity on a derived table requires
  // compatibility metadata. Only an existing parallel REL carrier still
  // needs the workload route below.
  const auto& regular = source->constCast<BoundQueryScanSource>()
                            .statement->constCast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1) return true;
  const auto* query = regular.getSingleQuery(0);
  if (query->getNumQueryParts() != 1) return true;
  const auto* part = query->getQueryPart(0);
  if (part->hasProjectionBody() && part->getProjectionBody()->isDistinct())
    return true;
  bool hasRelationship = false;
  for (uint32_t i = 0; i < part->getNumReadingClause(); ++i) {
    const auto* clause = part->getReadingClause(i);
    if (clause->getClauseType() != common::ClauseType::MATCH) return true;
    const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
    if (match->getMatchClauseType() != common::MatchClauseType::MATCH) return true;
    for (const auto& rel : match->getQueryGraphCollection()->getQueryRels()) {
      hasRelationship = true;
      if (rel->isRecursive()) return true;
    }
  }
  if (!hasRelationship) return true;
  const auto expressions = source->getColumns();
  if (expressions.size() <= (isNode ? 1u : 2u)) return true;
  // Preserve workload NODE families whose target key spans physical labels;
  // the regular builder currently requires a single-label target.
  if (isNode &&
      expressions[0]->expressionType == common::ExpressionType::PROPERTY &&
      !expressions[0]->constCast<binder::PropertyExpression>().isSingleLabel())
    return true;
  for (size_t i = isNode ? 1 : 2; i < expressions.size(); ++i) {
    if (!isCanonicalFillAggregate(*expressions[i]) &&
        !isMaintainableFillFinalizer(*expressions[i])) return true;
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto& name = columns[i];
    if (!table->containsProperty(name)) continue;
    const auto type = table->getProperty(name).getType().getLogicalTypeID();
    if (type != common::LogicalTypeID::INT32 &&
        type != common::LogicalTypeID::INT64) {
      const auto expression = expressions.at(i + (isNode ? 1 : 2));
      const bool exactHalfSum = expression->expressionType == common::ExpressionType::AGGREGATE_FUNCTION &&
          expression->getNumChildren() == 1 &&
          expression->getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE &&
          isExactHalfIntegerFillExpression(*expression->getChild(0));
      if (type != common::LogicalTypeID::DOUBLE ||
          (!exactHalfSum && !isMaintainableFillFinalizer(*expression)))
        return true;
    }
  }
  return false;
}

// Helper: collect all QueryGraphs from all QueryParts, convert each to a
// PatternIR, and merge them into a single full PatternIR.
static common::PatternIR buildFullPatternIR(
    const NormalizedSingleQuery& singleQuery,
    std::vector<common::PatternIR>& outPerGraphPatterns) {
  outPerGraphPatterns.clear();
  for (auto i = 0u; i < singleQuery.getNumQueryParts(); i++) {
    auto queryPart = singleQuery.getQueryPart(i);
    for (uint32_t ci = 0; ci < queryPart->getNumReadingClause(); ++ci) {
      auto clause = queryPart->getReadingClause(ci);
      if (clause->getClauseType() != common::ClauseType::MATCH) continue;
      auto matchClause = clause->constPtrCast<binder::BoundMatchClause>();
      auto patternGraph = matchClause->getQueryGraphCollection();
      for (auto gi = 0u; gi < patternGraph->getNumQueryGraphs(); ++gi) {
        const auto* graph = patternGraph->getQueryGraph(gi);
        outPerGraphPatterns.emplace_back(common::convertGraphPatternToIR(*graph));
      }
    }
  }
  if (outPerGraphPatterns.empty()) {
    return common::PatternIR{};
  }
  if (outPerGraphPatterns.size() == 1) {
    return outPerGraphPatterns[0];
  }
  std::vector<std::vector<uint32_t>> _nodeRemaps;
  return common::PatternIR::merge(outPerGraphPatterns, _nodeRemaps);
}

namespace {

// DISTINCT(primary-key, true...) stores an existential value, not an arbitrary
// source row. A private COUNT(*) per key makes deleting one of several parallel
// witnesses safe. Keep this proof separate from property-copy functionality:
// neither DISTINCT(key, other-property) nor an OPTIONAL row proves existence.
bool isDistinctNodePresenceSource(const binder::NormalizedSingleQuery& query,
                                 const TableCatalogEntry& table) {
  if (table.getType() != CatalogEntryType::NODE_TABLE_ENTRY ||
      query.getNumQueryParts() != 1) return false;
  const auto* part = query.getQueryPart(0);
  if (!part->hasProjectionBody() || part->hasUpdatingClause() ||
      part->hasProjectionBodyPredicate() || part->getNumReadingClause() != 1)
    return false;
  const auto* body = part->getProjectionBody();
  if (!body->isDistinct() || body->hasAggregateExpressions() ||
      body->hasOrderByExpressions() || body->hasSkipOrLimit()) return false;
  const auto* clause = part->getReadingClause(0);
  if (clause->getClauseType() != common::ClauseType::MATCH) return false;
  const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
  if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
      match->hasHint()) return false;
  for (const auto& rel : match->getQueryGraphCollection()->getQueryRels()) {
    if (rel->isRecursive()) return false;
  }
  const auto columns = query.getStatementResult()->getColumns();
  if (columns.size() < 2 ||
      columns[0]->expressionType != common::ExpressionType::PROPERTY)
    return false;
  const auto& key = columns[0]->constCast<binder::PropertyExpression>();
  return key.isSingleLabel() && key.isPrimaryKey(table.getTableID()) &&
      std::all_of(columns.begin() + 1, columns.end(), [](const auto& column) {
        return isTrueLiteral(*column);
      });
}

// An identity grouping key partitions bags without losing information needed
// by a later mandatory MATCH. Computed/grouped scalar values cannot substitute
// for the bound node identity in this proof.
std::string identityGroupNode(const binder::Expression& expression) {
  if (expression.expressionType == common::ExpressionType::PATTERN &&
      expression.dataType.getLogicalTypeID() == common::LogicalTypeID::NODE)
    return expression.getUniqueName();
  if (expression.expressionType == common::ExpressionType::PROPERTY) {
    const auto& property = expression.constCast<binder::PropertyExpression>();
    if (property.isSingleLabel() &&
        property.isPrimaryKey(property.getSingleTableID()))
      return property.getVariableName();
  }
  return {};
}

// COUNT(*) (or COUNT of a required node) grouped by identities, followed by
// mandatory MATCH and SUM of that
// count, equals COUNT(*) on the union of the MATCH patterns. Every aggregation
// boundary must carry exactly that count and identity keys; neither DISTINCT,
// optional rows, filtering aggregate results nor a second COUNT is distributive.
std::shared_ptr<binder::Expression> flattenedCountExpression(
    const binder::NormalizedSingleQuery& query, const TableCatalogEntry& table) {
  std::shared_ptr<binder::Expression> count;
  uint32_t lastAggregation = UINT32_MAX;
  std::unordered_map<std::string, std::string> nodeBindings;
  std::unordered_set<std::string> requiredNodes;
  std::unordered_set<std::string> relationshipBindings;
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi) {
    const auto* part = query.getQueryPart(pi);
    if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
        part->hasProjectionBodyPredicate()) return nullptr;
    const auto* projection = part->getProjectionBody();
    if (projection->isDistinct() || projection->hasOrderByExpressions() ||
        projection->hasSkipOrLimit()) return nullptr;
    for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
      const auto* clause = part->getReadingClause(ci);
      if (clause->getClauseType() != common::ClauseType::MATCH) return nullptr;
      const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
      if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
          match->hasHint()) return nullptr;
      for (const auto& node : match->getQueryGraphCollection()->getQueryNodes()) {
        requiredNodes.insert(node->getUniqueName());
        const auto& variable = node->getVariableName();
        if (variable.empty()) continue;
        const auto [it, inserted] =
            nodeBindings.emplace(variable, node->getUniqueName());
        // PatternIR's legacy merge falls back to raw variable names. A name
        // reused after WITH has a new identity and must never be merged with
        // the dropped binding by this flattening proof.
        if (!inserted && it->second != node->getUniqueName()) return nullptr;
      }
      for (const auto& rel : match->getQueryGraphCollection()->getQueryRels()) {
        if (rel->isRecursive() ||
            !relationshipBindings.insert(rel->getUniqueName()).second)
          return nullptr;
      }
    }
    const auto aggregates = projection->getAggregateExpressions();
    if (!aggregates.empty()) {
      if (aggregates.size() != 1) return nullptr;
      const auto& aggregate =
          aggregates[0]->constCast<binder::AggregateFunctionExpression>();
      if (aggregate.isDistinct()) return nullptr;
      const auto& name = aggregate.getFunction().name;
      if (!count) {
        if (name != "COUNT_STAR" && name != "COUNT") return nullptr;
        if (aggregate.getNumChildren() != 0) {
          if (aggregate.getNumChildren() != 1) return nullptr;
          const auto& argument = aggregate.getChild(0);
          // A node bound by one of the mandatory MATCH clauses is present in
          // every input row. COUNT(node) is therefore exactly COUNT(*), while
          // COUNT(properties/nullable expressions) must retain its own state.
          if (argument->expressionType != common::ExpressionType::PATTERN ||
              argument->getDataType().getLogicalTypeID() != common::LogicalTypeID::NODE ||
              !requiredNodes.contains(argument->getUniqueName())) return nullptr;
        }
      } else if (name != "SUM" || aggregate.getNumChildren() != 1 ||
                 aggregate.getChild(0).get() != count.get()) {
        return nullptr;
      }
      count = aggregates[0];
      lastAggregation = pi;
      if (projection->getGroupByExpressions().empty()) return nullptr;
      for (const auto& key : projection->getGroupByExpressions())
        if (identityGroupNode(*key).empty() && !isTrueLiteral(*key)) return nullptr;
    }
    for (const auto& value : projection->getProjectionExpressions()) {
      if (value.get() != count.get() && identityGroupNode(*value).empty() &&
          !isTrueLiteral(*value)) return nullptr;
    }
  }
  if (!count) return nullptr;
  const auto output = query.getStatementResult()->getColumns();
  const bool isNode = table.getType() == CatalogEntryType::NODE_TABLE_ENTRY;
  const auto keyCount = isNode ? 1u : 2u;
  if (output.size() <= keyCount) return nullptr;
  std::unordered_set<std::string> anchors;
  for (uint32_t i = 0; i < keyCount; ++i) {
    if (output[i]->expressionType != common::ExpressionType::PROPERTY) return nullptr;
    const auto& key = output[i]->constCast<binder::PropertyExpression>();
    const auto tableID = isNode ? table.getTableID()
        : i == 0 ? table.constCast<RelTableCatalogEntry>().getSrcTableID()
                 : table.constCast<RelTableCatalogEntry>().getDstTableID();
    if (!key.isSingleLabel() || !key.isPrimaryKey(tableID)) return nullptr;
    anchors.insert(key.getVariableName());
  }
  std::unordered_set<std::string> finalGroups;
  for (const auto& group : query.getQueryPart(lastAggregation)
                               ->getProjectionBody()->getGroupByExpressions()) {
    if (isTrueLiteral(*group)) continue;
    const auto node = identityGroupNode(*group);
    if (!anchors.contains(node)) return nullptr;
    finalGroups.insert(node);
  }
  if (finalGroups != anchors) return nullptr;
  for (uint32_t pi = lastAggregation + 1; pi < query.getNumQueryParts(); ++pi)
    if (query.getQueryPart(pi)->hasReadingClause()) return nullptr;
  for (size_t i = keyCount; i < output.size(); ++i)
    if (output[i].get() != count.get() && !isTrueLiteral(*output[i])) return nullptr;
  if (count->dataType.getLogicalTypeID() != common::LogicalTypeID::INT64)
    return nullptr;
  return count;
}

}  // namespace

bool normalizeFillSingleEdgeDistinctCount(
    std::unique_ptr<binder::BoundBaseScanSource>& source, TableCatalogEntry* table,
    const std::shared_ptr<binder::Expression>& rowCount) {
  using common::ExpressionType;
  if (source->type != common::ScanSourceType::QUERY ||
      table->getType() != CatalogEntryType::REL_TABLE_ENTRY || !rowCount)
    return false;
  auto& regular = source->cast<BoundQueryScanSource>()
                      .statement->cast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1 ||
      !regular.getPreQueryPart().empty() || regular.getPostSingleQuery())
    return false;
  auto* query = regular.getSingleQueryUnsafe(0);
  if (query->getNumQueryParts() != 1) return false;
  auto* part = query->getQueryPartUnsafe(0);
  if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
      part->hasProjectionBodyPredicate() || part->getNumReadingClause() != 1)
    return false;
  auto* body = part->getProjectionBodyUnsafe();
  if (body->isDistinct() || body->hasOrderByExpressions() ||
      body->hasSkipOrLimit()) return false;
  const auto* clause = part->getReadingClause(0);
  if (clause->getClauseType() != common::ClauseType::MATCH) return false;
  const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
  if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
      match->hasHint()) return false;
  const auto* collection = match->getQueryGraphCollection();
  if (collection->getNumQueryGraphs() != 1) return false;
  const auto* graph = collection->getQueryGraph(0);
  const auto rels = graph->getQueryRels();
  if (graph->getNumQueryNodes() != 2 || rels.size() != 1 ||
      rels[0]->isRecursive() ||
      rels[0]->getDirectionType() != binder::RelDirectionType::SINGLE ||
      rels[0]->getSrcNodeName() == rels[0]->getDstNodeName()) return false;
  for (const auto* entry : rels[0]->getEntries())
    if (entry->getTableID() == table->getTableID()) return false;
  for (const auto& predicate : match->getConjunctivePredicates()) {
    binder::SubqueryExprCollector collector;
    collector.visit(predicate);
    if (collector.hasSubquery()) return false;
  }
  const auto columns = source->getColumns();
  const auto aggregates = body->getAggregateExpressions();
  const auto projections = body->getProjectionExpressions();
  if (columns.size() != 3 || aggregates.size() != 1 || projections.size() != 3 ||
      columns[2].get() != aggregates[0].get()) return false;
  for (size_t i = 0; i < columns.size(); ++i)
    if (columns[i].get() != projections[i].get()) return false;
  const auto& aggregate = aggregates[0]->constCast<binder::AggregateFunctionExpression>();
  if (aggregate.getFunction().name != "COUNT" || !aggregate.isDistinct() ||
      aggregate.getNumChildren() != 1 ||
      aggregate.getChild(0)->expressionType != ExpressionType::PATTERN ||
      aggregate.getChild(0)->getDataType().getLogicalTypeID() != common::LogicalTypeID::REL ||
      aggregate.getChild(0)->getUniqueName() != rels[0]->getUniqueName()) return false;
  const auto& target = table->constCast<RelTableCatalogEntry>();
  std::unordered_set<std::string> keys;
  for (size_t i = 0; i < 2; ++i) {
    if (columns[i]->expressionType != ExpressionType::PROPERTY) return false;
    const auto& property = columns[i]->constCast<binder::PropertyExpression>();
    const auto tableID = i == 0 ? target.getSrcTableID() : target.getDstTableID();
    if (!property.isSingleLabel() || !property.isPrimaryKey(tableID)) return false;
    keys.insert(property.getVariableName());
  }
  if (keys.size() != 2 || !keys.contains(rels[0]->getSrcNodeName()) ||
      !keys.contains(rels[0]->getDstNodeName())) return false;
  std::unordered_set<std::string> groups;
  for (const auto& group : body->getGroupByExpressions()) {
    const auto identity = identityGroupNode(*group);
    if (!keys.contains(identity)) return false;
    groups.insert(identity);
  }
  if (groups != keys) return false;

  // A directed single-edge MATCH visits each physical edge once per ordered
  // endpoint group. Parallel edges have distinct identities and still count
  // separately. BOTH traversal is excluded because a self-loop may be visited
  // twice. Use a fully bound ordinary COUNT, never just change a DISTINCT flag
  // while retaining the old aggregation implementation.
  rowCount->setUniqueName(aggregates[0]->getUniqueName());
  rowCount->setAlias(aggregate.getAlias());
  rowCount->setParsedName("COUNT(*)");
  auto projected = projections;
  projected[2] = rowCount;
  body->setProjectionExpressions(std::move(projected));
  body->setAggregateExpressions({rowCount});
  auto replaceResult = [&](const binder::BoundStatementResult& result) {
    auto values = result.getColumns();
    values[2] = rowCount;
    return binder::BoundStatementResult(std::move(values), result.getColumnNames());
  };
  *regular.getStatementResultUnsafe() = replaceResult(*regular.getStatementResult());
  query->setStatementResult(replaceResult(*query->getStatementResult()));
  return true;
}

std::optional<std::vector<std::pair<std::string, common::LogicalType>>>
tryCreateFlattenedCountIndexEntry(
    std::unique_ptr<binder::BoundBaseScanSource>& boundSource,
    TableCatalogEntry* table, main::ClientContext* context,
    std::vector<std::string> columnNames, const std::string& sourceQuery,
    std::shared_ptr<binder::Expression> presenceRowCount) {
  if (boundSource->type != common::ScanSourceType::QUERY) return std::nullopt;
  auto& regular = boundSource->cast<BoundQueryScanSource>()
                      .statement->cast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1 ||
      !regular.getPreQueryPart().empty() || regular.getPostSingleQuery())
    return std::nullopt;
  auto* query = regular.getSingleQueryUnsafe(0);
  auto count = flattenedCountExpression(*query, *table);
  const bool distinctPresence = !count && presenceRowCount &&
      isDistinctNodePresenceSource(*query, *table);
  if (distinctPresence) count = std::move(presenceRowCount);
  if (!count) return std::nullopt;
  const bool isNode = table->getType() == CatalogEntryType::NODE_TABLE_ENTRY;
  const auto keyCount = isNode ? 1u : 2u;
  const auto fillType = isNode ? FillEntryType::NODE : FillEntryType::REL;
  const auto columns = boundSource->getColumns();
  std::vector<std::string> aggregateTargets, presenceTargets;
  for (size_t i = keyCount; i < columns.size(); ++i) {
    const auto& name = columnNames.at(i - keyCount);
    const auto type = table->getProperty(name).getType().getLogicalTypeID();
    if (isTrueLiteral(*columns[i])) {
      if (type != common::LogicalTypeID::BOOL)
        THROW_BINDER_EXCEPTION("FILL presence output requires BOOL storage.");
      presenceTargets.push_back(name);
    } else {
      if (!presenceTargets.empty())
        THROW_BINDER_EXCEPTION(
            "Flattened COUNT FILL must project count values before presence outputs.");
      if (type != common::LogicalTypeID::INT64)
        THROW_BINDER_EXCEPTION("Flattened COUNT FILL requires INT64 storage.");
      aggregateTargets.push_back(name);
    }
  }
  std::vector<common::PatternIR> graphs;
  auto pattern = buildFullPatternIR(*query, graphs);
  if (pattern.relsInfo.empty()) return std::nullopt;
  // Only a newly derived relation has one physical edge per endpoint group.
  // Existing parallel carriers require an independent physical-edge proof.
  if (!isNode && std::any_of(pattern.relsInfo.begin(), pattern.relsInfo.end(),
      [&](const auto& rel) {
        return std::find(rel.relLabels.begin(), rel.relLabels.end(),
                         table->getTableID()) != rel.relLabels.end();
      })) return std::nullopt;
  for (size_t ni = 0; ni < pattern.nodesInfo.size(); ++ni)
    if (std::none_of(pattern.relsInfo.begin(), pattern.relsInfo.end(),
        [&](const auto& rel) { return rel.srcNodeIdx == ni || rel.dstNodeIdx == ni; }))
      return std::nullopt;
  std::vector<size_t> anchorPositions;
  for (uint32_t i = 0; i < keyCount; ++i) {
    const auto& key = columns[i]->constCast<binder::PropertyExpression>();
    const auto found = pattern.nodeUniqNameToPos.find(key.getVariableName());
    if (found == pattern.nodeUniqNameToPos.end()) return std::nullopt;
    const auto& labels = pattern.nodesInfo[found->second].nodeLabels;
    if (labels.size() != 1 || labels.front() != key.getSingleTableID())
      return std::nullopt;
    anchorPositions.push_back(found->second);
  }
  const auto anchor = pattern.nodesInfo[anchorPositions[0]];
  const auto destination = pattern.nodesInfo[anchorPositions.back()];

  std::string signature = sourceQuery;
  for (const auto& name : columnNames) signature += '\0' + name;
  std::ostringstream hiddenName;
  hiddenName << common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX << std::hex
             << std::hash<std::string>{}(signature);
  const auto hidden = hiddenName.str();
  aggregateTargets.push_back(hidden);
  std::vector<std::shared_ptr<IndexColnumInfo>> infos;
  for (size_t i = 0; i < aggregateTargets.size(); ++i) {
    if (isNode) {
      auto info = std::make_shared<NodeAggregationIndexColnumInfo>();
      info->columnType = IndexColumnType::NODE_AGGREGATE;
      info->aggregateFuncName = "COUNT";
      info->nameTemplate = "COUNT(*)";
      infos.push_back(std::move(info));
    } else {
      auto info = std::make_shared<RelIndexColnumInfo>();
      info->columnType = IndexColumnType::REL_AGGREGATE;
      info->aggregateFuncName = "COUNT";
      info->nameTemplate = "COUNT(*)";
      infos.push_back(std::move(info));
    }
  }
  auto targets = aggregateTargets;
  targets.insert(targets.end(), presenceTargets.begin(), presenceTargets.end());
  auto aux = std::make_unique<FillIndexAuxInfo>(
      fillType, table->getName(), isNode ? anchor.uniqName : "",
      isNode ? anchor.varName : "",
      targets, infos, std::move(pattern), sourceQuery);
  if (!isNode) {
    const auto& relation = table->constCast<RelTableCatalogEntry>();
    aux->setSrcTableId(relation.getSrcTableID());
    aux->setDstTableId(relation.getDstTableID());
    aux->setEndpointUniqueNames(anchor.uniqName, destination.uniqName);
    aux->setFillUndirectedRel(false);
  }
  aux->setAggregateNumericContract(FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  aux->setAggregateForm(FillAggregateForm::CANONICAL_BASE);
  aux->setPresenceColumns(std::move(presenceTargets));
  setFillPredicateMetadata(*aux, *query);
  aux->setPredicateTemplates(collectPredicateTemplates(*query, aux->getPatternIR()));
  auto* catalog = context->getCatalog();
  auto* transaction = context->getTransaction();
  validateFillStateOwnership(*aux, table->getTableID(), catalog, transaction);
  auto baseName = makeFillIndexBaseName(
      table->getName(), targets, fillType, aux->getPatternIR(), infos,
      makePredicateSignature(aux->getPredicateTemplates()));
  const auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  std::vector<common::property_id_t> propertyIDs;
  for (const auto& name : columnNames) propertyIDs.push_back(table->getPropertyID(name));
  catalog->createExtendedIndex(transaction, std::make_unique<IndexCatalogEntry>(
      "FILL", table->getTableID(), indexName, std::move(propertyIDs), std::move(aux)));

  if (distinctPresence) {
    // All public values are true literals and the sole key is this carrier's
    // primary key. Grouping the original witness bag therefore has exactly
    // the original DISTINCT public output, including no row on empty input.
    // The hidden count is not a public multiplicity available to rewrites.
    auto* part = query->getQueryPartUnsafe(0);
    binder::BoundProjectionBody grouped{false};
    grouped.setProjectionExpressions(part->getProjectionBody()->getProjectionExpressions());
    grouped.setProjectionNames(part->getProjectionBody()->getProjectionNames());
    grouped.setGroupByExpressions({columns[0]});
    grouped.setAggregateExpressions({count});
    part->setProjectionBody(std::move(grouped));
  }

  // Copy the already-computed aggregate into a distinct output slot. A plain
  // second reference is deduplicated by the logical schema, losing the hidden
  // COPY column; adding integer zero preserves the count and its own identity.
  // This does not count groups or alter an aggregation boundary.
  auto state = std::make_shared<binder::ScalarFunctionExpression>(
      common::ExpressionType::FUNCTION,
      std::make_unique<function::ScalarFunction>(
          "+", std::vector<common::LogicalTypeID>{common::LogicalTypeID::INT64,
                                                 common::LogicalTypeID::INT64},
          common::LogicalTypeID::INT64),
      std::make_unique<function::FunctionBindData>(common::LogicalType::INT64()),
      binder::expression_vector{
          count, std::make_shared<binder::LiteralExpression>(common::Value(int64_t{0}),
                                                             hidden + "_zero")},
      hidden);
  state->setAlias(hidden);
  auto* projection = query->getQueryPartUnsafe(query->getNumQueryParts() - 1)
                         ->getProjectionBodyUnsafe();
  auto projected = projection->getProjectionExpressions();
  projected.push_back(state);
  projection->setProjectionExpressions(std::move(projected));
  auto appendResult = [&](const binder::BoundStatementResult& original) {
    auto values = original.getColumns();
    auto names = original.getColumnNames();
    values.push_back(state);
    names.push_back(hidden);
    return binder::BoundStatementResult(std::move(values), std::move(names));
  };
  *regular.getStatementResultUnsafe() = appendResult(*regular.getStatementResult());
  query->setStatementResult(appendResult(*query->getStatementResult()));
  std::vector<std::pair<std::string, common::LogicalType>> states;
  states.emplace_back(hidden, count->dataType.copy());
  return states;
}

// Helper: find the node position in a PatternIR by table label ID, and record
// the table uniqName and varName.
static bool findTableNodeInPattern(const common::PatternIR& pattern,
                                    common::table_id_t tableID,
                                    size_t& outNodePos,
                                    std::string& outUniqName,
                                    std::string& outVarName) {
  for (size_t i = 0; i < pattern.nodesInfo.size(); ++i) {
    const auto& labels = pattern.nodesInfo[i].nodeLabels;
    if (std::find(labels.begin(), labels.end(), tableID) != labels.end()) {
      outNodePos = i;
      outUniqName = pattern.nodesInfo[i].uniqName;
      outVarName = pattern.nodesInfo[i].varName;
      return true;
    }
  }
  return false;
}

static void rejectUnmaintainedGraphPredicates(
    const NormalizedSingleQuery& query) {
  auto reject = [](const std::shared_ptr<binder::Expression>& expression) {
    if (!expression) return;
    binder::SubqueryExprCollector collector;
    collector.visit(expression);
    if (collector.hasSubquery())
      THROW_BINDER_EXCEPTION(
          "Correlated EXISTS FILL requires canonical aggregates or a "
          "proven identity-grouped COUNT(*) / SUM(count) pipeline.");
  };
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi) {
    const auto* part = query.getQueryPart(pi);
    if (part->hasProjectionBodyPredicate()) reject(part->getProjectionBodyPredicate());
    if (part->hasProjectionBody()) {
      for (const auto& expression : part->getProjectionBody()->getProjectionExpressions())
        reject(expression);
    }
    for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
      const auto* clause = part->getReadingClause(ci);
      if (clause->getClauseType() != common::ClauseType::MATCH) continue;
      const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
      for (const auto& predicate : match->getConjunctivePredicates()) reject(predicate);
    }
  }
}

// These values are results of the complete staged source. In particular an
// OPTIONAL COUNT followed by OPTIONAL expansion and SUM is not COUNT over a
// merged mandatory graph. Keep only the source and its identity/presence
// contract; typed computation-prefix matching rebinds and proves every stage.
static std::string createStagedCountSumNodeIndexEntry(
    const BoundRegularQuery& source, TableCatalogEntry* table,
    main::ClientContext* ctx, const std::vector<std::string>& columns,
    const std::string& sourceText) {
  if (source.getNumSingleQueries() != 1 ||
      !source.getPreQueryPart().empty() || source.getPostSingleQuery() ||
      sourceText.empty()) return {};
  const auto& single = *source.getSingleQuery(0);
  if (single.getNumQueryParts() < 3) return {};
  const auto* wrapper = single.getQueryPart(single.getNumQueryParts() - 1);
  if (!wrapper->hasProjectionBody()) return {};
  const auto values = wrapper->getProjectionBody()->getProjectionExpressions();
  if (values.size() != columns.size() + 1 || values.empty() ||
      values[0]->expressionType != common::ExpressionType::PROPERTY) return {};
  const auto& key = values[0]->constCast<binder::PropertyExpression>();
  if (!key.isSingleLabel() || !key.isPrimaryKey(table->getTableID()) ||
      !binder::supportsStagedCountSumFill(single, key.getVariableName())) return {};
  std::vector<common::PatternIR> local;
  auto pattern = buildFullPatternIR(single, local);
  const auto anchor = std::find_if(pattern.nodesInfo.begin(), pattern.nodesInfo.end(),
      [&](const auto& node) { return node.uniqName == key.getVariableName(); });
  if (anchor == pattern.nodesInfo.end() || anchor->nodeLabels.size() != 1 ||
      anchor->nodeLabels[0] != table->getTableID()) return {};
  const auto anchorName = anchor->uniqName, anchorVariable = anchor->varName;
  std::vector<std::string> presence;
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i]->getDataType() != table->getProperty(columns[i - 1]).getType())
      return {};
    if (isTrueLiteral(*values[i])) presence.push_back(columns[i - 1]);
  }
  auto predicates = collectPredicateTemplates(single, pattern);
  predicates.push_back("@staged-count-sum-fill-v1");
  // Store exactly the source properties used by predicates/aggregates/keys.
  // A projected node is an identity, not a read of every schema property.
  // These persisted dependencies let writes fail before exposing a stale view
  // until ordered pipeline incremental maintenance is implemented.
  std::unordered_set<std::string> propertyDependencies;
  std::function<void(const std::shared_ptr<binder::Expression>&)> collect;
  collect = [&](const auto& expression) {
    if (!expression) return;
    if (expression->expressionType == common::ExpressionType::PROPERTY) {
      const auto& property = expression->template constCast<binder::PropertyExpression>();
      if (property.isInternalID()) return;
      const auto binding = property.getVariableName();
      const auto add = [&](const auto& labels) {
        for (const auto label : labels)
          propertyDependencies.insert("@staged-source-property:" + std::to_string(label) +
                                      ":" + property.getPropertyName());
      };
      for (const auto& node : pattern.nodesInfo)
        if (node.uniqName == binding) add(node.nodeLabels);
      for (const auto& rel : pattern.relsInfo)
        if (rel.uniqName == binding) add(rel.relLabels);
      return;
    }
    if (expression->expressionType == common::ExpressionType::PATTERN) return;
    for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(*expression))
      collect(child);
  };
  for (size_t p = 0; p < single.getNumQueryParts(); ++p) {
    const auto* part = single.getQueryPart(p);
    for (const auto& value : part->getProjectionBody()->getProjectionExpressions()) collect(value);
    collect(part->getProjectionBodyPredicate());
    for (size_t c = 0; c < part->getNumReadingClause(); ++c)
      collect(part->getReadingClause(c)->getPredicate());
  }
  predicates.insert(predicates.end(), propertyDependencies.begin(), propertyDependencies.end());
  std::sort(predicates.begin(), predicates.end());
  std::vector<std::shared_ptr<IndexColnumInfo>> noFlatColumns;
  auto aux = std::make_unique<FillIndexAuxInfo>(FillEntryType::NODE,
      table->getName(), anchorName, anchorVariable, columns, noFlatColumns,
      std::move(pattern), sourceText, std::move(predicates));
  aux->setWorkloadCompatibility(true);
  aux->setPresenceColumns(std::move(presence));
  aux->setAggregateNumericContract(FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  auto* catalog = ctx->getCatalog();
  auto* transaction = ctx->getTransaction();
  validateFillStateOwnership(*aux, table->getTableID(), catalog, transaction);
  std::vector<common::property_id_t> properties;
  for (const auto& column : columns) properties.push_back(table->getPropertyID(column));
  const auto name = makeUniqueExtendedIndexName(catalog, transaction,
      makeFillIndexBaseName(table->getName(), columns, FillEntryType::NODE,
          aux->getPatternIR(), noFlatColumns, sourceText));
  catalog->createExtendedIndex(transaction, std::make_unique<IndexCatalogEntry>(
      "FILL", table->getTableID(), name, properties, std::move(aux)));
  return name;
}

// MAX of per-(carrier, subgroup) COUNTs. Keep the subgroup key explicitly:
// treating this as MAX of per-witness contributions loses deletion semantics.
static std::optional<std::string> groupedCountMaxKey(
    const NormalizedSingleQuery& query, const binder::expression_vector& columns,
    common::table_id_t targetTable) {
  if (query.getNumQueryParts() != 3 || columns.size() != 2 ||
      columns[0]->expressionType != common::ExpressionType::PROPERTY)
    return std::nullopt;
  const auto& carrier = columns[0]->constCast<binder::PropertyExpression>();
  if (!carrier.isPrimaryKey(targetTable)) return std::nullopt;
  for (uint32_t i = 0; i < 3; ++i) {
    const auto* part = query.getQueryPart(i);
    const auto* body = part->getProjectionBody();
    if (part->getNumUpdatingClause() || part->hasProjectionBodyPredicate() ||
        body->isDistinct() || body->hasSkipOrLimit() || body->hasOrderByExpressions() ||
        (i && part->getNumReadingClause())) return std::nullopt;
  }
  const auto* inner = query.getQueryPart(0)->getProjectionBody();
  const auto* outer = query.getQueryPart(1)->getProjectionBody();
  const auto innerAggs = inner->getAggregateExpressions();
  const auto outerAggs = outer->getAggregateExpressions();
  if (innerAggs.size() != 1 || outerAggs.size() != 1 ||
      inner->getGroupByExpressions().size() != 2 ||
      outer->getGroupByExpressions().size() != 1) return std::nullopt;
  const auto& count = innerAggs[0]->constCast<binder::AggregateFunctionExpression>();
  const auto& maximum = outerAggs[0]->constCast<binder::AggregateFunctionExpression>();
  if (count.getFunction().name != "COUNT" || maximum.getFunction().name != "MAX" ||
      count.isDistinct() || maximum.isDistinct() || count.getNumChildren() != 1 ||
      maximum.getNumChildren() != 1 ||
      count.getChild(0)->expressionType != common::ExpressionType::PATTERN ||
      count.getChild(0)->dataType.getLogicalTypeID() != common::LogicalTypeID::NODE ||
      maximum.getChild(0)->getUniqueName() != innerAggs[0]->getUniqueName() ||
      columns[1]->getUniqueName() != outerAggs[0]->getUniqueName() ||
      outer->getGroupByExpressions()[0]->getUniqueName() != carrier.getVariableName())
    return std::nullopt;
  bool hasCarrier = false;
  std::string subgroup;
  for (const auto& key : inner->getGroupByExpressions()) {
    if (key->expressionType != common::ExpressionType::PATTERN ||
        key->dataType.getLogicalTypeID() != common::LogicalTypeID::NODE)
      return std::nullopt;
    if (key->getUniqueName() == carrier.getVariableName()) hasCarrier = true;
    else subgroup = key->getUniqueName();
  }
  return hasCarrier && !subgroup.empty() ? std::optional<std::string>(subgroup)
                                        : std::nullopt;
}

std::string createWorkloadNodeIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::TableCatalogEntry* tableEntry,
    neug::main::ClientContext* ctx, std::vector<std::string> columnNames,
    const std::string& sourceQuery) {
  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION(
        common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  for (uint32_t qi = 0; qi < boundQuerySource.getNumSingleQueries(); ++qi)
    rejectUnmaintainedGraphPredicates(*boundQuerySource.getSingleQuery(qi));
  if (boundQuerySource.getNumSingleQueries() != 1) {
    return "";
  }
  if (auto staged = createStagedCountSumNodeIndexEntry(
          boundQuerySource, tableEntry, ctx, columnNames, sourceQuery);
      !staged.empty()) return staged;
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);
  const bool hasDistinctParts = singleQuery->getNumQueryParts() > 1;
  auto queryPart = singleQuery->getQueryPart(0);
  if (queryPart->getNumReadingClause() != 1) {
    return "";
  }
  auto readingClause = queryPart->getReadingClause(0);
  if (auto type = readingClause->getClauseType();
      type != common::ClauseType::MATCH) {
    return "";
  }

  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();

  // Build the full merged PatternIR from all QueryGraphs.
  std::vector<common::PatternIR> perGraphPatterns;
  auto fullPatternIR = buildFullPatternIR(*singleQuery, perGraphPatterns);
  NEUG_ASSERT(!fullPatternIR.nodesInfo.empty());
  if (fullPatternIR.relsInfo.empty())
    THROW_BINDER_EXCEPTION("Node-only FILL has no incremental vertex insertion/deletion maintenance; use an explicit source query instead.");
  std::optional<std::string> distinctShape;
  const auto groupedCountMax = groupedCountMaxKey(
      *singleQuery, boundSource->getColumns(), tableEntry->getTableID());
  if (groupedCountMax) {
    const auto* match = readingClause->constPtrCast<binder::BoundMatchClause>();
    if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
        !match->getConjunctivePredicates().empty() ||
        std::any_of(fullPatternIR.relsInfo.begin(), fullPatternIR.relsInfo.end(),
                    [](const auto& rel) { return rel.relType != common::QueryRelType::NON_RECURSIVE; }))
      THROW_BINDER_EXCEPTION("Grouped COUNT/MAX FILL requires a fixed-hop mandatory MATCH without predicates.");
  }
  if (hasDistinctParts) {
    if (!groupedCountMax) {
      distinctShape = binder::distinctFillShape(*singleQuery,
          singleQuery->getNumQueryParts(), fullPatternIR, false);
      if (!distinctShape.has_value()) return "";
      const auto columns = boundSource->getColumns();
      for (size_t i = 1; i < columns.size(); ++i) {
        if (columns[i]->expressionType != common::ExpressionType::AGGREGATE_FUNCTION)
          return "";
      }
    }
  }


  // Also get the original QueryGraphCollection for variable name resolution.
  auto matchClause = readingClause->constPtrCast<binder::BoundMatchClause>();
  auto patternGraph = matchClause->getQueryGraphCollection();

  // Build a cross-graph variable-name -> pattern unique-name map for
  // aggregation variable resolution. Aggregates may reference either node or
  // relationship properties (for example sum(edge.ratio)).
  std::unordered_map<std::string, std::string> varNameToPatternUniqName;
  for (auto gi = 0u; gi < patternGraph->getNumQueryGraphs(); ++gi) {
    const auto* graph = patternGraph->getQueryGraph(gi);
    for (auto j = 0u; j < graph->getNumQueryNodes(); j++) {
      auto node = graph->getQueryNode(j);
      if (!node->getVariableName().empty()) {
        varNameToPatternUniqName[node->getVariableName()] =
            node->getUniqueName();
      }
    }
    for (auto j = 0u; j < graph->getNumQueryRels(); ++j) {
      auto rel = graph->getQueryRel(j);
      if (!rel->getVariableName().empty()) {
        varNameToPatternUniqName[rel->getVariableName()] =
            rel->getUniqueName();
      }
    }
  }

  // Multi-part DISTINCT fills reference variables from every MATCH.
  for (const auto& node : fullPatternIR.nodesInfo) {
    if (!node.varName.empty()) varNameToPatternUniqName[node.varName] = node.uniqName;
  }
  for (const auto& rel : fullPatternIR.relsInfo) {
    if (!rel.varName.empty()) varNameToPatternUniqName[rel.varName] = rel.uniqName;
  }
  size_t startIdx{1};
  auto queryColumns = boundSource->getColumns();
  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;
  std::vector<std::string> presenceColumns;
  for (size_t i = startIdx; i < queryColumns.size(); ++i) {
    auto& queryColumn = queryColumns[i];
    auto& columnName = columnNames[i - startIdx];
    auto uniqName = queryColumn->getUniqueName();

    if (queryColumn->expressionType == common::ExpressionType::LITERAL) {
      if (!isTrueLiteral(*queryColumn) ||
          tableEntry->getProperty(columnName).getType().getLogicalTypeID() !=
              common::LogicalTypeID::BOOL) {
        THROW_BINDER_EXCEPTION(
            "Workload FILL presence requires a true literal and BOOL storage.");
      }
      presenceColumns.push_back(columnName);
      continue;
    }
    // IndexColumnInfo positions identify the public values in targetColumns.
    // Presence is a separate suffix and must not shift those positions.
    if (!presenceColumns.empty()) {
      THROW_BINDER_EXCEPTION(
          "Workload FILL must project value columns before presence outputs.");
    }
    if (groupedCountMax) {
      auto info = std::make_shared<NodeAggregationIndexColnumInfo>();
      info->columnType = IndexColumnType::NODE_AGGREGATE;
      info->aggregateFuncName = "MAX_GROUP_COUNT";
      info->nodeUniqueNames = {*groupedCountMax};
      info->nameTemplate = queryColumn->toString();
      indexColumnInfos.push_back(std::move(info));
      continue;
    }

    if (queryColumn->expressionType ==
        common::ExpressionType::AGGREGATE_FUNCTION) {
      auto info = std::make_shared<NodeAggregationIndexColnumInfo>();
      info->columnType = IndexColumnType::NODE_AGGREGATE;

      auto aggFuncExpr = queryColumn->ptrCast<binder::AggregateFunctionExpression>();
      info->nameTemplate = aggFuncExpr->getParsedName();
      info->aggregateFuncName = aggFuncExpr->getFunction().name;
      info->aggregateArgExpr = workloadSumArgument(*aggFuncExpr, *singleQuery, fullPatternIR);

      // Collect the unique names of nodes referenced by this aggregate.
      for (auto var : aggFuncExpr->getRawNameVariables()) {
        auto it = varNameToPatternUniqName.find(var);
        if (it != varNameToPatternUniqName.end()) {
          info->nodeUniqueNames.push_back(it->second);
        }
      }

      // Extract nodeUnique and propertyName from the aggregate's child expression.
      if (aggFuncExpr->getNumChildren() == 1) {
        auto child = aggFuncExpr->getChild(0);
        if (child->expressionType == common::ExpressionType::PROPERTY) {
          auto& propExpr = child->constCast<binder::PropertyExpression>();
          info->nodeUnique = propExpr.getVariableName();
          info->propertyName = propExpr.getPropertyName();
        } else if (child->expressionType == common::ExpressionType::PATTERN &&
                   child->dataType.getLogicalTypeID() == common::LogicalTypeID::NODE) {
          info->nodeUnique = child->getUniqueName();
        }
      }

      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType ==
               common::ExpressionType::PROPERTY) {
      auto pos = uniqName.find('.');
      NEUG_ASSERT(pos != std::string::npos);
      auto fir = uniqName.substr(0, pos);
      auto sec = uniqName.substr(pos + 1);
      bool found{false};
      bool isRelProperty{false};
      common::table_id_t tId{common::INVALID_TABLE_ID};

      for (size_t gi = 0; gi < patternGraph->getNumQueryGraphs(); gi++) {
        auto graph = patternGraph->getQueryGraph(gi);

        if (graph->containsQueryNode(fir)) {
          auto nodeExpr = graph->getQueryNode(fir);
          tId = nodeExpr->getEntries()[0]->getTableID();
          found = true;
          break;
        } else if (graph->containsQueryRel(fir)) {
          auto relExpr = graph->getQueryRel(fir);
          tId = relExpr->getEntries()[0]->getTableID();
          found = true;
          isRelProperty = true;
          break;
        }
      }
      if (!found) {
        THROW_BINDER_EXCEPTION(common::stringFormat(
            "Cannot find pattern variable {} in FILL query for column {}.", fir,
            columnName));
      }
      auto tbEntry = catalog->getTableCatalogEntry(transaction, tId);
      NEUG_ASSERT(tbEntry->containsProperty(sec));
      auto info = std::make_shared<IndexColnumInfo>();
      info->columnType = isRelProperty ? IndexColumnType::REL_PROPERTY
                                       : IndexColumnType::NODE_PROPERTY;
      info->nodeUnique = fir;
      info->propertyName = tbEntry->getProperty(sec).getName();
      indexColumnInfos.emplace_back(std::move(info));
    }
  }

  // The first projected property is the materialization key and identifies
  // the anchor even when label IDs in a merged PatternIR are ambiguous.
  auto labelName = tableEntry->getName();
  auto tableID = tableEntry->getTableID();
  size_t nodePos = UINT64_MAX;
  std::string tableUniq, tableVar;
  const auto trySelectAnchorByName = [&](const std::string& name) {
    if (name.empty()) {
      return false;
    }
    for (size_t i = 0; i < fullPatternIR.nodesInfo.size(); ++i) {
      const auto& node = fullPatternIR.nodesInfo[i];
      if (node.uniqName == name || node.varName == name) {
        nodePos = i;
        tableUniq = node.uniqName;
        tableVar = node.varName;
        return true;
      }
    }
    return false;
  };
  if (!queryColumns.empty() &&
      queryColumns[0]->expressionType == common::ExpressionType::PROPERTY) {
    auto* keyProperty =
        queryColumns[0]->ptrCast<binder::PropertyExpression>();
    if (keyProperty != nullptr && !keyProperty->isInternalID()) {
      trySelectAnchorByName(keyProperty->getVariableName()) ||
          trySelectAnchorByName(keyProperty->getRawVariableName());
    }
  }
  if (nodePos == UINT64_MAX) {
    NEUG_ASSERT(findTableNodeInPattern(fullPatternIR, tableID, nodePos,
                                       tableUniq, tableVar));
  }

  std::string functionalAssumptionSignature;
  if (!presenceColumns.empty()) {
    if (indexColumnInfos.size() + presenceColumns.size() != columnNames.size())
      return "";
    if (!queryPart->hasProjectionBody() ||
        !queryPart->getProjectionBody()->hasAggregateExpressions()) {
      // A projection attribute stores one source row per anchor. Presence
      // cannot repair multiplicity or several different destination values.
      // Prove functionality from physical catalog multiplicity or an explicit
      // source-local declaration validated against the complete bound graph.
      // A selection-only flag or DISTINCT(anchor, destination) is not a proof.
      if (hasDistinctParts || !boundQuerySource.getPreQueryPart().empty() ||
          boundQuerySource.getPostSingleQuery() ||
          !queryPart->hasProjectionBody() || queryPart->hasUpdatingClause() ||
          queryPart->hasProjectionBodyPredicate() ||
          queryPart->getProjectionBody()->hasOrderByExpressions() ||
          queryPart->getProjectionBody()->hasSkipOrLimit() ||
          matchClause->getMatchClauseType() != common::MatchClauseType::MATCH ||
          matchClause->hasHint() || queryColumns.empty() ||
          queryColumns[0]->expressionType != common::ExpressionType::PROPERTY)
        return "";
      const auto& key = queryColumns[0]->constCast<binder::PropertyExpression>();
      if (!key.isSingleLabel() || !key.isPrimaryKey(tableID)) return "";
      const auto assumption = proveFillFunctionalAssumption(
          sourceQuery, *patternGraph, key, *tableEntry, *ctx);
      if (assumption) {
        functionalAssumptionSignature = *assumption;
      } else {
        const auto nodes = patternGraph->getQueryNodes();
        const auto rels = patternGraph->getQueryRels();
        for (const auto& node : nodes)
          if (node->getEntries().size() != 1) return "";
        for (const auto& rel : rels)
          if (rel->isRecursive() || rel->getEntries().size() != 1 ||
              rel->getDirectionType() != binder::RelDirectionType::SINGLE)
            return "";
        std::unordered_set<std::string> reached{tableUniq}, provedRelations;
        bool progress = true;
        while (progress) {
          progress = false;
          for (const auto& rel : rels) {
            if (provedRelations.contains(rel->getUniqueName())) continue;
            const auto& entry = rel->getSingleEntry()->constCast<RelTableCatalogEntry>();
            const auto src = rel->getSrcNodeName(), dst = rel->getDstNodeName();
            if ((reached.contains(src) &&
                 entry.isSingleMultiplicity(common::RelDataDirection::FWD)) ||
                (reached.contains(dst) &&
                 entry.isSingleMultiplicity(common::RelDataDirection::BWD))) {
              reached.insert(src);
              reached.insert(dst);
              provedRelations.insert(rel->getUniqueName());
              progress = true;
            }
          }
        }
        if (provedRelations.size() != rels.size() ||
            std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) {
              return !reached.contains(node->getUniqueName());
            })) return "";
      }
    }
  }

  auto predicateTemplates = collectPredicateTemplates(*singleQuery, fullPatternIR);
  if (distinctShape.has_value()) predicateTemplates.push_back(*distinctShape);
  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::NODE, labelName, tableUniq, tableVar,
      columnNames, indexColumnInfos, std::move(fullPatternIR), sourceQuery,
      std::move(predicateTemplates));
  aux->setWorkloadCompatibility(true);
  aux->setAggregateNumericContract(FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  aux->setPresenceColumns(std::move(presenceColumns));
  std::vector<common::property_id_t> propertyIDs;
  propertyIDs.reserve(columnNames.size());
  for (const auto& c : columnNames) {
    propertyIDs.push_back(tableEntry->getPropertyID(c));
  }
  auto baseName =
      makeFillIndexBaseName(labelName, columnNames, catalog::FillEntryType::NODE,
                            aux->getPatternIR(), indexColumnInfos,
                            makePredicateSignature(
                                aux->getPredicateTemplates()) +
                                functionalAssumptionSignature);
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  validateStagedFillDependencies(*aux, tableID, catalog, transaction);
  validateWorkloadFillStateOwnership(*aux, tableID, ctx);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

// Property-free derived relations project two endpoint identities, with exact
// label dispatch. Bag projection preserves one row per source row. DISTINCT
// instead records a separate set contract that requires an explicit proof in
// the consuming query; aggregation and arbitrary stages remain unsupported.
struct EndpointProjectionPattern {
  std::string first;
  std::string second;
  bool distinct;
  bool recursive;
};
static std::optional<EndpointProjectionPattern>
normalizeBagRelationPattern(const BoundRegularQuery& regular,
                            const binder::expression_vector& columns,
                            RelTableCatalogEntry& target,
                            main::ClientContext& context,
                            common::PatternIR& pattern) {
  using namespace common;
  using namespace binder;
  if (regular.getNumSingleQueries() != 1 ||
      !regular.getPreQueryPart().empty() || regular.getPostSingleQuery() ||
      columns.size() != 2) return std::nullopt;
  const auto& query = *regular.getSingleQuery(0);
  std::unordered_map<std::string, table_id_t> keys;
  std::vector<std::string> ordered;
  for (size_t i = 0; i < 2; ++i) {
    const auto table = i == 0 ? target.getSrcTableID() : target.getDstTableID();
    const auto& expression = columns[i];
    std::string name;
    if (expression->expressionType == ExpressionType::PATTERN &&
        expression->dataType.getLogicalTypeID() == LogicalTypeID::NODE) {
      name = expression->getUniqueName();
    } else if (expression->expressionType == ExpressionType::PROPERTY) {
      const auto& property = expression->constCast<PropertyExpression>();
      if (!property.isPrimaryKey(table)) return std::nullopt;
      name = property.getVariableName();
    } else return std::nullopt;
    const auto position = pattern.nodeUniqNameToPos.find(name);
    if (position == pattern.nodeUniqNameToPos.end()) return std::nullopt;
    const auto& labels = pattern.nodesInfo[position->second].nodeLabels;
    if (std::find(labels.begin(), labels.end(), table) == labels.end() ||
        !keys.emplace(name, table).second) return std::nullopt;
    ordered.push_back(name);
  }
  const auto literalString = [](const Expression& expression)
      -> std::optional<std::string> {
    if (expression.expressionType != ExpressionType::LITERAL ||
        expression.dataType.getLogicalTypeID() != LogicalTypeID::STRING)
      return std::nullopt;
    const auto& literal = expression.constCast<LiteralExpression>();
    if (literal.isNull()) return std::nullopt;
    return literal.getValue().getValue<std::string>();
  };
  std::unordered_set<std::string> dispatched;
  bool distinct = false;
  bool recursive = false;
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi) {
    const auto& part = *query.getQueryPart(pi);
    if (part.hasUpdatingClause() || !part.hasProjectionBody() ||
        (pi == 0 ? part.getNumReadingClause() != 1 : part.hasReadingClause()))
      return std::nullopt;
    const auto& body = *part.getProjectionBody();
    if (body.hasAggregateExpressions() || body.hasOrderByExpressions() ||
        body.hasSkipOrLimit()) return std::nullopt;
    distinct |= body.isDistinct();
    std::unordered_set<std::string> projected;
    for (const auto& expression : body.getProjectionExpressions()) {
      std::string name;
      if (expression->expressionType == ExpressionType::PATTERN &&
          expression->dataType.getLogicalTypeID() == LogicalTypeID::NODE)
        name = expression->getUniqueName();
      else if (expression->expressionType == ExpressionType::PROPERTY) {
        const auto& property = expression->constCast<PropertyExpression>();
        name = property.getVariableName();
        const auto key = keys.find(name);
        if (key == keys.end() || !property.isPrimaryKey(key->second)) return std::nullopt;
      }
      if (!keys.contains(name)) return std::nullopt;
      // Before label dispatch only typed graph identities may cross a stage.
      if (pi + 1 < query.getNumQueryParts() &&
          expression->expressionType != ExpressionType::PATTERN)
        return std::nullopt;
      projected.insert(name);
    }
    if (projected.size() != 2) return std::nullopt;
    if (pi == 0) {
      const auto* clause = part.getReadingClause(0);
      if (clause->getClauseType() != ClauseType::MATCH) return std::nullopt;
      const auto& match = clause->constCast<BoundMatchClause>();
      if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasHint())
        return std::nullopt;
      for (const auto& rel : match.getQueryGraphCollection()->getQueryRels()) {
        if (!rel->isRecursive()) continue;
        const auto* recursion = rel->getRecursiveInfo();
        if ((rel->getRelType() != QueryRelType::VARIABLE_LENGTH_WALK &&
             rel->getRelType() != QueryRelType::VARIABLE_LENGTH_TRAIL &&
             rel->getRelType() != QueryRelType::VARIABLE_LENGTH_ACYCLIC) ||
            recursion == nullptr || recursion->bindData == nullptr ||
            recursion->nodePredicate || recursion->relPredicate ||
            recursion->bindData->weightPropertyExpr ||
            recursion->bindData->weightOutputExpr)
          return std::nullopt;
        recursive = true;
      }
    }
    if (!part.hasProjectionBodyPredicate()) continue;
    for (const auto& predicate : part.getProjectionBodyPredicate()->splitOnAND()) {
      if (isTrueLiteral(*predicate)) continue;
      if (predicate->expressionType != ExpressionType::EQUALS ||
          predicate->getNumChildren() != 2) return std::nullopt;
      auto label = predicate->getChild(0);
      auto value = literalString(*predicate->getChild(1));
      if (!value) { value = literalString(*label); label = predicate->getChild(1); }
      if (!value) return std::nullopt;
      if (const auto constant = literalString(*label)) {
        if (*constant != *value) return std::nullopt;
        continue;
      }
      if (label->expressionType != ExpressionType::FUNCTION ||
          label->constCast<ScalarFunctionExpression>().getFunction().name != "LABEL" ||
          label->getNumChildren() != 2 ||
          label->getChild(0)->expressionType != ExpressionType::PROPERTY ||
          label->getChild(1)->expressionType != ExpressionType::LITERAL)
        return std::nullopt;
      const auto& id = label->getChild(0)->constCast<PropertyExpression>();
      const auto key = keys.find(id.getVariableName());
      if (!id.isInternalID() || key == keys.end()) return std::nullopt;
      auto* entry = context.getCatalog()->getTableCatalogEntry(
          context.getTransaction(), key->second);
      if (entry->getLabel(context.getCatalog(), context.getTransaction()) != *value)
        return std::nullopt;
      dispatched.insert(key->first);
    }
  }
  auto narrowed = pattern;
  for (auto& node : narrowed.nodesInfo) {
    if (const auto key = keys.find(node.uniqName); key != keys.end()) {
      if (node.nodeLabels.size() != 1 && !dispatched.contains(key->first))
        return std::nullopt;
      node.nodeLabels = {key->second};
    }
    node.outgoing.clear(); node.incoming.clear(); node.undirected.clear();
  }
  for (auto& rel : narrowed.relsInfo) {
    if (std::find(rel.relLabels.begin(), rel.relLabels.end(), target.getTableID()) !=
        rel.relLabels.end()) return std::nullopt;
    const auto& src = narrowed.nodesInfo.at(rel.srcNodeIdx).nodeLabels;
    const auto& dst = narrowed.nodesInfo.at(rel.dstNodeIdx).nodeLabels;
    const auto contains = [](const auto& labels, table_id_t id) {
      return std::find(labels.begin(), labels.end(), id) != labels.end();
    };
    // A recursive edge's labels describe its traversal domain. Its output
    // endpoints need not share the physical edge's labels (notably at zero
    // hops), so only fixed-hop domains can be narrowed by endpoint dispatch.
    if (rel.relType == QueryRelType::NON_RECURSIVE)
      std::erase_if(rel.relLabels, [&](table_id_t id) {
      const auto& entry = context.getCatalog()->getTableCatalogEntry(
          context.getTransaction(), id)->constCast<RelTableCatalogEntry>();
      const bool forward = contains(src, entry.getSrcTableID()) &&
                           contains(dst, entry.getDstTableID());
      const bool reverse = rel.edgeType == EdgeType::UNDIRECTED &&
                           contains(dst, entry.getSrcTableID()) &&
                           contains(src, entry.getDstTableID());
      return !forward && !reverse;
      });
    if (rel.relLabels.empty()) return std::nullopt;
    if (rel.edgeType == EdgeType::UNDIRECTED) {
      narrowed.nodesInfo[rel.srcNodeIdx].undirected.emplace_back(rel.dstNodeIdx, rel.relLabels);
      narrowed.nodesInfo[rel.dstNodeIdx].undirected.emplace_back(rel.srcNodeIdx, rel.relLabels);
    } else {
      narrowed.nodesInfo[rel.srcNodeIdx].outgoing.emplace_back(rel.dstNodeIdx, rel.relLabels);
      narrowed.nodesInfo[rel.dstNodeIdx].incoming.emplace_back(rel.srcNodeIdx, rel.relLabels);
    }
  }
  if (narrowed.relsInfo.empty()) return std::nullopt;
  pattern = std::move(narrowed);
  return EndpointProjectionPattern{ordered[0], ordered[1], distinct, recursive};
}

std::string createWorkloadRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    std::vector<std::string> fillColumnNames,
    neug::catalog::RelTableCatalogEntry* relGroupEntry,
    main::ClientContext* ctx, const std::string& sourceQuery,
    std::string indexTableName) {
  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION(
        common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  for (uint32_t qi = 0; qi < boundQuerySource.getNumSingleQueries(); ++qi)
    rejectUnmaintainedGraphPredicates(*boundQuerySource.getSingleQuery(qi));
  NEUG_ASSERT(boundQuerySource.getNumSingleQueries() == 1);
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);


  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();

  auto labelName = indexTableName.empty() ? relGroupEntry->getLabel(catalog, transaction)
                                          : std::move(indexTableName);
  std::string relUniqName, relVarName;
  bool fillUndirectedRel = false;

  // Collect per-QueryPart graphs and build variable-name -> pattern
  // unique-name maps. Relationship aggregates commonly reference the filled
  // edge itself, so node-only maps silently lose their template variables.
  std::vector<std::unordered_map<std::string, std::string>> variableNameMaps;
  for (auto i = 0u; i < singleQuery->getNumQueryParts(); i++) {
    auto queryPart = singleQuery->getQueryPart(i);
    NEUG_ASSERT(queryPart->getNumReadingClause() <= 1);
    if (queryPart->getNumReadingClause() < 1 ||
        queryPart->getReadingClause(0)->getClauseType() != common::ClauseType::MATCH) {
      continue;
    }
    auto clause = queryPart->getReadingClause(0);
    auto matchClause = clause->constPtrCast<binder::BoundMatchClause>();
    auto patternGraph = matchClause->getQueryGraphCollection();
    NEUG_ASSERT(patternGraph->getNumQueryGraphs() == 1);
    const auto* graph = patternGraph->getQueryGraph(0);
    std::unordered_map<std::string, std::string> varMap;
    for (auto j = 0u; j < graph->getNumQueryNodes(); j++) {
      auto node = graph->getQueryNode(j);
      if (!node->getVariableName().empty()) {
        varMap[node->getVariableName()] = node->getUniqueName();
      }
    }
    for (uint32_t j = 0; j < graph->getNumQueryRels(); ++j) {
      auto rel = graph->getQueryRel(j);
      if (!rel->getVariableName().empty()) {
        varMap[rel->getVariableName()] = rel->getUniqueName();
      }
      if (rel->getSingleEntry()->getName() == labelName) {
        relVarName = rel->getVariableName();
        relUniqName = rel->getUniqueName();
        fillUndirectedRel =
            rel->getDirectionType() == binder::RelDirectionType::BOTH;
      }
    }
    variableNameMaps.push_back(std::move(varMap));
  }

  // Build the full merged PatternIR.
  std::vector<common::PatternIR> perGraphPatterns;
  auto fullPatternIR = buildFullPatternIR(*singleQuery, perGraphPatterns);

  auto startIdx = 2u;
  auto queryColumns = boundSource->getColumns();
  auto bagEndpoints = fillColumnNames.empty()
      ? normalizeBagRelationPattern(boundQuerySource, queryColumns,
                                    *relGroupEntry, *ctx, fullPatternIR)
      : std::nullopt;

  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;
  for (size_t i = startIdx; i < queryColumns.size(); ++i) {
    auto queryColumn = queryColumns[i];
    if (queryColumn->expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      auto info = std::make_shared<RelIndexColnumInfo>();
      auto aggFuncExpr = queryColumn->ptrCast<binder::AggregateFunctionExpression>();
      info->columnType = IndexColumnType::REL_AGGREGATE;
      info->nameTemplate = aggFuncExpr->getParsedName();
      info->aggregateFuncName = aggFuncExpr->getFunction().name;
      info->aggregateArgExpr = workloadSumArgument(*aggFuncExpr, *singleQuery, fullPatternIR);
      if (aggFuncExpr->getFunction().name == "COUNT" && aggFuncExpr->isDistinct() &&
          aggFuncExpr->getNumChildren() == 1 &&
          aggFuncExpr->getChild(0)->expressionType == common::ExpressionType::PATTERN &&
          aggFuncExpr->getChild(0)->dataType.getLogicalTypeID() == common::LogicalTypeID::NODE &&
          singleQuery->getNumQueryParts() == 1 && queryColumns.size() == 3 &&
          collectPredicateTemplates(*singleQuery, fullPatternIR).empty())
        info->aggregateFuncName = "COUNT_DISTINCT";

      // Resolve variable names to unique names using the variableNameMaps
      // and the full merged PatternIR's uniqName lookup.
      for (auto var : aggFuncExpr->getRawNameVariables()) {
        // Find which pattern this variable belongs to, then get its node's uniqName.
        for (size_t pi = 0; pi < variableNameMaps.size(); ++pi) {
          auto it = variableNameMaps[pi].find(var);
          if (it != variableNameMaps[pi].end()) {
            info->nodeUniqueNames.push_back(it->second);
            break;
          }
        }
      }
      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType == common::ExpressionType::FUNCTION) {
      auto scalarExpr = queryColumn->ptrCast<binder::ScalarFunctionExpression>();
      auto info = std::make_shared<RelComputationWithAggreColumnInfo>();
      info->columnType = IndexColumnType::REL_COMPUATION_WITH_AGGREGATION;
      info->nameTemplate = scalarExpr->getParsedName();
      for (auto var : scalarExpr->getRawNameVariables()) {
        for (size_t pi = 0; pi < variableNameMaps.size(); ++pi) {
          auto it = variableNameMaps[pi].find(var);
          if (it != variableNameMaps[pi].end()) {
            info->nodeUniqueNames.push_back(it->second);
            break;
          }
        }
      }
      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType == common::ExpressionType::PROPERTY) {
      // TODO: handle rel property columns if needed
    }
  }

  std::vector<common::property_id_t> propertyIDs;
  propertyIDs.reserve(fillColumnNames.size());
  for (const auto& c : fillColumnNames) {
    propertyIDs.push_back(relGroupEntry->getPropertyID(c));
  }

  auto predicateTemplates = collectPredicateTemplates(*singleQuery, fullPatternIR);
  if (fillColumnNames.empty() && !bagEndpoints)
    predicateTemplates.push_back("@unsupported-projection-shape");
  if (bagEndpoints && bagEndpoints->distinct)
    predicateTemplates.emplace_back(binder::ENDPOINT_SET_CONTRACT);
  if (bagEndpoints && bagEndpoints->recursive)
    predicateTemplates.emplace_back(binder::RECURSIVE_ENDPOINT_CONTRACT);
  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::REL, labelName, relUniqName, relVarName,
      fillColumnNames, indexColumnInfos, std::move(fullPatternIR), sourceQuery,
      std::move(predicateTemplates));
  const auto resolveEndpointUniqueName = [&](size_t columnPos) {
    if (columnPos >= queryColumns.size() ||
        queryColumns[columnPos]->expressionType !=
            common::ExpressionType::PROPERTY) {
      return std::string{};
    }
    const auto* property =
        queryColumns[columnPos]->constPtrCast<binder::PropertyExpression>();
    if (property == nullptr) {
      return std::string{};
    }
    for (const auto& node : aux->getPatternIR().nodesInfo) {
      if (node.uniqName == property->getVariableName() ||
          node.varName == property->getRawVariableName()) {
        return node.uniqName;
      }
    }
    return std::string{};
  };
  aux->setWorkloadCompatibility(true);
  aux->setAggregateNumericContract(FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  aux->setEndpointNodeUniqueNames(
      bagEndpoints ? bagEndpoints->first : resolveEndpointUniqueName(0),
      bagEndpoints ? bagEndpoints->second : resolveEndpointUniqueName(1));
  aux->setSrcTableId(relGroupEntry->getSrcTableID());
  aux->setDstTableId(relGroupEntry->getDstTableID());
  aux->setFillUndirectedRel(fillUndirectedRel);
  auto tableID = relGroupEntry->getTableID();
  auto baseName = makeFillIndexBaseName(
      relGroupEntry->getName(), fillColumnNames, catalog::FillEntryType::REL,
      aux->getPatternIR(), indexColumnInfos,
      makePredicateSignature(aux->getPredicateTemplates()));
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  validateStagedFillDependencies(*aux, tableID, catalog, transaction);
  validateWorkloadFillStateOwnership(*aux, tableID, ctx);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

std::string createWorkloadVaradicRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::RelTableCatalogEntry* tableEntry, main::ClientContext* ctx,
    std::string indexTableName, const std::string& sourceQuery) {
  if (boundSource->type != common::ScanSourceType::QUERY) {
      THROW_BINDER_EXCEPTION(
      common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  for (uint32_t qi = 0; qi < boundQuerySource.getNumSingleQueries(); ++qi)
    rejectUnmaintainedGraphPredicates(*boundQuerySource.getSingleQuery(qi));
  NEUG_ASSERT(boundQuerySource.getNumSingleQueries() == 1);
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);
  auto queryPart = singleQuery->getQueryPart(0);
  NEUG_ASSERT(queryPart->getNumReadingClause() == 1);
  auto readingClause = queryPart->getReadingClause(0);
  if (auto type = readingClause->getClauseType(); type != common::ClauseType::MATCH) {
      return "";
  }

  auto matchClause = readingClause->constPtrCast<binder::BoundMatchClause>();
  auto patternGraph = matchClause->getQueryGraphCollection();
  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();
  if (patternGraph->getNumQueryGraphs() != 1) {
    return "";
  }
  const auto* graph = patternGraph->getQueryGraph(0);
  auto fullPatternIR = common::convertGraphPatternToIR(*graph);
  if (graph->getNumQueryNodes() != 2 || graph->getNumQueryRels() != 1) {
    return "";
  }
  auto rel = graph->getQueryRel(0);
  if (!rel->isRecursive() || rel->getRecursiveInfo() == nullptr ||
      rel->getRecursiveInfo()->bindData == nullptr) {
    return "";
  }
  auto relRecursiveInfo = rel->getRecursiveInfo();

  auto queryColumns = boundSource->getColumns();
  if (queryColumns.size() != 2) {
    return "";
  }
  // A relation-group FILL writes every child, not just tableEntry (the first
  // child used by the binder). Validate its entire ordered endpoint domain.
  const bool groupTarget = !indexTableName.empty() &&
      catalog->containsRelGroup(transaction, indexTableName);
  std::vector<std::pair<common::table_id_t, common::table_id_t>> targetPairs;
  if (groupTarget) {
    const auto* group = catalog->getRelGroupEntry(transaction, indexTableName);
    for (const auto id : group->getRelTableIDs()) {
      const auto* child = catalog->getTableCatalogEntry(transaction, id)
                              ->constPtrCast<RelTableCatalogEntry>();
      targetPairs.emplace_back(child->getSrcTableID(), child->getDstTableID());
    }
  }
  // The physical edge follows the ordered RETURN keys, which can be the
  // reverse of the recursive traversal. Table labels alone cannot recover
  // this order, especially for two bindings with the same label or a typed
  // partition of a heterogeneous endpoint (including zero-length paths).
  const auto resolveEndpoint = [&](size_t columnPos) {
    const auto& column = queryColumns[columnPos];
    const auto tableID = columnPos == 0 ? tableEntry->getSrcTableID()
                                      : tableEntry->getDstTableID();
    std::string uniqueName;
    if (column->expressionType == common::ExpressionType::PATTERN &&
        column->getDataType().getLogicalTypeID() == common::LogicalTypeID::NODE) {
      uniqueName = column->getUniqueName();
    } else if (column->expressionType == common::ExpressionType::PROPERTY) {
      const auto& property = column->constCast<binder::PropertyExpression>();
      if (!groupTarget &&
          (!property.isSingleLabel() || property.getSingleTableID() != tableID ||
           !property.isPrimaryKey(tableID))) {
        THROW_BINDER_EXCEPTION("Recursive FILL endpoint must be its target primary key.");
      }
      uniqueName = property.getVariableName();
    }
    const auto found = fullPatternIR.nodeUniqNameToPos.find(uniqueName);
    if (found == fullPatternIR.nodeUniqNameToPos.end()) {
      THROW_BINDER_EXCEPTION("Recursive FILL must return its two bound endpoint identities.");
    }
    const auto& labels = fullPatternIR.nodesInfo[found->second].nodeLabels;
    if (labels.empty() || (!groupTarget &&
        std::find(labels.begin(), labels.end(), tableID) == labels.end())) {
      THROW_BINDER_EXCEPTION("Recursive FILL endpoint label does not match its target partition.");
    }
    if (groupTarget) {
      for (const auto label : labels) {
        if (std::none_of(targetPairs.begin(), targetPairs.end(),
                        [&](const auto& pair) {
                          return (columnPos == 0 ? pair.first : pair.second) == label;
                        }))
          THROW_BINDER_EXCEPTION("Recursive FILL endpoint label does not match its target group.");
        if (column->expressionType == common::ExpressionType::PROPERTY &&
            !column->constCast<binder::PropertyExpression>().isPrimaryKey(label))
          THROW_BINDER_EXCEPTION("Recursive FILL endpoint must be its target primary key.");
      }
    }
    return uniqueName;
  };
  auto sourceEndpoint = resolveEndpoint(0);
  auto destinationEndpoint = resolveEndpoint(1);
  if (sourceEndpoint == destinationEndpoint) {
    THROW_BINDER_EXCEPTION("Recursive FILL must preserve both distinct endpoint bindings.");
  }
  if (groupTarget) {
    const auto& sourceLabels = fullPatternIR.nodesInfo[
        fullPatternIR.nodeUniqNameToPos.at(sourceEndpoint)].nodeLabels;
    const auto& destinationLabels = fullPatternIR.nodesInfo[
        fullPatternIR.nodeUniqNameToPos.at(destinationEndpoint)].nodeLabels;
    for (const auto sourceLabel : sourceLabels)
      for (const auto destinationLabel : destinationLabels)
        if (std::find(targetPairs.begin(), targetPairs.end(),
                      std::make_pair(sourceLabel, destinationLabel)) == targetPairs.end())
          THROW_BINDER_EXCEPTION("Recursive FILL target group does not cover its endpoint pairs.");
  }
  const auto projectionEndpoint = [&](const binder::Expression& expression) {
    if (!groupTarget || expression.expressionType != common::ExpressionType::PROPERTY)
      return identityGroupNode(expression);
    const auto& property = expression.constCast<binder::PropertyExpression>();
    const auto found = fullPatternIR.nodeUniqNameToPos.find(property.getVariableName());
    if (found == fullPatternIR.nodeUniqNameToPos.end()) return std::string{};
    for (const auto label : fullPatternIR.nodesInfo[found->second].nodeLabels)
      if (!property.isPrimaryKey(label)) return std::string{};
    return property.getVariableName();
  };
  const auto stringLiteral = [](const binder::Expression& expression)
      -> std::optional<std::string> {
    if (expression.expressionType != common::ExpressionType::LITERAL ||
        expression.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING)
      return std::nullopt;
    const auto& literal = expression.constCast<binder::LiteralExpression>();
    return literal.isNull() ? std::nullopt
        : std::optional<std::string>(literal.getValue().getValue<std::string>());
  };
  const auto validDispatch = [&](const std::shared_ptr<binder::Expression>& predicate) {
    if (isTrueLiteral(*predicate)) return true;
    if (predicate->expressionType != common::ExpressionType::EQUALS ||
        predicate->getNumChildren() != 2) return false;
    auto label = predicate->getChild(0);
    auto value = stringLiteral(*predicate->getChild(1));
    if (!value) {
      value = stringLiteral(*label);
      label = predicate->getChild(1);
    }
    if (!value) return false;
    if (const auto constant = stringLiteral(*label)) return *constant == *value;
    // A group entry describes its entire endpoint domain. A dynamic label
    // filter would narrow that domain and cannot be erased from its metadata.
    if (groupTarget) return false;
    if (label->expressionType != common::ExpressionType::FUNCTION ||
        label->constCast<binder::ScalarFunctionExpression>().getFunction().name != "LABEL" ||
        label->getNumChildren() != 2 ||
        label->getChild(0)->expressionType != common::ExpressionType::PROPERTY ||
        label->getChild(1)->expressionType != common::ExpressionType::LITERAL)
      return false;
    const auto& id = label->getChild(0)->constCast<binder::PropertyExpression>();
    if (!id.isInternalID()) return false;
    common::table_id_t tableID;
    if (id.getVariableName() == sourceEndpoint) tableID = tableEntry->getSrcTableID();
    else if (id.getVariableName() == destinationEndpoint) tableID = tableEntry->getDstTableID();
    else return false;
    return catalog->getTableCatalogEntry(transaction, tableID)
               ->getLabel(catalog, transaction) == *value;
  };
  // This shortcut describes unfiltered reachability partitioned by endpoint
  // labels. Other filters, aggregate stages and bounds on the output rows do
  // not have that meaning and cannot be discarded from its metadata.
  if (!boundQuerySource.getPreQueryPart().empty() ||
      boundQuerySource.getPostSingleQuery() || matchClause->hasPredicate() ||
      matchClause->hasHint() ||
      matchClause->getMatchClauseType() != common::MatchClauseType::MATCH ||
      relRecursiveInfo->relPredicate || relRecursiveInfo->nodePredicate) {
    THROW_BINDER_EXCEPTION("Recursive FILL shortcut requires an unfiltered mandatory path.");
  }
  for (uint32_t pi = 0; pi < singleQuery->getNumQueryParts(); ++pi) {
    const auto* part = singleQuery->getQueryPart(pi);
    if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
        (pi != 0 && part->hasReadingClause()) ||
        part->getProjectionBody()->hasAggregateExpressions() ||
        part->getProjectionBody()->hasOrderByExpressions() ||
        part->getProjectionBody()->hasSkipOrLimit()) {
      THROW_BINDER_EXCEPTION("Recursive FILL shortcut only supports endpoint projection and DISTINCT.");
    }
    for (const auto& expression : part->getProjectionBody()->getProjectionExpressions()) {
      const auto name = projectionEndpoint(*expression);
      if (name != sourceEndpoint && name != destinationEndpoint)
        THROW_BINDER_EXCEPTION("Recursive FILL projection must preserve only its endpoint identities.");
    }
    if (part->hasProjectionBodyPredicate())
      for (const auto& predicate : part->getProjectionBodyPredicate()->splitOnAND())
        if (!validDispatch(predicate))
          THROW_BINDER_EXCEPTION("Recursive FILL only supports exact target-label dispatch after projection.");
  }
  if (groupTarget && std::any_of(queryColumns.begin(), queryColumns.end(),
          [](const auto& column) {
            return column->expressionType == common::ExpressionType::PROPERTY;
          })) {
    // Preserve labels before DISTINCT and before routing to child tables.
    // Scalar PKs alone can conflate Comment(7) and Post(7). Direct endpoint
    // projections have exact lineage, so materialize their typed identities.
    // Earlier scalar stages cannot be repaired after they have already lost it.
    if (singleQuery->getNumQueryParts() != 1)
      THROW_BINDER_EXCEPTION("Recursive group FILL must preserve typed endpoints across projection stages.");
    auto& mutableQuery = boundSource->cast<BoundQueryScanSource>()
                             .statement->cast<BoundRegularQuery>();
    auto* mutableSingle = mutableQuery.getSingleQueryUnsafe(0);
    auto* body = mutableSingle->getQueryPartUnsafe(0)->getProjectionBodyUnsafe();
    const auto projections = body->getProjectionExpressions();
    if (projections.size() != 2 || projections[0].get() != queryColumns[0].get() ||
        projections[1].get() != queryColumns[1].get())
      THROW_BINDER_EXCEPTION("Recursive group FILL requires direct endpoint projections.");
    binder::expression_vector endpoints;
    for (const auto& name : {sourceEndpoint, destinationEndpoint}) {
      for (const auto& node : graph->getQueryNodes())
        if (node->getUniqueName() == name) endpoints.push_back(node);
    }
    NEUG_ASSERT(endpoints.size() == 2);
    body->setProjectionExpressions(endpoints);
    const auto names = mutableQuery.getStatementResult()->getColumnNames();
    mutableSingle->setStatementResult(binder::BoundStatementResult(endpoints, names));
    *mutableQuery.getStatementResultUnsafe() = binder::BoundStatementResult(endpoints, names);
  }
  std::vector<std::string> columnNames;
  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;

  auto tableName = indexTableName.empty() ? tableEntry->getName()
                                          : std::move(indexTableName);
  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::VARIADIC_REL, tableName,
      rel->getUniqueName(), rel->getVariableName(), columnNames,
      indexColumnInfos, std::move(fullPatternIR), sourceQuery);

  aux->setWorkloadCompatibility(true);
  aux->setSrcTableId(tableEntry->getSrcTableID());
  aux->setDstTableId(tableEntry->getDstTableID());
  aux->setEndpointUniqueNames(std::move(sourceEndpoint),
                              std::move(destinationEndpoint));
  uint32_t lower = 0;
  uint32_t upper = UINT32_MAX;
  if (relRecursiveInfo->bindData->lowerBound > 0) {
    lower = relRecursiveInfo->bindData->lowerBound;
  }
  if (relRecursiveInfo->bindData->upperBound != UINT32_MAX) {
    upper = relRecursiveInfo->bindData->upperBound;
  }
  aux->setRecursiveRelBounds(lower, upper);

  std::vector<common::property_id_t> propertyIDs;
  auto tableID = tableEntry->getTableID();
  auto extraSignature =
      neug::common::stringFormat("|bounds={}:{}",
                                 relRecursiveInfo->bindData->lowerBound,
                                 relRecursiveInfo->bindData->upperBound);
  auto baseName = makeFillIndexBaseName(
      tableName + "_variableLength", columnNames,
      catalog::FillEntryType::VARIADIC_REL, aux->getPatternIR(),
      indexColumnInfos, extraSignature);
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  validateStagedFillDependencies(*aux, tableID, catalog, transaction);
  validateWorkloadFillStateOwnership(*aux, tableID, ctx);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

}  // namespace catalog
}  // namespace neug

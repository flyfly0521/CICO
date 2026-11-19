#include "neug/compiler/binder/rewriter/aggregate_endpoint_set_rewriter.h"

#include <set>
#include <unordered_map>

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/bound_scan_source.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/parser.h"

namespace neug::binder {

bool provesAggregateEndpointSetSource(
    const catalog::FillIndexAuxInfo& info, main::ClientContext& context) {
  using namespace common;
  using namespace catalog;
  if (info.getFillType() != FillEntryType::REL ||
      !info.getTableUniqName().empty() || info.isFillUndirectedRel() ||
      info.getSourceQuery().empty() || info.getSrcNodeUniqueName().empty() ||
      info.getDstNodeUniqueName().empty() ||
      info.getSrcNodeUniqueName() == info.getDstNodeUniqueName()) return false;
  auto* carrier = context.getCatalog()->getTableCatalogEntry(
      context.getTransaction(), info.getTableName(), context.useInternalCatalogEntry());
  if (carrier == nullptr || carrier->getType() != CatalogEntryType::REL_TABLE_ENTRY)
    return false;
  const auto& relation = carrier->constCast<RelTableCatalogEntry>();

  try {
    const auto parsed = parser::Parser::parseQuery(info.getSourceQuery());
    if (parsed.size() != 1 || parsed[0]->getStatementType() != StatementType::QUERY)
      return false;
    Binder sourceBinder(&context);
    sourceBinder.setAllowIndexRewrite(false);
    auto statement = sourceBinder.bind(*parsed[0]);
    std::unique_ptr<BoundBaseScanSource> source =
        std::make_unique<BoundQueryScanSource>(
            std::move(statement), BoundQueryScanSourceInfo({}));
    // This existing normalizer accepts only a pure typed-identity grouping
    // followed by exact target-label dispatch. In particular, numeric keys
    // from different node tables cannot be grouped before label narrowing.
    normalizeFillAggregateProjection(source, carrier, &context);
    const auto& regular = source->constCast<BoundQueryScanSource>()
                              .statement->constCast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1 ||
        !regular.getPreQueryPart().empty() || regular.getPostSingleQuery())
      return false;
    const auto& single = *regular.getSingleQuery(0);
    if (single.getNumQueryParts() != 1) return false;
    const auto& part = *single.getQueryPart(0);
    if (part.hasUpdatingClause() || part.getNumReadingClause() != 1 ||
        !part.hasProjectionBody() || part.hasProjectionBodyPredicate()) return false;
    const auto* clause = part.getReadingClause(0);
    if (clause->getClauseType() != ClauseType::MATCH) return false;
    const auto& match = clause->constCast<BoundMatchClause>();
    if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasHint())
      return false;
    std::unordered_map<std::string, std::shared_ptr<NodeExpression>> nodes;
    for (const auto& node : match.getQueryGraphCollection()->getQueryNodes())
      nodes.emplace(node->getUniqueName(), node);
    const auto rels = match.getQueryGraphCollection()->getQueryRels();
    if (rels.empty()) return false;
    for (const auto& rel : rels) {
      if (rel->isRecursive() || rel->getTableIDsSet().contains(carrier->getTableID()))
        return false;
    }
    const auto& body = *part.getProjectionBody();
    if (!body.hasAggregateExpressions() || body.isDistinct() ||
        body.hasSkipOrLimit() || body.hasOrderByExpressions()) return false;
    const auto values = body.getProjectionExpressions();
    if (values.size() < 3) return false;
    std::unordered_map<std::string, table_id_t> keys;
    const auto identity = [&](const std::shared_ptr<Expression>& expression,
                              table_id_t table) -> std::string {
      std::string name;
      if (expression->expressionType == ExpressionType::PATTERN &&
          expression->dataType.getLogicalTypeID() == LogicalTypeID::NODE) {
        name = expression->getUniqueName();
      } else if (expression->expressionType == ExpressionType::PROPERTY) {
        const auto& property = expression->constCast<PropertyExpression>();
        if (!property.isPrimaryKey(table)) return {};
        name = property.getVariableName();
      } else return {};
      const auto node = nodes.find(name);
      if (node == nodes.end() || node->second->getTableIDsSet() !=
              std::unordered_set<table_id_t>{table}) return {};
      return name;
    };
    for (size_t i = 0; i < 2; ++i) {
      const auto table = i == 0 ? relation.getSrcTableID() : relation.getDstTableID();
      const auto key = identity(values[i], table);
      if (key.empty() || !keys.emplace(key, table).second) return false;
    }
    std::set<std::string> groups;
    for (const auto& group : body.getGroupByExpressions()) {
      std::string name;
      for (const auto& [key, table] : keys) {
        if (identity(group, table) == key) { name = key; break; }
      }
      if (name.empty()) return false;
      groups.insert(name);
    }
    if (groups.size() != keys.size()) return false;
    // No extra grouping value, scalar post-aggregation expression or HAVING
    // can remove endpoint pairs. The aggregate outputs themselves are unused.
    for (size_t i = 2; i < values.size(); ++i)
      if (values[i]->expressionType != ExpressionType::AGGREGATE_FUNCTION)
        return false;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace neug::binder

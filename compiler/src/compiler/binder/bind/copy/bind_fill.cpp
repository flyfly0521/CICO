
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/catalog/catalog_entry/table_catalog_entry.h"
#include "neug/compiler/common/assert.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/parser/copy.h"
#include "neug/compiler/parser/expression/parsed_function_expression.h"

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/bound_scan_source.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/catalog_entry_type.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/common/enums/column_evaluate_type.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/scan_source.h"
#include "neug/utils/exception/exception.h"

using namespace neug::common;
using namespace neug::parser;
namespace neug {
namespace binder {
std::vector<std::pair<std::string, common::LogicalType>>
Binder::bindFillClause(
    const parser::Statement& statement, catalog::TableCatalogEntry* tableEntry,
    std::unique_ptr<binder::BoundBaseScanSource>& boundSource) {
  auto& copyFrom = statement.constCast<neug::parser::CopyFrom>();
  auto tableName = copyFrom.getTableName();
  const auto& copyInfo = copyFrom.getCopyColumnInfo();
  auto columnNames = copyInfo.columnNames;

  const bool isNode = tableEntry->getType() ==
                      neug::catalog::CatalogEntryType::NODE_TABLE_ENTRY;
  NEUG_ASSERT(tableEntry &&
              (isNode || tableEntry->getType() ==
                             neug::catalog::CatalogEntryType::REL_TABLE_ENTRY));

  // FILL R() inserts derived edges. Its data outputs map to the user schema
  // just like COPY; route it through the same aggregate-state preparation as
  // the property-update form FILL R(value).
  if (!isNode && columnNames.empty() && boundSource->getColumns().size() > 2) {
    for (const auto& property : tableEntry->getProperties()) {
      if (!Binder::reservedInColumnName(property.getName()) &&
          property.getType().getLogicalTypeID() != LogicalTypeID::SERIAL) {
        columnNames.push_back(property.getName());
      }
    }
  }

  // Validate columns exist in table
  for (const auto& columnName : columnNames) {
    if (columnName.starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX)) {
      THROW_BINDER_EXCEPTION("FILL cannot write reserved internal state columns.");
    }
    if (!tableEntry->containsProperty(columnName)) {
      THROW_BINDER_EXCEPTION(stringFormat(
          "Column {} does not exist in table{}.", columnName, tableName));
    }
  }

  // Validate query returns correct number of columns
  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION("FILL requires a query as the scan source.");
  }
  catalog::normalizeFillAggregateProjection(boundSource, tableEntry,
                                             clientContext);
  auto presenceRowCount = expressionBinder.bindAggregateFunctionExpression(
      ParsedFunctionExpression("COUNT_STAR", "COUNT(*)", false), "COUNT_STAR",
      false);
  presenceRowCount->setParsedName("COUNT(*)");
  catalog::normalizeFillSingleEdgeDistinctCount(boundSource, tableEntry,
                                               presenceRowCount);
  auto queryColumns = boundSource->getColumns();
  const auto userQueryColumnCount = queryColumns.size();
  const auto expectedCols =
      isNode ? (columnNames.size() + 1) : (columnNames.size() + 2);
  if (queryColumns.size() != expectedCols) {
    THROW_BINDER_EXCEPTION(common::stringFormat(
        isNode
            ? "FILL query must return {} columns (1 ID + {} data columns), but "
              "got {}.\nExpected: RETURN node.id, val1, val2, ..., val{}"
            : "FILL query must return {} columns (src.id, dst.id + {} data), "
              "but got {}.\nExpected: RETURN src.id, dst.id, val1, ... val{}",
        expectedCols, columnNames.size(), queryColumns.size(),
        columnNames.size()));
  }

  const char* legacyProfile = std::getenv("NEUG_ALLOW_LEGACY_FILL_EXPRESSIONS");
  const bool allowLegacyExpressions =
      legacyProfile != nullptr && std::string_view(legacyProfile) == "1";

  // Compatibility definitions obey the same exact storage contract.
  // Validate before either metadata route can create an index or write values.
  for (size_t i = 0; i < columnNames.size(); ++i) {
    const auto& actual = queryColumns[i + (isNode ? 1 : 2)]->getDataType();
    const auto& expected = tableEntry->getProperty(columnNames[i]).getType();
    if (actual != expected)
      THROW_BINDER_EXCEPTION(common::stringFormat(
          "FILL target type {} differs from source type {}; use an explicit cast.",
          expected.toString(), actual.toString()));
  }

  std::vector<std::pair<std::string, common::LogicalType>> internalStates;
  const bool recompute =
      catalog::requiresSourceRecompute(*boundSource, *tableEntry) ||
      catalog::fillSourceReadsMaterializedProperty(*boundSource, clientContext);
  if (recompute) {
    auto& regular = boundSource->cast<BoundQueryScanSource>()
                        .statement->cast<BoundRegularQuery>();
    if (regular.getNumSingleQueries() != 1)
      THROW_BINDER_EXCEPTION("FILL requires one non-UNION source");
    auto* single = regular.getSingleQueryUnsafe(0);
    auto present =
        expressionBinder.createLiteralExpression(common::Value(int64_t{1}));
    std::ostringstream name;
    std::string signature = copyFrom.getFillSourceQuery();
    for (const auto& column : columnNames) {
      signature += '\0';
      signature += column;
    }
    name << InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX << std::hex
         << std::hash<std::string>{}(signature);
    present->setUniqueName(name.str());
    auto* body = single->getQueryPartUnsafe(single->getNumQueryParts() - 1)
                     ->getProjectionBodyUnsafe();
    auto expressions = body->getProjectionExpressions();
    expressions.push_back(present);
    body->setProjectionExpressions(std::move(expressions));
    auto columns = regular.getStatementResult()->getColumns();
    auto names = regular.getStatementResult()->getColumnNames();
    columns.push_back(present);
    names.push_back(name.str());
    *regular.getStatementResultUnsafe() = BoundStatementResult(columns, names);
    single->setStatementResult(BoundStatementResult(columns, names));
    columnNames.push_back(name.str());
    internalStates.emplace_back(name.str(), common::LogicalType::INT64());
    if (isNode)
      createNodeIndexEntry(boundSource, tableEntry, clientContext, columnNames,
                           copyFrom.getFillSourceQuery());
    else
      createRelIndexEntry(boundSource, columnNames,
                          tableEntry->ptrCast<catalog::RelTableCatalogEntry>(),
                          clientContext, copyFrom.getFillSourceQuery());
    return internalStates;
  }

  if (!allowLegacyExpressions) {
    // Compatibility metadata is an implementation choice, not an escape
    // from the strict aggregate and exact-arithmetic contract.
    for (size_t i = isNode ? 1u : 2u; i < queryColumns.size(); ++i)
      catalog::validateCanonicalFillProjection(*queryColumns[i]);
  }

  if (catalog::requiresWorkloadFillCompatibility(boundSource, tableEntry,
                                                 columnNames)) {
    // A DISTINCT identity plus true is an existential value even when its
    // witnesses are many-to-many. The catalog helper proves that exact shape
    // before using this count as private incremental-maintenance state.
    if (auto states = catalog::tryCreateFlattenedCountIndexEntry(
            boundSource, tableEntry, clientContext, columnNames,
            copyFrom.getFillSourceQuery(), std::move(presenceRowCount))) {
      return std::move(*states);
    }
    if (isNode) {
      const auto indexName = catalog::createWorkloadNodeIndexEntry(
          boundSource, tableEntry, clientContext, columnNames,
          copyFrom.getFillSourceQuery());
      if (indexName.empty()) {
        THROW_BINDER_EXCEPTION(
            "FILL source has no supported canonical or workload registration "
            "shape; no index was registered.");
      }
    } else {
      catalog::createWorkloadRelIndexEntry(
          boundSource, columnNames, tableEntry->ptrCast<catalog::RelTableCatalogEntry>(),
          clientContext, copyFrom.getFillSourceQuery());
    }
    return {};
  }

  bool hasAggregate = false;
  {
    // Legacy expression materializations remain an explicit compatibility
    // profile. Their public values keep their original meaning and are not
    // advertised as canonical SUM/COUNT state to new algebraic rewrites.
    // A finalized aggregate such as 1 / (SUM(x) + 1) needs SUM(x) persisted
    // even though it is not part of the user's RETURN list. Reuse an explicit
    // aggregate output when present; otherwise append the aggregate to the
    // bound final projection and give it an internal target property.
    std::unordered_map<std::string, std::string> persistedAggregates;
    const size_t dataStart = isNode ? 1 : 2;
    for (size_t i = dataStart; i < queryColumns.size(); ++i) {
      if (queryColumns[i]->expressionType ==
          common::ExpressionType::AGGREGATE_FUNCTION) {
        persistedAggregates.emplace(queryColumns[i]->getUniqueName(),
                                     columnNames[i - dataStart]);
      }
    }

    std::vector<std::pair<std::shared_ptr<Expression>, std::string>> toAppend;
    for (size_t i = dataStart; i < userQueryColumnCount; ++i) {
      std::vector<std::shared_ptr<Expression>> nestedAggregates;
      std::unordered_set<std::string> seenAggregates;
      std::function<void(const std::shared_ptr<Expression>&)> collect =
          [&](const std::shared_ptr<Expression>& expr) {
            if (expr->expressionType ==
                common::ExpressionType::AGGREGATE_FUNCTION) {
              if (seenAggregates.insert(expr->getUniqueName()).second)
                nestedAggregates.push_back(expr);
              return;
            }
            for (const auto& child : ExpressionChildrenCollector::collectChildren(*expr)) {
              collect(child);
            }
          };
      collect(queryColumns[i]);
      if (nestedAggregates.empty()) {
        continue;
      }
      if (!allowLegacyExpressions &&
          !catalog::isCanonicalFillAggregate(*queryColumns[i])) {
        THROW_BINDER_EXCEPTION(
            "FILL requires canonical base aggregates SUM(x), COUNT(x) or "
            "COUNT(*). Persist the base SUM and matching COUNT separately; "
            "evaluate affine expressions and aggregate finalizers in the "
            "query after merging. NEUG_ALLOW_LEGACY_FILL_EXPRESSIONS=1 "
            "explicitly enables legacy computed-value materialization.");
      }
      hasAggregate = true;
      if (nestedAggregates.size() != 1) {
        THROW_BINDER_EXCEPTION(
            "A finalized FILL expression must contain exactly one aggregate "
            "state.");
      }
      const auto& aggregate = nestedAggregates.front();
      const auto& aggregateExpr =
          aggregate->constCast<AggregateFunctionExpression>();
      std::string functionName = aggregateExpr.getFunction().name;
      std::transform(functionName.begin(), functionName.end(),
                     functionName.begin(), [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                     });
      if (aggregateExpr.isDistinct() ||
          (functionName != "SUM" && functionName != "COUNT" &&
           functionName != "COUNT_STAR")) {
        THROW_BINDER_EXCEPTION(
            "Finalized FILL expressions currently require one non-DISTINCT "
            "SUM or COUNT aggregate.");
      }
      if (persistedAggregates.contains(aggregate->getUniqueName())) {
        continue;
      }
      if (isNode) {
        THROW_BINDER_EXCEPTION(
            "Finalized NODE FILL expressions are not yet supported; persist "
            "the SUM or COUNT state directly.");
      }

      std::ostringstream stateName;
      stateName << common::InternalKeyword::FILL_AGGREGATE_STATE_PREFIX
                << columnNames[i - dataStart] << '_' << std::hex
                << std::hash<std::string>{}(aggregate->getUniqueName());
      auto hiddenName = stateName.str();
      persistedAggregates.emplace(aggregate->getUniqueName(), hiddenName);
      internalStates.emplace_back(hiddenName, aggregate->getDataType().copy());
      toAppend.emplace_back(aggregate, std::move(hiddenName));
    }

    if (hasAggregate) {
      auto count = expressionBinder.bindAggregateFunctionExpression(
          ParsedFunctionExpression("COUNT_STAR", "COUNT(*)", false), "COUNT_STAR",
          false);
      count->setParsedName("COUNT(*)");
      std::ostringstream stateName;
      std::string stateSignature = copyFrom.getFillSourceQuery();
      for (const auto& column : columnNames) {
        stateSignature += '\0';
        stateSignature += column;
      }
      stateName << InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX << std::hex
                << std::hash<std::string>{}(stateSignature);
      internalStates.emplace_back(stateName.str(), count->getDataType().copy());
      toAppend.emplace_back(std::move(count), stateName.str());
    }

    if (!toAppend.empty()) {
      auto& querySource = boundSource->cast<BoundQueryScanSource>();
      auto& regularQuery = querySource.statement->cast<BoundRegularQuery>();
      if (regularQuery.getNumSingleQueries() != 1) {
        THROW_BINDER_EXCEPTION(
            "UNION is not supported in incremental FILL sources.");
      }
      auto* singleQuery = regularQuery.getSingleQueryUnsafe(0);
      NEUG_ASSERT(singleQuery->getNumQueryParts() > 0);
      uint32_t aggregatePartIdx = UINT32_MAX;
      for (uint32_t pi = 0; pi < singleQuery->getNumQueryParts(); ++pi) {
        auto* part = singleQuery->getQueryPartUnsafe(pi);
        if (!part->hasProjectionBody() ||
            !part->getProjectionBody()->hasAggregateExpressions()) {
          continue;
        }
        if (aggregatePartIdx != UINT32_MAX) {
          THROW_BINDER_EXCEPTION(
              "Multiple aggregation stages are not supported in FILL sources.");
        }
        aggregatePartIdx = pi;
      }
      if (aggregatePartIdx == UINT32_MAX) {
        THROW_BINDER_EXCEPTION("Cannot resolve the FILL aggregation stage.");
      }

      auto topColumns = regularQuery.getStatementResult()->getColumns();
      auto topNames = regularQuery.getStatementResult()->getColumnNames();
      auto singleColumns = singleQuery->getStatementResult()->getColumns();
      auto singleNames = singleQuery->getStatementResult()->getColumnNames();
      for (const auto& [aggregate, hiddenName] : toAppend) {
        // WITH keys, SUM(x) AS value RETURN keys.id, value is a single
        // aggregation followed by projection. Count input witnesses at the
        // actual aggregation stage and carry that state through later parts;
        // adding COUNT(*) only to RETURN would count groups instead.
        for (uint32_t pi = aggregatePartIdx;
             pi < singleQuery->getNumQueryParts(); ++pi) {
          auto* projection =
              singleQuery->getQueryPartUnsafe(pi)->getProjectionBodyUnsafe();
          auto projectionExpressions = projection->getProjectionExpressions();
          const auto containsAggregate = [&](const auto& expressions) {
            return std::any_of(expressions.begin(), expressions.end(),
                               [&](const auto& existing) {
                                 return existing->getUniqueName() ==
                                        aggregate->getUniqueName();
                               });
          };
          if (pi + 1 == singleQuery->getNumQueryParts() ||
              !containsAggregate(projectionExpressions)) {
            projectionExpressions.push_back(aggregate);
            projection->setProjectionExpressions(std::move(projectionExpressions));
          }
          if (pi == aggregatePartIdx) {
            auto aggregateExpressions = projection->getAggregateExpressions();
            if (!containsAggregate(aggregateExpressions)) {
              aggregateExpressions.push_back(aggregate);
              projection->setAggregateExpressions(std::move(aggregateExpressions));
            }
          }
        }
        topColumns.push_back(aggregate);
        topNames.push_back(hiddenName);
        singleColumns.push_back(aggregate);
        singleNames.push_back(hiddenName);
        columnNames.push_back(hiddenName);
      }
      *regularQuery.getStatementResultUnsafe() = BoundStatementResult(
          std::move(topColumns), std::move(topNames));
      singleQuery->setStatementResult(BoundStatementResult(
          std::move(singleColumns), std::move(singleNames)));
      queryColumns = boundSource->getColumns();
    }
  }

  // Build column expressions for COPY_FROM mechanism
  expression_vector columnExprs;
  std::vector<ColumnEvaluateType> evaluateTypes;

  // Map property columns to table properties (skip leading primary key columns)
  auto startIdx = isNode ? 1u : 2u;
  for (auto i = startIdx; i < queryColumns.size(); ++i) {
    auto& queryColumn = queryColumns[i];
    auto& columnName = columnNames[i - startIdx];
    if (!tableEntry->containsProperty(columnName)) {
      // Internal aggregate state properties are created by the FILL write
      // operator immediately before materialization.
      continue;
    }
    auto& property = tableEntry->getProperty(columnName);
    if (hasAggregate) {
      const auto targetType = property.getType().getLogicalTypeID();
      const bool finalizer = catalog::isMaintainableFillFinalizer(*queryColumn);
      const bool exactHalfSum = queryColumn->expressionType == ExpressionType::AGGREGATE_FUNCTION &&
          queryColumn->getNumChildren() == 1 &&
          catalog::isExactHalfIntegerFillExpression(*queryColumn->getChild(0));
      const bool exactDouble = targetType == LogicalTypeID::DOUBLE &&
          (finalizer || exactHalfSum);
      if (targetType != LogicalTypeID::INT32 && targetType != LogicalTypeID::INT64 && !exactDouble) {
        THROW_BINDER_EXCEPTION(
            "FILL aggregate targets require exact INT32 or INT64 storage; "
            "FLOAT/DOUBLE cannot guarantee reversible aggregate maintenance "
            "and unsigned state cannot represent retractions.");
      }
      const auto sourceType = queryColumn->getDataType().getLogicalTypeID();
      if (sourceType != LogicalTypeID::INT8 && sourceType != LogicalTypeID::INT16 &&
          sourceType != LogicalTypeID::INT32 && sourceType != LogicalTypeID::INT64 &&
          !(exactDouble && sourceType == LogicalTypeID::DOUBLE)) {
        THROW_BINDER_EXCEPTION(
            "FILL aggregate output must be a signed integer; a cast into an "
            "integer target cannot make floating aggregate state exact.");
      }
      if (targetType != sourceType) {
        THROW_BINDER_EXCEPTION(
            "FILL aggregate output and target must have the same signed-integer "
            "type; changing storage width changes the aggregate arithmetic "
            "contract. Use a target with the aggregate's result type.");
      }
    }

    // Check if types match or need casting
    if (queryColumn->getDataType() == property.getType()) {
      columnExprs.push_back(queryColumn);
      // reference directly the column returned by the sub query
      evaluateTypes.push_back(ColumnEvaluateType::REFERENCE);
    } else {
      columnExprs.push_back(
          expressionBinder.forceCast(queryColumn, property.getType()));
      evaluateTypes.push_back(ColumnEvaluateType::CAST);
    }
  }

  if (isNode) {
    createNodeIndexEntry(boundSource, tableEntry, clientContext, columnNames,
                         copyFrom.getFillSourceQuery());
  } else {
    createRelIndexEntry(boundSource, columnNames,
                        tableEntry->ptrCast<catalog::RelTableCatalogEntry>(),
                        clientContext, copyFrom.getFillSourceQuery());
  }
  return internalStates;
}
}  // namespace binder
}  // namespace neug

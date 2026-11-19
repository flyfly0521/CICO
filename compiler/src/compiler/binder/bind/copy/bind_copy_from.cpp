/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

/**
 * This file is originally from the Kùzu project
 * (https://github.com/kuzudb/kuzu) Licensed under the MIT License. Modified by
 * Zhou Xiaoli in 2025 to support Neug-specific features.
 */

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/copy/bound_copy_from.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/common/string_format.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/copy.h"
#include "neug/utils/exception/exception.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace neug::binder;
using namespace neug::catalog;
using namespace neug::common;
using namespace neug::parser;
using namespace neug::function;

namespace neug {
namespace binder {

std::unique_ptr<BoundStatement> Binder::bindCopyFromClause(
    const Statement& statement) {
  auto& copyStatement = neug_dynamic_cast<const CopyFrom&>(statement);
  auto tableName = copyStatement.getTableName();
  auto catalog = clientContext->getCatalog();
  auto transaction = clientContext->getTransaction();
  if (catalog->containsRelGroup(transaction, tableName)) {
    auto entry = catalog->getRelGroupEntry(transaction, tableName);
    if (entry->getNumRelTables() == 1) {
      auto tableEntry = catalog->getTableCatalogEntry(
          transaction, entry->getRelTableIDs()[0]);
      return bindCopyRelFrom(statement,
                             tableEntry->ptrCast<RelTableCatalogEntry>());
    } else {
      if (copyStatement.createIndex() &&
          copyStatement.getCopyColumnInfo().columnNames.empty() &&
          copyStatement.getSource()->type == common::ScanSourceType::QUERY) {
        auto tableEntry = catalog->getTableCatalogEntry(
            transaction, entry->getRelTableIDs()[0]);
        return bindCopyRelFrom(statement,
                               tableEntry->ptrCast<RelTableCatalogEntry>(),
                               entry->getRelTableIDs(),
                               tableName);
      }
      auto options = bindParsingOptions(copyStatement.getParsingOptions());
      if (!options.contains(CopyConstants::FROM_OPTION_NAME) ||
          !options.contains(CopyConstants::TO_OPTION_NAME)) {
        THROW_BINDER_EXCEPTION(
            stringFormat("The table {} has multiple FROM and TO pairs defined "
                         "in the schema. A specific "
                         "pair of FROM and TO options is expected when copying "
                         "data into the {} table.",
                         tableName, tableName));
      }
      auto from =
          options.at(CopyConstants::FROM_OPTION_NAME).getValue<std::string>();
      auto to =
          options.at(CopyConstants::TO_OPTION_NAME).getValue<std::string>();
      auto relTableName =
          RelGroupCatalogEntry::getChildTableName(tableName, from, to);
      if (catalog->containsTable(transaction, relTableName)) {
        auto relEntry =
            catalog->getTableCatalogEntry(transaction, relTableName);
        return bindCopyRelFrom(statement,
                               relEntry->ptrCast<RelTableCatalogEntry>());
      }
    }
    THROW_BINDER_EXCEPTION(
        stringFormat("REL GROUP {} does not exist.", tableName));
  } else if (catalog->getTableCatalogEntry(transaction, tableName)) {
    auto tableEntry = catalog->getTableCatalogEntry(transaction, tableName);
    switch (tableEntry->getType()) {
    case CatalogEntryType::NODE_TABLE_ENTRY: {
      auto nodeTableEntry = tableEntry->ptrCast<NodeTableCatalogEntry>();
      return bindCopyNodeFrom(statement, nodeTableEntry);
    }
    case CatalogEntryType::REL_TABLE_ENTRY: {
      auto relTableEntry = tableEntry->ptrCast<RelTableCatalogEntry>();
      return bindCopyRelFrom(statement, relTableEntry);
    }
    default: {
      NEUG_UNREACHABLE;
    }
    }
  }
  THROW_BINDER_EXCEPTION(stringFormat("Table {} does not exist.", tableName));
}

static void bindExpectedNodeColumns(const NodeTableCatalogEntry* nodeTableEntry,
                                    const CopyFromColumnInfo& info,
                                    std::vector<std::string>& columnNames,
                                    std::vector<LogicalType>& columnTypes,
                                    bool allowNodeColumnList);
static void bindExpectedRelColumns(const RelTableCatalogEntry* relTableEntry,
                                   const CopyFromColumnInfo& info,
                                   std::vector<std::string>& columnNames,
                                   std::vector<LogicalType>& columnTypes,
                                   const main::ClientContext* context,
                                   bool allowRelColumnList);

static std::pair<ColumnEvaluateType, std::shared_ptr<Expression>>
matchColumnExpression(const expression_vector& columns,
                      const PropertyDefinition& property,
                      ExpressionBinder& expressionBinder) {
  for (auto& column : columns) {
    if (property.getName() == column->toString()) {
      if (column->dataType == property.getType()) {
        return {ColumnEvaluateType::REFERENCE, column};
      } else {
        return {ColumnEvaluateType::CAST,
                expressionBinder.forceCast(column, property.getType())};
      }
    }
  }
  return {ColumnEvaluateType::DEFAULT,
          expressionBinder.bindExpression(*property.defaultExpr)};
}

std::unique_ptr<BoundStatement> Binder::bindCopyNodeFrom(
    const Statement& statement, NodeTableCatalogEntry* nodeTableEntry) {
  auto& copyStatement = neug_dynamic_cast<const CopyFrom&>(statement);
  // Bind expected columns based on catalog information.
  std::vector<std::string> expectedColumnNames;
  std::vector<LogicalType> expectedColumnTypes;
  bindExpectedNodeColumns(nodeTableEntry, copyStatement.getCopyColumnInfo(),
                          expectedColumnNames, expectedColumnTypes,
                          copyStatement.createIndex());
  if (!copyStatement.createIndex()) {
    for (const auto& name : expectedColumnNames) {
      catalog::validateFillUserPropertyWrite(clientContext,
                                             nodeTableEntry->getTableID(), name);
    }
  }
  // The FILL source must not rewrite against the index being created. The
  // guard also restores rewriting if binding throws below.
  std::optional<ScopedAllowRewriteOverride> rewriteGuard;
  if (copyStatement.createIndex()) {
    rewriteGuard.emplace(this->allowRewrite);
  }
  auto boundSource = bindScanSource(copyStatement.getSource(),
                                    copyStatement.getParsingOptions(),
                                    expectedColumnNames, expectedColumnTypes);
  if (copyStatement.createIndex()) {
    rewriteGuard.reset();
    auto internalStates = bindFillClause(statement, nodeTableEntry, boundSource);
    for (auto& [name, type] : internalStates) {
      expectedColumnNames.push_back(std::move(name));
      expectedColumnTypes.push_back(std::move(type));
    }
  }
  expression_vector columns =
      createVariables(expectedColumnNames, expectedColumnTypes);
  std::vector<ColumnEvaluateType> evaluateTypes(columns.size(),
                                                ColumnEvaluateType::REFERENCE);
  auto offset = createInvisibleVariable(
      std::string(InternalKeyword::ROW_OFFSET), LogicalType::INT64());
  auto boundCopyFromInfo = BoundCopyFromInfo(
      nodeTableEntry, std::move(boundSource), std::move(offset),
      std::move(columns), std::move(evaluateTypes), nullptr /* extraInfo */);
  return std::make_unique<BoundCopyFrom>(
      std::move(boundCopyFromInfo), copyStatement.createIndex(),
      copyStatement.createIndex());
}

static options_t getScanSourceOptions(const CopyFrom& copyFrom) {
  options_t options;
  static case_insensitve_set_t copyFromPairsOptions = {
      CopyConstants::FROM_OPTION_NAME, CopyConstants::TO_OPTION_NAME};
  for (auto& option : copyFrom.getParsingOptions()) {
    if (copyFromPairsOptions.contains(option.first)) {
      continue;
    }
    options.emplace(option.first, option.second->copy());
  }
  return options;
}

std::unique_ptr<BoundStatement> Binder::bindCopyRelFrom(
    const Statement& statement, RelTableCatalogEntry* relTableEntry,
    std::vector<table_id_t> relGroupTargetTableIDs,
    std::string variadicIndexTableName) {
  auto& copyStatement = statement.constCast<CopyFrom>();
  if (copyStatement.byColumn()) {
    THROW_BINDER_EXCEPTION(stringFormat(
        "Copy by column is not supported for relationship table."));
  }
  // Bind expected columns based on catalog information.
  std::vector<std::string> expectedColumnNames;
  std::vector<LogicalType> expectedColumnTypes;
  // For relationship COPY, we historically required the source to provide
  // (from, to) followed by *all* relationship properties.
  // For FILL (parsed as CopyFrom with createIndex_ flag), allow specifying a
  // subset of relationship properties via column list.
  bindExpectedRelColumns(relTableEntry, copyStatement.getCopyColumnInfo(),
                         expectedColumnNames, expectedColumnTypes,
                         clientContext, copyStatement.createIndex());
  if (!copyStatement.createIndex()) {
    catalog::validateFillUserRelationshipWrite(clientContext,
                                               relTableEntry->getTableID());
    for (size_t i = 2; i < expectedColumnNames.size(); ++i) {
      catalog::validateFillUserPropertyWrite(clientContext,
                                             relTableEntry->getTableID(),
                                             expectedColumnNames[i]);
    }
  }
  // The FILL source must not rewrite against the index being created. The
  // guard also restores rewriting if binding throws below.
  std::optional<ScopedAllowRewriteOverride> rewriteGuard;
  if (copyStatement.createIndex()) {
    rewriteGuard.emplace(this->allowRewrite);
  }
  auto boundSource = bindScanSource(copyStatement.getSource(),
                                    getScanSourceOptions(copyStatement),
                                    expectedColumnNames, expectedColumnTypes);
  bool variadicIndexOnly = false;
  const bool relGroupEdgeFill = !relGroupTargetTableIDs.empty();
  bool copyIndex = copyStatement.createIndex() && !relGroupEdgeFill;
  if (copyStatement.createIndex()) {
    rewriteGuard.reset();
    auto hasColumns = (!copyStatement.getCopyColumnInfo().columnNames.empty());
    const bool materializedRelWithProperties =
        !hasColumns && !relGroupEdgeFill && boundSource->getColumns().size() > 2;
    if (hasColumns) {
      // Property-fill mode: build a fill index for the specified columns.
      auto internalStates =
          bindFillClause(statement, relTableEntry, boundSource);
      for (auto& [name, type] : internalStates) {
        expectedColumnNames.push_back(std::move(name));
        expectedColumnTypes.push_back(std::move(type));
      }
    } else if (materializedRelWithProperties) {
      // A no-column FILL still carries relationship properties when its source
      // returns more than the two endpoint keys. Storage must insert the new
      // edges, while the catalog records a regular relationship
      // materialization so the rewriter can use its property columns.
      auto internalStates = bindFillClause(statement, relTableEntry, boundSource);
      for (auto& [name, type] : internalStates) {
        expectedColumnNames.push_back(std::move(name));
        expectedColumnTypes.push_back(std::move(type));
      }
      copyIndex = false;
    } else if (relGroupEdgeFill) {
      // Rel-group edge-fill mode: FILL <rel-group>() FROM (<query>) writes
      // actual edges into each concrete child table. The storage layer filters
      // rows whose src/dst keys do not belong to that child table.
      //
      // A single recursive source edge is a transitive-closure index. Other
      // source patterns are ordinary relationship materializations and must
      // retain the full source pattern so the rewriter can replace it with the
      // derived edge.
      auto indexName = catalog::createVaradicRelIndexEntry(
          boundSource, relTableEntry, clientContext, variadicIndexTableName,
          copyStatement.getFillSourceQuery());
      if (indexName.empty()) {
        const auto columns = boundSource->getColumns();
        if (columns.size() > 2) {
          // A derived relationship group may carry aggregate properties.
          // Register those columns on the child selected by the projected
          // endpoint keys, rather than a property-free index on the first
          // child. Ambiguous endpoints are materialized without a rewrite.
          const auto* src = columns[0]->expressionType == ExpressionType::PROPERTY
              ? columns[0]->constPtrCast<PropertyExpression>() : nullptr;
          const auto* dst = columns[1]->expressionType == ExpressionType::PROPERTY
              ? columns[1]->constPtrCast<PropertyExpression>() : nullptr;
          if (src != nullptr && dst != nullptr && src->isSingleLabel() &&
              dst->isSingleLabel()) {
            for (const auto tableID : relGroupTargetTableIDs) {
              auto* child = clientContext->getCatalog()->getTableCatalogEntry(
                  clientContext->getTransaction(), tableID)
                  ->ptrCast<RelTableCatalogEntry>();
              if (child->getSrcTableID() != src->getSingleTableID() ||
                  child->getDstTableID() != dst->getSingleTableID()) {
                continue;
              }
              std::vector<std::string> fillColumnNames(
                  expectedColumnNames.begin() + 2, expectedColumnNames.end());
              catalog::createRelIndexEntry(boundSource,
                  std::move(fillColumnNames), child, clientContext,
                  copyStatement.getFillSourceQuery());
              break;
            }
          }
        } else {
          catalog::createWorkloadRelIndexEntry(boundSource, {}, relTableEntry,
                                       clientContext, copyStatement.getFillSourceQuery(),
                                       std::move(variadicIndexTableName));
        }
      }
    } else {
      // Transitive-closure mode: FILL <rel> () FROM (MATCH ... *lo..hi ...).
      // Register a recursive REL index so that future queries with the same
      // variable-length pattern can be rewritten to use this single-hop edge.
      // Also keep the regular COPY_FROM path active so the materialized
      // relationship table is populated with the query's (src, dst) rows.
      LOG(INFO) << "Creating recursive REL fill index for relationship table "
                << relTableEntry->getName();
      auto indexName = catalog::createVaradicRelIndexEntry(
          boundSource, relTableEntry, clientContext,
          std::move(variadicIndexTableName),
          copyStatement.getFillSourceQuery());
      if (indexName.empty()) {
        catalog::createRelIndexEntry(boundSource, {}, relTableEntry,
                                     clientContext,
                                     copyStatement.getFillSourceQuery());
      }
      copyIndex = false;
    }
  }
  expression_vector warningDataExprs;
  auto offset = createInvisibleVariable(
      std::string(InternalKeyword::ROW_OFFSET), LogicalType::INT64());
  auto srcTableID = relTableEntry->getSrcTableID();
  auto dstTableID = relTableEntry->getDstTableID();

  auto srcOffset = createVariable(std::string(InternalKeyword::SRC_OFFSET),
                                  LogicalType::INT64());
  auto dstOffset = createVariable(std::string(InternalKeyword::DST_OFFSET),
                                  LogicalType::INT64());
  expression_vector columns =
      createVariables(expectedColumnNames, expectedColumnTypes);
  std::vector<ColumnEvaluateType> evaluateTypes{columns.size(),
                                                ColumnEvaluateType::REFERENCE};
  std::shared_ptr<Expression> srcKey = nullptr, dstKey = nullptr;
  if (expectedColumnTypes[0] != columns[0]->getDataType()) {
    srcKey = expressionBinder.forceCast(columns[0], expectedColumnTypes[0]);
  } else {
    srcKey = columns[0];
  }
  if (expectedColumnTypes[1] != columns[1]->getDataType()) {
    dstKey = expressionBinder.forceCast(columns[1], expectedColumnTypes[1]);
  } else {
    dstKey = columns[1];
  }
  auto srcLookUpInfo =
      IndexLookupInfo(srcTableID, srcOffset, srcKey, warningDataExprs);
  auto dstLookUpInfo =
      IndexLookupInfo(dstTableID, dstOffset, dstKey, warningDataExprs);
  auto lookupInfos = std::vector<IndexLookupInfo>{srcLookUpInfo, dstLookUpInfo};
  auto internalIDColumnIndices = std::vector<idx_t>{0, 1, 2};
  auto extraCopyRelInfo = std::make_unique<ExtraBoundCopyRelInfo>(
      internalIDColumnIndices, lookupInfos, std::move(relGroupTargetTableIDs));
  auto boundCopyFromInfo = BoundCopyFromInfo(
      relTableEntry, boundSource->copy(), offset, std::move(columns),
      std::move(evaluateTypes), std::move(extraCopyRelInfo));
  auto result = std::make_unique<BoundCopyFrom>(
      std::move(boundCopyFromInfo), copyIndex, copyStatement.createIndex());
  if (variadicIndexOnly) {
    result->setVariadicIndexOnly();
  }
  return result;
}

static bool skipPropertyInFile(const PropertyDefinition& property) {
  if (property.getName() == InternalKeyword::ID) {
    return true;
  }
  return false;
}

static bool skipPropertyInSchema(const PropertyDefinition& property) {
  if (property.getName().starts_with(
          InternalKeyword::FILL_AGGREGATE_STATE_PREFIX)) {
    return true;
  }
  if (property.getType().getLogicalTypeID() == LogicalTypeID::SERIAL) {
    return true;
  }
  if (property.getName() == InternalKeyword::ID) {
    return true;
  }
  if (property.getName() == common::InternalKeyword::ID ||
      property.getName() == common::InternalKeyword::LABEL ||
      property.getName() == common::InternalKeyword::ROW_OFFSET ||
      property.getName() == common::InternalKeyword::SRC_OFFSET ||
      property.getName() == common::InternalKeyword::DST_OFFSET ||
      property.getName() == common::InternalKeyword::SRC ||
      property.getName() == common::InternalKeyword::DST) {
    return true;
  }
  return false;
}

static void bindExpectedColumns(const TableCatalogEntry* tableEntry,
                                const CopyFromColumnInfo& info,
                                std::vector<std::string>& columnNames,
                                std::vector<LogicalType>& columnTypes) {
  if (!info.columnNames.empty()) {
    std::unordered_map<std::string, LogicalType> nameToType;
    nameToType.reserve(tableEntry->getProperties().size());
    for (auto& property : tableEntry->getProperties()) {
      if (skipPropertyInSchema(property)) {
        continue;
      }
      nameToType.emplace(property.getName(), property.getType().copy());
    }
    std::unordered_set<std::string> seen;
    seen.reserve(info.columnNames.size());
    for (auto& name : info.columnNames) {
      if (!seen.emplace(name).second) {
        THROW_BINDER_EXCEPTION(
            stringFormat("Duplicate column name {} in column list.", name));
      }
      auto it = nameToType.find(name);
      if (it == nameToType.end()) {
        THROW_BINDER_EXCEPTION(
            stringFormat("Column {} does not exist in table {}.", name,
                         tableEntry->getName()));
      }
      columnNames.push_back(name);
      columnTypes.push_back(it->second.copy());
    }
    return;
  }

  for (auto& property : tableEntry->getProperties()) {
    if (skipPropertyInSchema(property)) {
      continue;
    }
    columnNames.push_back(property.getName());
    columnTypes.push_back(property.getType().copy());
  }
}

void bindExpectedNodeColumns(const NodeTableCatalogEntry* nodeTableEntry,
                             const CopyFromColumnInfo& info,
                             std::vector<std::string>& columnNames,
                             std::vector<LogicalType>& columnTypes,
                             bool allowNodeColumnList) {
  NEUG_ASSERT(columnNames.empty() && columnTypes.empty());
  if (allowNodeColumnList) {
    // Node COPY/FILL expects the first column to be primary key.
    // FILL (allowRelColumnList=true): respect the provided column list so that
    // users can fill a subset of properties by providing (pk, cols...).
    auto& pkDefinition = nodeTableEntry->getPrimaryKeyDefinition();
    columnNames.push_back(pkDefinition.getName());
    columnTypes.push_back(pkDefinition.getType().copy());
  }
  bindExpectedColumns(nodeTableEntry, info, columnNames, columnTypes);
}

void bindExpectedRelColumns(const RelTableCatalogEntry* relTableEntry,
                            const CopyFromColumnInfo& info,
                            std::vector<std::string>& columnNames,
                            std::vector<LogicalType>& columnTypes,
                            const main::ClientContext* context,
                            bool allowRelColumnList) {
  NEUG_ASSERT(columnNames.empty() && columnTypes.empty());
  auto catalog = context->getCatalog();
  auto transaction = context->getTransaction();
  auto srcTable =
      catalog->getTableCatalogEntry(transaction, relTableEntry->getSrcTableID())
          ->ptrCast<NodeTableCatalogEntry>();
  auto dstTable =
      catalog->getTableCatalogEntry(transaction, relTableEntry->getDstTableID())
          ->ptrCast<NodeTableCatalogEntry>();
  columnNames.push_back("from");
  columnNames.push_back("to");
  auto srcPKColumnType = srcTable->getPrimaryKeyDefinition().getType().copy();
  if (srcPKColumnType.getLogicalTypeID() == LogicalTypeID::SERIAL) {
    srcPKColumnType = LogicalType::INT64();
  }
  auto dstPKColumnType = dstTable->getPrimaryKeyDefinition().getType().copy();
  if (dstPKColumnType.getLogicalTypeID() == LogicalTypeID::SERIAL) {
    dstPKColumnType = LogicalType::INT64();
  }
  columnTypes.push_back(std::move(srcPKColumnType));
  columnTypes.push_back(std::move(dstPKColumnType));
  // Relationship COPY/FILL expects the first two columns to be src/dst keys.
  // - COPY: keep the legacy behavior: bind all relationship properties after
  //   (from, to), ignoring any column list.
  // - FILL (allowRelColumnList=true): respect the provided column list so that
  //   users can fill a subset of properties by providing (from, to, cols...).
  if (allowRelColumnList) {
    bindExpectedColumns(relTableEntry, info, columnNames, columnTypes);
  } else {
    CopyFromColumnInfo infoNoCols = info;
    infoNoCols.columnNames.clear();
    bindExpectedColumns(relTableEntry, infoNoCols, columnNames, columnTypes);
  }
}

}  // namespace binder
}  // namespace neug

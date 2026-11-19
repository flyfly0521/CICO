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

#include "neug/compiler/catalog/catalog.h"

#include "neug/compiler/binder/ddl/bound_create_sequence_info.h"
#include "neug/compiler/binder/ddl/bound_create_table_info.h"
#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/scalar_macro_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/sequence_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/type_catalog_entry.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/serializer/deserializer.h"
#include "neug/compiler/common/serializer/buffered_reader.h"
#include "neug/compiler/common/serializer/buffered_serializer.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/compiler/common/string_format.h"
#include "neug/compiler/gopt/g_constants.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/compiler/extension/extension_api.h"
#include "neug/compiler/extension/extension_manager.h"
#include "neug/compiler/function/function_collection.h"
#include "neug/compiler/function/scalar_function.h"
#include "neug/compiler/main/option_config.h"
#include "neug/compiler/transaction/transaction.h"
#include "neug/utils/file_utils.h"
#include "neug/utils/exception/exception.h"

#include <filesystem>

using namespace neug::binder;
using namespace neug::common;
using namespace neug::storage;
using namespace neug::transaction;

namespace neug {
namespace catalog {

Catalog::Catalog() : version{0} { initCatalogSets(); }

Catalog::Catalog(const std::string& directory, VirtualFileSystem* vfs)
    : version{0} {}


static std::string getCatalogFilePath(const std::string& directory,
                                      FileVersionType versionType) {
  const auto fileName = versionType == FileVersionType::ORIGINAL
                            ? StorageConstants::CATALOG_FILE_NAME
                            : StorageConstants::CATALOG_FILE_NAME_FOR_WAL;
  return (std::filesystem::path{directory} / fileName).string();
}

static std::string getCatalogIndexesFilePath(const std::string& directory,
                                             FileVersionType versionType) {
  // Keep this local to avoid changing global storage constants.
  static constexpr const char* kIndexesFileName = "catalog_indexes.kz";
  static constexpr const char* kIndexesFileNameForWal =
      "catalog_indexes.shadow";
  const auto fileName = versionType == FileVersionType::ORIGINAL
                            ? kIndexesFileName
                            : kIndexesFileNameForWal;
  return (std::filesystem::path{directory} / fileName).string();
}

// Sidecar for the transient FILL build state. Lives next to the index
// checkpoint so an interrupted build can be detected on reopen. The set is
// intentionally NOT part of the catalog file: BUILDING is transient execution
// state, not part of the index definition.
static std::string getBuildingIndexesFilePath(const std::string& directory) {
  return getCatalogIndexesFilePath(directory, FileVersionType::WAL_VERSION) +
         ".building";
}

static void writeBuildingIndexesFile(
    const std::string& directory,
    const std::unordered_set<std::string>& buildingIndexes) {
  if (directory.empty()) {
    return;
  }
  std::filesystem::create_directories(directory);
  auto bufferedWriter = std::make_shared<BufferedSerializer>();
  Serializer serializer(bufferedWriter);
  std::vector<std::string> names(buildingIndexes.begin(),
                                 buildingIndexes.end());
  std::sort(names.begin(), names.end());
  serializer.serializeVector(names);
  auto binaryData = bufferedWriter->getData();
  const auto targetPath = getBuildingIndexesFilePath(directory);
  const auto tmpPath = targetPath + ".tmp";
  neug::write_file(tmpPath, binaryData.data.get(), sizeof(uint8_t),
                   binaryData.size);
  std::error_code ec;
  std::filesystem::rename(tmpPath, targetPath, ec);
  if (ec) {
    std::filesystem::remove(targetPath, ec);
    ec.clear();
    std::filesystem::rename(tmpPath, targetPath, ec);
  }
  if (ec) {
    THROW_IO_EXCEPTION(common::stringFormat(
        "Catalog::writeBuildingIndexesFile failed to rename {} -> {}: {}",
        tmpPath, targetPath, ec.message()));
  }
}

static std::unordered_set<std::string> readBuildingIndexesFile(
    const std::string& directory) {
  const auto inputPath = getBuildingIndexesFilePath(directory);
  if (!std::filesystem::exists(inputPath)) {
    return {};
  }
  const auto fileSize = std::filesystem::file_size(inputPath);
  if (fileSize == 0) {
    return {};
  }
  std::vector<uint8_t> buffer(fileSize);
  neug::read_file(inputPath, buffer.data(), sizeof(uint8_t), fileSize);
  auto reader = std::make_unique<BufferReader>(buffer.data(), buffer.size());
  Deserializer deserializer(std::move(reader));
  std::vector<std::string> names;
  deserializer.deserializeVector(names);
  return {names.begin(), names.end()};
}

void Catalog::initCatalogSets() {
  tables = std::make_unique<CatalogSet>();
  relGroups = std::make_unique<CatalogSet>();
  sequences = std::make_unique<CatalogSet>();
  functions = std::make_unique<CatalogSet>();
  types = std::make_unique<CatalogSet>();
  indexes = std::make_unique<CatalogSet>();
  extendedIndexes = std::make_unique<CatalogSet>();
  internalTables = std::make_unique<CatalogSet>(true /* isInternal */);
  internalSequences = std::make_unique<CatalogSet>(true /* isInternal */);
  internalFunctions = std::make_unique<CatalogSet>(true /* isInternal */);
}

bool Catalog::containsTable(const Transaction* transaction,
                            const std::string& tableName,
                            bool useInternal) const {
  if (tables->containsEntry(transaction, tableName)) {
    return true;
  }
  if (useInternal) {
    return internalTables->containsEntry(transaction, tableName);
  }
  return false;
}

bool Catalog::containsTable(const Transaction* transaction, table_id_t tableID,
                            bool useInternal) const {
  if (tables->getEntryOfOID(transaction, tableID) != nullptr) {
    return true;
  }
  if (useInternal) {
    return internalTables->getEntryOfOID(transaction, tableID) != nullptr;
  }
  return false;
}

TableCatalogEntry* Catalog::getTableCatalogEntry(const Transaction* transaction,
                                                 table_id_t tableID) const {
  auto result = tables->getEntryOfOID(transaction, tableID);
  if (result == nullptr) {
    result = internalTables->getEntryOfOID(transaction, tableID);
  }
  if (result == nullptr) {
    THROW_RUNTIME_ERROR(
        stringFormat("Cannot find table catalog entry with id {}.",
                     std::to_string(tableID)));
  }
  return result->ptrCast<TableCatalogEntry>();
}

TableCatalogEntry* Catalog::getTableCatalogEntry(const Transaction* transaction,
                                                 const std::string& tableName,
                                                 bool useInternal) const {
  CatalogEntry* result = nullptr;
  if (!tables->containsEntry(transaction, tableName)) {
    if (!useInternal) {
      THROW_CATALOG_EXCEPTION(
          stringFormat("{} does not exist in catalog.", tableName));
    } else {
      result = internalTables->getEntry(transaction, tableName);
    }
  } else {
    result = tables->getEntry(transaction, tableName);
  }
  return result->ptrCast<TableCatalogEntry>();
}

std::vector<NodeTableCatalogEntry*> Catalog::getNodeTableEntries(
    const Transaction* transaction, bool useInternal) const {
  std::vector<NodeTableCatalogEntry*> result;
  for (auto& [_, entry] : tables->getEntries(transaction)) {
    if (entry->getType() != CatalogEntryType::NODE_TABLE_ENTRY) {
      continue;
    }
    result.push_back(entry->ptrCast<NodeTableCatalogEntry>());
  }
  if (useInternal) {
    for (auto& [_, entry] : internalTables->getEntries(transaction)) {
      if (entry->getType() != CatalogEntryType::NODE_TABLE_ENTRY) {
        continue;
      }
      result.push_back(entry->ptrCast<NodeTableCatalogEntry>());
    }
  }
  return result;
}

std::vector<RelTableCatalogEntry*> Catalog::getRelTableEntries(
    const Transaction* transaction, bool useInternal) const {
  std::vector<RelTableCatalogEntry*> result;
  for (auto& [_, entry] : tables->getEntries(transaction)) {
    if (entry->getType() != CatalogEntryType::REL_TABLE_ENTRY) {
      continue;
    }
    result.push_back(entry->ptrCast<RelTableCatalogEntry>());
  }
  if (useInternal) {
    for (auto& [_, entry] : internalTables->getEntries(transaction)) {
      if (entry->getType() != CatalogEntryType::REL_TABLE_ENTRY) {
        continue;
      }
      result.push_back(entry->ptrCast<RelTableCatalogEntry>());
    }
  }
  return result;
}

std::vector<TableCatalogEntry*> Catalog::getTableEntries(
    const Transaction* transaction, bool useInternal) const {
  std::vector<TableCatalogEntry*> result;
  for (auto& [_, entry] : tables->getEntries(transaction)) {
    result.push_back(entry->ptrCast<TableCatalogEntry>());
  }
  if (useInternal) {
    for (auto& [_, entry] : internalTables->getEntries(transaction)) {
      result.push_back(entry->ptrCast<RelTableCatalogEntry>());
    }
  }
  return result;
}

void Catalog::dropTableEntryAndIndex(Transaction* transaction,
                                     const std::string& name) {
  auto tableID = getTableCatalogEntry(transaction, name)->getTableID();
  dropAllIndexes(transaction, tableID);
  dropAllExtendedIndexes(transaction, tableID);
  dropTableEntry(transaction, tableID);
}

void Catalog::dropTableEntry(Transaction* transaction, table_id_t tableID) {
  dropTableEntry(transaction, getTableCatalogEntry(transaction, tableID));
}

void Catalog::dropTableEntry(Transaction* transaction,
                             const TableCatalogEntry* entry) {
  dropSerialSequence(transaction, entry);
  if (tables->containsEntry(transaction, entry->getName())) {
    tables->dropEntry(transaction, entry->getName(), entry->getOID());
  } else {
    internalTables->dropEntry(transaction, entry->getName(), entry->getOID());
  }
}

void Catalog::alterRelGroupEntry(Transaction* transaction,
                                 const BoundAlterInfo& info) {
  relGroups->alterRelGroupEntry(transaction, info);
}

void Catalog::alterTableEntry(Transaction* transaction,
                              const BoundAlterInfo& info) {
  tables->alterTableEntry(transaction, info);
}

bool Catalog::containsRelGroup(const Transaction* transaction,
                               const std::string& name) const {
  return relGroups->containsEntry(transaction, name);
}

RelGroupCatalogEntry* Catalog::getRelGroupEntry(const Transaction* transaction,
                                                const std::string& name) const {
  if (!containsRelGroup(transaction, name)) {
    THROW_RUNTIME_ERROR(stringFormat("Cannot find rel group entry {}.", name));
  }
  return relGroups->getEntry(transaction, name)
      ->ptrCast<RelGroupCatalogEntry>();
}

std::vector<RelGroupCatalogEntry*> Catalog::getRelGroupEntries(
    const Transaction* transaction) const {
  std::vector<RelGroupCatalogEntry*> result;
  for (auto& [_, entry] : relGroups->getEntries(transaction)) {
    result.push_back(entry->ptrCast<RelGroupCatalogEntry>());
  }
  return result;
}

void Catalog::dropRelGroupEntry(Transaction* transaction, oid_t id) {
  dropRelGroupEntry(transaction, relGroups->getEntryOfOID(transaction, id)
                                     ->ptrCast<RelGroupCatalogEntry>());
}

void Catalog::dropRelGroupEntry(Transaction* transaction,
                                const RelGroupCatalogEntry* entry) {
  for (auto& relTableID : entry->getRelTableIDs()) {
    dropTableEntry(transaction, relTableID);
  }
  relGroups->dropEntry(transaction, entry->getName(), entry->getOID());
}

CatalogEntry* Catalog::createRelGroupEntry(Transaction* transaction,
                                           const BoundCreateTableInfo& info) {
  const auto extraInfo =
      info.extraInfo->ptrCast<BoundExtraCreateRelTableGroupInfo>();
  std::vector<table_id_t> childrenTableIDs;
  for (auto& childInfo : extraInfo->infos) {
    NEUG_ASSERT(childInfo.hasParent);
    auto childEntry = createRelTableEntry(transaction, childInfo);
    childrenTableIDs.push_back(
        childEntry->ptrCast<TableCatalogEntry>()->getTableID());
  }
  return createRelGroupEntry(transaction, info.tableName,
                             std::move(childrenTableIDs));
}

CatalogEntry* Catalog::createRelGroupEntry(
    Transaction* transaction, const std::string& entryName,
    std::vector<table_id_t> childrenTableIDs) {
  auto entry = std::make_unique<RelGroupCatalogEntry>(
      entryName, std::move(childrenTableIDs));
  relGroups->createEntry(transaction, std::move(entry));
  return relGroups->getEntry(transaction, entryName);
}

bool Catalog::containsSequence(const Transaction* transaction,
                               const std::string& name) const {
  return sequences->containsEntry(transaction, name);
}

SequenceCatalogEntry* Catalog::getSequenceEntry(const Transaction* transaction,
                                                const std::string& sequenceName,
                                                bool useInternalSeq) const {
  CatalogEntry* entry = nullptr;
  if (!sequences->containsEntry(transaction, sequenceName) && useInternalSeq) {
    entry = internalSequences->getEntry(transaction, sequenceName);
  } else {
    entry = sequences->getEntry(transaction, sequenceName);
  }
  NEUG_ASSERT(entry);
  return entry->ptrCast<SequenceCatalogEntry>();
}

SequenceCatalogEntry* Catalog::getSequenceEntry(
    const Transaction* transaction, sequence_id_t sequenceID) const {
  auto entry = internalSequences->getEntryOfOID(transaction, sequenceID);
  if (entry == nullptr) {
    entry = sequences->getEntryOfOID(transaction, sequenceID);
  }
  NEUG_ASSERT(entry);
  return entry->ptrCast<SequenceCatalogEntry>();
}

std::vector<SequenceCatalogEntry*> Catalog::getSequenceEntries(
    const Transaction* transaction) const {
  std::vector<SequenceCatalogEntry*> result;
  for (auto& [_, entry] : sequences->getEntries(transaction)) {
    result.push_back(entry->ptrCast<SequenceCatalogEntry>());
  }
  return result;
}

sequence_id_t Catalog::createSequence(Transaction* transaction,
                                      const BoundCreateSequenceInfo& info) {
  auto entry = std::make_unique<SequenceCatalogEntry>(info);
  entry->setHasParent(info.hasParent);
  if (info.isInternal) {
    return internalSequences->createEntry(transaction, std::move(entry));
  } else {
    return sequences->createEntry(transaction, std::move(entry));
  }
}

void Catalog::dropSequence(Transaction* transaction, const std::string& name) {
  const auto entry = getSequenceEntry(transaction, name);
  dropSequence(transaction, entry->getOID());
}

void Catalog::dropSequence(Transaction* transaction, sequence_id_t sequenceID) {
  const auto sequenceEntry = getSequenceEntry(transaction, sequenceID);
  CatalogSet* set = nullptr;
  set = sequences->containsEntry(transaction, sequenceEntry->getName())
            ? sequences.get()
            : internalSequences.get();
  set->dropEntry(transaction, sequenceEntry->getName(),
                 sequenceEntry->getOID());
}

void Catalog::createType(Transaction* transaction, std::string name,
                         LogicalType type) {
  if (types->containsEntry(transaction, name)) {
    return;
  }
  auto entry =
      std::make_unique<TypeCatalogEntry>(std::move(name), std::move(type));
  types->createEntry(transaction, std::move(entry));
}

static std::string getInstallExtensionMessage(std::string_view extensionName,
                                              std::string_view entryType) {
  return stringFormat(
      "This {} exists in the {} "
      "extension. You can install and load the "
      "extension by running 'INSTALL {}; LOAD EXTENSION {};'.",
      entryType, extensionName, extensionName, extensionName);
}

static std::string getTypeDoesNotExistMessage(std::string_view entryName) {
  std::string message = stringFormat(
      "{} is neither an internal type nor a user defined type.", entryName);
  return message;
}

LogicalType Catalog::getType(const Transaction* transaction,
                             const std::string& name) const {
  if (!types->containsEntry(transaction, name)) {
    THROW_CATALOG_EXCEPTION(getTypeDoesNotExistMessage(name));
  }
  return types->getEntry(transaction, name)
      ->constCast<TypeCatalogEntry>()
      .getLogicalType()
      .copy();
}

bool Catalog::containsType(const Transaction* transaction,
                           const std::string& typeName) const {
  return types->containsEntry(transaction, typeName);
}

static void checkpointCatalogIfPossible(const Catalog& catalog) {
  const auto& dir = catalog.getCatalogDirectory();
  // A31: failure to persist index metadata is an operation failure, not a
  // successful catalog change that may disappear on the next schema refresh.
  if (!dir.empty()) catalog.checkpointIndexes(dir, nullptr);
}

void Catalog::createIndex(
    Transaction* transaction,
    std::unique_ptr<IndexCatalogEntry> indexCatalogEntry) {
  indexes->createEntry(transaction, std::move(indexCatalogEntry));
}

IndexCatalogEntry* Catalog::getIndex(const Transaction* transaction,
                                     table_id_t tableID,
                                     const std::string& indexName) const {
  auto internalName =
      IndexCatalogEntry::getInternalIndexName(tableID, indexName);
  return indexes->getEntry(transaction, internalName)
      ->ptrCast<IndexCatalogEntry>();
}

std::vector<IndexCatalogEntry*> Catalog::getIndexEntries(
    const Transaction* transaction) const {
  std::vector<IndexCatalogEntry*> result;
  for (auto& [_, entry] : indexes->getEntries(transaction)) {
    result.push_back(entry->ptrCast<IndexCatalogEntry>());
  }
  return result;
}

std::vector<IndexCatalogEntry*> Catalog::getExtendedIndexEntries(
    const Transaction* transaction) const {
  std::vector<IndexCatalogEntry*> result;
  for (auto& [_, entry] : extendedIndexes->getEntries(transaction)) {
    result.push_back(entry->ptrCast<IndexCatalogEntry>());
  }
  return result;
}

IndexCatalogEntry* Catalog::getExtendedIndexEntry(const transaction::Transaction* transaction,
    const std::string& indexName) const {
    return extendedIndexes->getEntry(transaction, indexName)->ptrCast<IndexCatalogEntry>();
}

void Catalog::createExtendedIndex(transaction::Transaction* transaction,
    std::unique_ptr<CatalogEntry> indexCatalogEntry) {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
    NEUG_ASSERT(indexCatalogEntry->getType() == CatalogEntryType::INDEX_ENTRY);
  // A FILL index is created at bind time, before its source pipeline writes
  // any data. Persist the BUILDING marker first so an interrupted build is
  // dropped on reopen; the catalog entry checkpoint follows.
    const auto name = indexCatalogEntry->getName();
    const auto building = building_indexes_;
    const auto ready = runtime_ready_indexes_;
    bool inserted = false;
    try {
      markExtendedIndexBuilding(name);
      extendedIndexes->createEntry(transaction, std::move(indexCatalogEntry));
      inserted = true;
      checkpointCatalogIfPossible(*this);
    } catch (...) {
      if (inserted) {
        auto* entry = extendedIndexes->getEntry(transaction, name);
        extendedIndexes->dropEntry(transaction, name, entry->getOID());
      }
      building_indexes_ = building;
      runtime_ready_indexes_ = ready;
      try {
        checkpointCatalogIfPossible(*this);
        writeBuildingIndexesFile(catalogDirectory_, building_indexes_);
      } catch (const std::exception& error) {
        // Keep the on-disk BUILDING marker if restoring metadata also fails;
        // an uncertain catalog publication must never become durable READY.
        LOG(ERROR) << "Cannot persist restored creation metadata: "
                   << error.what();
      }
      throw;
    }
}

void Catalog::markExtendedIndexBuilding(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
  runtime_ready_indexes_.erase(name);
  if (building_indexes_.insert(name).second) {
    writeBuildingIndexesFile(catalogDirectory_, building_indexes_);
  }
}

void Catalog::markExtendedIndexReady(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
  if (!extendedIndexes->containsEntry(&neug::Constants::DEFAULT_TRANSACTION, name)) {
    runtime_ready_indexes_.erase(name);
    building_indexes_.erase(name);
    writeBuildingIndexesFile(catalogDirectory_, building_indexes_);
  } else if (building_indexes_.contains(name)) {
    runtime_ready_indexes_.insert(name);
  }
}

// Runtime readiness is separate from persisted readiness. Only publish after
// the graph checkpoint has successfully written its base and derived data.
void Catalog::publishCheckpointedIndexes() const {
  std::lock_guard<std::recursive_mutex> lock(index_lifecycle_mutex_);
  checkpointCatalogIfPossible(*this);
  auto pending = building_indexes_;
  for (const auto& name : runtime_ready_indexes_) pending.erase(name);
  writeBuildingIndexesFile(catalogDirectory_, pending);
  building_indexes_ = std::move(pending);
  runtime_ready_indexes_.clear();
}

bool Catalog::isExtendedIndexBuilding(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
  return building_indexes_.contains(name) && !runtime_ready_indexes_.contains(name);
}

void Catalog::dropBuildingExtendedIndexes(transaction::Transaction* transaction) {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
  if (building_indexes_.empty()) {
    return;
  }
  const auto interrupted = building_indexes_;
  for (const auto& name : interrupted) {
    if (runtime_ready_indexes_.contains(name)) continue;
    try {
      if (!extendedIndexes->containsEntry(transaction, name)) {
        // The sidecar was persisted before the entry checkpoint; nothing to
        // drop.
        building_indexes_.erase(name);
        continue;
      }
      const auto entry = extendedIndexes->getEntry(transaction, name);
      extendedIndexes->dropEntry(transaction, name, entry->getOID());
      building_indexes_.erase(name);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Failed to drop interrupted FILL index entry " << name
                   << ": " << e.what();
    }
  }
  writeBuildingIndexesFile(catalogDirectory_, building_indexes_);
  checkpointCatalogIfPossible(*this);
}

void Catalog::dropExtendedIndex(Transaction* transaction, table_id_t tableID,
                                const std::string& indexName) const {
  std::lock_guard<std::recursive_mutex> lifecycle_lock(index_lifecycle_mutex_);
  auto remove = [&](const std::string& name) {
    auto* entry = extendedIndexes->getEntry(transaction, name)
                      ->ptrCast<IndexCatalogEntry>();
    auto previous = entry->copy();
    const auto building = building_indexes_;
    const auto ready = runtime_ready_indexes_;
    extendedIndexes->dropEntry(transaction, name, entry->getOID());
    try {
      markExtendedIndexReady(name);
      checkpointCatalogIfPossible(*this);
    } catch (...) {
      extendedIndexes->createEntry(transaction, std::move(previous));
      building_indexes_ = building;
      runtime_ready_indexes_ = ready;
      // A failed atomic replacement normally leaves the old file in place.
      // Also attempt restoration if the error happened after rename.
      try {
        checkpointCatalogIfPossible(*this);
        writeBuildingIndexesFile(catalogDirectory_, building_indexes_);
      } catch (const std::exception& error) {
        LOG(ERROR) << "Cannot persist restored index metadata: "
                   << error.what();
      }
      throw;
    }
  };
  if (extendedIndexes->containsEntry(transaction, indexName)) {
    auto* entry = extendedIndexes->getEntry(transaction, indexName)
                      ->ptrCast<IndexCatalogEntry>();
    if (entry->getTableID() != tableID)
      THROW_INVALID_ARGUMENT_EXCEPTION("dropExtendedIndex: wrong target table");
    remove(indexName);
    return;
  }
  for (const auto& [name, entry] : extendedIndexes->getEntries(transaction)) {
    const auto& index = entry->constCast<IndexCatalogEntry>();
    if (index.getTableID() == tableID && index.getIndexName() == indexName) {
      remove(name);
      return;
    }
  }

  THROW_INVALID_ARGUMENT_EXCEPTION(
      "dropExtendedIndex: index not found: " + indexName);
}

bool Catalog::containsExtendedIndex(const transaction::Transaction* transaction,
    const std::string& indexName) const {
    return extendedIndexes->containsEntry(transaction, indexName);
}

bool Catalog::containsIndex(const Transaction* transaction, table_id_t tableID,
                            const std::string& indexName) const {
  return indexes->containsEntry(
      transaction, IndexCatalogEntry::getInternalIndexName(tableID, indexName));
}

void Catalog::dropAllIndexes(Transaction* transaction, table_id_t tableID) {
  bool dropped = false;
  for (auto catalogEntry : indexes->getEntries(transaction)) {
    auto& indexCatalogEntry =
        catalogEntry.second->constCast<IndexCatalogEntry>();
    if (indexCatalogEntry.getTableID() == tableID) {
      indexes->dropEntry(transaction, catalogEntry.first,
                         catalogEntry.second->getOID());
      dropped = true;
    }
  }
  if (dropped) {
    checkpointCatalogIfPossible(*this);
  }
}

void Catalog::dropAllExtendedIndexes(Transaction* transaction,
                                     table_id_t tableID) {
  bool dropped = false;
  for (auto catalogEntry : extendedIndexes->getEntries(transaction)) {
    auto& indexCatalogEntry =
        catalogEntry.second->constCast<IndexCatalogEntry>();
    if (indexCatalogEntry.getTableID() == tableID) {
      extendedIndexes->dropEntry(transaction, catalogEntry.first,
                                 catalogEntry.second->getOID());
      dropped = true;
    }
  }
  if (dropped) {
    checkpointCatalogIfPossible(*this);
  }
}

void Catalog::dropIndex(Transaction* transaction, table_id_t tableID,
                        const std::string& indexName) const {
  auto uniqueName = IndexCatalogEntry::getInternalIndexName(tableID, indexName);
  const auto entry = indexes->getEntry(transaction, uniqueName);
  indexes->dropEntry(transaction, uniqueName, entry->getOID());
  checkpointCatalogIfPossible(*this);
}

bool Catalog::containsFunction(const Transaction* transaction,
                               const std::string& name,
                               bool useInternal) const {
  auto hasEntry = functions->containsEntry(transaction, name);
  if (!hasEntry && useInternal) {
    return internalFunctions->containsEntry(transaction, name);
  }
  return hasEntry;
}

void Catalog::addFunction(Transaction* transaction, CatalogEntryType entryType,
                          std::string name, function::function_set functionSet,
                          bool isInternal) {
  auto& catalogSet = isInternal ? internalFunctions : functions;
  if (catalogSet->containsEntry(transaction, name)) {
    THROW_CATALOG_EXCEPTION(stringFormat("function {} already exists.", name));
  }
  catalogSet->createEntry(
      transaction, std::make_unique<FunctionCatalogEntry>(
                       entryType, std::move(name), std::move(functionSet)));
}

static std::string getFunctionDoesNotExistMessage(std::string_view entryName) {
  std::string message = stringFormat("function {} does not exist.", entryName);
  return message;
}

void Catalog::dropFunction(Transaction* transaction, const std::string& name) {
  if (!containsFunction(transaction, name)) {
    THROW_CATALOG_EXCEPTION(stringFormat("function {} doesn't exist.", name));
  }
  auto entry = getFunctionEntry(transaction, name);
  functions->dropEntry(transaction, name, entry->getOID());
}

CatalogEntry* Catalog::getFunctionEntry(const Transaction* transaction,
                                        const std::string& name,
                                        bool useInternal) const {
  CatalogEntry* result = nullptr;
  if (!functions->containsEntry(transaction, name)) {
    if (!useInternal) {
      THROW_CATALOG_EXCEPTION(getFunctionDoesNotExistMessage(name));
    }
    result = internalFunctions->getEntry(transaction, name);
  } else {
    result = functions->getEntry(transaction, name);
  }
  return result;
}

std::vector<FunctionCatalogEntry*> Catalog::getFunctionEntries(
    const Transaction* transaction) const {
  std::vector<FunctionCatalogEntry*> result;
  for (auto& [_, entry] : functions->getEntries(transaction)) {
    result.push_back(entry->ptrCast<FunctionCatalogEntry>());
  }
  return result;
}

bool Catalog::containsMacro(const Transaction* transaction,
                            const std::string& macroName) const {
  return functions->containsEntry(transaction, macroName);
}

function::ScalarMacroFunction* Catalog::getScalarMacroFunction(
    const Transaction* transaction, const std::string& name) const {
  return functions->getEntry(transaction, name)
      ->constCast<ScalarMacroCatalogEntry>()
      .getMacroFunction();
}

void Catalog::addScalarMacroFunction(
    Transaction* transaction, std::string name,
    std::unique_ptr<function::ScalarMacroFunction> macro) {
  auto entry = std::make_unique<ScalarMacroCatalogEntry>(std::move(name),
                                                         std::move(macro));
  functions->createEntry(transaction, std::move(entry));
}

std::vector<std::string> Catalog::getMacroNames(
    const Transaction* transaction) const {
  std::vector<std::string> macroNames;
  for (auto& [_, function] : functions->getEntries(transaction)) {
    if (function->getType() == CatalogEntryType::SCALAR_MACRO_ENTRY) {
      macroNames.push_back(function->getName());
    }
  }
  return macroNames;
}

void Catalog::checkpoint(const std::string& databasePath,
                         VirtualFileSystem* fs) const {
  NEUG_ASSERT(!databasePath.empty());
  saveToFile(databasePath, fs, FileVersionType::WAL_VERSION);
}

void Catalog::checkpointIndexes(const std::string& databasePath,
                                VirtualFileSystem* fs) const {
  std::lock_guard<std::recursive_mutex> lock(index_lifecycle_mutex_);
  (void)fs;
  if (databasePath.empty()) {
    return;
  }

  std::filesystem::create_directories(databasePath);

  auto bufferedWriter =
      std::make_shared<BufferedSerializer>( /* maximumSize */);
  Serializer serializer(bufferedWriter);

  serializer.writeDebuggingInfo("catalogIndexesVersion");
  serializer.serializeValue<uint64_t>(version);
  indexes->serialize(serializer);
  extendedIndexes->serialize(serializer);

  auto binaryData = bufferedWriter->getData();
  const auto targetPath =
      getCatalogIndexesFilePath(databasePath, FileVersionType::WAL_VERSION);
  const auto tmpPath = targetPath + ".tmp";
  neug::write_file(tmpPath, binaryData.data.get(), sizeof(uint8_t),
                   binaryData.size);

  std::error_code ec;
  std::filesystem::rename(tmpPath, targetPath, ec);
  if (ec) {
    std::filesystem::remove(targetPath, ec);
    ec.clear();
    std::filesystem::rename(tmpPath, targetPath, ec);
  }
  if (ec) {
    THROW_IO_EXCEPTION(common::stringFormat(
        "Catalog::checkpointIndexes failed to rename {} -> {}: {}", tmpPath,
        targetPath, ec.message()));
  }
}

void Catalog::saveToFile(const std::string& directory, VirtualFileSystem* fs,
                         FileVersionType versionType) const {
  (void) fs;

  std::filesystem::create_directories(directory);

  auto bufferedWriter =
      std::make_shared<BufferedSerializer>( /* maximumSize */);
  Serializer serializer(bufferedWriter);
  serialize(serializer);
  auto binaryData = bufferedWriter->getData();

  const auto targetPath = getCatalogFilePath(directory, versionType);
  const auto tmpPath = targetPath + ".tmp";
  neug::write_file(tmpPath, binaryData.data.get(), sizeof(uint8_t),
                   binaryData.size);

  std::error_code ec;
  std::filesystem::rename(tmpPath, targetPath, ec);
  if (ec) {
    // Try replacing existing file if needed.
    std::filesystem::remove(targetPath, ec);
    ec.clear();
    std::filesystem::rename(tmpPath, targetPath, ec);
  }
  if (ec) {
    THROW_IO_EXCEPTION(common::stringFormat(
        "Catalog::saveToFile failed to rename {} -> {}: {}", tmpPath,
        targetPath, ec.message()));
  }
}

void Catalog::readFromFile(const std::string& directory, VirtualFileSystem* fs,
                           FileVersionType versionType,
                           main::ClientContext* context) {
  (void) fs;
  (void) context;
  if (directory.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Catalog::readFromFile: directory is empty");
  }

  const auto originalPath = getCatalogFilePath(directory, FileVersionType::ORIGINAL);
  const auto shadowPath = getCatalogFilePath(directory, FileVersionType::WAL_VERSION);
  std::filesystem::path inputPath;
  if (versionType == FileVersionType::WAL_VERSION) {
    inputPath = std::filesystem::path{shadowPath};
  } else {
    // Prefer shadow if it exists (crash-safe shadowing).
    inputPath = std::filesystem::exists(shadowPath) ? std::filesystem::path{shadowPath}
                                                    : std::filesystem::path{originalPath};
  }

  if (!std::filesystem::exists(inputPath)) {
    // No persisted catalog yet.
    return;
  }

  const auto fileSize = std::filesystem::file_size(inputPath);
  if (fileSize == 0) {
    return;
  }
  std::vector<uint8_t> buffer(fileSize);
  neug::read_file(inputPath.string(), buffer.data(), sizeof(uint8_t), fileSize);

  auto reader = std::make_unique<BufferReader>(buffer.data(), buffer.size());
  Deserializer deserializer(std::move(reader));
  deserialize(deserializer);
}

void Catalog::restoreIndexesFromFile(const std::string& directory,
                                     VirtualFileSystem* fs,
                                     FileVersionType versionType) {
  std::lock_guard<std::recursive_mutex> lock(index_lifecycle_mutex_);
  (void)fs;
  if (directory.empty()) {
    return;
  }

  // Prefer indexes-only file (cheap checkpoint after index DDL). Fallback to
  // full catalog file for backward compatibility.
  {
    const auto originalPath =
        getCatalogIndexesFilePath(directory, FileVersionType::ORIGINAL);
    const auto shadowPath =
        getCatalogIndexesFilePath(directory, FileVersionType::WAL_VERSION);
    std::filesystem::path inputPath;
    if (versionType == FileVersionType::WAL_VERSION) {
      inputPath = std::filesystem::path{shadowPath};
    } else {
      inputPath = std::filesystem::exists(shadowPath)
                      ? std::filesystem::path{shadowPath}
                      : std::filesystem::path{originalPath};
    }

    if (std::filesystem::exists(inputPath)) {
      const auto fileSize = std::filesystem::file_size(inputPath);
      if (fileSize != 0) {
        std::vector<uint8_t> buffer(fileSize);
        neug::read_file(inputPath.string(), buffer.data(), sizeof(uint8_t),
                        fileSize);

        auto reader =
            std::make_unique<BufferReader>(buffer.data(), buffer.size());
        Deserializer deserializer(std::move(reader));

        std::string debuggingInfo;
        deserializer.validateDebuggingInfo(debuggingInfo,
                                           "catalogIndexesVersion");
        uint64_t persistedVersion = 0;
        deserializer.deserializeValue<uint64_t>(persistedVersion);
        auto persistedIndexes = CatalogSet::deserialize(deserializer);
        auto persistedExtendedIndexes = CatalogSet::deserialize(deserializer);

        auto* tx = &neug::Constants::DEFAULT_TRANSACTION;
        {
          std::vector<std::pair<std::string, common::oid_t>> toDrop;
          for (auto& [name, entry] : persistedIndexes->getEntries(tx)) {
            auto& idxEntry = entry->constCast<IndexCatalogEntry>();
            if (!containsTable(tx, idxEntry.getTableID(),
                               true /*useInternal*/)) {
              toDrop.emplace_back(name, entry->getOID());
            }
          }
          for (auto& [name, oid] : toDrop) {
            persistedIndexes->dropEntry(tx, name, oid);
          }
        }
        {
          std::vector<std::pair<std::string, common::oid_t>> toDrop;
          for (auto& [name, entry] : persistedExtendedIndexes->getEntries(tx)) {
            auto& idxEntry = entry->constCast<IndexCatalogEntry>();
            if (!containsTable(tx, idxEntry.getTableID(),
                               true /*useInternal*/)) {
              toDrop.emplace_back(name, entry->getOID());
            }
          }
          for (auto& [name, oid] : toDrop) {
            persistedExtendedIndexes->dropEntry(tx, name, oid);
          }
        }

        version = persistedVersion;
        indexes = std::move(persistedIndexes);
        extendedIndexes = std::move(persistedExtendedIndexes);
        building_indexes_ = readBuildingIndexesFile(directory);
        dropBuildingExtendedIndexes(tx);
        return;
      }
    }
  }

  const auto originalPath =
      getCatalogFilePath(directory, FileVersionType::ORIGINAL);
  const auto shadowPath =
      getCatalogFilePath(directory, FileVersionType::WAL_VERSION);
  std::filesystem::path inputPath;
  if (versionType == FileVersionType::WAL_VERSION) {
    inputPath = std::filesystem::path{shadowPath};
  } else {
    inputPath = std::filesystem::exists(shadowPath)
                    ? std::filesystem::path{shadowPath}
                    : std::filesystem::path{originalPath};
  }

  if (!std::filesystem::exists(inputPath)) {
    return;
  }
  const auto fileSize = std::filesystem::file_size(inputPath);
  if (fileSize == 0) {
    return;
  }

  std::vector<uint8_t> buffer(fileSize);
  neug::read_file(inputPath.string(), buffer.data(), sizeof(uint8_t), fileSize);

  auto reader = std::make_unique<BufferReader>(buffer.data(), buffer.size());
  Deserializer deserializer(std::move(reader));

  Catalog persisted;
  persisted.deserialize(deserializer);

  // Best-effort sanity check: drop indexes that reference missing tableIDs in
  // the current schema (schema may come from YAML and differs from persisted).
  auto* tx = &neug::Constants::DEFAULT_TRANSACTION;
  {
    std::vector<std::pair<std::string, common::oid_t>> toDrop;
    for (auto& [name, entry] : persisted.indexes->getEntries(tx)) {
      auto& idxEntry = entry->constCast<IndexCatalogEntry>();
      if (!containsTable(tx, idxEntry.getTableID(), true /*useInternal*/)) {
        toDrop.emplace_back(name, entry->getOID());
      }
    }
    for (auto& [name, oid] : toDrop) {
      persisted.indexes->dropEntry(tx, name, oid);
    }
  }
  {
    std::vector<std::pair<std::string, common::oid_t>> toDrop;
    for (auto& [name, entry] : persisted.extendedIndexes->getEntries(tx)) {
      auto& idxEntry = entry->constCast<IndexCatalogEntry>();
      if (!containsTable(tx, idxEntry.getTableID(), true /*useInternal*/)) {
        toDrop.emplace_back(name, entry->getOID());
      }
    }
    for (auto& [name, oid] : toDrop) {
      persisted.extendedIndexes->dropEntry(tx, name, oid);
    }
  }

  indexes = std::move(persisted.indexes);
  extendedIndexes = std::move(persisted.extendedIndexes);
  building_indexes_ = readBuildingIndexesFile(directory);
  dropBuildingExtendedIndexes(&neug::Constants::DEFAULT_TRANSACTION);
}

CatalogEntry* Catalog::createTableEntry(Transaction* transaction,
                                        const BoundCreateTableInfo& info) {
  switch (info.type) {
  case CatalogEntryType::NODE_TABLE_ENTRY: {
    return createNodeTableEntry(transaction, info);
  }
  case CatalogEntryType::REL_TABLE_ENTRY: {
    return createRelTableEntry(transaction, info);
  }
  default:
    NEUG_UNREACHABLE;
  }
}

CatalogEntry* Catalog::createNodeTableEntry(Transaction* transaction,
                                            const BoundCreateTableInfo& info) {
  const auto extraInfo =
      info.extraInfo->constPtrCast<BoundExtraCreateNodeTableInfo>();
  auto entry = std::make_unique<NodeTableCatalogEntry>(
      info.tableName, extraInfo->primaryKeyName);
  for (auto& definition : extraInfo->propertyDefinitions) {
    entry->addProperty(definition);
  }
  entry->setHasParent(info.hasParent);
  createSerialSequence(transaction, entry.get(), info.isInternal);
  auto catalogSet = info.isInternal ? internalTables.get() : tables.get();
  catalogSet->createEntry(transaction, std::move(entry));
  return catalogSet->getEntry(transaction, info.tableName);
}

CatalogEntry* Catalog::createRelTableEntry(Transaction* transaction,
                                           const BoundCreateTableInfo& info) {
  const auto extraInfo =
      info.extraInfo.get()->constPtrCast<BoundExtraCreateRelTableInfo>();
  auto entry = std::make_unique<RelTableCatalogEntry>(
      info.tableName, extraInfo->srcMultiplicity, extraInfo->dstMultiplicity,
      extraInfo->srcTableID, extraInfo->dstTableID,
      extraInfo->storageDirection);
  for (auto& definition : extraInfo->propertyDefinitions) {
    entry->addProperty(definition);
  }
  entry->setHasParent(info.hasParent);
  createSerialSequence(transaction, entry.get(), info.isInternal);
  auto catalogSet = info.isInternal ? internalTables.get() : tables.get();
  catalogSet->createEntry(transaction, std::move(entry));
  return catalogSet->getEntry(transaction, info.tableName);
}

void Catalog::createSerialSequence(Transaction* transaction,
                                   const TableCatalogEntry* entry,
                                   bool isInternal) {
  for (auto& definition : entry->getProperties()) {
    if (definition.getType().getLogicalTypeID() != LogicalTypeID::SERIAL) {
      continue;
    }
    const auto seqName = SequenceCatalogEntry::getSerialName(
        entry->getName(), definition.getName());
    auto seqInfo = BoundCreateSequenceInfo(
        seqName, 0, 1, 0, std::numeric_limits<int64_t>::max(), false,
        ConflictAction::ON_CONFLICT_THROW, isInternal);
    seqInfo.hasParent = true;
    createSequence(transaction, seqInfo);
  }
}

void Catalog::dropSerialSequence(Transaction* transaction,
                                 const TableCatalogEntry* entry) {
  for (auto& definition : entry->getProperties()) {
    if (definition.getType().getLogicalTypeID() != LogicalTypeID::SERIAL) {
      continue;
    }
    auto seqName = SequenceCatalogEntry::getSerialName(entry->getName(),
                                                       definition.getName());
    dropSequence(transaction, seqName);
  }
}


void Catalog::serialize(Serializer& ser) const {
  ser.writeDebuggingInfo("catalogVersion");
  ser.serializeValue<uint64_t>(version);
  tables->serialize(ser);
  relGroups->serialize(ser);
  sequences->serialize(ser);
  functions->serialize(ser);
  types->serialize(ser);
  indexes->serialize(ser);
  extendedIndexes->serialize(ser);
  internalTables->serialize(ser);
  internalSequences->serialize(ser);
  internalFunctions->serialize(ser);
}

void Catalog::deserialize(Deserializer& deSer) {
  std::string debuggingInfo;
  deSer.validateDebuggingInfo(debuggingInfo, "catalogVersion");
  deSer.deserializeValue<uint64_t>(version);
  tables = CatalogSet::deserialize(deSer);
  relGroups = CatalogSet::deserialize(deSer);
  sequences = CatalogSet::deserialize(deSer);
  functions = CatalogSet::deserialize(deSer);
  types = CatalogSet::deserialize(deSer);
  indexes = CatalogSet::deserialize(deSer);
  extendedIndexes = CatalogSet::deserialize(deSer);
  internalTables = CatalogSet::deserialize(deSer);
  internalSequences = CatalogSet::deserialize(deSer);
  internalFunctions = CatalogSet::deserialize(deSer);
}

}  // namespace catalog
}  // namespace neug

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

#pragma once
#include <mutex>

#include "neug/compiler/catalog/catalog_entry/function_catalog_entry.h"
#include "neug/compiler/catalog/catalog_set.h"
#include "neug/compiler/common/cast.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/compiler/function/function.h"
#include "neug/compiler/gopt/g_constants.h"

namespace neug::main {
struct DBConfig;
}  // namespace neug::main

namespace neug {
namespace main {
class AttachedKuzuDatabase;
}  // namespace main

namespace binder {
struct BoundAlterInfo;
struct BoundCreateTableInfo;
struct BoundCreateSequenceInfo;
}  // namespace binder

namespace common {
class VirtualFileSystem;
}  // namespace common

namespace function {
struct ScalarMacroFunction;
}  // namespace function

namespace storage {
class WAL;
}  // namespace storage

namespace transaction {
class Transaction;
}  // namespace transaction

namespace extension {
class ExtensionAPI;
}

namespace catalog {
class TableCatalogEntry;
class NodeTableCatalogEntry;
class RelTableCatalogEntry;
class RelGroupCatalogEntry;
class FunctionCatalogEntry;
class SequenceCatalogEntry;
class IndexCatalogEntry;

class NEUG_API Catalog {
  friend class main::AttachedKuzuDatabase;
  friend class neug::extension::ExtensionAPI;

 public:
  // This is extended by DuckCatalog and PostgresCatalog.
  Catalog();
  Catalog(const std::string& directory, common::VirtualFileSystem* vfs);
  virtual ~Catalog() = default;

  // Directory used to persist this catalog's index metadata (checkpoints).
  // Must be per-instance: multiple NeugDB instances can be open in one
  // process, and index checkpointing must never resolve the directory via the
  // process-global MetadataRegistry (which points at the most recently opened
  // DB).  See checkpointCatalogIfPossible.
  void setCatalogDirectory(std::string directory) {
    catalogDirectory_ = std::move(directory);
  }
  const std::string& getCatalogDirectory() const { return catalogDirectory_; }

  // ----------------------------- Tables ----------------------------

  // Check if table entry exists.
  bool containsTable(const transaction::Transaction* transaction,
                     const std::string& tableName,
                     bool useInternal = true) const;
  bool containsTable(const transaction::Transaction* transaction,
                     common::table_id_t tableID, bool useInternal = true) const;
  // Get table entry with name.
  TableCatalogEntry* getTableCatalogEntry(
      const transaction::Transaction* transaction, const std::string& tableName,
      bool useInternal = true) const;
  // Get table entry with id.
  TableCatalogEntry* getTableCatalogEntry(
      const transaction::Transaction* transaction,
      common::table_id_t tableID) const;
  // Get all node table entries.
  std::vector<NodeTableCatalogEntry*> getNodeTableEntries(
      const transaction::Transaction* transaction,
      bool useInternal = true) const;
  // Get all rel table entries.
  std::vector<RelTableCatalogEntry*> getRelTableEntries(
      const transaction::Transaction* transaction,
      bool useInternal = true) const;
  // Get all table entries.
  std::vector<TableCatalogEntry*> getTableEntries(
      const transaction::Transaction* transaction,
      bool useInternal = true) const;

  // Create table catalog entry.
  CatalogEntry* createTableEntry(transaction::Transaction* transaction,
                                 const binder::BoundCreateTableInfo& info);
  // Drop table entry and all indices within the table.
  void dropTableEntryAndIndex(transaction::Transaction* transaction,
                              const std::string& name);
  // Drop table entry with id.
  void dropTableEntry(transaction::Transaction* transaction,
                      common::table_id_t tableID);
  // Drop table entry.
  void dropTableEntry(transaction::Transaction* transaction,
                      const TableCatalogEntry* entry);
  // Alter table entry.
  void alterTableEntry(transaction::Transaction* transaction,
                       const binder::BoundAlterInfo& info);
  // Alter a rel group entry
  // alterTableEntry() still needs to be called separately for each member of
  // the group
  void alterRelGroupEntry(transaction::Transaction* transaction,
                          const binder::BoundAlterInfo& info);

  // ----------------------------- Rel groups ----------------------------

  // Check if rel group entry exists.
  bool containsRelGroup(const transaction::Transaction* transaction,
                        const std::string& name) const;
  // Get rel group entry with name.
  RelGroupCatalogEntry* getRelGroupEntry(
      const transaction::Transaction* transaction,
      const std::string& name) const;
  // Get all rel group entries.
  std::vector<RelGroupCatalogEntry*> getRelGroupEntries(
      const transaction::Transaction* transaction) const;

  // Create rel group entry and its children rel entries.
  CatalogEntry* createRelGroupEntry(transaction::Transaction* transaction,
                                    const binder::BoundCreateTableInfo& info);
  // Create rel group entry
  CatalogEntry* createRelGroupEntry(
      transaction::Transaction* transaction, const std::string& entryName,
      std::vector<common::table_id_t> childrenTableIDs);
  // Drop rel group entry.
  void dropRelGroupEntry(transaction::Transaction* transaction,
                         common::oid_t id);
  // Drop rel group entry.
  void dropRelGroupEntry(transaction::Transaction* transaction,
                         const RelGroupCatalogEntry* entry);

  // ----------------------------- Sequences ----------------------------

  // Check if sequence entry exists.
  bool containsSequence(const transaction::Transaction* transaction,
                        const std::string& name) const;
  // Get sequence entry with name.
  SequenceCatalogEntry* getSequenceEntry(
      const transaction::Transaction* transaction,
      const std::string& sequenceName, bool useInternalSeq = true) const;
  // Get sequence entry with id.
  SequenceCatalogEntry* getSequenceEntry(
      const transaction::Transaction* transaction,
      common::sequence_id_t sequenceID) const;
  // Get all sequence entries.
  std::vector<SequenceCatalogEntry*> getSequenceEntries(
      const transaction::Transaction* transaction) const;

  // Create sequence entry.
  common::sequence_id_t createSequence(
      transaction::Transaction* transaction,
      const binder::BoundCreateSequenceInfo& info);
  // Drop sequence entry with name.
  void dropSequence(transaction::Transaction* transaction,
                    const std::string& name);
  // Drop sequence entry with id.
  void dropSequence(transaction::Transaction* transaction,
                    common::sequence_id_t sequenceID);

  // ----------------------------- Types ----------------------------

  // Check if type entry exists.
  bool containsType(const transaction::Transaction* transaction,
                    const std::string& name) const;
  // Get type entry with name.
  common::LogicalType getType(const transaction::Transaction*,
                              const std::string& name) const;

  // Create type entry.
  void createType(transaction::Transaction* transaction, std::string name,
                  common::LogicalType type);

  // ----------------------------- Indexes ----------------------------

  // Check if index entry exists.
  bool containsIndex(const transaction::Transaction* transaction,
                     common::table_id_t tableID,
                     const std::string& indexName) const;
  // Get index entry with name.
  IndexCatalogEntry* getIndex(const transaction::Transaction* transaction,
                              common::table_id_t tableID,
                              const std::string& indexName) const;
  // Get all index entries.
  std::vector<IndexCatalogEntry*> getIndexEntries(
      const transaction::Transaction* transaction) const;
  std::vector<IndexCatalogEntry*> getExtendedIndexEntries(
      const transaction::Transaction* transaction) const;
  bool containsExtendedIndex(const transaction::Transaction* transaction,
                             const std::string& indexName) const;
  void createExtendedIndex(transaction::Transaction* transaction,
                           std::unique_ptr<CatalogEntry> indexCatalogEntry);
  IndexCatalogEntry* getExtendedIndexEntry(
      const transaction::Transaction* transaction,
      const std::string& indexName) const;

  // --- Transient FILL build lifecycle ---
  // A FILL index is created at bind time (BUILDING) and becomes usable
  // (READY) only after its source pipeline has written the complete
  // materialization. The building set is persisted in a sidecar file next to
  // the catalog index checkpoint so an interrupted build is dropped on reopen
  // instead of being registered for incremental maintenance or matched by the
  // rewriter. Legacy catalogs (no sidecar) treat every entry as READY.
  void markExtendedIndexBuilding(const std::string& name) const;
  void markExtendedIndexReady(const std::string& name) const;
  void publishCheckpointedIndexes() const;
  bool isExtendedIndexBuilding(const std::string& name) const;
  // Drop every index still in BUILDING state (incomplete materialization left
  // by a crashed or interrupted FILL) and clear the persisted set.
  void dropBuildingExtendedIndexes(transaction::Transaction* transaction);

    // Drop an extended index. `indexName` here is the logical/user-facing index
    // name (i.e., `IndexCatalogEntry::getIndexName()`), not necessarily the
    // internal catalog entry name.
    void dropExtendedIndex(transaction::Transaction* transaction,
                                                 common::table_id_t tableID,
                                                 const std::string& indexName) const;

  // Create index entry.
  void createIndex(transaction::Transaction* transaction,
                   std::unique_ptr<IndexCatalogEntry> indexCatalogEntry);
  // Drop all index entries within a table.
  void dropAllIndexes(transaction::Transaction* transaction,
                      common::table_id_t tableID);
  void dropAllExtendedIndexes(transaction::Transaction* transaction,
                              common::table_id_t tableID);
  // Drop index entry with name.
  void dropIndex(transaction::Transaction* transaction,
                 common::table_id_t tableID,
                 const std::string& indexName) const;

  // ----------------------------- Functions ----------------------------

  // Check if function exists.
  bool containsFunction(const transaction::Transaction* transaction,
                        const std::string& name,
                        bool useInternal = false) const;
  // Get function entry by name.
  // Note we cannot cast to FunctionEntry here because result could also be a
  // MacroEntry.
  CatalogEntry* getFunctionEntry(const transaction::Transaction* transaction,
                                 const std::string& name,
                                 bool useInternal = false) const;
  // Get all function entries.
  std::vector<FunctionCatalogEntry*> getFunctionEntries(
      const transaction::Transaction* transaction) const;

  // Add function with name.
  void addFunction(transaction::Transaction* transaction,
                   CatalogEntryType entryType, std::string name,
                   function::function_set functionSet, bool isInternal = false);

  // Drop function with name.
  void dropFunction(transaction::Transaction* transaction,
                    const std::string& name);

  // ----------------------------- Macro ----------------------------

  // Check if macro entry exists.
  bool containsMacro(const transaction::Transaction* transaction,
                     const std::string& macroName) const;
  void addScalarMacroFunction(
      transaction::Transaction* transaction, std::string name,
      std::unique_ptr<function::ScalarMacroFunction> macro);
  function::ScalarMacroFunction* getScalarMacroFunction(
      const transaction::Transaction* transaction,
      const std::string& name) const;
  std::vector<std::string> getMacroNames(
      const transaction::Transaction* transaction) const;

  void incrementVersion() { version++; }
  uint64_t getVersion() const { return version; }
  void checkpoint(const std::string& databasePath,
                  common::VirtualFileSystem* fs) const;

  // Persist only index-related metadata (indexes + extendedIndexes).
  // Intended to be cheap enough to call after index DDL.
  void checkpointIndexes(const std::string& databasePath,
                         common::VirtualFileSystem* fs) const;

  // Restore only index-related metadata from the persisted catalog file, while
  // keeping the in-memory schema (tables/rel groups/types/etc.) intact.
  // This is intended for "merge" recovery where schema is provided externally
  // (e.g., YAML) but indexes should survive restarts.
  void restoreIndexesFromFile(
      const std::string& directory, common::VirtualFileSystem* fs,
      common::FileVersionType versionType = common::FileVersionType::ORIGINAL);

  template <class TARGET>
  TARGET* ptrCast() {
    return common::neug_dynamic_cast<TARGET*>(this);
  }

 private:
  // The clientContext needs to be used when reading from a remote filesystem
  // which requires some user-specific configs (e.g. s3 username, password).
  void readFromFile(const std::string& directory, common::VirtualFileSystem* fs,
                    common::FileVersionType versionType,
                    main::ClientContext* context = nullptr);
  void saveToFile(const std::string& directory, common::VirtualFileSystem* fs,
                  common::FileVersionType versionType) const;

 private:
  void initCatalogSets();

  CatalogEntry* createNodeTableEntry(transaction::Transaction* transaction,
                                     const binder::BoundCreateTableInfo& info);
  CatalogEntry* createRelTableEntry(transaction::Transaction* transaction,
                                    const binder::BoundCreateTableInfo& info);

  void createSerialSequence(transaction::Transaction* transaction,
                            const TableCatalogEntry* entry, bool isInternal);
  void dropSerialSequence(transaction::Transaction* transaction,
                          const TableCatalogEntry* entry);
  void deserialize(common::Deserializer& deSer);
  void serialize(common::Serializer& ser) const;

 protected:
  std::unique_ptr<CatalogSet> tables;
  std::unique_ptr<CatalogSet> relGroups;

 private:
  // Own directory for index-metadata checkpoints (see setCatalogDirectory).
  std::string catalogDirectory_;
  std::unique_ptr<CatalogSet> sequences;
  std::unique_ptr<CatalogSet> functions;
  std::unique_ptr<CatalogSet> types;
  std::unique_ptr<CatalogSet> indexes;
  std::unique_ptr<CatalogSet> extendedIndexes;
  std::unique_ptr<CatalogSet> internalTables;
  std::unique_ptr<CatalogSet> internalSequences;
  std::unique_ptr<CatalogSet> internalFunctions;

  // Transient FILL build state: entry names whose materialization has not
  // completed. Loaded from the sidecar file on restore; never serialized
  // into the catalog file itself. Mutable so the const drop paths can clear
  // the marker of the entry they remove.
  mutable std::unordered_set<std::string> building_indexes_;
  mutable std::unordered_set<std::string> runtime_ready_indexes_;
  mutable std::recursive_mutex index_lifecycle_mutex_;

  uint64_t version;
};

}  // namespace catalog
}  // namespace neug

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

#include "neug/compiler/function/drop_index_function.h"

#include <glog/logging.h>

#include <optional>

#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/table_catalog_entry.h"
#include "neug/compiler/gopt/g_constants.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/execution/common/columns/value_columns.h"
#include "neug/execution/common/context.h"
#include "neug/transaction/fill_index_maintenance.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace function {

static std::string getConstStringArg(const ::procedure::Argument& arg,
                                     const std::string& funcName) {
  if (!arg.has_const_()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        funcName + ": only constant string arguments are supported.");
  }
  const auto& v = arg.const_();
  if (!v.has_str()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(funcName + ": expected STRING argument.");
  }
  return v.str();
}

function_set DropIndexFunction::getFunctionSet() {
  auto function = std::make_unique<NeugCallFunction>(
      DropIndexFunction::name,
      std::vector<neug::common::LogicalTypeID>{common::LogicalTypeID::STRING,
                                               common::LogicalTypeID::STRING},
      std::vector<std::pair<std::string, neug::common::LogicalTypeID>>{
          {"status", common::LogicalTypeID::STRING}});
  function->isReadOnly = false;

  function->bindFunc = [](const neug::Schema&,
                          const neug::execution::ContextMeta&,
                          const ::physical::PhysicalPlan& plan,
                          int op_idx) -> std::unique_ptr<CallFuncInputBase> {
    const auto& opr = plan.plan(op_idx).opr();
    if (!opr.has_procedure_call()) {
      THROW_RUNTIME_ERROR("drop_index: expected procedure_call operator");
    }
    const auto& procedurePB = opr.procedure_call();
    const auto& query = procedurePB.query();

    if (query.arguments_size() != 2) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "drop_index(table_name, index_name) expects 2 arguments.");
    }

    auto input = std::make_unique<DropIndexFuncInput>();
    input->tableName = getConstStringArg(query.arguments(0), "drop_index");
    input->indexName = getConstStringArg(query.arguments(1), "drop_index");
    return input;
  };

  function->execFunc =
      [](const CallFuncInputBase& inputBase,
         neug::IStorageInterface& storage) -> neug::execution::Context {
    try {
      const auto& input = dynamic_cast<const DropIndexFuncInput&>(inputBase);
      auto* catalog = neug::main::MetadataRegistry::getCatalog();
      if (catalog == nullptr) {
        THROW_RUNTIME_ERROR("drop_index: catalog is nullptr");
      }

      auto& tx = neug::Constants::DEFAULT_TRANSACTION;
      // Resolve the index's table ID. The table may be a rel GROUP (multi-
      // subtable edge type, e.g. ROOT_POST(from Comment to Post, from Post
      // to Post)): rel groups live in their own catalog set and carry no
      // table ID of their own, so use the group's first child rel-table ID.
      // The extended-index fast path matches by index name, so this ID only
      // feeds the regular-index lookup.
      auto tableID = [&]() -> std::optional<common::table_id_t> {
        if (catalog->containsRelGroup(&tx, input.tableName)) {
          auto* group = catalog->getRelGroupEntry(&tx, input.tableName);
          if (group->getNumRelTables() > 0)
            return group->getRelTableIDs()[0];
          return std::nullopt;
        }
        auto* tableEntry = catalog->getTableCatalogEntry(&tx, input.tableName);
        if (tableEntry == nullptr)
          return std::nullopt;
        return tableEntry->getTableID();
      }();
      if (!tableID.has_value()) {
        THROW_INVALID_ARGUMENT_EXCEPTION("drop_index: table not found: " +
                                         input.tableName);
      }

      if (catalog->containsIndex(&tx, *tableID, input.indexName)) {
        catalog->dropIndex(&tx, *tableID, input.indexName);
      } else if (catalog->containsExtendedIndex(&tx, input.indexName)) {
        // FILL-related indexes are stored in the extended index catalog.
        // Use the table ID the index entry itself recorded: for a rel-group
        // table this is the concrete child sub-table the FILL was created
        // against — no guessing which child the group ID should map to.
        auto* extEntry = catalog->getExtendedIndexEntry(&tx, input.indexName);
        bool ownsIndex = extEntry->getTableID() == *tableID;
        if (catalog->containsRelGroup(&tx, input.tableName)) {
          for (auto child : catalog->getRelGroupEntry(&tx, input.tableName)->getRelTableIDs())
            ownsIndex = ownsIndex || child == extEntry->getTableID();
        }
        if (!ownsIndex)
          THROW_INVALID_ARGUMENT_EXCEPTION("drop_index: index belongs to another table: " + input.indexName);
        const auto catalogEntryName = extEntry->getName();
        catalog->dropExtendedIndex(&tx, extEntry->getTableID(),
                                   input.indexName);
        if (auto* registry = storage.GetFillIndexMaintenanceRegistry()) {
          registry->Unregister(catalogEntryName);
        }
      } else {
        THROW_INVALID_ARGUMENT_EXCEPTION("drop_index: index not found: " + input.indexName);
      }

      neug::execution::Context ctx;
      neug::execution::ValueColumnBuilder<std::string> status_builder;
      status_builder.reserve(1);
      status_builder.push_back_opt("OK");
      ctx.set(0, status_builder.finish());
      ctx.tag_ids = {0};
      return ctx;
    } catch (const std::exception& e) {
      LOG(ERROR) << "drop_index failed: " << e.what();
      THROW_RUNTIME_ERROR(std::string{"drop_index failed: "} + e.what());
    }
  };

  function_set functionSet;
  functionSet.push_back(std::move(function));
  return functionSet;
}

}  // namespace function
}  // namespace neug

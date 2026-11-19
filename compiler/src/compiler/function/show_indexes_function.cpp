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

#include "neug/compiler/function/show_indexes_function.h"

#include <glog/logging.h>

#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/table_catalog_entry.h"
#include "neug/compiler/gopt/g_constants.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/execution/common/columns/value_columns.h"
#include "neug/execution/common/context.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace function {

function_set ShowIndexesFunction::getFunctionSet() {
  auto function = std::make_unique<NeugCallFunction>(
      ShowIndexesFunction::name, std::vector<neug::common::LogicalTypeID>{},
      std::vector<std::pair<std::string, neug::common::LogicalTypeID>>{
          {"table_name", common::LogicalTypeID::STRING},
          {"index_name", common::LogicalTypeID::STRING},
          {"index_type", common::LogicalTypeID::STRING},
          {"properties", common::LogicalTypeID::STRING}});

  function->bindFunc = [](const neug::Schema&, const neug::execution::ContextMeta&,
                          const ::physical::PhysicalPlan&, int)
      -> std::unique_ptr<CallFuncInputBase> {
    return std::make_unique<ShowIndexesFuncInput>();
  };

  function->execFunc = [](const CallFuncInputBase&, neug::IStorageInterface&)
      -> neug::execution::Context {
    try {
      auto* catalog = neug::main::MetadataRegistry::getCatalog();
      if (catalog == nullptr) {
        THROW_RUNTIME_ERROR("show_indexes: catalog is nullptr");
      }

      auto& tx = neug::Constants::DEFAULT_TRANSACTION;
      const auto entries = catalog->getExtendedIndexEntries(&tx);

      neug::execution::Context ctx;
      neug::execution::ValueColumnBuilder<std::string> table_builder;
      neug::execution::ValueColumnBuilder<std::string> index_builder;
      neug::execution::ValueColumnBuilder<std::string> type_builder;
      neug::execution::ValueColumnBuilder<std::string> props_builder;

      table_builder.reserve(entries.size());
      index_builder.reserve(entries.size());
      type_builder.reserve(entries.size());
      props_builder.reserve(entries.size());

      for (const auto* entry : entries) {
        if (entry == nullptr) {
          continue;
        }
        auto* tableEntry = catalog->getTableCatalogEntry(&tx, entry->getTableID());
        const auto tableName = tableEntry ? tableEntry->getName() : std::string{};

        std::string propsStr {"["};
        if (tableEntry) {
          const auto propIDs = entry->getPropertyIDs();
          for (size_t i = 0; i < propIDs.size(); ++i) {
            if (i > 0) {
              propsStr += ",";
            }
            try {
              propsStr += tableEntry
                              ->getProperty(static_cast<common::idx_t>(propIDs[i]))
                              .getName();
            } catch (const std::exception&) {
              propsStr += "<property:" + std::to_string(propIDs[i]) + ">";
            }
          }
        }
        propsStr += "]";

        table_builder.push_back_opt(tableName);
        index_builder.push_back_opt(entry->getIndexName());
        type_builder.push_back_opt(entry->getIndexType());
        props_builder.push_back_opt(propsStr);
      }

      ctx.set(0, table_builder.finish());
      ctx.set(1, index_builder.finish());
      ctx.set(2, type_builder.finish());
      ctx.set(3, props_builder.finish());
      ctx.tag_ids = {0, 1, 2, 3};
      return ctx;
    } catch (const std::exception& e) {
      LOG(ERROR) << "show_indexes failed: " << e.what();
      THROW_RUNTIME_ERROR(std::string{"show_indexes failed: "} + e.what());
    }
  };

  function_set functionSet;
  functionSet.push_back(std::move(function));
  return functionSet;
}

}  // namespace function
}  // namespace neug

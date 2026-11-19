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

#include <rapidjson/document.h>
#include <filesystem>
#include <map>
#include <vector>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/table_catalog_entry.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/common/enums/rel_direction.h"
#include "neug/compiler/main/metadata_manager.h"
#include "neug/compiler/storage/buffer_manager/memory_manager.h"
#include "neug/compiler/storage/store/table.h"

namespace neug {
namespace main {
class MetadataManager;
}

namespace catalog {
class CatalogEntry;
}

namespace storage {
class Table;

// Each key identifies one directed incidence of one typed relationship table.
// Missing observations remain unknown; an observed zero is a valid statistic.
struct EndpointDegreeKey {
  common::table_id_t relTableID;
  common::table_id_t nodeTableID;
  common::RelDataDirection direction;
  bool operator<(const EndpointDegreeKey& other) const {
    return std::tie(relTableID, nodeTableID, direction) <
           std::tie(other.relTableID, other.nodeTableID, other.direction);
  }
};
struct EndpointDegreeStatistics {
  uint64_t vertex_count;
  uint64_t edge_count;
  uint64_t nonzero_vertex_count;
  // Equal to edge_count only when every ordered endpoint pair is unique.
  // A missing/limited observation must never imply uniqueness.
  std::optional<uint64_t> distinct_endpoint_pair_count = std::nullopt;
};
// Equi-depth quantiles (milliseconds) of a sampled TIMESTAMP vertex column.
struct RangeStatistics {
  uint64_t vertex_count;
  uint64_t sample_count;
  uint64_t sample_null_count;
  std::vector<int64_t> quantiles;
};
struct EndpointPairStatistics {
  uint64_t degree_product_sum;
  uint64_t present_first_degree_second_sum;
  uint64_t positive_both_count;
};
using PropertyValue = std::variant<std::string, int64_t, bool>;
struct PropertyValueStatistics {
  uint64_t vertex_count;
  uint64_t value_count;
  // Degree totals over precisely the vertices matching this property value.
  // A missing endpoint is unknown, including when other endpoints are present.
  std::map<EndpointDegreeKey, EndpointDegreeStatistics> endpoint_degrees;
};
class DiskArrayCollection;

class NEUG_API StatsManager {
 public:
  StatsManager(MemoryManager& memoryManager) : memoryManager(memoryManager) {}

  StatsManager(const std::filesystem::path& statsPath,
               main::MetadataManager* database, MemoryManager& memoryManager);

  StatsManager(const std::string& statsData, main::MetadataManager* catalog,
               MemoryManager& memoryManager);

  ~StatsManager() = default;

  Table* getTable(common::table_id_t tableID);

  // Planner fallback cardinalities keep binding possible without statistics.
  // Cost comparisons must distinguish that fallback from an observed count
  // whose actual value happens to be one (or zero).
  bool hasTableCardinality(common::table_id_t tableID);

  // Update cardinalities without rebuilding every compiler table wrapper.
  // Returns false when the catalog shape changed and a full rebuild is needed.
  bool updateCardinalities(const std::string& statsData);

  std::optional<EndpointDegreeStatistics> getEndpointDegreeStatistics(
      const EndpointDegreeKey& endpoint);
  std::optional<EndpointPairStatistics> getEndpointPairStatistics(
      const EndpointDegreeKey& first, const EndpointDegreeKey& second);
  std::optional<PropertyValueStatistics> getPropertyValueStatistics(
      common::table_id_t nodeTableID, const std::string& property,
      const PropertyValue& value);
  std::optional<RangeStatistics> getRangeStatistics(common::table_id_t nodeTableID,
                                                    const std::string& property);

  void loadTables(const catalog::Catalog& catalog,
                  common::VirtualFileSystem* vfs,
                  main::ClientContext* context) {}

 private:
  main::MetadataManager* database = nullptr;
  std::unordered_map<common::table_id_t, std::unique_ptr<Table>> tables;
  std::unordered_set<common::table_id_t> observedCardinalityTables;
  MemoryManager& memoryManager;
  std::mutex mtx;
  std::map<EndpointDegreeKey, EndpointDegreeStatistics> endpointDegrees;
  std::map<std::pair<EndpointDegreeKey, EndpointDegreeKey>, EndpointPairStatistics>
      endpointPairs;
  struct PropertyStatistics {
    uint64_t vertex_count;
    uint64_t null_count;
    std::map<PropertyValue, PropertyValueStatistics> values;
  };
  std::map<std::pair<common::table_id_t, std::string>, PropertyStatistics> properties;
  std::map<std::pair<common::table_id_t, std::string>, RangeStatistics> ranges;

 private:
  void loadStats(
      const catalog::Catalog& catalog,
      const std::unordered_map<std::string, common::row_idx_t>& countMap);
  void getCardMap(const std::string& jsonData,
                  std::unordered_map<std::string, common::row_idx_t>& countMap);
  void loadDegreeStatistics(const std::string& statsData);
  void loadPropertyStatistics(const std::string& statsData);
  void loadRangeStatistics(const std::string& statsData);
  Table* getTableByName(common::table_id_t tableID,
                        catalog::TableCatalogEntry* curEntry);
  bool checkTableConsistency(Table* oldTable,
                             catalog::TableCatalogEntry* curEntry);
};

}  // namespace storage
}  // namespace neug

#include <fstream>
#include <sstream>
#include "neug/compiler/storage/stats_manager.h"

#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/table_catalog_entry.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/gopt/g_constants.h"
#include "neug/compiler/gopt/g_node_table.h"
#include "neug/compiler/gopt/g_rel_table.h"
#include "neug/compiler/main/metadata_manager.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace storage {

StatsManager::StatsManager(const std::string& statsData,
                           main::MetadataManager* database,
                           MemoryManager& memoryManager)
    : memoryManager(memoryManager), database(database) {
  std::unordered_map<std::string, common::row_idx_t> countMap;
  getCardMap(statsData, countMap);
  if (!database || !database->getCatalog()) {
    THROW_EXCEPTION_WITH_FILE_LINE("Database or catalog is not initialized");
  }
  loadStats(*database->getCatalog(), countMap);
  loadDegreeStatistics(statsData);
  loadPropertyStatistics(statsData);
  loadRangeStatistics(statsData);
}

bool StatsManager::updateCardinalities(const std::string& statsData) {
  std::unordered_map<std::string, common::row_idx_t> countMap;
  getCardMap(statsData, countMap);
  if (!database || !database->getCatalog()) {
    return false;
  }

  struct PendingUpdate {
    GNodeTable* node = nullptr;
    GRelTable* rel = nullptr;
    common::row_idx_t count = 1;
  };
  std::vector<PendingUpdate> updates;
  std::unordered_set<common::table_id_t> observed;
  std::lock_guard<std::mutex> lock(mtx);
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  size_t table_entry_count = 0;
  for (auto* entry : database->getCatalog()->getTableEntries(&transaction)) {
    if (entry->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY &&
        entry->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY) {
      continue;
    }
    ++table_entry_count;
    const auto table_it = tables.find(entry->getTableID());
    if (table_it == tables.end() ||
        !checkTableConsistency(table_it->second.get(), entry)) {
      return false;
    }
    const auto count_it = countMap.find(entry->getName());
    if (count_it != countMap.end()) observed.insert(entry->getTableID());
    const auto count =
        count_it == countMap.end() ? common::row_idx_t{1} : count_it->second;
    if (entry->getType() == catalog::CatalogEntryType::NODE_TABLE_ENTRY) {
      auto* node = dynamic_cast<GNodeTable*>(table_it->second.get());
      if (node == nullptr) {
        return false;
      }
      updates.push_back(PendingUpdate{node, nullptr, count});
    } else {
      auto* rel = dynamic_cast<GRelTable*>(table_it->second.get());
      if (rel == nullptr) {
        return false;
      }
      updates.push_back(PendingUpdate{nullptr, rel, count});
    }
  }
  if (table_entry_count != tables.size()) {
    return false;
  }
  for (const auto& update : updates) {
    if (update.node != nullptr) {
      update.node->setNumRows(update.count);
    } else {
      update.rel->setNumRows(update.count);
    }
  }
  observedCardinalityTables = std::move(observed);
  loadDegreeStatistics(statsData);
  loadPropertyStatistics(statsData);
  loadRangeStatistics(statsData);
  return true;
}

bool StatsManager::hasTableCardinality(common::table_id_t tableID) {
  std::lock_guard<std::mutex> lock(mtx);
  if (database == nullptr || !observedCardinalityTables.contains(tableID)) return false;
  const auto table = tables.find(tableID);
  auto* catalog = database->getCatalog();
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  return table != tables.end() && catalog && catalog->containsTable(&transaction, tableID) &&
      checkTableConsistency(table->second.get(), catalog->getTableCatalogEntry(&transaction, tableID));
}

bool StatsManager::checkTableConsistency(Table* oldTable,
                                         catalog::TableCatalogEntry* curEntry) {
  if (oldTable->getTableType() != curEntry->getTableType() ||
      oldTable->getTableName() != curEntry->getName()) {
    return false;
  }
  if (oldTable->getTableType() == common::TableType::REL) {
    auto catalog = database->getCatalog();
    auto& transaction = neug::Constants ::DEFAULT_TRANSACTION;
    // check if the src and dst table ids are consistent
    auto oldRelTable = oldTable->ptrCast<GRelTable>();
    if (!catalog->containsTable(&transaction, oldRelTable->getSrcTableId()) ||
        !catalog->containsTable(&transaction, oldRelTable->getDstTableId())) {
      return false;
    }
    auto oldSrcEntry = catalog->getTableCatalogEntry(
        &transaction, oldRelTable->getSrcTableId());
    auto oldDstEntry = catalog->getTableCatalogEntry(
        &transaction, oldRelTable->getDstTableId());
    auto curRelEntry = curEntry->ptrCast<catalog::RelTableCatalogEntry>();
    auto curSrcEntry = catalog->getTableCatalogEntry(
        &transaction, curRelEntry->getSrcTableID());
    auto curDstEntry = catalog->getTableCatalogEntry(
        &transaction, curRelEntry->getDstTableID());
    if (curSrcEntry->getTableType() != oldSrcEntry->getTableType() ||
        curDstEntry->getTableType() != oldDstEntry->getTableType() ||
        curSrcEntry->getName() != oldSrcEntry->getName() ||
        curDstEntry->getName() != oldDstEntry->getName()) {
      return false;
    }
  }
  return true;
}

// check if cached table data is consistent with the schema, return false if
// not, otherwise return true
Table* StatsManager::getTableByName(common::table_id_t tableID,
                                    catalog::TableCatalogEntry* curEntry) {
  if (tables.contains(tableID)) {
    auto oldTable = tables.at(tableID).get();
    if (checkTableConsistency(oldTable, curEntry)) {
      return oldTable;
    }
  }
  for (auto& [_, table] : tables) {
    if (checkTableConsistency(table.get(), curEntry)) {
      return table.get();
    }
  }
  return nullptr;
}

Table* StatsManager::getTable(common::table_id_t tableID) {
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  auto catalog = database->getCatalog();
  if (!catalog) {
    THROW_EXCEPTION_WITH_FILE_LINE("Catalog is not initialized");
  }
  NEUG_ASSERT(catalog->containsTable(&transaction, tableID));
  auto curEntry = catalog->getTableCatalogEntry(&transaction, tableID);
  Table* oldTable = getTableByName(tableID, curEntry);
  if (oldTable) {
    return oldTable;
  }
  switch (curEntry->getTableType()) {
  case common::TableType::NODE: {
    auto defaultNode = std::make_unique<GNodeTable>(
        curEntry->ptrCast<catalog::NodeTableCatalogEntry>(), this,
        &memoryManager, 1);
    return defaultNode.release();
  }
  case common::TableType::REL:
  default: {
    auto defaultRel = std::make_unique<GRelTable>(
        1, curEntry->ptrCast<catalog::RelTableCatalogEntry>(), this);
    return defaultRel.release();
  }
  }
}

StatsManager::StatsManager(const std::filesystem::path& statsPath,
                           main::MetadataManager* database,
                           MemoryManager& memoryManager)
    : memoryManager(memoryManager), database(database) {
  std::ifstream file(statsPath);
  if (!file.is_open()) {
    THROW_EXCEPTION_WITH_FILE_LINE("Statistics file " + statsPath.string() +
                                   " not found");
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string statsJson = buffer.str();

  std::unordered_map<std::string, common::row_idx_t> countMap;
  getCardMap(statsJson, countMap);
  if (!database || !database->getCatalog()) {
    THROW_EXCEPTION_WITH_FILE_LINE("Database or catalog is not initialized");
  }
  loadStats(*database->getCatalog(), countMap);
  loadDegreeStatistics(statsJson);
  loadPropertyStatistics(statsJson);
  loadRangeStatistics(statsJson);
}

std::optional<PropertyValueStatistics> StatsManager::getPropertyValueStatistics(
    common::table_id_t nodeTableID, const std::string& property,
    const PropertyValue& value) {
  std::lock_guard<std::mutex> lock(mtx);
  const auto found = properties.find({nodeTableID, property});
  if (found == properties.end() || !observedCardinalityTables.contains(nodeTableID))
    return std::nullopt;
  const auto match = found->second.values.find(value);
  if (match != found->second.values.end()) return match->second;
  // Only complete, internally consistent histograms are loaded. Absence from
  // such a histogram is an observed zero, unlike an uncollected property.
  return PropertyValueStatistics{found->second.vertex_count, 0, {}};
}

std::optional<RangeStatistics> StatsManager::getRangeStatistics(
    common::table_id_t nodeTableID, const std::string& property) {
  std::lock_guard<std::mutex> lock(mtx);
  const auto found = ranges.find({nodeTableID, property});
  if (found == ranges.end()) return std::nullopt;
  return found->second;
}

void StatsManager::loadRangeStatistics(const std::string& statsData) {
  ranges.clear();
  rapidjson::Document document;
  if (document.Parse(statsData.c_str()).HasParseError() || !document.IsObject() ||
      !document.HasMember("range_statistics")) return;
  const auto& section = document["range_statistics"];
  if (!section.IsObject() || !section.HasMember("version") || !section["version"].IsInt() ||
      section["version"].GetInt() != 1 || !section.HasMember("properties") ||
      !section["properties"].IsArray()) return;
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  std::unordered_map<std::string, catalog::TableCatalogEntry*> byName;
  for (auto* entry : database->getCatalog()->getTableEntries(&transaction))
    byName.emplace(entry->getName(), entry);
  for (const auto& record : section["properties"].GetArray()) {
    if (!record.IsObject()) continue;
    bool valid = true;
    const bool edge = record.HasMember("edge_type");
    const char* countField = edge ? "edge_count" : "vertex_count";
    for (const auto* field : {"property", "type", "unit"})
      valid &= record.HasMember(field) && record[field].IsString();
    if (edge)
      for (const auto* field : {"edge_type", "source_vertex", "destination_vertex"})
        valid &= record.HasMember(field) && record[field].IsString();
    else valid &= record.HasMember("vertex_type") && record["vertex_type"].IsString();
    for (const auto* field : {countField, "sample_count", "sample_null_count"})
      valid &= record.HasMember(field) && record[field].IsUint64();
    if (!valid || !record.HasMember("quantiles") || !record["quantiles"].IsArray() ||
        std::string(record["type"].GetString()) != "TIMESTAMP" ||
        std::string(record["unit"].GetString()) != "milliseconds") continue;
    auto entry = byName.end();
    if (edge) {
      // A typed child of a relationship group, or a single-pair relationship.
      entry = byName.find(catalog::RelGroupCatalogEntry::getChildTableName(
          record["edge_type"].GetString(), record["source_vertex"].GetString(),
          record["destination_vertex"].GetString()));
      if (entry == byName.end()) entry = byName.find(record["edge_type"].GetString());
    } else entry = byName.find(record["vertex_type"].GetString());
    const std::string property = record["property"].GetString();
    const auto expected = edge ? catalog::CatalogEntryType::REL_TABLE_ENTRY
                               : catalog::CatalogEntryType::NODE_TABLE_ENTRY;
    if (entry == byName.end() || entry->second->getType() != expected ||
        !entry->second->containsProperty(property) ||
        entry->second->getProperty(property).getType().getLogicalTypeID() !=
            common::LogicalTypeID::TIMESTAMP) continue;
    const auto id = entry->second->getTableID();
    if (!observedCardinalityTables.contains(id) || !tables.contains(id) ||
        record[countField].GetUint64() != tables.at(id)->getNumTotalRows(&transaction)) continue;
    RangeStatistics range{record[countField].GetUint64(), record["sample_count"].GetUint64(),
                          record["sample_null_count"].GetUint64(), {}};
    if (!range.sample_count || range.sample_null_count > range.sample_count) continue;
    for (const auto& value : record["quantiles"].GetArray()) {
      if (!value.IsInt64() || (!range.quantiles.empty() && value.GetInt64() < range.quantiles.back())) {
        valid = false;
        break;
      }
      range.quantiles.push_back(value.GetInt64());
    }
    if (!valid || range.quantiles.size() < 2) continue;
    ranges[{id, property}] = std::move(range);
  }
}

void StatsManager::loadPropertyStatistics(const std::string& statsData) {
  properties.clear();
  rapidjson::Document document;
  if (document.Parse(statsData.c_str()).HasParseError() || !document.IsObject() ||
      !document.HasMember("property_statistics")) return;
  const auto& section = document["property_statistics"];
  if (!section.IsObject() || !section.HasMember("version") ||
      !section["version"].IsInt() || section["version"].GetInt() != 1 ||
      !section.HasMember("scope") || !section["scope"].IsString() ||
      std::string(section["scope"].GetString()) != "visible_typed_vertex_properties" ||
      !section.HasMember("properties") || !section["properties"].IsArray()) return;
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  std::unordered_map<std::string, catalog::TableCatalogEntry*> byName;
  for (auto* entry : database->getCatalog()->getTableEntries(&transaction))
    byName.emplace(entry->getName(), entry);
  for (const auto& record : section["properties"].GetArray()) {
    if (!record.IsObject()) continue;
    bool valid = true;
    for (const auto* field : {"vertex_type", "property", "type"})
      valid &= record.HasMember(field) && record[field].IsString();
    for (const auto* field : {"vertex_count", "null_count"})
      valid &= record.HasMember(field) && record[field].IsUint64();
    if (!valid || !record.HasMember("complete") || !record["complete"].IsBool() ||
        !record["complete"].GetBool() || !record.HasMember("values") || !record["values"].IsArray())
      continue;
    const std::string node = record["vertex_type"].GetString(), property = record["property"].GetString();
    const auto entry = byName.find(node);
    if (entry == byName.end() || entry->second->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY ||
        !entry->second->containsProperty(property)) continue;
    const auto id = entry->second->getTableID();
    if (!observedCardinalityTables.contains(id)) continue;
    PropertyStatistics histogram{record["vertex_count"].GetUint64(), record["null_count"].GetUint64(), {}};
    if (histogram.null_count > histogram.vertex_count ||
        histogram.vertex_count != tables.at(id)->getNumTotalRows(&transaction)) continue;
    const std::string type = record["type"].GetString();
    const auto logicalType = entry->second->getProperty(property).getType().getLogicalTypeID();
    if (!((type == "STRING" && logicalType == common::LogicalTypeID::STRING) ||
          (type == "BOOL" && logicalType == common::LogicalTypeID::BOOL) ||
          (type == "INT32" && logicalType == common::LogicalTypeID::INT32) ||
          (type == "INT64" && logicalType == common::LogicalTypeID::INT64))) continue;
    uint64_t total = histogram.null_count;
    for (const auto& value : record["values"].GetArray()) {
      if (!value.IsObject() || !value.HasMember("value") || !value.HasMember("count") ||
          !value["count"].IsUint64()) { valid = false; break; }
      const auto& literal = value["value"];
      std::optional<PropertyValue> key;
      if (type == "STRING" && literal.IsString()) key = std::string(literal.GetString(), literal.GetStringLength());
      else if (type == "BOOL" && literal.IsBool()) key = literal.GetBool();
      else if ((type == "INT32" || type == "INT64") && literal.IsInt64()) key = literal.GetInt64();
      const auto count = value["count"].GetUint64();
      if (!key || !count || count > histogram.vertex_count - total || histogram.values.contains(*key)) {
        valid = false; break;
      }
      total += count;
      PropertyValueStatistics observation{histogram.vertex_count, count, {}};
      if (value.HasMember("endpoint_degrees") && value["endpoint_degrees"].IsArray())
        for (const auto& endpoint : value["endpoint_degrees"].GetArray()) {
          bool endpointValid = endpoint.IsObject();
          if (!endpointValid) continue;
          for (const auto* field : {"relationship", "source_vertex", "destination_vertex", "direction"})
            endpointValid &= endpoint.HasMember(field) && endpoint[field].IsString();
          for (const auto* field : {"edge_count", "nonzero_vertex_count"})
            endpointValid &= endpoint.HasMember(field) && endpoint[field].IsUint64();
          if (!endpointValid) continue;
          const std::string source = endpoint["source_vertex"].GetString();
          const std::string destination = endpoint["destination_vertex"].GetString();
          const std::string direction = endpoint["direction"].GetString();
          const std::string relationship = endpoint["relationship"].GetString();
          if ((direction != "OUT" && direction != "IN") ||
              (direction == "OUT" ? source : destination) != node) continue;
          const auto child = catalog::RelGroupCatalogEntry::getChildTableName(relationship, source, destination);
          const auto relation = byName.find(byName.contains(child) ? child : relationship);
          if (relation == byName.end() || relation->second->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY ||
              !byName.contains(source) || !byName.contains(destination)) continue;
          const auto& rel = relation->second->constCast<catalog::RelTableCatalogEntry>();
          if (rel.getSrcTableID() != byName.at(source)->getTableID() ||
              rel.getDstTableID() != byName.at(destination)->getTableID() ||
              !observedCardinalityTables.contains(rel.getTableID())) continue;
          const auto edges = endpoint["edge_count"].GetUint64(), positive = endpoint["nonzero_vertex_count"].GetUint64();
          if (edges > tables.at(rel.getTableID())->getNumTotalRows(&transaction) ||
              positive > count || positive > edges || ((positive == 0) != (edges == 0))) continue;
          const EndpointDegreeKey endpointKey{rel.getTableID(), id,
              direction == "OUT" ? common::RelDataDirection::FWD : common::RelDataDirection::BWD};
          if (!observation.endpoint_degrees.emplace(endpointKey,
                  EndpointDegreeStatistics{count, edges, positive}).second) { valid = false; break; }
        }
      if (!valid) break;
      histogram.values.emplace(std::move(*key), std::move(observation));
    }
    if (!valid || total != histogram.vertex_count) continue;
    if (!properties.emplace(std::make_pair(id, property), std::move(histogram)).second) {
      properties.clear(); return;
    }
  }
}

std::optional<EndpointDegreeStatistics> StatsManager::getEndpointDegreeStatistics(
    const EndpointDegreeKey& endpoint) {
  std::lock_guard<std::mutex> lock(mtx);
  const auto found = endpointDegrees.find(endpoint);
  if (found == endpointDegrees.end() ||
      !observedCardinalityTables.contains(endpoint.relTableID) ||
      !observedCardinalityTables.contains(endpoint.nodeTableID)) return std::nullopt;
  return found->second;
}

std::optional<EndpointPairStatistics> StatsManager::getEndpointPairStatistics(
    const EndpointDegreeKey& first, const EndpointDegreeKey& second) {
  std::lock_guard<std::mutex> lock(mtx);
  const auto found = endpointPairs.find({first, second});
  if (found == endpointPairs.end()) return std::nullopt;
  return found->second;
}

void StatsManager::loadDegreeStatistics(const std::string& statsData) {
  endpointDegrees.clear();
  endpointPairs.clear();
  rapidjson::Document document;
  if (document.Parse(statsData.c_str()).HasParseError() || !document.IsObject() ||
      !document.HasMember("degree_statistics")) return;
  const auto& section = document["degree_statistics"];
  if (!section.IsObject() || !section.HasMember("version") ||
      !section["version"].IsInt() || section["version"].GetInt() != 1 ||
      (section.HasMember("scope") && (!section["scope"].IsString() ||
       std::string(section["scope"].GetString()) != "visible_typed_directed_endpoint_degrees")) ||
      !section.HasMember("endpoints") || !section["endpoints"].IsArray()) return;
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;
  std::unordered_map<std::string, catalog::TableCatalogEntry*> byName;
  for (auto* entry : database->getCatalog()->getTableEntries(&transaction))
    byName.emplace(entry->getName(), entry);
  auto key = [&](const rapidjson::Value& value) -> std::optional<EndpointDegreeKey> {
    if (!value.IsObject()) return std::nullopt;
    for (const auto* name : {"relationship", "source_vertex", "destination_vertex", "direction"})
      if (!value.HasMember(name) || !value[name].IsString()) return std::nullopt;
    const std::string source = value["source_vertex"].GetString();
    const std::string destination = value["destination_vertex"].GetString();
    const std::string direction = value["direction"].GetString();
    if (direction != "OUT" && direction != "IN") return std::nullopt;
    const std::string relationship = value["relationship"].GetString();
    const auto childName = catalog::RelGroupCatalogEntry::getChildTableName(
        relationship, source, destination);
    // GCatalog creates child names only when a relationship has multiple typed
    // endpoint pairs. A single typed relation keeps its public relationship name.
    const auto relation = byName.contains(childName) ? childName : relationship;
    if (!byName.contains(relation) || !byName.contains(source) || !byName.contains(destination))
      return std::nullopt;
    auto* entry = byName.at(relation);
    if (entry->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY ||
        byName.at(source)->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY ||
        byName.at(destination)->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY)
      return std::nullopt;
    auto* rel = entry->ptrCast<catalog::RelTableCatalogEntry>();
    if (rel->getSrcTableID() != byName.at(source)->getTableID() ||
        rel->getDstTableID() != byName.at(destination)->getTableID()) return std::nullopt;
    const auto boundID = byName.at(direction == "OUT" ? source : destination)->getTableID();
    if (!observedCardinalityTables.contains(entry->getTableID()) ||
        !observedCardinalityTables.contains(boundID)) return std::nullopt;
    return EndpointDegreeKey{entry->getTableID(), boundID,
        direction == "OUT" ? common::RelDataDirection::FWD : common::RelDataDirection::BWD};
  };
  auto unsignedFields = [](const rapidjson::Value& value, std::initializer_list<const char*> names) {
    if (!value.IsObject()) return false;
    for (const auto* name : names)
      if (!value.HasMember(name) || !value[name].IsUint64()) return false;
    return true;
  };
  for (const auto& value : section["endpoints"].GetArray()) {
    const auto endpoint = key(value);
    if (!endpoint || !unsignedFields(value, {"vertex_count", "edge_count", "nonzero_vertex_count"})) continue;
    EndpointDegreeStatistics stats{value["vertex_count"].GetUint64(),
        value["edge_count"].GetUint64(), value["nonzero_vertex_count"].GetUint64()};
    if (stats.nonzero_vertex_count > stats.vertex_count || stats.nonzero_vertex_count > stats.edge_count ||
        (stats.edge_count == 0) != (stats.nonzero_vertex_count == 0) ||
        stats.vertex_count != tables.at(endpoint->nodeTableID)->getNumTotalRows(&transaction) ||
        stats.edge_count != tables.at(endpoint->relTableID)->getNumTotalRows(&transaction)) continue;
    if (value.HasMember("distinct_endpoint_pair_count") &&
        value["distinct_endpoint_pair_count"].IsUint64()) {
      const auto distinct = value["distinct_endpoint_pair_count"].GetUint64();
      if (distinct >= stats.nonzero_vertex_count && distinct <= stats.edge_count &&
          (distinct == 0) == (stats.edge_count == 0))
        stats.distinct_endpoint_pair_count = distinct;
    }
    // Duplicate keys are malformed evidence, even when the repeated values agree.
    if (endpointDegrees.contains(*endpoint)) {
      endpointDegrees.clear(); endpointPairs.clear(); return;
    }
    endpointDegrees.emplace(*endpoint, stats);
  }
  // Both incidences count the same typed ordered pairs. Conflicting evidence
  // invalidates this optional observation, while retaining valid degree data.
  std::unordered_map<common::table_id_t, uint64_t> distinctByRelation;
  std::unordered_set<common::table_id_t> inconsistentDistinct;
  for (const auto& [endpoint, stats] : endpointDegrees) {
    if (!stats.distinct_endpoint_pair_count) continue;
    const auto [found, inserted] = distinctByRelation.emplace(
        endpoint.relTableID, *stats.distinct_endpoint_pair_count);
    if (!inserted && found->second != *stats.distinct_endpoint_pair_count)
      inconsistentDistinct.insert(endpoint.relTableID);
  }
  for (auto& [endpoint, stats] : endpointDegrees)
    if (inconsistentDistinct.contains(endpoint.relTableID))
      stats.distinct_endpoint_pair_count.reset();
  if (!section.HasMember("pairs") || !section["pairs"].IsArray()) return;
  for (const auto& value : section["pairs"].GetArray()) {
    if (!value.IsObject() || !value.HasMember("first") || !value.HasMember("second") ||
        !unsignedFields(value, {"degree_product_sum", "present_first_degree_second_sum", "positive_both_count"})) continue;
    const auto first = key(value["first"]), second = key(value["second"]);
    if (!first || !second || first->nodeTableID != second->nodeTableID ||
        !endpointDegrees.contains(*first) || !endpointDegrees.contains(*second)) continue;
    const EndpointPairStatistics stats{value["degree_product_sum"].GetUint64(),
        value["present_first_degree_second_sum"].GetUint64(), value["positive_both_count"].GetUint64()};
    const auto& a = endpointDegrees.at(*first); const auto& b = endpointDegrees.at(*second);
    if (stats.present_first_degree_second_sum > b.edge_count ||
        stats.positive_both_count > std::min(a.nonzero_vertex_count, b.nonzero_vertex_count) ||
        stats.positive_both_count > stats.present_first_degree_second_sum ||
        stats.present_first_degree_second_sum > stats.degree_product_sum) continue;
    const auto pair = std::make_pair(*first, *second);
    if (endpointPairs.contains(pair)) {
      endpointPairs.clear(); return;
    }
    endpointPairs.emplace(pair, stats);
  }
}

void StatsManager::getCardMap(
    const std::string& json,
    std::unordered_map<std::string, common::row_idx_t>& countMap) {
  try {
    if (json.empty()) {
      // If JSON is empty, just return an empty countMap
      return;
    }

    rapidjson::Document jsonData;

    if (jsonData.Parse(json.c_str()).HasParseError()) {
      THROW_EXCEPTION_WITH_FILE_LINE("Invalid JSON format: " +
                                     std::to_string(jsonData.GetParseError()));
    }
    // Process node statistics if valid
    if (jsonData.HasMember("vertex_type_statistics") &&
        jsonData["vertex_type_statistics"].IsArray()) {
      for (const auto& nodeStat :
           jsonData["vertex_type_statistics"].GetArray()) {
        if (nodeStat.HasMember("type_name") && nodeStat.HasMember("count") &&
            nodeStat["type_name"].IsString() && nodeStat["count"].IsInt64()) {
          auto nodeName = nodeStat["type_name"].GetString();
          countMap[nodeName] = nodeStat["count"].GetInt64();
        }
      }
    }

    if (jsonData.HasMember("edge_type_statistics") &&
        jsonData["edge_type_statistics"].IsArray()) {
      for (const auto& relStat : jsonData["edge_type_statistics"].GetArray()) {
        if (relStat.HasMember("type_name") &&
            relStat.HasMember("vertex_type_pair_statistics") &&
            relStat["type_name"].IsString() &&
            relStat["vertex_type_pair_statistics"].IsArray()) {
          auto relName = relStat["type_name"].GetString();
          common::row_idx_t totalCount = 0;

          for (const auto& srcDst :
               relStat["vertex_type_pair_statistics"].GetArray()) {
            if (srcDst.HasMember("source_vertex") &&
                srcDst.HasMember("destination_vertex") &&
                srcDst.HasMember("count") &&
                srcDst["source_vertex"].IsString() &&
                srcDst["destination_vertex"].IsString() &&
                srcDst["count"].IsInt64()) {
              auto srcName = srcDst["source_vertex"].GetString();
              auto dstName = srcDst["destination_vertex"].GetString();
              auto childName =
                  neug::catalog::RelGroupCatalogEntry::getChildTableName(
                      relName, srcName, dstName);
              auto count = srcDst["count"].GetInt64();
              countMap[childName] = count;
              totalCount += count;
            }
          }
          countMap[relName] = totalCount;
        }
      }
    }
  } catch (const std::exception& e) {
    // If any error occurs during JSON processing, just continue with empty
    // countMap All counts will default to 0
  }
}

void StatsManager::loadStats(
    const catalog::Catalog& catalog,
    const std::unordered_map<std::string, common::row_idx_t>& countMap) {
  auto& transaction = neug::Constants::DEFAULT_TRANSACTION;

  // Process all node tables from catalog
  for (auto& tableEntry : catalog.getTableEntries(&transaction)) {
    if (countMap.contains(tableEntry->getName()))
      observedCardinalityTables.insert(tableEntry->getTableID());
    if (tableEntry->getType() == catalog::CatalogEntryType::NODE_TABLE_ENTRY) {
      auto* nodeTableEntry =
          dynamic_cast<catalog::NodeTableCatalogEntry*>(tableEntry);
      auto nodeName = nodeTableEntry->getName();
      auto count = countMap.count(nodeName) ? countMap.at(nodeName) : 1;
      tables[nodeTableEntry->getTableID()] = std::make_unique<GNodeTable>(
          nodeTableEntry, this, &memoryManager, count);
    } else if (tableEntry->getType() ==
               catalog::CatalogEntryType::REL_TABLE_ENTRY) {
      auto* relTableEntry =
          dynamic_cast<catalog::RelTableCatalogEntry*>(tableEntry);
      auto relName = relTableEntry->getName();
      auto count = countMap.count(relName) ? countMap.at(relName) : 1;
      tables[relTableEntry->getTableID()] =
          std::make_unique<neug::storage::GRelTable>(count, relTableEntry, this);
    }
  }
}

}  // namespace storage
}  // namespace neug

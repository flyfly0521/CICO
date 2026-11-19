/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "neug/storages/graph/property_graph.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <numeric>
#include <optional>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <arrow/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <tl/expected.hpp>

#include "neug/execution/common/columns/edge_columns.h"
#include "neug/storages/csr/generic_view_utils.h"
#include "neug/storages/file_names.h"
#include "neug/storages/loader/loader_utils.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/file_utils.h"
#include "neug/utils/indexers.h"
#include "neug/utils/property/types.h"
#include "neug/utils/yaml_utils.h"

namespace neug {

namespace {

Status ReadVertexOidFromArrowArray(const std::shared_ptr<arrow::Array>& array,
                                   int64_t row, DataTypeId expected_type,
                                   Property& out) {
  if (array->IsNull(row)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "NULL vertex id is not supported.");
  }
  switch (expected_type) {
    case DataTypeId::kInt32:
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(static_cast<int32_t>(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kInt64:
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(static_cast<int64_t>(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kUInt32:
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(static_cast<uint32_t>(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kUInt64:
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(static_cast<uint64_t>(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kVarchar:
      if (array->type()->Equals(arrow::utf8())) {
        auto s =
            std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
        out = Property::From(std::string_view(s.data(), s.size()));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::large_utf8())) {
        auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                     ->GetView(row);
        out = Property::From(std::string_view(s.data(), s.size()));
        return Status::OK();
      }
      break;
    default:
      break;
  }
  return Status(StatusCode::ERR_INVALID_ARGUMENT,
                "Type mismatch for vertex id: expect " +
                    std::to_string(static_cast<int>(expected_type)) +
                    ", got " + array->type()->ToString());
}

Status ReadPropertyFromArrowArray(const std::shared_ptr<arrow::Array>& array,
                                  int64_t row, DataTypeId expected_type,
                                  Property& out) {
  if (array->IsNull(row)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "NULL property value is not supported.");
  }
  switch (expected_type) {
    case DataTypeId::kBoolean:
      if (array->type()->Equals(arrow::boolean())) {
        out = Property::From(
            std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row));
        return Status::OK();
      }
      break;
    case DataTypeId::kInt32:
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(static_cast<int32_t>(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kUInt32:
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(static_cast<uint32_t>(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kInt64:
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(static_cast<int64_t>(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kUInt64:
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(static_cast<uint64_t>(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kFloat:
      if (array->type()->Equals(arrow::float32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
        return Status::OK();
      }
      break;
    case DataTypeId::kDouble:
      if (array->type()->Equals(arrow::float64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::float32())) {
        out = Property::From(static_cast<double>(
            std::static_pointer_cast<arrow::FloatArray>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kVarchar:
      if (array->type()->Equals(arrow::utf8())) {
        auto s =
            std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
        out = Property::From(std::string_view(s.data(), s.size()));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::large_utf8())) {
        auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                     ->GetView(row);
        out = Property::From(std::string_view(s.data(), s.size()));
        return Status::OK();
      }
      break;
    case DataTypeId::kDate:
      if (array->type()->Equals(arrow::date32())) {
        out = Property::from_date(Date(
            std::static_pointer_cast<arrow::Date32Array>(array)->Value(row)));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::date64())) {
        Date date;
        date.from_timestamp(
            std::static_pointer_cast<arrow::Date64Array>(array)->Value(row));
        out = Property::from_date(date);
        return Status::OK();
      }
      break;
    case DataTypeId::kTimestampMs:
      if (array->type()->Equals(
              arrow::timestamp(arrow::TimeUnit::type::MILLI))) {
        out = Property::from_datetime(DateTime(
            std::static_pointer_cast<arrow::TimestampArray>(array)->Value(row)));
        return Status::OK();
      }
      break;
    case DataTypeId::kInterval:
      if (array->type()->Equals(arrow::utf8())) {
        auto s =
            std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
        out = Property::from_interval(
            Interval(std::string_view(s.data(), s.size())));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::large_utf8())) {
        auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                     ->GetView(row);
        out = Property::from_interval(
            Interval(std::string_view(s.data(), s.size())));
        return Status::OK();
      }
      break;
    case DataTypeId::kEmpty:
      out = Property::empty();
      return Status::OK();
    default:
      break;
  }
  return Status(StatusCode::ERR_INVALID_ARGUMENT,
                "Type mismatch for vertex property: expect " +
                    std::to_string(static_cast<int>(expected_type)) +
                    ", got " + array->type()->ToString());
}

bool CanWriteArrowArrayDirectly(const std::shared_ptr<arrow::Array>& array,
                                DataTypeId property_type) {
  switch (property_type) {
    case DataTypeId::kBoolean:
      return array->type()->Equals(arrow::boolean());
    case DataTypeId::kInt32:
      return array->type()->Equals(arrow::int32());
    case DataTypeId::kUInt32:
      return array->type()->Equals(arrow::uint32());
    case DataTypeId::kInt64:
      return array->type()->Equals(arrow::int64());
    case DataTypeId::kUInt64:
      return array->type()->Equals(arrow::uint64());
    case DataTypeId::kFloat:
      return array->type()->Equals(arrow::float32());
    case DataTypeId::kDouble:
      return array->type()->Equals(arrow::float64());
    case DataTypeId::kVarchar:
    case DataTypeId::kInterval:
      return array->type()->Equals(arrow::utf8()) ||
             array->type()->Equals(arrow::large_utf8());
    case DataTypeId::kDate:
      return array->type()->Equals(arrow::date32()) ||
             array->type()->Equals(arrow::date64());
    case DataTypeId::kTimestampMs:
      return array->type()->Equals(
          arrow::timestamp(arrow::TimeUnit::type::MILLI));
    default:
      return false;
  }
}

}  // namespace

PropertyGraph::PropertyGraph()
    : vertex_label_total_count_(0),
      edge_label_total_count_(0),
      memory_level_(1) {}

PropertyGraph::~PropertyGraph() { Clear(); }

void PropertyGraph::loadSchema(const std::string& schema_path) {
  std::ifstream in(schema_path);
  schema_.Deserialize(in);
}

void PropertyGraph::Clear() {
  vertex_tables_.clear();
  edge_tables_.clear();
  vertex_label_total_count_ = 0;
  edge_label_total_count_ = 0;
  schema_.Clear();
}

Status PropertyGraph::EnsureCapacity(label_t v_label, size_t capacity) {
  if (schema_.vertex_label_valid(v_label)) {
    auto old_cap = vertex_tables_[v_label].Capacity();
    if (capacity <= old_cap) {
      return neug::Status::OK();
    }
    auto v_new_cap = vertex_tables_[v_label].EnsureCapacity(capacity);
    for (label_t dst_label = 0; dst_label < vertex_label_total_count_;
         ++dst_label) {
      if (!schema_.vertex_label_valid(dst_label)) {
        continue;
      }
      for (label_t e_label = 0; e_label < edge_label_total_count_; ++e_label) {
        size_t index = schema_.generate_edge_label(v_label, dst_label, e_label);
        if (edge_tables_.count(index) > 0) {
          edge_tables_.at(index).EnsureCapacity(
              v_new_cap, vertex_tables_[dst_label].Capacity());
        }
        if (v_label != dst_label) {
          index = schema_.generate_edge_label(dst_label, v_label, e_label);
          if (edge_tables_.count(index) > 0) {
            edge_tables_.at(index).EnsureCapacity(
                vertex_tables_[dst_label].Capacity(), v_new_cap);
          }
        }
      }
    }
    return neug::Status::OK();
  } else {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Vertex label does not exist.");
  }
}

Status PropertyGraph::EnsureCapacity(label_t src_label, label_t dst_label,
                                     label_t edge_label, size_t capacity) {
  if (!schema_.exist(src_label, dst_label, edge_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge label does not exist for the given source and "
                  "destination vertex labels.");
  }
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    return Status(
        StatusCode::ERR_INVALID_ARGUMENT,
        "Edge table for the given edge label triplet does not exist.");
  }
  size_t old_cap = edge_tables_.at(index).Capacity();
  if (capacity <= old_cap) {
    return neug::Status::OK();
  }
  edge_tables_.at(index).EnsureCapacity(capacity);
  return neug::Status::OK();
}

Status PropertyGraph::EnsureCapacity(label_t src_label, label_t dst_label,
                                     label_t edge_label, size_t src_v_cap,
                                     size_t dst_v_cap, size_t capacity) {
  if (!schema_.exist(src_label, dst_label, edge_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge label does not exist for the given source and "
                  "destination vertex labels.");
  }
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    return Status(
        StatusCode::ERR_INVALID_ARGUMENT,
        "Edge table for the given edge label triplet does not exist.");
  }
  size_t old_cap = edge_tables_.at(index).Capacity();
  if (capacity <= old_cap) {
    return neug::Status::OK();
  }
  edge_tables_.at(index).EnsureCapacity(src_v_cap, dst_v_cap, capacity);
  return neug::Status::OK();
}

Status PropertyGraph::BatchAddVertices(
    label_t v_label, std::shared_ptr<IRecordBatchSupplier> supplier) {
  assert(v_label < vertex_tables_.size());
  vertex_tables_[v_label].insert_vertices(supplier);
  return neug::Status::OK();
}

Status PropertyGraph::BatchAddEdges(
    label_t src_v_label, label_t dst_v_label, label_t e_label,
    std::shared_ptr<IRecordBatchSupplier> supplier) {
  schema_.ensure_vertex_label_valid(src_v_label);
  schema_.ensure_vertex_label_valid(dst_v_label);
  schema_.ensure_edge_triplet_valid(src_v_label, dst_v_label, e_label);
  size_t index = schema_.generate_edge_label(src_v_label, dst_v_label, e_label);
  assert(edge_tables_.count(index) > 0);
  edge_tables_.at(index).BatchAddEdges(
      vertex_tables_.at(src_v_label).get_indexer(),
      vertex_tables_.at(dst_v_label).get_indexer(), supplier);
  return neug::Status::OK();
}

Status PropertyGraph::BatchAddEdgesByInternalIds(
    label_t src_v_label, label_t dst_v_label, label_t e_label,
    const std::vector<vid_t>& src_lids,
    const std::vector<vid_t>& dst_lids,
    const std::vector<size_t>* edge_chunk_ends,
    const std::vector<std::vector<uint32_t>>* out_degree_chunks,
    const std::vector<std::vector<uint32_t>>* in_degree_chunks,
    size_t requested_worker_count) {
  schema_.ensure_vertex_label_valid(src_v_label);
  schema_.ensure_vertex_label_valid(dst_v_label);
  schema_.ensure_edge_triplet_valid(src_v_label, dst_v_label, e_label);
  if (src_lids.size() != dst_lids.size()) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Source and destination endpoint counts do not match");
  }
  const auto src_vertex_num =
      vertex_tables_.at(src_v_label).get_indexer().size();
  const auto dst_vertex_num =
      vertex_tables_.at(dst_v_label).get_indexer().size();
  for (size_t i = 0; i < src_lids.size(); ++i) {
    if (src_lids[i] >= src_vertex_num || dst_lids[i] >= dst_vertex_num) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Internal edge endpoint is out of range");
    }
  }
  const size_t index =
      schema_.generate_edge_label(src_v_label, dst_v_label, e_label);
  assert(edge_tables_.count(index) > 0);
  auto& edge_table = edge_tables_.at(index);
  edge_table.EnsureCapacity(static_cast<vid_t>(src_vertex_num),
                            static_cast<vid_t>(dst_vertex_num));
  edge_table.BatchAddEmptyEdges(src_lids, dst_lids, edge_chunk_ends,
                                out_degree_chunks, in_degree_chunks,
                                requested_worker_count);
  return neug::Status::OK();
}

Status PropertyGraph::AccumulateEdgeDegrees(
    label_t src_v_label, label_t dst_v_label, label_t e_label,
    std::shared_ptr<IRecordBatchSupplier> supplier,
    std::vector<uint32_t>& out_degrees, std::vector<uint32_t>& in_degrees,
    size_t& edge_count) const {
  schema_.ensure_vertex_label_valid(src_v_label);
  schema_.ensure_vertex_label_valid(dst_v_label);
  schema_.ensure_edge_triplet_valid(src_v_label, dst_v_label, e_label);
  size_t index = schema_.generate_edge_label(src_v_label, dst_v_label, e_label);
  assert(edge_tables_.count(index) > 0);
  edge_tables_.at(index).AccumulateEdgeDegrees(
      vertex_tables_.at(src_v_label).get_indexer(),
      vertex_tables_.at(dst_v_label).get_indexer(), supplier, out_degrees,
      in_degrees, edge_count);
  return neug::Status::OK();
}

Status PropertyGraph::PrepareBatchAddEdges(
    label_t src_v_label, label_t dst_v_label, label_t e_label,
    const std::vector<uint32_t>& out_degrees,
    const std::vector<uint32_t>& in_degrees, size_t edge_count) {
  schema_.ensure_vertex_label_valid(src_v_label);
  schema_.ensure_vertex_label_valid(dst_v_label);
  schema_.ensure_edge_triplet_valid(src_v_label, dst_v_label, e_label);
  size_t index = schema_.generate_edge_label(src_v_label, dst_v_label, e_label);
  assert(edge_tables_.count(index) > 0);
  edge_tables_.at(index).PrepareBatchAddEdges(out_degrees, in_degrees,
                                             edge_count);
  return neug::Status::OK();
}

Status PropertyGraph::CreateVertexType(
    const std::string& vertex_type_name,
    const std::vector<std::tuple<DataType, std::string, Property>>& properties,
    const std::vector<std::string>& primary_key_names, bool error_on_conflict) {
  if (schema_.contains_vertex_label(vertex_type_name)) {
    if (error_on_conflict) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Vertex label already exists.");
    } else {
      return Status(StatusCode::OK, "Vertex label already exists.");
    }
  }
  std::vector<std::string> property_names;
  std::vector<DataType> property_types;
  std::vector<Property> default_property_values;
  std::vector<std::tuple<DataType, std::string, size_t>> primary_keys;
  std::vector<int> primary_key_inds(primary_key_names.size(), -1);
  if (primary_key_inds.size() > 1) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Multi primary keys are not supported.");
  } else if (primary_key_inds.size() == 0) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "At least one primary key is required.");
  }
  for (size_t i = 0; i < properties.size(); i++) {
    auto [type, name, default_value] = properties[i];
    property_names.emplace_back(name);
    property_types.emplace_back(type);
    default_property_values.emplace_back(default_value);
  }
  for (size_t i = 0; i < primary_key_names.size(); i++) {
    std::string primary_key_name = primary_key_names.at(i);
    for (size_t j = 0; j < property_names.size(); j++) {
      if (property_names[j] == primary_key_name) {
        primary_key_inds[i] = j;
        break;
      }
    }
    if (primary_key_inds[i] == -1) {
      LOG(ERROR) << "Primary key " << primary_key_name
                 << " is not found in properties";
      return Status(
          StatusCode::ERR_INVALID_ARGUMENT,
          "Primary key " + primary_key_name + " is not found in properties");
    }
    auto type_id = property_types[primary_key_inds[i]].id();
    if (type_id != DataTypeId::kInt64 && type_id != DataTypeId::kVarchar &&
        type_id != DataTypeId::kUInt64 && type_id != DataTypeId::kInt32 &&
        type_id != DataTypeId::kUInt32) {
      LOG(ERROR) << "Primary key " << primary_key_name
                 << " should be int64/int32/uint64/uint32 or string/varchar";
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Primary key " + primary_key_name +
                        " should be int64/int32/uint64/"
                        "uint32 or string/varchar");
    }
    primary_keys.emplace_back(property_types[primary_key_inds[i]],
                              property_names[primary_key_inds[i]],
                              primary_key_inds[i]);
    property_names.erase(property_names.begin() + primary_key_inds[i]);
    property_types.erase(property_types.begin() + primary_key_inds[i]);
    default_property_values.erase(default_property_values.begin() +
                                  primary_key_inds[i]);
  }
  std::vector<StorageStrategy> strategies(property_types.size(),
                                          StorageStrategy::kMem);
  std::string description;
  schema_.AddVertexLabel(vertex_type_name, property_types, property_names,
                         primary_keys, strategies, Schema::MAX_VNUM,
                         description, default_property_values);
  label_t vertex_label_id = schema_.get_vertex_label_id(vertex_type_name);
  if (vertex_label_id < vertex_tables_.size()) {
    auto& vtable = vertex_tables_[vertex_label_id];
    if (vtable.is_dropped()) {
      // Reuse a dropped vertex table
      auto new_v_table =
          VertexTable(schema_.get_vertex_schema(vertex_label_id));

      vtable.Swap(new_v_table);
    } else {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Vertex label id conflict.");
    }
  } else {
    vertex_tables_.emplace_back(schema_.get_vertex_schema(vertex_label_id));
  }

  auto& vtable = vertex_tables_.back();
  vtable.Open(work_dir_, memory_level_);
  vtable.EnsureCapacity(4096);
  vertex_label_total_count_ = schema_.vertex_label_frontier();
  assert(vertex_tables_.size() == vertex_label_total_count_);

  while (v_mutex_.size() < vertex_label_total_count_) {
    v_mutex_.emplace_back(std::make_shared<std::mutex>());
  }

  LOG(INFO) << "CreateVertexType: vertex_type_name: " << vertex_type_name
            << ", vertex_label_id: " << static_cast<int32_t>(vertex_label_id)
            << ",properties " << property_names.size()
            << ", primary_key_names: " << primary_key_names[0];

  return neug::Status::OK();
}

Status PropertyGraph::CreateEdgeType(
    const std::string& src_vertex_type, const std::string& dst_vertex_type,
    const std::string& edge_type_name,
    const std::vector<std::tuple<DataType, std::string, Property>>& properties,
    bool error_on_conflict, EdgeStrategy oe_edge_strategy,
    EdgeStrategy ie_edge_strategy) {
  LOG(INFO) << "CreateEdgeType: src_vertex_type: " << src_vertex_type
            << ", dst_vertex_type: " << dst_vertex_type
            << ", edge_type_name: " << edge_type_name;
  if (!schema_.contains_vertex_label(src_vertex_type)) {
    LOG(ERROR) << "Source_vertex [" << src_vertex_type
               << "] does not exist in the graph.";
    return Status(
        StatusCode::ERR_INVALID_ARGUMENT,
        "Source_vertex [" + src_vertex_type + "] does not exist in the graph.");
  }
  if (!schema_.contains_vertex_label(dst_vertex_type)) {
    LOG(ERROR) << "Destination_vertex [" << dst_vertex_type
               << "] does not exist in the graph.";
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Destination_vertex [" + dst_vertex_type +
                      "] does not exist in the graph.");
  }
  if (schema_.has_edge_label(src_vertex_type, dst_vertex_type,
                             edge_type_name)) {
    LOG(ERROR) << "Edge [" << edge_type_name << "] from [" << src_vertex_type
               << "] to [" << dst_vertex_type << "] already exists";
    if (error_on_conflict) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Edge [" + edge_type_name + "] from [" + src_vertex_type +
                        "] to [" + dst_vertex_type + "] already exists");
    } else {
      return Status(StatusCode::OK, "Edge triplet already exists.");
    }
  }
  std::vector<std::string> property_names;
  std::vector<DataType> property_types;
  std::vector<Property> default_property_values;
  for (size_t i = 0; i < properties.size(); i++) {
    auto [type, name, default_value] = properties[i];
    property_names.emplace_back(name);
    property_types.emplace_back(type);
    default_property_values.emplace_back(default_value);
  }
  EdgeStrategy cur_ie = ie_edge_strategy;
  EdgeStrategy cur_oe = oe_edge_strategy;
  bool oe_mutable = true, ie_mutable = true;
  bool cur_sort_on_compaction = false;
  std::string description;
  schema_.AddEdgeLabel(src_vertex_type, dst_vertex_type, edge_type_name,
                       property_types, property_names, {}, cur_oe, cur_ie,
                       oe_mutable, ie_mutable, cur_sort_on_compaction,
                       description, default_property_values);
  edge_label_total_count_ = schema_.edge_label_frontier();

  label_t src_label_i = schema_.get_vertex_label_id(src_vertex_type);
  label_t dst_label_i = schema_.get_vertex_label_id(dst_vertex_type);
  label_t e_label_i = schema_.get_edge_label_id(edge_type_name);
  size_t index =
      schema_.generate_edge_label(src_label_i, dst_label_i, e_label_i);

  if (edge_tables_.count(index) > 0) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT, "Edge label id conflict.");
  }
  EdgeTable edge_table(
      schema_.get_edge_schema(src_label_i, dst_label_i, e_label_i));
  edge_tables_.emplace(index, std::move(edge_table));
  auto src_v_capacity = std::max(
      vertex_tables_[src_label_i].get_indexer().capacity(), (size_t) 4096);
  auto dst_v_capacity = std::max(
      vertex_tables_[dst_label_i].get_indexer().capacity(), (size_t) 4096);
  edge_tables_.at(index).OpenInMemory(work_dir_);
  edge_tables_.at(index).EnsureCapacity(src_v_capacity, dst_v_capacity, 4096);

  return neug::Status::OK();
}

Status PropertyGraph::AddVertexProperties(
    const std::string& vertex_type_name,
    const std::vector<std::tuple<DataType, std::string, Property>>&
        add_properties,
    bool error_on_conflict) {
  RETURN_IF_NOT_OK_CONFLICT(vertex_label_check(vertex_type_name),
                            error_on_conflict);
  std::vector<std::string> add_property_names;
  std::vector<DataType> add_property_types;
  std::vector<StorageStrategy> add_property_storages;
  std::vector<Property> add_default_property_values;
  for (size_t i = 0; i < add_properties.size(); i++) {
    auto [property_type, property_name, default_value] = add_properties[i];
    if (schema_.vertex_has_property(vertex_type_name, property_name)) {
      if (error_on_conflict) {
        LOG(ERROR) << "Property [" << property_name
                   << "] already exists in vertex [" << vertex_type_name << "].";
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Property [" + property_name +
                          "] already exists in vertex [" + vertex_type_name +
                          "].");
      } else {
        continue;
      }
    }
    add_property_names.emplace_back(property_name);
    add_property_types.emplace_back(property_type);
    if (memory_level_ == 0) {
      add_property_storages.emplace_back(StorageStrategy::kDisk);
    } else if (memory_level_ >= 1) {
      add_property_storages.emplace_back(StorageStrategy::kMem);
    } else {
      add_property_storages.emplace_back(StorageStrategy::kNone);
    }
    add_default_property_values.emplace_back(default_value);
  }
  schema_.AddVertexProperties(vertex_type_name, add_property_names,
                              add_property_types, add_property_storages,
                              add_default_property_values);
  label_t v_label = schema_.get_vertex_label_id(vertex_type_name);
  vertex_tables_[v_label].AddProperties(add_property_names, add_property_types,
                                        add_default_property_values);
  return neug::Status::OK();
}

Status PropertyGraph::SetEdgeProperties(label_t src_label, label_t dst_label, label_t edge_label, std::vector<std::string> property_names, std::shared_ptr<IRecordBatchSupplier> supplier) {
  if (!schema_.vertex_label_valid(src_label) ||
      !schema_.vertex_label_valid(dst_label) || !schema_.edge_label_valid(edge_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Invalid src/dst/edge label id.");
  }
  if (!schema_.exist(src_label, dst_label, edge_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge triplet does not exist.");
  }

  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge table does not exist for label triplet.");
  }

  auto& edge_table = edge_tables_.at(index);
  const auto oe_view = edge_table.get_outgoing_view(neug::timestamp_t(0));
  const auto ie_view = edge_table.get_incoming_view(neug::timestamp_t(0));
  const auto edge_prop_types = schema_.get_edge_properties(src_label, dst_label, edge_label);
  const auto edge_prop_names = schema_.get_edge_property_names(src_label, dst_label, edge_label);
  const bool unbundled = !edge_prop_types.empty() && !edge_table.get_edge_data_accessor(0).is_bundled();

  std::vector<int32_t> prop_col_ids;
  prop_col_ids.reserve(property_names.size());
  for (const auto& prop_name : property_names) {
    auto it = std::find(edge_prop_names.begin(), edge_prop_names.end(), prop_name);
    if (it == edge_prop_names.end()) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Property [" + prop_name + "] does not exist in edge type.");
    }
    prop_col_ids.emplace_back(static_cast<int32_t>(std::distance(edge_prop_names.begin(), it)));
  }

  auto parse_endpoint_column = [](const IndexerType& indexer,
                                  const std::shared_ptr<arrow::Array>& array,
                                  std::vector<vid_t>& lids) -> Status {
    lids.clear();
    lids.reserve(static_cast<size_t>(array->length()));
    if (array->type()->Equals(arrow::utf8())) {
      auto casted = std::static_pointer_cast<arrow::StringArray>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        auto str = casted->GetView(i);
        std::string_view sv(str.data(), str.size());
        lids.push_back(indexer.get_index(Property::From(sv)));
      }
      return Status::OK();
    }
    if (array->type()->Equals(arrow::large_utf8())) {
      auto casted = std::static_pointer_cast<arrow::LargeStringArray>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        auto str = casted->GetView(i);
        std::string_view sv(str.data(), str.size());
        lids.push_back(indexer.get_index(Property::From(sv)));
      }
      return Status::OK();
    }
    if (array->type()->Equals(arrow::int64())) {
      auto casted = std::static_pointer_cast<arrow::Int64Array>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        lids.push_back(indexer.get_index(Property::From(casted->Value(i))));
      }
      return Status::OK();
    }
    if (array->type()->Equals(arrow::uint64())) {
      auto casted = std::static_pointer_cast<arrow::UInt64Array>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        lids.push_back(indexer.get_index(Property::From(casted->Value(i))));
      }
      return Status::OK();
    }
    if (array->type()->Equals(arrow::int32())) {
      auto casted = std::static_pointer_cast<arrow::Int32Array>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        lids.push_back(indexer.get_index(Property::From(casted->Value(i))));
      }
      return Status::OK();
    }
    if (array->type()->Equals(arrow::uint32())) {
      auto casted = std::static_pointer_cast<arrow::UInt32Array>(array);
      for (int64_t i = 0; i < casted->length(); ++i) {
        if (casted->IsNull(i)) {
          lids.push_back(std::numeric_limits<vid_t>::max());
          continue;
        }
        lids.push_back(indexer.get_index(Property::From(casted->Value(i))));
      }
      return Status::OK();
    }
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Unsupported endpoint column type: " + array->type()->ToString());
  };

  auto read_prop_value = [](const std::shared_ptr<arrow::Array>& array,
                            int64_t row, DataTypeId expected_type,
                            Property& out) -> Status {
    if (array->IsNull(row)) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "NULL value is not supported for edge property update.");
    }
    switch (expected_type) {
    case DataTypeId::kBoolean: {
      if (!array->type()->Equals(arrow::boolean())) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Type mismatch: expect boolean, got " +
                          array->type()->ToString());
      }
      out = Property::From(
          std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row));
      return Status::OK();
    }
    case DataTypeId::kInt32: {
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(static_cast<int32_t>(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect int32, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kUInt32: {
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(static_cast<uint32_t>(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect uint32, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kInt64: {
      if (array->type()->Equals(arrow::int64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::int32())) {
        out = Property::From(static_cast<int64_t>(
            std::static_pointer_cast<arrow::Int32Array>(array)->Value(row)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect int64, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kUInt64: {
      if (array->type()->Equals(arrow::uint64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint32())) {
        out = Property::From(static_cast<uint64_t>(
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect uint64, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kFloat: {
      if (array->type()->Equals(arrow::float32())) {
        out = Property::From(
            std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect float, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kDouble: {
      if (array->type()->Equals(arrow::float64())) {
        out = Property::From(
            std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
        return Status::OK();
      }
      if (array->type()->Equals(arrow::float32())) {
        out = Property::From(static_cast<double>(
            std::static_pointer_cast<arrow::FloatArray>(array)->Value(row)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect double, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kVarchar: {
      if (array->type()->Equals(arrow::utf8())) {
        auto s = std::static_pointer_cast<arrow::StringArray>(array)->GetView(
            row);
        std::string_view sv(s.data(), s.size());
        out = Property::From(sv);
        return Status::OK();
      }
      if (array->type()->Equals(arrow::large_utf8())) {
        auto s =
            std::static_pointer_cast<arrow::LargeStringArray>(array)->GetView(
                row);
        std::string_view sv(s.data(), s.size());
        out = Property::From(sv);
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect string, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kDate: {
      if (array->type()->Equals(arrow::int32())) {
        auto v = std::static_pointer_cast<arrow::Int32Array>(array)->Value(row);
        Property p;
        p.set_date(static_cast<uint32_t>(v));
        out = p;
        return Status::OK();
      }
      if (array->type()->Equals(arrow::uint32())) {
        auto v =
            std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row);
        Property p;
        p.set_date(v);
        out = p;
        return Status::OK();
      }
      if (array->type()->Equals(arrow::utf8()) ||
          array->type()->Equals(arrow::large_utf8())) {
        std::string_view sv;
        if (array->type()->Equals(arrow::utf8())) {
          auto s =
              std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
          sv = std::string_view(s.data(), s.size());
        } else {
          auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                       ->GetView(row);
          sv = std::string_view(s.data(), s.size());
        }
        out = Property::from_date(Date(std::string(sv)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect date, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kTimestampMs: {
      if (array->type()->Equals(arrow::int64())) {
        Property p;
        p.set_datetime(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
        out = p;
        return Status::OK();
      }
      if (array->type()->Equals(arrow::utf8()) ||
          array->type()->Equals(arrow::large_utf8())) {
        std::string_view sv;
        if (array->type()->Equals(arrow::utf8())) {
          auto s =
              std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
          sv = std::string_view(s.data(), s.size());
        } else {
          auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                       ->GetView(row);
          sv = std::string_view(s.data(), s.size());
        }
        out = Property::from_datetime(DateTime(std::string(sv)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect timestamp(ms), got " +
                        array->type()->ToString());
    }
    case DataTypeId::kInterval: {
      if (array->type()->Equals(arrow::utf8()) ||
          array->type()->Equals(arrow::large_utf8())) {
        std::string_view sv;
        if (array->type()->Equals(arrow::utf8())) {
          auto s =
              std::static_pointer_cast<arrow::StringArray>(array)->GetView(row);
          sv = std::string_view(s.data(), s.size());
        } else {
          auto s = std::static_pointer_cast<arrow::LargeStringArray>(array)
                       ->GetView(row);
          sv = std::string_view(s.data(), s.size());
        }
        out = Property::from_interval(Interval(std::string(sv)));
        return Status::OK();
      }
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Type mismatch: expect interval, got " +
                        array->type()->ToString());
    }
    case DataTypeId::kEmpty:
      out = Property::empty();
      return Status::OK();
    default:
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Unsupported property type for update: " +
                        std::to_string(static_cast<int>(expected_type)));
    }
  };

  const auto& src_indexer = vertex_tables_[src_label].get_indexer();
  const auto& dst_indexer = vertex_tables_[dst_label].get_indexer();

  std::vector<vid_t> src_lids;
  std::vector<vid_t> dst_lids;

  while (true) {
    auto batch = supplier->GetNextBatch();
    if (!batch) {
      break;
    }

    if (batch->num_columns() !=
        static_cast<int>(2 + property_names.size())) {
      return Status(
          StatusCode::ERR_INVALID_ARGUMENT,
          "SetEdgeProperties expects " +
              std::to_string(2 + property_names.size()) +
              " columns (src, dst, values...), got " +
              std::to_string(batch->num_columns()));
    }

    auto status = parse_endpoint_column(src_indexer, batch->column(0), src_lids);
    if (!status.ok()) {
      return status;
    }
    status = parse_endpoint_column(dst_indexer, batch->column(1), dst_lids);
    if (!status.ok()) {
      return status;
    }

    struct EdgeLocation {
      int32_t outgoing_offset;
      int32_t incoming_offset;
    };
    const auto row_count = static_cast<size_t>(batch->num_rows());
    std::vector<std::vector<EdgeLocation>> locations(row_count);
    std::vector<size_t> row_order(row_count);
    std::iota(row_order.begin(), row_order.end(), 0);
    std::sort(row_order.begin(), row_order.end(), [&](size_t lhs, size_t rhs) {
      if (src_lids[lhs] != src_lids[rhs]) {
        return src_lids[lhs] < src_lids[rhs];
      }
      if (dst_lids[lhs] != dst_lids[rhs]) {
        return dst_lids[lhs] < dst_lids[rhs];
      }
      return lhs < rhs;
    });

    // Resolve all requested (src, dst) pairs by scanning each source CSR only
    // once. The previous row-at-a-time implementation rescanned the same
    // adjacency list for every aggregate result row, which dominated REL FILL
    // writes such as BI14 and BI20.
    size_t source_begin = 0;
    while (source_begin < row_order.size()) {
      const auto src_lid = src_lids[row_order[source_begin]];
      if (src_lid >= std::numeric_limits<vid_t>::max() ||
          dst_lids[row_order[source_begin]] >=
              std::numeric_limits<vid_t>::max()) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Source or destination vertex does not exist.");
      }
      size_t source_end = source_begin + 1;
      while (source_end < row_order.size() &&
             src_lids[row_order[source_end]] == src_lid) {
        if (dst_lids[row_order[source_end]] >=
            std::numeric_limits<vid_t>::max()) {
          return Status(StatusCode::ERR_INVALID_ARGUMENT,
                        "Source or destination vertex does not exist.");
        }
        ++source_end;
      }

      std::unordered_map<vid_t, std::vector<size_t>> rows_by_destination;
      rows_by_destination.reserve(source_end - source_begin);
      for (size_t pos = source_begin; pos < source_end; ++pos) {
        const auto row = row_order[pos];
        rows_by_destination[dst_lids[row]].push_back(row);
      }
      auto oe_edges = oe_view.get_edges(src_lid);
      for (auto it = oe_edges.begin(); it != oe_edges.end(); ++it) {
        const auto dst_lid = it.get_vertex();
        auto requested = rows_by_destination.find(dst_lid);
        if (requested != rows_by_destination.end()) {
          neug::execution::EdgeRecord record;
          record.src = src_lid;
          record.dst = dst_lid;
          record.dir = neug::execution::Direction::kOut;
          record.prop = it.get_data_ptr();
          // Unbundled CSRs share a unique property-row ID, while only the
          // outgoing timestamp changes on a property write. Bundled equal
          // parallel records instead need an ordinal-preserving pairing.
          auto ie_offset = unbundled
              ? fuzzy_search_offset_from_nbr_list(ie_view.get_edges(dst_lid), src_lid,
                    it.get_data_ptr(), DataTypeId::kUInt64)
              : record_to_csr_offset_pair(oe_view, ie_view, record, edge_prop_types).second;
          if (ie_offset == std::numeric_limits<int32_t>::max()) {
            return Status(StatusCode::ERR_INTERNAL_ERROR,
                          "Failed to locate reverse edge offset.");
          }

          for (auto row : requested->second) {
            // Visible iteration skips tombstones; writes need the physical
            // CSR slot rather than the ordinal among remaining edges.
            locations[row].push_back(EdgeLocation{it.get_offset(), ie_offset});
          }
        }
      }
      for (size_t pos = source_begin; pos < source_end; ++pos) {
        if (locations[row_order[pos]].empty()) {
          return Status(StatusCode::ERR_INVALID_ARGUMENT,
                        "Edge does not exist for given (src, dst).");
        }
      }
      source_begin = source_end;
    }

    // Apply values in original input order so duplicate update rows retain the
    // same last-row-wins behaviour as the serial implementation. Parallel
    // edges with the same endpoints are all updated.
    for (size_t row = 0; row < row_count; ++row) {
      const auto src_lid = src_lids[row];
      const auto dst_lid = dst_lids[row];
      for (const auto& location : locations[row]) {
        for (size_t p = 0; p < prop_col_ids.size(); ++p) {
          const auto col_id = prop_col_ids[p];
          if (col_id < 0 ||
              static_cast<size_t>(col_id) >= edge_prop_types.size()) {
            return Status(StatusCode::ERR_INVALID_ARGUMENT,
                          "Invalid edge property column id.");
          }
          Property value;
          auto expected = edge_prop_types[static_cast<size_t>(col_id)].id();
          auto st = read_prop_value(batch->column(static_cast<int>(2 + p)),
                                    static_cast<int64_t>(row), expected, value);
          if (!st.ok()) {
            return st;
          }
          RETURN_IF_NOT_OK(UpdateEdgeProperty(
              src_label, src_lid, dst_label, dst_lid, edge_label,
              location.outgoing_offset, location.incoming_offset, col_id,
              value, neug::timestamp_t(0)));
        }
      }
    }
  }

  return Status::OK();
}

Status PropertyGraph::BatchUpdateEdgePropertiesConstant(
    label_t src_label, label_t dst_label, label_t edge_label,
    const std::vector<std::string>& property_names,
    const std::vector<Property>& values, timestamp_t ts,
    size_t requested_worker_count) {
  if (!schema_.vertex_label_valid(src_label) ||
      !schema_.vertex_label_valid(dst_label) ||
      !schema_.edge_label_valid(edge_label) ||
      !schema_.exist(src_label, dst_label, edge_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Invalid edge label triplet for constant property update");
  }
  if (property_names.empty() || property_names.size() != values.size()) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Constant edge property update requires matching non-empty "
                  "property names and values");
  }

  const size_t table_index =
      schema_.generate_edge_label(src_label, dst_label, edge_label);
  auto table_it = edge_tables_.find(table_index);
  if (table_it == edge_tables_.end()) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge table does not exist for constant property update");
  }

  const auto property_schema =
      schema_.get_edge_properties(src_label, dst_label, edge_label);
  const auto schema_names =
      schema_.get_edge_property_names(src_label, dst_label, edge_label);
  std::vector<int32_t> property_ids;
  property_ids.reserve(property_names.size());
  for (size_t i = 0; i < property_names.size(); ++i) {
    auto name_it =
        std::find(schema_names.begin(), schema_names.end(), property_names[i]);
    if (name_it == schema_names.end()) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Property [" + property_names[i] +
                        "] does not exist in edge type");
    }
    const auto property_id =
        static_cast<int32_t>(std::distance(schema_names.begin(), name_it));
    if (property_schema[static_cast<size_t>(property_id)].id() !=
        values[i].type()) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Constant value type does not match edge property [" +
                        property_names[i] + "]");
    }
    if (values[i].type() == DataTypeId::kVarchar) {
      return Status(
          StatusCode::ERR_NOT_IMPLEMENTED,
          "Constant batch update does not support string edge properties");
    }
    property_ids.emplace_back(property_id);
  }

  table_it->second.BatchUpdatePropertiesConstant(
      property_ids, values, ts, requested_worker_count);
  return Status::OK();
}

Status PropertyGraph::AddEdgeProperties(
    const std::string& src_type_name, const std::string& dst_type_name,
    const std::string& edge_type_name,
    const std::vector<std::tuple<DataType, std::string, Property>>&
        add_properties,
    bool error_on_conflict) {
  RETURN_IF_NOT_OK_CONFLICT(
      edge_triplet_check(src_type_name, dst_type_name, edge_type_name),
      error_on_conflict);
  std::vector<std::string> add_property_names;
  std::vector<DataType> add_property_types;
  std::vector<Property> add_default_property_values;
  for (size_t i = 0; i < add_properties.size(); i++) {
    auto [property_type, property_name, default_value] = add_properties[i];
    if (schema_.edge_has_property(src_type_name, dst_type_name, edge_type_name,
                                  property_name)) {
      std::string msg = "Property [" + property_name +
                        "] already exists in edge [" + edge_type_name +
                        "] from [" + src_type_name + "] to [" + dst_type_name +
                        "].";
      if (error_on_conflict) {
        LOG(ERROR) << msg;
        return Status(StatusCode::ERR_INVALID_ARGUMENT, msg);
      }
      continue;
    }
    add_property_names.emplace_back(property_name);
    add_property_types.emplace_back(property_type);
    add_default_property_values.emplace_back(default_value);
  }
  label_t e_label = schema_.get_edge_label_id(edge_type_name);

  std::vector<std::tuple<label_t, label_t, label_t>> related_triplets;
  for (const auto& [edge_label, _] : schema_.get_all_edge_schemas()) {
    auto [src_label, dst_label, edge] = schema_.parse_edge_label(edge_label);
    if (edge == e_label && schema_.edge_triplet_valid(src_label, dst_label, edge)) {
      related_triplets.emplace_back(src_label, dst_label, edge);
    }
  }

  for (const auto& [src_label, dst_label, edge] : related_triplets) {
    std::vector<std::string> triplet_property_names;
    std::vector<DataType> triplet_property_types;
    std::vector<Property> triplet_default_property_values;
    for (size_t i = 0; i < add_property_names.size(); i++) {
      if (schema_.edge_has_property(src_label, dst_label, edge,
                                    add_property_names[i])) {
        continue;
      }
      triplet_property_names.emplace_back(add_property_names[i]);
      triplet_property_types.emplace_back(add_property_types[i]);
      triplet_default_property_values.emplace_back(
          add_default_property_values[i]);
    }
    if (triplet_property_names.empty()) {
      continue;
    }

    const auto& triplet_src_type = schema_.get_vertex_label_name(src_label);
    const auto& triplet_dst_type = schema_.get_vertex_label_name(dst_label);
    schema_.AddEdgeProperties(triplet_src_type, triplet_dst_type,
                              edge_type_name, triplet_property_names,
                              triplet_property_types,
                              triplet_default_property_values);
    size_t index = schema_.generate_edge_label(src_label, dst_label, edge);
    if (!edge_tables_.count(index)) {
      LOG(ERROR) << "Edge [" << edge_type_name << "] from ["
                 << triplet_src_type << "] to [" << triplet_dst_type
                 << "] does not exist, cannot add properties.";
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Edge [" + edge_type_name + "] from [" +
                        triplet_src_type + "] to [" + triplet_dst_type +
                        "] does not exist, cannot add properties.");
    }

    auto& edge_table = edge_tables_.at(index);
    edge_table.AddProperties(triplet_property_names, triplet_property_types,
                             triplet_default_property_values);
  }

  return neug::Status::OK();
}

Status PropertyGraph::RenameVertexProperties(
    const std::string& vertex_type_name,
    const std::vector<std::pair<std::string, std::string>>& update_properties,
    bool error_on_conflict) {
  RETURN_IF_NOT_OK_CONFLICT(vertex_label_check(vertex_type_name),
                            error_on_conflict);
  std::vector<std::string> update_property_names;
  std::vector<std::string> update_property_renames;
  for (size_t i = 0; i < update_properties.size(); i++) {
    auto [property_name, property_rename] = update_properties[i];
    if (!schema_.vertex_has_property(vertex_type_name, property_name)) {
      std::string msg = "Property [" + property_name +
                        "] does not exist in vertex [" + vertex_type_name +
                        "].";
      LOG(ERROR) << msg;
      if (error_on_conflict) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT, msg);
      } else {
        return Status(StatusCode::OK, msg);
      }
    }
    update_property_names.emplace_back(property_name);
    update_property_renames.emplace_back(property_rename);
  }
  schema_.RenameVertexProperties(vertex_type_name, update_property_names,
                                 update_property_renames);
  label_t v_label = schema_.get_vertex_label_id(vertex_type_name);
  vertex_tables_[v_label].RenameProperties(update_property_names,
                                           update_property_renames);
  return neug::Status::OK();
}

Status PropertyGraph::RenameEdgeProperties(
    const std::string& src_type_name, const std::string& dst_type_name,
    const std::string& edge_type_name,
    const std::vector<std::pair<std::string, std::string>>& update_properties,
    bool error_on_conflict) {
  RETURN_IF_NOT_OK_CONFLICT(
      edge_triplet_check(src_type_name, dst_type_name, edge_type_name),
      error_on_conflict);
  std::vector<std::string> update_property_names;
  std::vector<std::string> update_property_renames;
  for (size_t i = 0; i < update_properties.size(); i++) {
    auto [property_name, property_rename] = update_properties[i];
    if (!schema_.edge_has_property(src_type_name, dst_type_name, edge_type_name,
                                   property_name)) {
      std::string msg = "Property [" + property_name +
                        "] does not exist in edge [" + edge_type_name +
                        "] from [" + src_type_name + "] to [" + dst_type_name +
                        "].";
      LOG(ERROR) << msg;
      if (error_on_conflict) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT, msg);
      } else {
        return Status(StatusCode::OK, msg);
      }
    }
    update_property_names.emplace_back(property_name);
    update_property_renames.emplace_back(property_rename);
  }
  schema_.RenameEdgeProperties(src_type_name, dst_type_name, edge_type_name,
                               update_property_names, update_property_renames);
  label_t src_label = schema_.get_vertex_label_id(src_type_name);
  label_t dst_label = schema_.get_vertex_label_id(dst_type_name);
  label_t e_label = schema_.get_edge_label_id(edge_type_name);
  size_t index = schema_.generate_edge_label(src_label, dst_label, e_label);
  if (edge_tables_.count(index) == 0) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge [" + edge_type_name + "] from [" + src_type_name +
                      "] to [" + dst_type_name +
                      "] does not exist, cannot rename properties.");
  }
  auto& edge_table = edge_tables_.at(index);

  edge_table.RenameProperties(update_property_names, update_property_renames);
  return neug::Status::OK();
}

Status PropertyGraph::delete_vertex_properties_check(
    const std::string& vertex_type_name, const std::vector<std::string>& props,
    bool error_on_conflict, std::vector<std::string>& valid_props) {
  RETURN_IF_NOT_OK_CONFLICT(vertex_label_check(vertex_type_name),
                            error_on_conflict);
  auto label_id = schema_.get_vertex_label_id(vertex_type_name);
  for (size_t i = 0; i < props.size(); i++) {
    auto property_name = props[i];
    if (!schema_.vertex_has_property_internal(label_id, property_name)) {
      std::string msg = "Property [" + property_name +
                        "] does not exist in vertex [" + vertex_type_name +
                        "].";
      if (error_on_conflict) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Property [" + property_name +
                          "] does not exist in vertex [" + vertex_type_name +
                          "].");
      }
      continue;
    }
    valid_props.emplace_back(property_name);
  }
  return neug::Status::OK();
}

Status PropertyGraph::DeleteVertexProperties(
    const std::string& vertex_type_name,
    const std::vector<std::string>& delete_properties, bool error_on_conflict) {
  std::vector<std::string> delete_property_names;
  auto status =
      delete_vertex_properties_check(vertex_type_name, delete_properties,
                                     error_on_conflict, delete_property_names);
  if (!status.ok()) {
    return status;
  }
  label_t v_label = schema_.get_vertex_label_id(vertex_type_name);

  schema_.DeleteVertexProperties(vertex_type_name, delete_property_names);
  vertex_tables_[v_label].DeleteProperties(delete_property_names);
  return neug::Status::OK();
}

Status PropertyGraph::delete_edge_properties_check(
    const std::string& src_type_name, const std::string& dst_type_name,
    const std::string& edge_type_name, const std::vector<std::string>& props,
    bool error_on_conflict, std::vector<std::string>& valid_props) {
  RETURN_IF_NOT_OK_CONFLICT(
      edge_triplet_check(src_type_name, dst_type_name, edge_type_name),
      error_on_conflict);
  label_t src_label = schema_.get_vertex_label_id_internal(src_type_name);
  label_t dst_label = schema_.get_vertex_label_id_internal(dst_type_name);
  label_t e_label = schema_.get_edge_label_id_internal(edge_type_name);

  for (size_t i = 0; i < props.size(); i++) {
    auto property_name = props[i];
    if (!schema_.edge_has_property_internal(src_label, dst_label, e_label,
                                            property_name)) {
      std::string msg = "Property [" + property_name +
                        "] does not exist in edge [" + edge_type_name +
                        "] from [" + src_type_name + "] to [" + dst_type_name +
                        "].";
      if (error_on_conflict) {
        LOG(ERROR) << msg;
        return Status(StatusCode::ERR_INVALID_ARGUMENT, msg);
      }
      continue;
    }
    valid_props.emplace_back(property_name);
  }
  return neug::Status::OK();
}

Status PropertyGraph::DeleteEdgeProperties(
    const std::string& src_type_name, const std::string& dst_type_name,
    const std::string& edge_type_name,
    const std::vector<std::string>& delete_properties, bool error_on_conflict) {
  std::vector<std::string> delete_property_names;
  RETURN_IF_NOT_OK_CONFLICT(
      delete_edge_properties_check(src_type_name, dst_type_name, edge_type_name,
                                   delete_properties, error_on_conflict,
                                   delete_property_names),
      error_on_conflict);
  label_t src_label = schema_.get_vertex_label_id_internal(src_type_name);
  label_t dst_label = schema_.get_vertex_label_id_internal(dst_type_name);
  label_t e_label = schema_.get_edge_label_id_internal(edge_type_name);
  size_t index = schema_.generate_edge_label(src_label, dst_label, e_label);
  // NOTE: We need to delete properties in edge table before updating schema,
  // since edge_tables_ use schema_ to determine delete logic.
  if (edge_tables_.count(index) == 0) {
    LOG(ERROR) << "Edge [" << edge_type_name << "] from [" << src_type_name
               << "] to [" << dst_type_name
               << "] does not exist, cannot delete properties.";
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge [" + edge_type_name + "] from [" + src_type_name +
                      "] to [" + dst_type_name +
                      "] does not exist, cannot delete properties.");
  }
  edge_tables_.at(index).DeleteProperties(delete_property_names);
  schema_.DeleteEdgeProperties(src_type_name, dst_type_name, edge_type_name,
                               delete_property_names);
  return neug::Status::OK();
}

Status PropertyGraph::DeleteVertexType(const std::string& vertex_type_name,
                                       bool error_on_conflict) {
  label_t v_label_id = schema_.get_vertex_label_id_internal(vertex_type_name);
  return DeleteVertexType(v_label_id, error_on_conflict);
}

Status PropertyGraph::DeleteVertexType(label_t v_label_id,
                                       bool error_on_conflict) {
  schema_.DeleteVertexLabel(v_label_id, false);
  vertex_tables_[v_label_id].Drop();

  for (label_t i = 0; i < vertex_label_total_count_; i++) {
    if (!schema_.vertex_label_valid(i)) {
      continue;
    }
    for (label_t j = 0; j < edge_label_total_count_; j++) {
      if (!schema_.edge_label_valid(j)) {
        continue;
      }
      if (schema_.exist(v_label_id, i, j)) {
        schema_.DeleteEdgeLabel(v_label_id, i, j);
        size_t index = schema_.generate_edge_label(v_label_id, i, j);
        if (edge_tables_.count(index) > 0) {
          edge_tables_.erase(index);
        }
      }
      if (schema_.exist(i, v_label_id, j)) {
        schema_.DeleteEdgeLabel(i, v_label_id, j);
        size_t index = schema_.generate_edge_label(i, v_label_id, j);
        if (edge_tables_.count(index) > 0) {
          edge_tables_.erase(index);
        }
      }
    }
  }

  return neug::Status::OK();
}

Status PropertyGraph::DeleteEdgeType(const std::string& src_vertex_type,
                                     const std::string& dst_vertex_type,
                                     const std::string& edge_type,
                                     bool error_on_conflict) {
  label_t src_v_label = schema_.get_vertex_label_id_internal(src_vertex_type);
  label_t dst_v_label = schema_.get_vertex_label_id_internal(dst_vertex_type);
  label_t edge_label = schema_.get_edge_label_id_internal(edge_type);
  return DeleteEdgeType(src_v_label, dst_v_label, edge_label,
                        error_on_conflict);
}
Status PropertyGraph::DeleteEdgeType(label_t src_v_label, label_t dst_v_label,
                                     label_t edge_label,
                                     bool error_on_conflict) {
  size_t index =
      schema_.generate_edge_label(src_v_label, dst_v_label, edge_label);
  schema_.DeleteEdgeLabel(src_v_label, dst_v_label, edge_label, false);
  if (edge_tables_.count(index) > 0) {
    edge_tables_.at(index).Drop();
    edge_tables_.erase(index);
  }
  return neug::Status::OK();
}

Status PropertyGraph::BatchDeleteVertices(label_t v_label_id,
                                          const std::vector<vid_t>& vids) {
  vertex_tables_[v_label_id].BatchDeleteVertices(vids);

  std::set<vid_t> vids_set(vids.begin(), vids.end());

  for (label_t i = 0; i < vertex_label_total_count_; i++) {
    if (!schema_.vertex_label_valid(i)) {
      continue;
    }
    for (label_t j = 0; j < edge_label_total_count_; j++) {
      if (schema_.has_edge_label(i, v_label_id, j)) {
        size_t index = schema_.generate_edge_label(i, v_label_id, j);
        edge_tables_.at(index).BatchDeleteVertices({}, vids_set);
      }
      if (schema_.has_edge_label(v_label_id, i, j)) {
        size_t index = schema_.generate_edge_label(v_label_id, i, j);
        edge_tables_.at(index).BatchDeleteVertices(vids_set, {});
      }
    }
  }

  return Status::OK();
}

Status PropertyGraph::DeleteVertex(label_t label, const Property& oid,
                                   timestamp_t ts) {
  vid_t lid;
  if (!vertex_tables_.at(label).get_index(oid, lid, ts)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Vertex oid does not exist.");
  }
  return DeleteVertex(label, lid, ts);
}

Status PropertyGraph::DeleteVertex(label_t label, vid_t lid, timestamp_t ts) {
  for (label_t i = 0; i < vertex_label_total_count_; i++) {
    if (!schema_.vertex_label_valid(i)) {
      continue;
    }
    for (label_t j = 0; j < edge_label_total_count_; j++) {
      if (schema_.has_edge_label(i, label, j)) {
        size_t index = schema_.generate_edge_label(i, label, j);
        assert(edge_tables_.count(index) > 0);
        edge_tables_.at(index).DeleteVertex(false, lid, ts);
      }
      if (schema_.has_edge_label(label, i, j)) {
        size_t index = schema_.generate_edge_label(label, i, j);
        assert(edge_tables_.count(index) > 0);
        edge_tables_.at(index).DeleteVertex(true, lid, ts);
      }
    }
  }
  vertex_tables_.at(label).DeleteVertex(lid, ts);
  return Status::OK();
}

Status PropertyGraph::DeleteEdge(label_t src_label, vid_t src_lid,
                                 label_t dst_label, vid_t dst_lid,
                                 label_t edge_label, int32_t oe_offset,
                                 int32_t ie_offset, timestamp_t ts) {
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge label does not exist.");
  }
  edge_tables_.at(index).DeleteEdge(src_lid, dst_lid, oe_offset, ie_offset, ts);
  return Status::OK();
}

Status PropertyGraph::BatchDeleteEdges(
    label_t src_v_label, label_t dst_v_label, label_t edge_label,
    const std::vector<std::tuple<vid_t, vid_t>>& edges_vec) {
  size_t index =
      schema_.generate_edge_label(src_v_label, dst_v_label, edge_label);
  std::vector<vid_t> src_vids, dst_vids;
  for (auto& edge : edges_vec) {
    src_vids.push_back(std::get<0>(edge));
    dst_vids.push_back(std::get<1>(edge));
  }
  edge_tables_.at(index).BatchDeleteEdges(src_vids, dst_vids);
  return Status::OK();
}

Status PropertyGraph::BatchDeleteEdges(
    label_t src_v_label, label_t dst_v_label, label_t edge_label,
    const std::vector<std::pair<vid_t, int32_t>>& oe_edges,
    const std::vector<std::pair<vid_t, int32_t>>& ie_edges) {
  size_t index =
      schema_.generate_edge_label(src_v_label, dst_v_label, edge_label);
  edge_tables_.at(index).BatchDeleteEdges(oe_edges, ie_edges);
  return Status::OK();
}

void PropertyGraph::DumpSchema() {
  auto _schema_path = schema_path(work_dir_);
  std::ofstream out(_schema_path);
  schema_.Serialize(out);
  out.flush();
  out.close();

  LOG(INFO) << "Dump schema to file: " << get_schema_yaml_path();
  std::string filename = get_schema_yaml_path();
  auto schema_res = schema_.to_yaml();
  if (!schema_res) {
    LOG(ERROR) << "Failed to dump schema to yaml: "
               << schema_res.error().error_message();
    return;
  }
  write_yaml_file(schema_res.value(), filename);
  LOG(INFO) << "Dump schema to yaml file: " << filename;
}

void PropertyGraph::Open(const Schema& schema, const std::string& work_dir,
                         int memory_level) {
  schema_ = schema;
  Open(work_dir, memory_level);
}

void PropertyGraph::Open(const std::string& work_dir, int memory_level) {
  // copy work_dir to work_dir_
  memory_level_ = memory_level;
  work_dir_.assign(work_dir);
  std::string schema_file = schema_path(work_dir_);
  std::string checkpoint_dir_path = checkpoint_dir(work_dir_);
  if (std::filesystem::exists(schema_file)) {
    loadSchema(schema_file);
  } else {
    LOG(INFO) << "Schema file not found, build empty graph";
    std::filesystem::create_directories(checkpoint_dir_path);
  }
  vertex_label_total_count_ = schema_.vertex_label_frontier();
  edge_label_total_count_ = schema_.edge_label_frontier();
  vertex_tables_.reserve(vertex_label_total_count_);
  std::shared_ptr<const VertexSchema> placeholder_vertex_schema;
  for (size_t i = 0; i < vertex_label_total_count_; i++) {
    if (schema_.vertex_label_valid(i)) {
      placeholder_vertex_schema = schema_.get_vertex_schema(i);
      break;
    }
  }
  for (size_t i = 0; i < vertex_label_total_count_; i++) {
    if (!schema_.vertex_label_valid(i)) {
      if (placeholder_vertex_schema == nullptr) {
        continue;
      }
      vertex_tables_.emplace_back(placeholder_vertex_schema);
      continue;
    }
    std::string v_label_name = schema_.get_vertex_label_name(i);
    auto properties = schema_.get_vertex_properties(i);
    auto property_names = schema_.get_vertex_property_names(i);
    auto property_strategies =
        schema_.get_vertex_storage_strategies(v_label_name);
    vertex_tables_.emplace_back(schema_.get_vertex_schema(i));
  }

  std::string tmp_dir_path = tmp_dir(work_dir_);

  if (std::filesystem::exists(tmp_dir_path)) {
    remove_directory(tmp_dir_path);
  }

  std::filesystem::create_directories(tmp_dir_path);

  std::vector<size_t> vertex_capacities(vertex_label_total_count_, 0);
  for (size_t i = 0; i < vertex_label_total_count_; ++i) {
    if (!schema_.vertex_label_valid(i)) {
      continue;
    }
    std::string v_label_name = schema_.get_vertex_label_name(i);

    vertex_tables_[i].Open(work_dir_, memory_level);
    // Case 1: Open from checkpoint, the capacity should be already reserved and
    // satisfied.
    // Case 2: Open from empty, Capacity should be the default minimum
    // capacity(4096)
    auto v_size = vertex_tables_[i].Size();
    vertex_tables_[i].EnsureCapacity(v_size < 4096 ? 4096
                                                   : v_size + v_size / 4);
    vertex_capacities[i] = vertex_tables_[i].Capacity();
  }

  for (size_t src_label_i = 0; src_label_i != vertex_label_total_count_;
       ++src_label_i) {
    if (!schema_.vertex_label_valid(src_label_i)) {
      continue;
    }
    std::string src_label =
        schema_.get_vertex_label_name(static_cast<label_t>(src_label_i));
    for (size_t dst_label_i = 0; dst_label_i != vertex_label_total_count_;
         ++dst_label_i) {
      if (!schema_.vertex_label_valid(dst_label_i)) {
        continue;
      }
      std::string dst_label =
          schema_.get_vertex_label_name(static_cast<label_t>(dst_label_i));
      for (size_t e_label_i = 0; e_label_i != edge_label_total_count_;
           ++e_label_i) {
        if (!schema_.edge_label_valid(e_label_i)) {
          continue;
        }
        std::string edge_label =
            schema_.get_edge_label_name(static_cast<label_t>(e_label_i));
        if (!schema_.exist(src_label, dst_label, edge_label)) {
          continue;
        }
        size_t index =
            schema_.generate_edge_label(src_label_i, dst_label_i, e_label_i);

        EdgeTable edge_table(
            schema_.get_edge_schema(src_label_i, dst_label_i, e_label_i));
        if (memory_level == 0) {
          edge_table.Open(work_dir_);
        } else if (memory_level >= 2) {
          edge_table.OpenWithHugepages(work_dir_);
        } else {
          edge_table.OpenInMemory(work_dir_);
        }
        auto e_size = edge_table.Size();
        size_t e_capacity = e_size < 4096 ? 4096 : e_size + (e_size + 4) / 5;
        edge_table.EnsureCapacity(vertex_capacities[src_label_i],
                                  vertex_capacities[dst_label_i], e_capacity);
        edge_tables_.emplace(index, std::move(edge_table));
      }
    }
  }
  v_mutex_.resize(vertex_label_total_count_);
  for (size_t i = 0; i < vertex_label_total_count_; ++i) {
    v_mutex_[i] = std::make_shared<std::mutex>();
  }
}

void PropertyGraph::compact_schema() {
  auto new_schema = schema_.Compact();
  std::vector<VertexTable> new_vertex_tables;
  std::unordered_map<uint32_t, EdgeTable> new_edge_tables;

  for (size_t old_v_label = 0; old_v_label != vertex_label_total_count_;
       ++old_v_label) {
    if (schema_.vertex_label_valid(old_v_label)) {
      auto src_name = schema_.get_vertex_label_name(old_v_label);
      size_t cur_new_label_id =
          new_schema.get_vertex_label_id_internal(src_name);
      new_vertex_tables.emplace_back(
          new_schema.get_vertex_schema(cur_new_label_id));
      new_vertex_tables.back().Swap(vertex_tables_[old_v_label]);
      // Update the handle to VertexSchema for the new vertex table.
      // The soft deleted properties should be removed physically in this step.
      new_vertex_tables.back().SetVertexSchema(
          new_schema.get_vertex_schema(cur_new_label_id));
    }
  }
  assert(new_vertex_tables.size() == new_schema.vertex_label_frontier());
  for (size_t old_src_label = 0; old_src_label != vertex_label_total_count_;
       ++old_src_label) {
    if (!schema_.vertex_label_valid(old_src_label)) {
      continue;
    }
    auto src_name = schema_.get_vertex_label_name(old_src_label);
    for (size_t old_dst_label = 0; old_dst_label != vertex_label_total_count_;
         ++old_dst_label) {
      if (!schema_.vertex_label_valid(old_dst_label)) {
        continue;
      }
      auto dst_name = schema_.get_vertex_label_name(old_dst_label);
      for (size_t old_e_label = 0; old_e_label != edge_label_total_count_;
           ++old_e_label) {
        if (!schema_.edge_label_valid(old_e_label) ||
            !schema_.exist(old_src_label, old_dst_label, old_e_label)) {
          continue;
        }
        auto e_name = schema_.get_edge_label_name(old_e_label);
        size_t old_index = schema_.generate_edge_label(
            old_src_label, old_dst_label, old_e_label);
        size_t new_src_label =
            new_schema.get_vertex_label_id_internal(src_name);
        size_t new_dst_label =
            new_schema.get_vertex_label_id_internal(dst_name);
        size_t new_e_label = new_schema.get_edge_label_id_internal(e_name);
        size_t new_index = new_schema.generate_edge_label(
            new_src_label, new_dst_label, new_e_label);
        new_edge_tables.emplace(
            new_index, EdgeTable(new_schema.get_edge_schema(
                           new_src_label, new_dst_label, new_e_label)));
        new_edge_tables.at(new_index).Swap(edge_tables_.at(old_index));
        new_edge_tables.at(new_index).SetEdgeSchema(new_schema.get_edge_schema(
            new_src_label, new_dst_label, new_e_label));
      }
    }
  }

  vertex_label_total_count_ = new_schema.vertex_label_frontier();
  edge_label_total_count_ = new_schema.edge_label_frontier();
  schema_ = new_schema;
  vertex_tables_.swap(new_vertex_tables);
  edge_tables_.swap(new_edge_tables);
  v_mutex_.resize(new_schema.vertex_label_frontier());
}

void PropertyGraph::Compact(bool compact_csr, float reserve_ratio,
                            timestamp_t ts) {
  /**
   * The compaction process includes two parts:
   * 1. Schema: remove the deleted properties and labels from
   *    schema.
   * 2. Data: for each vertex and edge table, remove the deleted
   *    data and compact the storage.
   *
   * Assume concurrency is controlled by the caller.
   */
  compact_schema();
  for (size_t src_label_i = 0; src_label_i != vertex_label_total_count_;
       ++src_label_i) {
    if (schema_.vertex_label_valid(src_label_i)) {
      vertex_tables_[src_label_i].Compact(ts);
    } else {
      continue;
    }
    for (size_t dst_label_i = 0; dst_label_i != vertex_label_total_count_;
         ++dst_label_i) {
      if (!schema_.vertex_label_valid(dst_label_i)) {
        continue;
      }
      for (size_t e_label_i = 0; e_label_i != edge_label_total_count_;
           ++e_label_i) {
        if (schema_.edge_label_valid(e_label_i) &&
            schema_.exist(src_label_i, dst_label_i, e_label_i)) {
          size_t index =
              schema_.generate_edge_label(src_label_i, dst_label_i, e_label_i);
          bool sort_on_compaction = schema_.get_sort_on_compaction(
              src_label_i, dst_label_i, e_label_i);
          if (edge_tables_.count(index) > 0) {
            auto& edge_table = edge_tables_.at(index);
            edge_table.Compact(compact_csr, sort_on_compaction, ts);
          }
        }
      }
    }
  }
  LOG(INFO) << "Compaction completed.";
}

void PropertyGraph::Dump(bool reopen) {
  const auto dump_start = std::chrono::steady_clock::now();
  // First dump to the  temp dir, then move to the checkpoint dir
  std::string target_dir = temp_checkpoint_dir(work_dir_);
  if (std::filesystem::exists(target_dir)) {
    remove_directory(target_dir);
  } else {
    std::filesystem::create_directories(target_dir);
  }

  std::error_code errorCode;
  std::filesystem::create_directories(target_dir, errorCode);
  if (errorCode) {
    std::stringstream ss;
    ss << "Failed to create snapshot directory: " << target_dir << ", "
       << errorCode.message();
    LOG(ERROR) << ss.str();
    THROW_RUNTIME_ERROR(ss.str());
  }
  for (size_t i = 0; i < vertex_label_total_count_; ++i) {
    if (!vertex_tables_[i].is_dropped()) {
      const auto table_start = std::chrono::steady_clock::now();
      vertex_tables_[i].Dump(target_dir);
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - table_start)
                               .count();
      LOG(INFO) << "Checkpoint vertex table "
                << schema_.get_vertex_label_name(static_cast<label_t>(i))
                << " dumped in " << elapsed << " ms";
    }
  }

  for (size_t src_label_i = 0; src_label_i != vertex_label_total_count_;
       ++src_label_i) {
    if (!schema_.vertex_label_valid(src_label_i)) {
      continue;
    }
    std::string src_label =
        schema_.get_vertex_label_name(static_cast<label_t>(src_label_i));
    for (size_t dst_label_i = 0; dst_label_i != vertex_label_total_count_;
         ++dst_label_i) {
      if (!schema_.vertex_label_valid(dst_label_i)) {
        continue;
      }
      std::string dst_label =
          schema_.get_vertex_label_name(static_cast<label_t>(dst_label_i));
      for (size_t e_label_i = 0; e_label_i != edge_label_total_count_;
           ++e_label_i) {
        if (!schema_.edge_label_valid(e_label_i)) {
          continue;
        }
        std::string edge_label =
            schema_.get_edge_label_name(static_cast<label_t>(e_label_i));
        if (!schema_.exist(src_label, dst_label, edge_label) ||
            !schema_.edge_triplet_valid(src_label_i, dst_label_i, e_label_i)) {
          continue;
        }
        size_t index =
            schema_.generate_edge_label(src_label_i, dst_label_i, e_label_i);
        if (edge_tables_.count(index) > 0) {
          const auto table_start = std::chrono::steady_clock::now();
          auto& edge_table = edge_tables_.at(index);
          edge_table.Dump(target_dir);
          const auto elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - table_start)
                  .count();
          LOG(INFO) << "Checkpoint edge table " << src_label << "-["
                    << edge_label << "]->" << dst_label << " dumped in "
                    << elapsed << " ms";
        }
      }
    }
  }
  DumpSchema();
  const auto promotion_start = std::chrono::steady_clock::now();
  copy_directory(target_dir, checkpoint_dir(work_dir_), true, true);
  const auto promotion_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - promotion_start)
          .count();
  LOG(INFO) << "Checkpoint directory promoted in " << promotion_elapsed
            << " ms";
  remove_directory(target_dir);
  remove_directory(tmp_dir(work_dir_));
  remove_directory(wal_dir(work_dir_));
  LOG(INFO) << "Dump graph to " << checkpoint_dir(work_dir_);
  Clear();
  if (reopen) {
    Open(work_dir_, memory_level_);
  }
  const auto total_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - dump_start)
          .count();
  if (checkpoint_complete_callback_) checkpoint_complete_callback_();
  LOG(INFO) << "Checkpoint completed in " << total_elapsed << " ms";
}

const Schema& PropertyGraph::schema() const { return schema_; }

Schema& PropertyGraph::mutable_schema() { return schema_; }

vid_t PropertyGraph::LidNum(label_t vertex_label) const {
  return vertex_tables_[vertex_label].LidNum();
}

vid_t PropertyGraph::VertexNum(label_t vertex_label, timestamp_t ts) const {
  return vertex_tables_[vertex_label].VertexNum(ts);
}

bool PropertyGraph::IsValidLid(label_t vertex_label, vid_t lid,
                               timestamp_t ts) const {
  return vertex_tables_[vertex_label].IsValidLid(lid, ts);
}

size_t PropertyGraph::EdgeNum(label_t src_label, label_t edge_label,
                              label_t dst_label) const {
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) > 0) {
    return edge_tables_.at(index).EdgeNum();
  } else {
    return 0;
  }
}

bool PropertyGraph::get_lid(label_t label, const Property& oid, vid_t& lid,
                            timestamp_t ts) const {
  return vertex_tables_[label].get_index(oid, lid, ts);
}

Property PropertyGraph::GetOid(label_t label, vid_t lid, timestamp_t ts) const {
  return vertex_tables_[label].GetOid(lid, ts);
}

Status PropertyGraph::AddVertex(label_t label, const Property& id,
                                const std::vector<Property>& props, vid_t& ret,
                                timestamp_t ts, bool insert_safe) {
  if (!vertex_tables_[label].AddVertex(id, props, ret, ts, insert_safe)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT, "Fail to add vertex.");
  }
  return Status::OK();
}

int32_t PropertyGraph::AddEdge(label_t src_label, vid_t src_lid,
                               label_t dst_label, vid_t dst_lid,
                               label_t edge_label,
                               const std::vector<Property>& properties,
                               timestamp_t ts, Allocator& alloc,
                               bool insert_safe) {
  return AddEdgeWithOffsets(src_label, src_lid, dst_label, dst_lid, edge_label,
                            properties, ts, alloc, insert_safe)
      .first;
}

std::pair<int32_t, int32_t> PropertyGraph::AddEdgeWithOffsets(
    label_t src_label, vid_t src_lid, label_t dst_label, vid_t dst_lid,
    label_t edge_label, const std::vector<Property>& properties,
    timestamp_t ts, Allocator& alloc, bool insert_safe) {
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    LOG(ERROR) << "Edge table does not exist for edge label: " << edge_label;
    THROW_INVALID_ARGUMENT_EXCEPTION("Edge table does not exist for label <" +
                                     std::to_string(src_label) + ", " +
                                     std::to_string(dst_label) + ", " +
                                     std::to_string(edge_label) + ">");
  }
  auto& edge_table = edge_tables_.at(index);
  // FILL/COPY can rebuild a CSR to the current vertex count while the vertex
  // table retains spare capacity. A subsequent AddVertex then needs no table
  // growth, so ensure both endpoint CSRs here before inserting its first edge.
  edge_table.EnsureCapacity(vertex_tables_[src_label].Capacity(),
                            vertex_tables_[dst_label].Capacity());
  return edge_table.AddEdgeWithOffsets(src_lid, dst_lid, properties, ts, alloc,
                                       insert_safe);
}

void PropertyGraph::BatchAddEdges(
    label_t src_label, label_t dst_label, label_t edge_label,
    const std::vector<std::tuple<vid_t, vid_t, std::vector<Property>>>& edges,
    timestamp_t ts, Allocator& alloc) {
  size_t index = schema_.generate_edge_label(src_label, dst_label, edge_label);
  if (edge_tables_.count(index) == 0) {
    LOG(ERROR) << "Edge table does not exist for edge label: " << edge_label;
    THROW_INVALID_ARGUMENT_EXCEPTION("Edge table does not exist for label <" +
                                     std::to_string(src_label) + ", " +
                                     std::to_string(dst_label) + ", " +
                                     std::to_string(edge_label) + ">");
  }
  auto& edge_table = edge_tables_.at(index);
  edge_table.EnsureCapacity(vertex_tables_[src_label].Capacity(),
                            vertex_tables_[dst_label].Capacity());
  edge_table.BatchAddEdges(edges, ts, alloc);
}

Status PropertyGraph::UpdateVertexProperty(label_t v_label, vid_t vid,
                                           int32_t prop_id,
                                           const Property& value,
                                           timestamp_t ts) {
  assert(prop_id >= 0);
  assert(schema_.vertex_label_valid(v_label));
  if (!vertex_tables_[v_label].UpdateProperty(vid, prop_id, value, ts)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Fail to update vertex property.");
  }
  return neug::Status::OK();
}

Status PropertyGraph::BatchUpdateVertexProperties(
    label_t v_label, std::vector<std::string> property_names,
    std::shared_ptr<IRecordBatchSupplier> supplier, timestamp_t ts) {
  if (!schema_.vertex_label_valid(v_label)) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Invalid vertex label: " + std::to_string(v_label));
  }
  if (supplier == nullptr) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Record batch supplier is null.");
  }
  if (property_names.empty()) {
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Batch vertex property update expects at least 1 property.");
  }

  const auto schema_property_names = schema_.get_vertex_property_names(v_label);
  const auto schema_property_types = schema_.get_vertex_properties(v_label);
  const auto pk_name = schema_.get_vertex_primary_key_name(v_label);
  const auto pk_type = std::get<0>(schema_.get_vertex_primary_key(v_label)[0])
                           .id();

  std::vector<int32_t> prop_col_ids;
  prop_col_ids.reserve(property_names.size());
  for (const auto& prop_name : property_names) {
    if (prop_name == pk_name) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Cannot update primary key property: " + prop_name);
    }
    auto it = std::find(schema_property_names.begin(),
                        schema_property_names.end(), prop_name);
    if (it == schema_property_names.end()) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Property [" + prop_name +
                        "] does not exist in vertex type");
    }
    prop_col_ids.emplace_back(
        static_cast<int32_t>(std::distance(schema_property_names.begin(), it)));
  }

  auto& vertex_table = vertex_tables_.at(v_label);
  while (true) {
    auto batch = supplier->GetNextBatch();
    if (!batch) {
      break;
    }
    const int expected_cols = static_cast<int>(1 + property_names.size());
    if (batch->num_columns() != expected_cols) {
      return Status(StatusCode::ERR_INVALID_ARGUMENT,
                    "Vertex property update expects " +
                        std::to_string(expected_cols) +
                        " columns (id, values...), got " +
                        std::to_string(batch->num_columns()));
    }

    std::vector<vid_t> vids;
    vids.reserve(static_cast<size_t>(batch->num_rows()));
    for (int64_t row = 0; row < batch->num_rows(); ++row) {
      Property oid;
      auto status =
          ReadVertexOidFromArrowArray(batch->column(0), row, pk_type, oid);
      if (!status.ok()) {
        return status;
      }
      vid_t lid = std::numeric_limits<vid_t>::max();
      if (!vertex_table.get_index(oid, lid, ts)) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Vertex does not exist for id: " + oid.to_string());
      }
      vids.emplace_back(lid);
    }

    std::vector<std::shared_ptr<arrow::Array>> property_arrays;
    property_arrays.reserve(prop_col_ids.size());
    bool can_write_directly = true;
    for (size_t p = 0; p < prop_col_ids.size(); ++p) {
      const auto col_id = prop_col_ids[p];
      if (col_id < 0 ||
          static_cast<size_t>(col_id) >= schema_property_types.size()) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "Invalid vertex property column id");
      }
      auto array = batch->column(static_cast<int>(1 + p));
      if (array->null_count() > 0) {
        return Status(StatusCode::ERR_INVALID_ARGUMENT,
                      "NULL property value is not supported.");
      }
      property_arrays.emplace_back(array);
      can_write_directly =
          can_write_directly &&
          CanWriteArrowArrayDirectly(
              array, schema_property_types[static_cast<size_t>(col_id)].id());
    }

    if (!can_write_directly) {
      for (int64_t row = 0; row < batch->num_rows(); ++row) {
        for (size_t p = 0; p < prop_col_ids.size(); ++p) {
          const auto col_id = prop_col_ids[p];
          Property value;
          auto status = ReadPropertyFromArrowArray(
              property_arrays[p], row,
              schema_property_types[static_cast<size_t>(col_id)].id(), value);
          if (!status.ok()) {
            return status;
          }
          RETURN_IF_NOT_OK(
              UpdateVertexProperty(v_label, vids[row], col_id, value, ts));
        }
      }
      continue;
    }

    for (size_t p = 0; p < prop_col_ids.size(); ++p) {
      const auto col_id = prop_col_ids[p];
      auto col = vertex_table.get_property_column(col_id);
      set_properties_column(
          col, std::make_shared<arrow::ChunkedArray>(property_arrays[p]), vids);
    }
  }
  return Status::OK();
}

Status PropertyGraph::UpdateEdgeProperty(label_t src_v_label, vid_t src_vid,
                                         label_t dst_v_label, vid_t dst_vid,
                                         label_t e_label, int32_t oe_offset,
                                         int32_t ie_offset, int32_t prop_id,
                                         const Property& value,
                                         timestamp_t ts) {
  assert(prop_id >= 0);
  assert(schema_.edge_label_valid(e_label));
  size_t index = schema_.generate_edge_label(src_v_label, dst_v_label, e_label);
  if (edge_tables_.count(index) == 0) {
    LOG(ERROR) << "Edge table does not exist for edge label: " << e_label;
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge table does not exist for label <" +
                      std::to_string(src_v_label) + ", " +
                      std::to_string(dst_v_label) + ", " +
                      std::to_string(e_label) + ">");
  }
  edge_tables_.at(index).UpdateEdgeProperty(src_vid, dst_vid, oe_offset,
                                            ie_offset, prop_id, value, ts);
  return neug::Status::OK();
}

std::string PropertyGraph::get_statistics_json() const {
  return get_statistics_json(DegreeStatisticsLimits{});
}

std::string PropertyGraph::get_statistics_json(
    const DegreeStatisticsLimits& limits) const {
  // Generate json string using rapidjson document
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
  size_t vertex_count = 0;
  rapidjson::Value vertex_type_statistics(rapidjson::kArrayType);
  auto label_ids = schema_.get_vertex_label_ids();
  for (const auto& label_id : label_ids) {
    auto label_name = schema_.get_vertex_label_name(label_id);
    rapidjson::Value vertex_type_stat(rapidjson::kObjectType);
    vertex_type_stat.AddMember(
        "type_id", rapidjson::Value().SetUint64(label_id), allocator);
    vertex_type_stat.AddMember(
        "type_name",
        rapidjson::Value().SetString(label_name.c_str(), allocator), allocator);
    size_t count = VertexNum(label_id, MAX_TIMESTAMP);
    vertex_type_stat.AddMember("count", rapidjson::Value().SetUint64(count),
                               allocator);
    vertex_count += count;
    vertex_type_statistics.PushBack(vertex_type_stat, allocator);
  }
  document.AddMember("vertex_type_statistics", vertex_type_statistics,
                     allocator);
  size_t edge_count = 0;
  rapidjson::Value edge_type_statistics(rapidjson::kArrayType);
  std::unordered_map<uint32_t, size_t> edge_count_map;
  for (const auto& iter : edge_tables_) {
    edge_count_map.emplace(iter.first, iter.second.EdgeNum());
  }
  for (label_t edge_label = 0; edge_label < edge_label_total_count_;
       ++edge_label) {
    if (!schema_.edge_label_valid(edge_label)) {
      continue;
    }
    auto edge_label_name = schema_.get_edge_label_name(edge_label);
    rapidjson::Value edge_type_stat(rapidjson::kObjectType);
    edge_type_stat.AddMember(
        "type_id", rapidjson::Value().SetUint64(edge_label), allocator);
    edge_type_stat.AddMember(
        "type_name",
        rapidjson::Value().SetString(edge_label_name.c_str(), allocator),
        allocator);
    rapidjson::Value vertex_type_pair_statistics(rapidjson::kArrayType);
    for (label_t src_label = 0; src_label < vertex_label_total_count_;
         ++src_label) {
      if (!schema_.vertex_label_valid(src_label)) {
        continue;
      }
      auto src_label_name = schema_.get_vertex_label_name(src_label);
      for (label_t dst_label = 0; dst_label < vertex_label_total_count_;
           ++dst_label) {
        if (!schema_.vertex_label_valid(dst_label)) {
          continue;
        }
        if (!schema_.exist(src_label, dst_label, edge_label)) {
          continue;
        }
        auto dst_label_name = schema_.get_vertex_label_name(dst_label);
        auto edge_triplet_id =
            schema_.generate_edge_label(src_label, dst_label, edge_label);
        assert(edge_count_map.find(edge_triplet_id) != edge_count_map.end());
        size_t value = edge_count_map.at(edge_triplet_id);

        rapidjson::Value vertex_type_pair_stat(rapidjson::kObjectType);
        vertex_type_pair_stat.AddMember(
            "source_vertex",
            rapidjson::Value().SetString(src_label_name.c_str(), allocator),
            allocator);
        vertex_type_pair_stat.AddMember(
            "destination_vertex",
            rapidjson::Value().SetString(dst_label_name.c_str(), allocator),
            allocator);
        vertex_type_pair_stat.AddMember(
            "count", rapidjson::Value().SetUint64(value), allocator);
        edge_count += value;
        vertex_type_pair_statistics.PushBack(vertex_type_pair_stat, allocator);
      }
    }
    if (vertex_type_pair_statistics.Empty()) {
      continue;
    } else {
      edge_type_stat.AddMember("vertex_type_pair_statistics",
                               vertex_type_pair_statistics, allocator);
      edge_type_statistics.PushBack(edge_type_stat, allocator);
    }
  }

  document.AddMember("edge_type_statistics", edge_type_statistics, allocator);
  document.AddMember("total_vertex_count",
                     rapidjson::Value().SetUint64(vertex_count), allocator);
  document.AddMember("total_edge_count",
                     rapidjson::Value().SetUint64(edge_count), allocator);
  // Shared, bounded graph statistics. These describe visible graph records,
  // never query latency or a particular rewrite. Typed incidences remain
  // directed, including self-relations; callers must prove BOTH semantics.
  struct DegreeEndpoint {
    std::string relationship, source, destination, direction;
    label_t bound, relationLabel;
    std::vector<uint64_t> degrees;
    uint64_t population = 0, edges = 0, nonzero = 0;
    uint64_t maximumDegree = 0;
    std::optional<uint64_t> distinctEndpointPairs = uint64_t{0};
  };
  constexpr uint64_t MAX_ENDPOINTS_PER_VERTEX = 32;
  constexpr size_t MAX_DISTINCT_NEIGHBORS_PER_OWNER = 65'536;
  uint64_t vertexSlots = 0, edgeVisits = 0, pairVisits = 0;
  bool degreeComplete = true;
  bool distinctPairsComplete = true;
  std::map<label_t, std::vector<DegreeEndpoint>> degreeEndpoints;
  for (const auto& relation : document["edge_type_statistics"].GetArray()) {
    const std::string relationName = relation["type_name"].GetString();
    const auto edgeLabel = static_cast<label_t>(relation["type_id"].GetUint64());
    for (const auto& pair : relation["vertex_type_pair_statistics"].GetArray()) {
      const std::string source = pair["source_vertex"].GetString();
      const std::string destination = pair["destination_vertex"].GetString();
      const auto sourceLabel = schema_.get_vertex_label_id(source);
      const auto destinationLabel = schema_.get_vertex_label_id(destination);
      for (const bool outgoing : {true, false}) {
        const auto bound = outgoing ? sourceLabel : destinationLabel;
        const auto neighbor = outgoing ? destinationLabel : sourceLabel;
        const auto slots = static_cast<uint64_t>(LidNum(bound));
        const auto expectedEdges = pair["count"].GetUint64();
        auto& group = degreeEndpoints[bound];
        if (group.size() >= MAX_ENDPOINTS_PER_VERTEX ||
            slots > limits.vertex_slots - vertexSlots ||
            expectedEdges > limits.edge_visits - edgeVisits) {
          degreeComplete = false;
          continue;
        }
        vertexSlots += slots;
        DegreeEndpoint endpoint{relationName, source, destination,
            outgoing ? "OUT" : "IN", bound, edgeLabel, std::vector<uint64_t>(slots, 0)};
        const auto view = outgoing
            ? GetGenericOutgoingGraphView(bound, neighbor, edgeLabel, MAX_TIMESTAMP)
            : GetGenericIncomingGraphView(bound, neighbor, edgeLabel, MAX_TIMESTAMP);
        bool bounded = true;
        // Mutable adjacency is not sorted. Deduplicate neighbors per owner,
        // bounding auxiliary memory independently of the degree synopsis.
        std::unordered_set<vid_t> distinctNeighbors;
        for (const auto vertex : GetVertexSet(bound, MAX_TIMESTAMP)) {
          ++endpoint.population;
          uint64_t degree = 0;
          distinctNeighbors.clear();
          for (const auto other : view.get_edges(vertex)) {
            if (edgeVisits == limits.edge_visits) { bounded = false; break; }
            ++edgeVisits;
            if (other < LidNum(neighbor) && IsValidLid(neighbor, other, MAX_TIMESTAMP)) {
              ++degree;
              if (endpoint.distinctEndpointPairs && !distinctNeighbors.contains(other)) {
                if (distinctNeighbors.size() == MAX_DISTINCT_NEIGHBORS_PER_OWNER) {
                  endpoint.distinctEndpointPairs.reset();
                  distinctPairsComplete = false;
                  distinctNeighbors.clear();
                } else {
                  distinctNeighbors.insert(other);
                  ++*endpoint.distinctEndpointPairs;
                }
              }
            }
          }
          if (!bounded) break;
          endpoint.degrees[vertex] = degree;
          endpoint.edges += degree;
          endpoint.nonzero += degree != 0;
          endpoint.maximumDegree = std::max(endpoint.maximumDegree, degree);
        }
        // A partially visited incidence is unknown, not an observed zero.
        if (!bounded || endpoint.edges != expectedEdges) {
          degreeComplete = false;
          continue;
        }
        group.push_back(std::move(endpoint));
      }
    }
  }
  auto endpointIdentity = [&](const DegreeEndpoint& endpoint) {
    rapidjson::Value value(rapidjson::kObjectType);
    value.AddMember("relationship", rapidjson::Value(endpoint.relationship.c_str(), allocator), allocator);
    value.AddMember("source_vertex", rapidjson::Value(endpoint.source.c_str(), allocator), allocator);
    value.AddMember("destination_vertex", rapidjson::Value(endpoint.destination.c_str(), allocator), allocator);
    value.AddMember("direction", rapidjson::Value(endpoint.direction.c_str(), allocator), allocator);
    return value;
  };
  rapidjson::Value degreeStats(rapidjson::kObjectType);
  rapidjson::Value endpoints(rapidjson::kArrayType), degreePairs(rapidjson::kArrayType);
  std::vector<std::pair<const DegreeEndpoint*, const DegreeEndpoint*>> pairRequests;
  for (const auto& [label, group] : degreeEndpoints) {
    for (const auto& endpoint : group) {
      auto value = endpointIdentity(endpoint);
      value.AddMember("vertex_count", endpoint.population, allocator);
      value.AddMember("edge_count", endpoint.edges, allocator);
      value.AddMember("nonzero_vertex_count", endpoint.nonzero, allocator);
      value.AddMember("max_degree", endpoint.maximumDegree, allocator);
      if (endpoint.distinctEndpointPairs)
        value.AddMember("distinct_endpoint_pair_count", *endpoint.distinctEndpointPairs, allocator);
      endpoints.PushBack(value, allocator);
    }
    for (size_t i = 0; i < group.size(); ++i)
      for (size_t j = i; j < group.size(); ++j)
        pairRequests.emplace_back(&group[i], &group[j]);
  }
  // Relationships added by FILL get later label IDs. Complete all pre-existing
  // base pairs before considering any pair containing a new derived relation;
  // otherwise a global work cap could erase a later vertex type's base evidence.
  auto pairOrder = [](const auto& pair) {
    return std::make_tuple(std::max(pair.first->relationLabel, pair.second->relationLabel),
        std::min(pair.first->relationLabel, pair.second->relationLabel),
        pair.first->bound, pair.first->direction, pair.second->direction);
  };
  std::stable_sort(pairRequests.begin(), pairRequests.end(), [&](const auto& a, const auto& b) {
    return pairOrder(a) < pairOrder(b);
  });
  for (const auto& request : pairRequests) {
    const auto& a = *request.first;
    const auto& b = *request.second;
    if (a.degrees.size() > limits.pair_vertex_visits - pairVisits) {
      degreeComplete = false;
      continue;
    }
    pairVisits += a.degrees.size();
    unsigned __int128 product = 0, presentAB = 0, presentBA = 0;
    uint64_t positiveBoth = 0;
    for (size_t v = 0; v < a.degrees.size(); ++v) {
      const auto x = a.degrees[v], y = b.degrees[v];
      product += static_cast<unsigned __int128>(x) * y;
      if (x) presentAB += y;
      if (y) presentBA += x;
      positiveBoth += x != 0 && y != 0;
    }
    const auto maximum = std::numeric_limits<uint64_t>::max();
    if (product > maximum || presentAB > maximum || presentBA > maximum) {
      degreeComplete = false;
      continue;
    }
    auto emitPair = [&](const DegreeEndpoint& first, const DegreeEndpoint& second,
                uint64_t presence) {
      rapidjson::Value value(rapidjson::kObjectType);
      value.AddMember("first", endpointIdentity(first), allocator);
      value.AddMember("second", endpointIdentity(second), allocator);
      value.AddMember("degree_product_sum", static_cast<uint64_t>(product), allocator);
      value.AddMember("present_first_degree_second_sum", presence, allocator);
      value.AddMember("positive_both_count", positiveBoth, allocator);
      degreePairs.PushBack(value, allocator);
    };
    emitPair(a, b, static_cast<uint64_t>(presentAB));
    if (request.first != request.second) emitPair(b, a, static_cast<uint64_t>(presentBA));
  }
  degreeStats.AddMember("version", 1, allocator);
  degreeStats.AddMember("scope", "visible_typed_directed_endpoint_degrees", allocator);
  degreeStats.AddMember("complete", degreeComplete, allocator);
  degreeStats.AddMember("distinct_pair_count_complete", degreeComplete && distinctPairsComplete, allocator);
  degreeStats.AddMember("maximum_distinct_neighbors_per_owner", uint64_t(MAX_DISTINCT_NEIGHBORS_PER_OWNER), allocator);
  degreeStats.AddMember("maximum_endpoints_per_vertex_type", MAX_ENDPOINTS_PER_VERTEX, allocator);
  rapidjson::Value collectionBudget(rapidjson::kObjectType);
  collectionBudget.AddMember("vertex_slots", limits.vertex_slots, allocator);
  collectionBudget.AddMember("edge_visits", limits.edge_visits, allocator);
  collectionBudget.AddMember("pair_vertex_visits", limits.pair_vertex_visits,
                             allocator);
  collectionBudget.AddMember("property_vertex_visits", limits.property_vertex_visits, allocator);
  collectionBudget.AddMember("property_max_distinct_values", limits.property_max_distinct_values, allocator);
  degreeStats.AddMember("collection_budget", collectionBudget, allocator);
  degreeStats.AddMember("degree_vertex_slots", vertexSlots, allocator);
  degreeStats.AddMember("degree_edge_visits", edgeVisits, allocator);
  degreeStats.AddMember("pair_vertex_visits", pairVisits, allocator);
  degreeStats.AddMember("endpoints", endpoints, allocator);
  degreeStats.AddMember("pairs", degreePairs, allocator);
  document.AddMember("degree_statistics", degreeStats, allocator);

  // Exact bounded source synopses, never observations of a materialized view.
  // Each published property contains its entire visible population. A scan
  // that exhausts either resource limit is omitted, so a missing value in a
  // published histogram means zero whereas a missing property means unknown.
  struct ConditionalDegree {
    uint64_t edges = 0, nonzero = 0, maximum = 0;
    unsigned __int128 squares = 0;
  };
  struct PropertyFrequency {
    uint64_t count = 0;
    std::vector<ConditionalDegree> degrees;
  };
  uint64_t propertyVisits = 0;
  rapidjson::Value propertyStats(rapidjson::kObjectType);
  rapidjson::Value properties(rapidjson::kArrayType);
  // Low-population tables yield complete generic dictionaries even when a
  // large message table cannot fit the same source-statistics work budget.
  std::stable_sort(label_ids.begin(), label_ids.end(), [&](label_t a, label_t b) {
    return std::make_pair(VertexNum(a, MAX_TIMESTAMP), a) <
           std::make_pair(VertexNum(b, MAX_TIMESTAMP), b);
  });
  for (const auto label : label_ids) {
    const auto population = VertexNum(label, MAX_TIMESTAMP);
    const auto names = schema_.get_vertex_property_names(label);
    const auto types = schema_.get_vertex_properties_id(label);
    const auto& group = degreeEndpoints[label];
    for (size_t property = 0; property < names.size(); ++property) {
      const auto type = types[property];
      const char* typeName = type == DataTypeId::kVarchar ? "STRING" :
          type == DataTypeId::kInt64 ? "INT64" :
          type == DataTypeId::kInt32 ? "INT32" :
          type == DataTypeId::kBoolean ? "BOOL" : nullptr;
      if (typeName == nullptr || limits.property_vertex_visits == 0 ||
          population > limits.property_vertex_visits - propertyVisits)
        continue;
      auto column = GetVertexPropertyColumn(label, property);
      if (!column) continue;
      std::map<std::string, PropertyFrequency> frequencies;
      uint64_t nulls = 0, visited = 0;
      bool complete = true;
      for (const auto vertex : GetVertexSet(label, MAX_TIMESTAMP)) {
        if (propertyVisits == limits.property_vertex_visits) {
          complete = false;
          break;
        }
        ++propertyVisits;
        ++visited;
        const auto value = column->get_prop(vertex);
        if (value.type() == DataTypeId::kEmpty) { ++nulls; continue; }
        if (value.type() != type) { complete = false; break; }
        const std::string key = type == DataTypeId::kVarchar ?
            std::string(value.as_string_view()) : type == DataTypeId::kInt64 ?
            std::to_string(value.as_int64()) : type == DataTypeId::kInt32 ?
            std::to_string(value.as_int32()) : value.as_bool() ? "true" : "false";
        auto found = frequencies.find(key);
        if (found == frequencies.end()) {
          if (frequencies.size() == limits.property_max_distinct_values) {
            complete = false;
            break;
          }
          found = frequencies.emplace(key, PropertyFrequency{
              0, std::vector<ConditionalDegree>(group.size())}).first;
        }
        auto& frequency = found->second;
        ++frequency.count;
        for (size_t i = 0; i < group.size(); ++i) {
          const auto degree = group[i].degrees[vertex];
          auto& conditional = frequency.degrees[i];
          conditional.edges += degree;
          conditional.nonzero += degree != 0;
          conditional.maximum = std::max(conditional.maximum, degree);
          conditional.squares += static_cast<unsigned __int128>(degree) * degree;
        }
      }
      if (!complete || visited != population) continue;
      rapidjson::Value entry(rapidjson::kObjectType), values(rapidjson::kArrayType);
      entry.AddMember("vertex_type", rapidjson::Value(schema_.get_vertex_label_name(label).c_str(), allocator), allocator);
      entry.AddMember("property", rapidjson::Value(names[property].c_str(), allocator), allocator);
      entry.AddMember("type", rapidjson::Value(typeName, allocator), allocator);
      entry.AddMember("vertex_count", uint64_t(population), allocator);
      entry.AddMember("null_count", nulls, allocator);
      entry.AddMember("complete", true, allocator);
      for (const auto& [key, frequency] : frequencies) {
        rapidjson::Value row(rapidjson::kObjectType), literal;
        if (type == DataTypeId::kVarchar) literal.SetString(key.c_str(), key.size(), allocator);
        else if (type == DataTypeId::kBoolean) literal.SetBool(key == "true");
        else literal.SetInt64(std::stoll(key));
        row.AddMember("value", literal, allocator);
        row.AddMember("count", frequency.count, allocator);
        rapidjson::Value conditionalEndpoints(rapidjson::kArrayType);
        for (size_t i = 0; i < group.size(); ++i) {
          const auto& degree = frequency.degrees[i];
          auto endpoint = endpointIdentity(group[i]);
          endpoint.AddMember("edge_count", degree.edges, allocator);
          endpoint.AddMember("nonzero_vertex_count", degree.nonzero, allocator);
          endpoint.AddMember("max_degree", degree.maximum, allocator);
          if (degree.squares <= std::numeric_limits<uint64_t>::max())
            endpoint.AddMember("degree_square_sum", uint64_t(degree.squares), allocator);
          conditionalEndpoints.PushBack(endpoint, allocator);
        }
        row.AddMember("endpoint_degrees", conditionalEndpoints, allocator);
        values.PushBack(row, allocator);
      }
      entry.AddMember("values", values, allocator);
      properties.PushBack(entry, allocator);
    }
  }
  // TIMESTAMP columns: equi-depth quantiles of an evenly strided sample per
  // vertex table, for range-predicate selectivity (date windows). Sampling is
  // bounded per property and independent of the dictionary visit budget.
  rapidjson::Value rangeStats(rapidjson::kObjectType), ranges(rapidjson::kArrayType);
  constexpr uint64_t kRangeSamples = 65536;
  constexpr size_t kRangeQuantiles = 256;
  for (const auto label : label_ids) {
    const auto population = VertexNum(label, MAX_TIMESTAMP);
    if (population == 0) continue;
    const auto names = schema_.get_vertex_property_names(label);
    const auto types = schema_.get_vertex_properties_id(label);
    for (size_t property = 0; property < names.size(); ++property) {
      if (types[property] != DataTypeId::kTimestampMs) continue;
      auto column = GetVertexPropertyColumn(label, property);
      if (!column) continue;
      const uint64_t stride = std::max<uint64_t>(1, population / kRangeSamples);
      std::vector<int64_t> sample;
      uint64_t nulls = 0, seen = 0, position = 0;
      for (const auto vertex : GetVertexSet(label, MAX_TIMESTAMP)) {
        if (position++ % stride != 0) continue;
        ++seen;
        const auto value = column->get_prop(vertex);
        if (value.type() == DataTypeId::kEmpty) { ++nulls; continue; }
        if (value.type() != DataTypeId::kTimestampMs) { sample.clear(); seen = 0; break; }
        sample.push_back(value.as_datetime().milli_second);
      }
      if (!seen || sample.size() < 2) continue;
      std::sort(sample.begin(), sample.end());
      rapidjson::Value entry(rapidjson::kObjectType), quantiles(rapidjson::kArrayType);
      entry.AddMember("vertex_type", rapidjson::Value(schema_.get_vertex_label_name(label).c_str(), allocator), allocator);
      entry.AddMember("property", rapidjson::Value(names[property].c_str(), allocator), allocator);
      entry.AddMember("type", "TIMESTAMP", allocator);
      entry.AddMember("unit", "milliseconds", allocator);
      entry.AddMember("vertex_count", uint64_t(population), allocator);
      entry.AddMember("sample_count", seen, allocator);
      entry.AddMember("sample_null_count", nulls, allocator);
      for (size_t i = 0; i <= kRangeQuantiles; ++i) {
        const auto index = std::min(sample.size() - 1, (sample.size() - 1) * i / kRangeQuantiles);
        quantiles.PushBack(int64_t(sample[index]), allocator);
      }
      entry.AddMember("quantiles", quantiles, allocator);
      ranges.PushBack(entry, allocator);
    }
  }
  // Edge TIMESTAMP properties (e.g. KNOWS.creationDate) per typed edge table,
  // sampled with an even stride over the outgoing adjacency.
  for (label_t edge_label = 0; edge_label < edge_label_total_count_; ++edge_label) {
    if (!schema_.edge_label_valid(edge_label)) continue;
    for (const auto src_label : label_ids)
      for (const auto dst_label : label_ids) {
        if (!schema_.exist(src_label, dst_label, edge_label)) continue;
        const auto index = schema_.generate_edge_label(src_label, dst_label, edge_label);
        const auto table = edge_tables_.find(index);
        if (table == edge_tables_.end()) continue;
        const uint64_t population = table->second.EdgeNum();
        if (population == 0) continue;
        const auto names = schema_.get_edge_property_names(src_label, dst_label, edge_label);
        const auto types = schema_.get_edge_properties(src_label, dst_label, edge_label);
        for (size_t property = 0; property < names.size() && property < types.size(); ++property) {
          if (types[property].id() != DataTypeId::kTimestampMs) continue;
          const auto view = GetGenericOutgoingGraphView(src_label, dst_label, edge_label, MAX_TIMESTAMP);
          const auto accessor = GetEdgeDataAccessor(src_label, dst_label, edge_label, static_cast<int>(property));
          const uint64_t stride = std::max<uint64_t>(1, population / kRangeSamples);
          std::vector<int64_t> sample;
          uint64_t seen = 0, position = 0;
          for (const auto vertex : GetVertexSet(src_label, MAX_TIMESTAMP)) {
            const auto edges = view.get_edges(vertex);
            for (auto it = edges.begin(); it != edges.end(); ++it) {
              if (position++ % stride != 0) continue;
              ++seen;
              sample.push_back(accessor.get_typed_data<DateTime>(it).milli_second);
            }
          }
          if (seen != sample.size() || sample.size() < 2) continue;
          std::sort(sample.begin(), sample.end());
          rapidjson::Value entry(rapidjson::kObjectType), quantiles(rapidjson::kArrayType);
          entry.AddMember("edge_type", rapidjson::Value(schema_.get_edge_label_name(edge_label).c_str(), allocator), allocator);
          entry.AddMember("source_vertex", rapidjson::Value(schema_.get_vertex_label_name(src_label).c_str(), allocator), allocator);
          entry.AddMember("destination_vertex", rapidjson::Value(schema_.get_vertex_label_name(dst_label).c_str(), allocator), allocator);
          entry.AddMember("property", rapidjson::Value(names[property].c_str(), allocator), allocator);
          entry.AddMember("type", "TIMESTAMP", allocator);
          entry.AddMember("unit", "milliseconds", allocator);
          entry.AddMember("edge_count", population, allocator);
          entry.AddMember("sample_count", seen, allocator);
          entry.AddMember("sample_null_count", uint64_t(0), allocator);
          for (size_t i = 0; i <= kRangeQuantiles; ++i) {
            const auto at = std::min(sample.size() - 1, (sample.size() - 1) * i / kRangeQuantiles);
            quantiles.PushBack(int64_t(sample[at]), allocator);
          }
          entry.AddMember("quantiles", quantiles, allocator);
          ranges.PushBack(entry, allocator);
        }
      }
  }
  rangeStats.AddMember("version", 1, allocator);
  rangeStats.AddMember("scope", "visible_typed_vertex_and_edge_timestamp_properties", allocator);
  rangeStats.AddMember("maximum_samples", kRangeSamples, allocator);
  rangeStats.AddMember("properties", ranges, allocator);
  document.AddMember("range_statistics", rangeStats, allocator);
  propertyStats.AddMember("version", 1, allocator);
  propertyStats.AddMember("scope", "visible_typed_vertex_properties", allocator);
  propertyStats.AddMember("property_vertex_visits", propertyVisits, allocator);
  propertyStats.AddMember("maximum_property_vertex_visits", limits.property_vertex_visits, allocator);
  propertyStats.AddMember("maximum_distinct_values", limits.property_max_distinct_values, allocator);
  propertyStats.AddMember("properties", properties, allocator);
  document.AddMember("property_statistics", propertyStats, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return buffer.GetString();
}

void PropertyGraph::generateStatistics() const {
  std::string filename = statisticsFilePath();

  {
    std::ofstream out(filename);
    if (!out.is_open()) {
      LOG(ERROR) << "Failed to open file: " << filename;
      return;
    }
    out << get_statistics_json();
    out.close();
  }
}

Status PropertyGraph::edge_triplet_check(const std::string& src_type_name,
                                         const std::string& dst_type_name,
                                         const std::string& edge_type_name) {
  if (!schema_.exist(src_type_name, dst_type_name, edge_type_name)) {
    LOG(ERROR) << "Edge [" << edge_type_name << "] from [" << src_type_name
               << "] to [" << dst_type_name << "] does not exist";
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge [" + edge_type_name + "] from [" + src_type_name +
                      "] to [" + dst_type_name + "] does not exist");
  }
  return neug::Status::OK();
}

Status PropertyGraph::edge_triplet_exist(const std::string& src_type_name,
                                         const std::string& dst_type_name,
                                         const std::string& edge_type_name) {
  auto ret =
      schema_.has_edge_label(src_type_name, dst_type_name, edge_type_name);
  if (!ret) {
    LOG(ERROR) << "Edge [" << edge_type_name << "] from [" << src_type_name
               << "] to [" << dst_type_name << "] does not exist";
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Edge [" + edge_type_name + "] from [" + src_type_name +
                      "] to [" + dst_type_name + "] does not exist");
  }
  return neug::Status::OK();
}

Status PropertyGraph::vertex_label_check(const std::string& vertex_type_name) {
  if (!schema_.contains_vertex_label(vertex_type_name)) {
    LOG(ERROR) << "Vertex label[" << vertex_type_name << "] does not exists.";
    return Status(StatusCode::ERR_INVALID_ARGUMENT,
                  "Vertex label[" + vertex_type_name + "] does not exists.");
  }
  return neug::Status::OK();
}

}  // namespace neug

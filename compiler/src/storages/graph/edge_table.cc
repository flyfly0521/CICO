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

#include "neug/storages/graph/edge_table.h"

#include <arrow/api.h>
#include <arrow/array/array_base.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>
#include <glog/logging.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <ostream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "neug/storages/csr/generic_view_utils.h"
#include "neug/storages/csr/immutable_csr.h"
#include "neug/storages/csr/mutable_csr.h"
#include "neug/storages/file_names.h"
#include "neug/storages/loader/loader_utils.h"
#include "neug/utils/arrow_utils.h"
#include "neug/utils/file_utils.h"
#include "neug/utils/property/types.h"

namespace neug {

std::tuple<std::vector<vid_t>, std::vector<vid_t>, std::vector<bool>>
filterInvalidEdges(const std::vector<vid_t>& src_lid,
                   const std::vector<vid_t>& dst_lid) {
  assert(src_lid.size() == dst_lid.size());
  std::vector<vid_t> filtered_src, filtered_dst;
  std::vector<bool> valid_flags;
  filtered_src.reserve(src_lid.size());
  filtered_dst.reserve(dst_lid.size());
  valid_flags.reserve(src_lid.size());
  for (size_t i = 0; i < src_lid.size(); ++i) {
    if (src_lid[i] != std::numeric_limits<vid_t>::max() &&
        dst_lid[i] != std::numeric_limits<vid_t>::max()) {
      filtered_src.push_back(src_lid[i]);
      filtered_dst.push_back(dst_lid[i]);
      valid_flags.push_back(true);
    } else {
      valid_flags.push_back(false);
    }
  }
  return std::make_tuple(std::move(filtered_src), std::move(filtered_dst),
                         std::move(valid_flags));
}

template <typename EDATA_T, typename ARROW_COL_T>
std::vector<Property> extract_edge_data(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& data_batches,
    const std::vector<bool>& valid_flags) {
  std::vector<Property> edge_data;
  assert([&]() {
    int64_t total = 0;
    for (auto rb : data_batches) {
      total += rb->num_rows();
    }
    return total == static_cast<int64_t>(valid_flags.size());
  }());
  edge_data.reserve(std::count(valid_flags.begin(), valid_flags.end(), true));
  size_t cur_index = 0;
  for (auto rb : data_batches) {
    auto array = rb->column(0);
    auto casted = std::static_pointer_cast<ARROW_COL_T>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      if (valid_flags[cur_index++]) {
        edge_data.emplace_back(
            neug::PropUtils<EDATA_T>::to_prop(casted->Value(i)));
      }
    }
  }
  return edge_data;
}

// Helper alias to resolve the Arrow array type for a property type.
template <typename T>
using ExtractEdgeArrowArray = typename TypeConverter<T>::ArrowArrayType;

std::vector<Property> extract_bundled_edge_data_from_batches(
    std::shared_ptr<const EdgeSchema> meta,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& data_batches,
    const std::vector<bool>& valid_flags) {
  assert(meta->is_bundled());
  if (meta->properties.empty() ||
      meta->properties[0].id() == DataTypeId::kEmpty) {
    return std::vector<Property>();
  }
  switch (meta->properties[0].id()) {
#define EXTRACT_EDGE_DATA_CASE(enum_val, type)                                \
  case DataTypeId::enum_val:                                                  \
    return extract_edge_data<type, ExtractEdgeArrowArray<type>>(data_batches, \
                                                                valid_flags);
    FOR_EACH_DATA_TYPE_PRIMITIVE(EXTRACT_EDGE_DATA_CASE)
  case DataTypeId::kDate:
    return extract_edge_data<Date, arrow::Date64Array>(data_batches,
                                                       valid_flags);
  case DataTypeId::kTimestampMs:
    return extract_edge_data<DateTime, arrow::TimestampArray>(data_batches,
                                                              valid_flags);
  case DataTypeId::kInterval:
    return extract_edge_data<Interval, arrow::LargeStringArray>(data_batches,
                                                                valid_flags);
#undef EXTRACT_EDGE_DATA_CASE
  default:
    THROW_NOT_SUPPORTED_EXCEPTION("not support edge data type: " +
                                  meta->properties[0].ToString());
  }
}

template <typename EDATA_T>
void batch_put_edges_with_default_edata_impl(const std::vector<vid_t>& src_lid,
                                             const std::vector<vid_t>& dst_lid,
                                             const EDATA_T& default_data,
                                             CsrBase* out_csr) {
  assert(src_lid.size() == dst_lid.size());
  std::vector<EDATA_T> default_datas(src_lid.size(), default_data);
  dynamic_cast<TypedCsrBase<EDATA_T>*>(out_csr)->batch_put_edges(
      src_lid, dst_lid, default_datas);
}

// TODO(zhanglei): Support default value for non-empty edge data type
void batch_put_edges_with_default_edata(const std::vector<vid_t>& src_lid,
                                        const std::vector<vid_t>& dst_lid,
                                        DataTypeId property_type,
                                        Property default_value,
                                        CsrBase* out_csr) {
  assert(src_lid.size() == dst_lid.size());
  switch (property_type) {
#define TYPE_DISPATCHER(enum_val, type)                                       \
  case DataTypeId::enum_val:                                                  \
    batch_put_edges_with_default_edata_impl<type>(                            \
        src_lid, dst_lid, PropUtils<type>::to_typed(default_value), out_csr); \
    break;
    FOR_EACH_DATA_TYPE_NO_STRING(TYPE_DISPATCHER)
#undef TYPE_DISPATCHER
  case DataTypeId::kEmpty:
    batch_put_edges_with_default_edata_impl<EmptyType>(src_lid, dst_lid,
                                                       EmptyType(), out_csr);
    break;
  default:
    THROW_NOT_SUPPORTED_EXCEPTION("not support edge data type: " +
                                  std::to_string(property_type));
  }
}

void batch_put_edges_to_bundled_csr(const std::vector<vid_t>& src_lid,
                                    const std::vector<vid_t>& dst_lid,
                                    DataTypeId property_type,
                                    const std::vector<Property>& edge_data,
                                    CsrBase* out_csr) {
  switch (property_type) {
#define TYPE_DISPATCHER(enum_val, type)                          \
  case DataTypeId::enum_val: {                                   \
    std::vector<type> typed_data;                                \
    typed_data.reserve(edge_data.size());                        \
    for (const auto& prop : edge_data) {                         \
      typed_data.emplace_back(PropUtils<type>::to_typed(prop));  \
    }                                                            \
    dynamic_cast<TypedCsrBase<type>*>(out_csr)->batch_put_edges( \
        src_lid, dst_lid, typed_data);                           \
    break;                                                       \
  }
    TYPE_DISPATCHER(kBoolean, bool);
    TYPE_DISPATCHER(kInt32, int32_t);
    TYPE_DISPATCHER(kUInt32, uint32_t);
    TYPE_DISPATCHER(kInt64, int64_t);
    TYPE_DISPATCHER(kUInt64, uint64_t);
    TYPE_DISPATCHER(kFloat, float);
    TYPE_DISPATCHER(kDouble, double);
    TYPE_DISPATCHER(kDate, Date);
    TYPE_DISPATCHER(kTimestampMs, DateTime);
    TYPE_DISPATCHER(kInterval, Interval);
#undef TYPE_DISPATCHER
  case DataTypeId::kEmpty: {
    dynamic_cast<TypedCsrBase<EmptyType>*>(out_csr)->batch_put_edges(
        src_lid, dst_lid, {});
    break;
  }
  case DataTypeId::kVarchar: {
    THROW_NOT_SUPPORTED_EXCEPTION("not support edge data type: " +
                                  std::to_string(property_type));
    break;
  }
  default:
    THROW_NOT_SUPPORTED_EXCEPTION(
        "Unsupported edge property type: " +
        std::to_string(static_cast<int>(property_type)));
  }
}

template <typename T>
std::unique_ptr<CsrBase> create_csr_impl(bool is_mutable,
                                         EdgeStrategy strategy) {
  if (strategy == EdgeStrategy::kSingle) {
    if (is_mutable) {
      return std::unique_ptr<CsrBase>(new SingleMutableCsr<T>());
    } else {
      return std::unique_ptr<CsrBase>(new SingleImmutableCsr<T>());
    }
  } else if (strategy == EdgeStrategy::kMultiple) {
    if (is_mutable) {
      return std::unique_ptr<CsrBase>(new MutableCsr<T>());
    } else {
      return std::unique_ptr<CsrBase>(new ImmutableCsr<T>());
    }
  } else {
    return std::unique_ptr<CsrBase>(new EmptyCsr<T>());
  }
}

static std::unique_ptr<CsrBase> create_csr(bool is_mutable,
                                           EdgeStrategy strategy,
                                           DataTypeId property_type) {
  switch (property_type) {
#define TYPE_DISPATCHER(enum_val, type) \
  case DataTypeId::enum_val:            \
    return create_csr_impl<type>(is_mutable, strategy);
    FOR_EACH_DATA_TYPE_NO_STRING(TYPE_DISPATCHER)
#undef TYPE_DISPATCHER
  case DataTypeId::kEmpty: {
    return create_csr_impl<EmptyType>(is_mutable, strategy);
  }
  default: {
    THROW_NOT_SUPPORTED_EXCEPTION("not support edge data type: " +
                                  std::to_string(property_type));
    return nullptr;
  }
  }
}

static void parse_endpoint_column(const IndexerType& indexer,
                                  const std::shared_ptr<arrow::Array>& array,
                                  std::vector<vid_t>& lids) {
  if (array->type()->Equals(arrow::utf8())) {
    auto casted = std::static_pointer_cast<arrow::StringArray>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto str = casted->GetView(i);
      std::string_view sv(str.data(), str.size());
      auto vid = indexer.get_index(Property::From(sv));
      lids.push_back(vid);
    }
  } else if (array->type()->Equals(arrow::large_utf8())) {
    auto casted = std::static_pointer_cast<arrow::LargeStringArray>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto str = casted->GetView(i);
      std::string_view sv(str.data(), str.size());
      auto vid = indexer.get_index(Property::From(sv));
      lids.push_back(vid);
    }
  } else if (array->type()->Equals(arrow::int64())) {
    auto casted = std::static_pointer_cast<arrow::Int64Array>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto vid = indexer.get_index(Property::From(casted->Value(i)));
      lids.push_back(vid);
    }
  } else if (array->type()->Equals(arrow::uint64())) {
    auto casted = std::static_pointer_cast<arrow::UInt64Array>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto vid = indexer.get_index(Property::From(casted->Value(i)));
      lids.push_back(vid);
    }
  } else if (array->type()->Equals(arrow::int32())) {
    auto casted = std::static_pointer_cast<arrow::Int32Array>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto vid = indexer.get_index(Property::From(casted->Value(i)));
      lids.push_back(vid);
    }
  } else if (array->type()->Equals(arrow::uint32())) {
    auto casted = std::static_pointer_cast<arrow::UInt32Array>(array);
    for (int64_t i = 0; i < casted->length(); ++i) {
      auto vid = indexer.get_index(Property::From(casted->Value(i)));
      lids.push_back(vid);
    }
  } else {
    LOG(FATAL) << "not support type " << array->type()->ToString();
  }
}

void insert_edges_empty_impl(TypedCsrBase<EmptyType>* out_csr,
                             TypedCsrBase<EmptyType>* in_csr,
                             const std::vector<vid_t>& src_lid,
                             const std::vector<vid_t>& dst_lid) {
  std::vector<EmptyType> empty_data(src_lid.size());
  out_csr->batch_put_edges(src_lid, dst_lid, empty_data);
  in_csr->batch_put_edges(dst_lid, src_lid, empty_data);
}

void insert_edges_empty_impl(TypedCsrBase<EmptyType>* out_csr,
                             TypedCsrBase<EmptyType>* in_csr,
                             const std::vector<vid_t>& src_lid,
                             const std::vector<vid_t>& dst_lid,
                             Allocator& alloc) {
  std::vector<EmptyType> empty_data(src_lid.size());
  out_csr->batch_put_edges(src_lid, dst_lid, empty_data, 0, alloc);
  in_csr->batch_put_edges(dst_lid, src_lid, empty_data, 0, alloc);
}

template <typename EDATA_T>
void insert_edges_bundled_typed_impl(
    TypedCsrBase<EDATA_T>* out_csr, TypedCsrBase<EDATA_T>* in_csr,
    const std::vector<vid_t>& src_lid, const std::vector<vid_t>& dst_lid,
    const std::vector<Property>& property_vec) {
  std::vector<EDATA_T> edge_data;
  edge_data.reserve(edge_data.size());
  for (const auto& prop : property_vec) {
    edge_data.push_back(PropUtils<EDATA_T>::to_typed(prop));
  }
  out_csr->batch_put_edges(src_lid, dst_lid, edge_data);
  in_csr->batch_put_edges(dst_lid, src_lid, edge_data);
}

template <typename EDATA_T>
void insert_edges_bundled_typed_impl(
    TypedCsrBase<EDATA_T>* out_csr, TypedCsrBase<EDATA_T>* in_csr,
    const std::vector<vid_t>& src_lid, const std::vector<vid_t>& dst_lid,
    const std::vector<Property>& property_vec, Allocator& alloc) {
  std::vector<EDATA_T> edge_data;
  edge_data.reserve(edge_data.size());
  for (const auto& prop : property_vec) {
    edge_data.push_back(PropUtils<EDATA_T>::to_typed(prop));
  }
  out_csr->batch_put_edges(src_lid, dst_lid, edge_data, 0, alloc);
  in_csr->batch_put_edges(dst_lid, src_lid, edge_data, 0, alloc);
}

void insert_edges_separated_impl(TypedCsrBase<uint64_t>* out_csr,
                                 TypedCsrBase<uint64_t>* in_csr,
                                 const std::vector<vid_t>& src_lid,
                                 const std::vector<vid_t>& dst_lid,
                                 size_t offset) {
  std::vector<uint64_t> edge_data(src_lid.size());
  for (size_t i = 0; i < src_lid.size(); ++i) {
    edge_data[i] = offset + i;
  }
  out_csr->batch_put_edges(src_lid, dst_lid, edge_data);
  in_csr->batch_put_edges(dst_lid, src_lid, edge_data);
}

void insert_edges_separated_impl(TypedCsrBase<uint64_t>* out_csr,
                                 TypedCsrBase<uint64_t>* in_csr,
                                 const std::vector<vid_t>& src_lid,
                                 const std::vector<vid_t>& dst_lid,
                                 size_t offset, Allocator& alloc) {
  std::vector<uint64_t> edge_data(src_lid.size());
  for (size_t i = 0; i < src_lid.size(); ++i) {
    edge_data[i] = offset + i;
  }
  out_csr->batch_put_edges(src_lid, dst_lid, edge_data, 0, alloc);
  in_csr->batch_put_edges(dst_lid, src_lid, edge_data, 0, alloc);
}

static std::vector<Property> get_row_from_recordbatch(
    const std::vector<DataType>& prop_types,
    const std::vector<std::shared_ptr<arrow::DataType>>& expected_types,
    const std::shared_ptr<arrow::RecordBatch>& rb, int64_t row_idx) {
  std::vector<Property> row;
  if ((int32_t) expected_types.size() != rb->num_columns()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "property types size not match recordbatch column size");
  }
  for (int i = 0; i < rb->num_columns(); ++i) {
    auto array = rb->column(i);
    if (!array->type()->Equals(expected_types[i])) {
      // Except for large string and string
      if ((expected_types[i]->Equals(arrow::utf8()) &&
           array->type()->Equals(arrow::large_utf8())) ||
          (expected_types[i]->Equals(arrow::large_utf8()) &&
           array->type()->Equals(arrow::utf8()))) {
        // pass
      } else {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            std::string("property type not match recordbatch column type: ") +
            prop_types[i].ToString() + "(" + expected_types[i]->ToString() +
            ") vs " + array->type()->ToString());
      }
    }
    if (array->IsNull(row_idx)) {
      row.push_back(Property());
      continue;
    }
    switch (prop_types[i].id()) {
#define GET_PRIMITIVE_PROPERTY_CASE(enum_val, type)                   \
  case DataTypeId::enum_val: {                                        \
    auto casted =                                                     \
        std::static_pointer_cast<ExtractEdgeArrowArray<type>>(array); \
    row.push_back(PropUtils<type>::to_prop(casted->Value(row_idx)));  \
    break;                                                            \
  }
      FOR_EACH_DATA_TYPE_PRIMITIVE(GET_PRIMITIVE_PROPERTY_CASE)
#undef GET_PRIMITIVE_PROPERTY_CASE
    case DataTypeId::kVarchar: {
      if (array->type()->Equals(arrow::utf8())) {
        auto casted = std::static_pointer_cast<arrow::StringArray>(array);
        auto str = casted->GetView(row_idx);
        row.push_back(Property::from_string_view(str));
      } else if (array->type()->Equals(arrow::large_utf8())) {
        auto casted = std::static_pointer_cast<arrow::LargeStringArray>(array);
        auto str = casted->GetView(row_idx);
        row.push_back(Property::from_string_view(str));
      } else {
        LOG(FATAL) << "not support type " << array->type()->ToString();
      }
      break;
    }
    case DataTypeId::kDate: {
      auto casted = std::static_pointer_cast<arrow::Date64Array>(array);
      Date d;
      d.from_timestamp(casted->Value(row_idx));
      row.push_back(Property::from_date(d));
      break;
    }
    case DataTypeId::kTimestampMs: {
      auto casted = std::static_pointer_cast<arrow::TimestampArray>(array);
      row.push_back(Property::from_datetime(DateTime(casted->Value(row_idx))));
      break;
    }
    case DataTypeId::kInterval: {
      if (array->type()->id() == arrow::Type::STRING) {
        auto casted = std::static_pointer_cast<arrow::StringArray>(array);
        row.push_back(
            Property::from_interval(Interval(casted->GetView(row_idx))));
      } else if (array->type()->id() == arrow::Type::LARGE_STRING) {
        auto casted = std::static_pointer_cast<arrow::LargeStringArray>(array);
        row.push_back(
            Property::from_interval(Interval(casted->GetView(row_idx))));
      } else {
        THROW_NOT_IMPLEMENTED_EXCEPTION("Not support typed: " +
                                        array->type()->ToString());
      }
      break;
    }
    default:
      LOG(FATAL) << "not support type " << array->type()->ToString();
    }
  }
  return row;
}

void batch_add_unbundled_edges_impl(
    const std::vector<vid_t>& src_lid_list,
    const std::vector<vid_t>& dst_lid_list, TypedCsrBase<uint64_t>* out_csr,
    TypedCsrBase<uint64_t>* in_csr, Table* table_,
    std::atomic<uint64_t>& table_idx_, std::atomic<uint64_t>& capacity_,
    const std::vector<DataType>& prop_types,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& data_batches,
    const std::vector<bool>& valid_flags) {
  size_t offset = table_idx_.fetch_add(src_lid_list.size());
  insert_edges_separated_impl(out_csr, in_csr, src_lid_list, dst_lid_list,
                              offset);
  std::vector<std::shared_ptr<arrow::DataType>> expected_types;
  for (auto pt : prop_types) {
    expected_types.emplace_back(PropertyTypeToArrowType(pt));
  }
  // assert the totoal number of rows in data_batches equals to
  // src_lid.size()
  assert([&]() {
    int64_t total = 0;
    for (auto rb : data_batches) {
      total += rb->num_rows();
    }
    return total == static_cast<int64_t>(valid_flags.size());
  }());
  size_t cur_index = 0;
  for (auto rb : data_batches) {
    for (int64_t i = 0; i < rb->num_rows(); ++i) {
      assert(cur_index < valid_flags.size());
      if (valid_flags[cur_index++]) {
        auto row = get_row_from_recordbatch(prop_types, expected_types, rb, i);
        table_->insert(offset++, row, true);
      }
    }
  }
}

void batch_add_bundled_edges_impl(CsrBase* out_csr, CsrBase* in_csr,
                                  const std::vector<DataType>& prop_types,
                                  const std::vector<vid_t>& src_lid_list,
                                  const std::vector<vid_t>& dst_lid_list,
                                  const std::vector<Property>& edge_data) {
  if (prop_types.empty() || prop_types[0].id() == DataTypeId::kEmpty) {
    insert_edges_empty_impl(dynamic_cast<TypedCsrBase<EmptyType>*>(out_csr),
                            dynamic_cast<TypedCsrBase<EmptyType>*>(in_csr),
                            src_lid_list, dst_lid_list);
    return;
  }
  switch (prop_types[0].id()) {
#define TYPE_DISPATCHER(enum_val, type)                                        \
  case DataTypeId::enum_val:                                                   \
    insert_edges_bundled_typed_impl(                                           \
        dynamic_cast<TypedCsrBase<type>*>(out_csr),                            \
        dynamic_cast<TypedCsrBase<type>*>(in_csr), src_lid_list, dst_lid_list, \
        edge_data);                                                            \
    break;
    FOR_EACH_DATA_TYPE_NO_STRING(TYPE_DISPATCHER)
#undef TYPE_DISPATCHER
  default:
    THROW_NOT_SUPPORTED_EXCEPTION("not support edge data type: " +
                                  std::to_string(prop_types[0].id()));
  }
}

EdgeTable::EdgeTable(std::shared_ptr<const EdgeSchema> meta) : meta_(meta) {
  table_ = std::make_unique<Table>();

  if (meta_->is_bundled()) {
    auto property_type = meta_->properties.empty() ? DataTypeId::kEmpty
                                                   : meta_->properties[0].id();
    out_csr_ = create_csr(meta_->oe_mutable, meta_->oe_strategy, property_type);
    in_csr_ = create_csr(meta_->ie_mutable, meta_->ie_strategy, property_type);
  } else {
    out_csr_ =
        create_csr(meta_->oe_mutable, meta_->oe_strategy, DataTypeId::kUInt64);
    in_csr_ =
        create_csr(meta_->ie_mutable, meta_->ie_strategy, DataTypeId::kUInt64);
  }
}

EdgeTable::EdgeTable(EdgeTable&& edge_table)
    : meta_(edge_table.meta_),
      work_dir_(edge_table.work_dir_),
      memory_level_(edge_table.memory_level_) {
  csr_alter_version_ = edge_table.csr_alter_version_.load();
  out_csr_ = std::move(edge_table.out_csr_);
  in_csr_ = std::move(edge_table.in_csr_);
  table_ = std::move(edge_table.table_);
  table_idx_ = edge_table.table_idx_.load();
  capacity_ = edge_table.capacity_.load();
}

void EdgeTable::Swap(EdgeTable& edge_table) {
  std::swap(meta_, edge_table.meta_);
  std::swap(work_dir_, edge_table.work_dir_);
  std::swap(memory_level_, edge_table.memory_level_);
  auto v = csr_alter_version_.load();
  csr_alter_version_.store(edge_table.csr_alter_version_.load());
  edge_table.csr_alter_version_.store(v);
  out_csr_.swap(edge_table.out_csr_);
  in_csr_.swap(edge_table.in_csr_);
  table_.swap(edge_table.table_);
  auto t_idx = table_idx_.load();
  table_idx_.store(edge_table.table_idx_.load());
  edge_table.table_idx_.store(t_idx);
  auto cap = capacity_.load();
  capacity_.store(edge_table.capacity_.load());
  edge_table.capacity_.store(cap);
}

void EdgeTable::SetEdgeSchema(std::shared_ptr<const EdgeSchema> meta) {
  meta_ = meta;
}

void load_statistic_file(const std::string& work_dir,
                         const std::string& src_label_name,
                         const std::string& dst_label_name,
                         const std::string& edge_label_name,
                         std::atomic<uint64_t>& cap_atomic,
                         std::atomic<uint64_t>& table_idx_atomic) {
  size_t cap = 0, size = 0;
  auto statistic_file_path =
      checkpoint_dir(work_dir) + "/" +
      statistics_file_prefix(src_label_name, dst_label_name, edge_label_name);
  if (!std::filesystem::exists(statistic_file_path)) {
    cap_atomic.store(0);
    table_idx_atomic.store(0);
    return;
  }
  read_statistic_file(statistic_file_path, cap, size);
  cap_atomic.store(cap);
  table_idx_atomic.store(size);
}

void EdgeTable::Open(const std::string& work_dir) {
  work_dir_ = work_dir;
  memory_level_ = 0;
  auto checkpoint_dir_path = checkpoint_dir(work_dir);
  in_csr_->open(ie_prefix(meta_->src_label_name, meta_->dst_label_name,
                          meta_->edge_label_name),
                checkpoint_dir_path, work_dir);
  out_csr_->open(oe_prefix(meta_->src_label_name, meta_->dst_label_name,
                           meta_->edge_label_name),
                 checkpoint_dir_path, work_dir);
  if (!meta_->is_bundled()) {
    table_->open(edata_prefix(meta_->src_label_name, meta_->dst_label_name,
                              meta_->edge_label_name),
                 work_dir, meta_->property_names, meta_->properties,
                 meta_->strategies);
    assert(table_->col_num() > 0);
    size_t table_cap = table_->get_column_by_id(0)->size();
    load_statistic_file(work_dir, meta_->src_label_name, meta_->dst_label_name,
                        meta_->edge_label_name, capacity_, table_idx_);
    if (table_cap != capacity_.load()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "capacity in statistic file not match actual table capacity, maybe "
          "the graph is not dumped properly");
    }
  }
}

void EdgeTable::OpenInMemory(const std::string& work_dir) {
  work_dir_ = work_dir;
  memory_level_ = 1;
  auto checkpoint_dir_path = checkpoint_dir(work_dir);
  in_csr_->open_in_memory(checkpoint_dir_path + "/" +
                          ie_prefix(meta_->src_label_name,
                                    meta_->dst_label_name,
                                    meta_->edge_label_name));
  out_csr_->open_in_memory(checkpoint_dir_path + "/" +
                           oe_prefix(meta_->src_label_name,
                                     meta_->dst_label_name,
                                     meta_->edge_label_name));
  if (!meta_->is_bundled()) {
    table_->open_in_memory(
        edata_prefix(meta_->src_label_name, meta_->dst_label_name,
                     meta_->edge_label_name),
        work_dir_, meta_->property_names, meta_->properties, meta_->strategies);
    assert(table_->col_num() > 0);
    size_t table_cap = table_->get_column_by_id(0)->size();
    load_statistic_file(work_dir, meta_->src_label_name, meta_->dst_label_name,
                        meta_->edge_label_name, capacity_, table_idx_);
    if (table_cap != capacity_.load()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "capacity in statistic file not match actual table capacity, maybe "
          "the graph is not dumped properly");
    }
  }
}

void EdgeTable::OpenWithHugepages(const std::string& work_dir) {
  work_dir_ = work_dir;
  memory_level_ = 2;  // 2 or 3?
  auto checkpoint_dir_path = checkpoint_dir(work_dir);
  in_csr_->open_with_hugepages(checkpoint_dir_path + "/" +
                               ie_prefix(meta_->src_label_name,
                                         meta_->dst_label_name,
                                         meta_->edge_label_name));
  out_csr_->open_with_hugepages(checkpoint_dir_path + "/" +
                                oe_prefix(meta_->src_label_name,
                                          meta_->dst_label_name,
                                          meta_->edge_label_name));
  if (!meta_->is_bundled()) {
    table_->open_with_hugepages(
        edata_prefix(meta_->src_label_name, meta_->dst_label_name,
                     meta_->edge_label_name),
        checkpoint_dir_path, meta_->property_names, meta_->properties,
        meta_->strategies, (memory_level_ > 2));
    assert(table_->col_num() > 0);
    size_t table_cap = table_->get_column_by_id(0)->size();
    load_statistic_file(work_dir, meta_->src_label_name, meta_->dst_label_name,
                        meta_->edge_label_name, capacity_, table_idx_);
    if (table_cap != capacity_.load()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "capacity in statistic file not match actual table capacity, maybe "
          "the graph is not dumped properly");
    }
  }
}

void EdgeTable::Dump(const std::string& checkpoint_dir_path) {
  // Checkpoint runs after writers/readers have stopped and removes the WAL.
  // The reopened graph starts at timestamp zero, including without compaction.
  in_csr_->reset_timestamp();
  out_csr_->reset_timestamp();
  in_csr_->dump(ie_prefix(meta_->src_label_name, meta_->dst_label_name,
                          meta_->edge_label_name),
                checkpoint_dir_path);
  out_csr_->dump(oe_prefix(meta_->src_label_name, meta_->dst_label_name,
                           meta_->edge_label_name),
                 checkpoint_dir_path);
  if (!meta_->is_bundled()) {
    table_->dump(edata_prefix(meta_->src_label_name, meta_->dst_label_name,
                              meta_->edge_label_name),
                 checkpoint_dir_path);
    auto statistc_file_path =
        checkpoint_dir_path + "/" +
        statistics_file_prefix(meta_->src_label_name, meta_->dst_label_name,
                               meta_->edge_label_name);
    write_statistic_file(statistc_file_path, Capacity(), Size());
  }
}

void EdgeTable::Drop() {
  const auto remove_matching_files = [](const std::string& dir,
                                        const std::vector<std::string>& prefixes) {
    if (!std::filesystem::exists(dir)) {
      return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      auto filename = entry.path().filename().string();
      for (const auto& prefix : prefixes) {
        if (filename.rfind(prefix, 0) == 0) {
          std::filesystem::remove_all(entry.path());
          break;
        }
      }
    }
  };

  std::vector<std::string> prefixes = {
      ie_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name),
      oe_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name),
      edata_prefix(meta_->src_label_name, meta_->dst_label_name,
                   meta_->edge_label_name),
      statistics_file_prefix(meta_->src_label_name, meta_->dst_label_name,
                             meta_->edge_label_name)};
  remove_matching_files(checkpoint_dir(work_dir_), prefixes);
  remove_matching_files(tmp_dir(work_dir_), prefixes);
  if (table_) {
    table_->drop();
  }
}

void EdgeTable::SortByEdgeData(timestamp_t ts) {
  // TODO
}

void EdgeTable::BatchDeleteVertices(const std::set<vid_t>& src_set,
                                    const std::set<vid_t>& dst_set) {
  out_csr_->batch_delete_vertices(src_set, dst_set);
  in_csr_->batch_delete_vertices(dst_set, src_set);
}

void EdgeTable::BatchDeleteEdges(const std::vector<vid_t>& src_list,
                                 const std::vector<vid_t>& dst_list) {
  out_csr_->batch_delete_edges(src_list, dst_list);
  in_csr_->batch_delete_edges(dst_list, src_list);
}

void EdgeTable::BatchDeleteEdges(
    const std::vector<std::pair<vid_t, int32_t>>& oe_edges,
    const std::vector<std::pair<vid_t, int32_t>>& ie_edges) {
  out_csr_->batch_delete_edges(oe_edges);
  in_csr_->batch_delete_edges(ie_edges);
}

void EdgeTable::DeleteEdge(vid_t src_lid, vid_t dst_lid, int32_t oe_offset,
                           int32_t ie_offset, timestamp_t ts) {
  out_csr_->delete_edge(src_lid, oe_offset, ts);
  in_csr_->delete_edge(dst_lid, ie_offset, ts);
}

void EdgeTable::DeleteVertex(bool is_src, vid_t vid, timestamp_t ts) {
  auto oe_view = get_outgoing_view(ts);
  auto ie_view = get_incoming_view(ts);
  if (is_src) {
    auto oe_edges = oe_view.get_edges(vid);
    auto begin_ptr = oe_edges.start_ptr;
    auto stride = oe_edges.cfg.stride;
    for (auto iter = oe_edges.begin(); iter != oe_edges.end(); ++iter) {
      if (iter.get_timestamp() > ts) {
        continue;
      }
      int32_t oe_offset = static_cast<int32_t>(
          (reinterpret_cast<const char*>(iter.get_nbr_ptr()) -
           reinterpret_cast<const char*>(begin_ptr)) /
          stride);
      auto ie_offset = search_other_offset_with_cur_offset(
          oe_view, ie_view, vid, iter.get_vertex(), oe_offset,
          meta_->properties);
      DeleteEdge(vid, iter.get_vertex(), oe_offset, ie_offset, ts);
    }
  } else {
    auto ie_edges = ie_view.get_edges(vid);
    auto begin_ptr = ie_edges.start_ptr;
    auto stride = ie_edges.cfg.stride;
    for (auto iter = ie_edges.begin(); iter != ie_edges.end(); ++iter) {
      if (iter.get_timestamp() > ts) {
        continue;
      }
      int32_t ie_offset = static_cast<int32_t>(
          (reinterpret_cast<const char*>(iter.get_nbr_ptr()) -
           reinterpret_cast<const char*>(begin_ptr)) /
          stride);
      auto oe_offset = search_other_offset_with_cur_offset(
          ie_view, oe_view, vid, iter.get_vertex(), ie_offset,
          meta_->properties);
      DeleteEdge(iter.get_vertex(), vid, oe_offset, ie_offset, ts);
    }
  }
}

void EdgeTable::RevertDeleteEdge(vid_t src_lid, vid_t dst_lid,
                                 int32_t oe_offset, int32_t ie_offset,
                                 timestamp_t oe_timestamp,
                                 timestamp_t ie_timestamp) {
  out_csr_->revert_delete_edge(src_lid, dst_lid, oe_offset, oe_timestamp);
  in_csr_->revert_delete_edge(dst_lid, src_lid, ie_offset, ie_timestamp);
}

void EdgeTable::UpdateEdgeProperty(vid_t src_lid, vid_t dst_lid,
                                   int32_t oe_offset, int32_t ie_offset,
                                   int32_t col_id, const Property& prop,
                                   timestamp_t ts,
                                   std::optional<std::pair<timestamp_t, timestamp_t>>
                                       restore_timestamps) {
  auto accessor = get_edge_data_accessor(col_id);
  const auto physicalSlot = [](const NbrList& edges, int32_t offset,
                               vid_t neighbor) {
    // Offsets identify physical CSR slots. Advancing a visible iterator skips
    // deleted slots, so begin()+offset can select another edge after Abort.
    if (offset < 0 || edges.start_ptr == edges.end_ptr || edges.cfg.stride <= 0) {
      THROW_INVALID_ARGUMENT_EXCEPTION("Invalid physical edge offset");
    }
    const auto* start = static_cast<const char*>(edges.start_ptr);
    const auto* end = static_cast<const char*>(edges.end_ptr);
    if (static_cast<size_t>(offset) >=
        static_cast<size_t>((end - start) / edges.cfg.stride)) {
      THROW_INVALID_ARGUMENT_EXCEPTION("Physical edge offset is out of range");
    }
    NbrIterator iterator;
    iterator.init(start + static_cast<size_t>(offset) * edges.cfg.stride,
                  edges.end_ptr, edges.cfg, edges.timestamp, edges.start_ptr);
    if (iterator == edges.end() || iterator.get_offset() != offset ||
        iterator.get_vertex() != neighbor) {
      THROW_INVALID_ARGUMENT_EXCEPTION("Physical edge offset is not visible or has a different endpoint");
    }
    return iterator;
  };
  const auto oe_edges = out_csr_->get_generic_view(ts).get_edges(src_lid);
  const auto oe_iter = physicalSlot(oe_edges, oe_offset, dst_lid);
  NbrIterator ie_iter{};
  if (meta_->is_bundled() || restore_timestamps) {
    const auto ie_edges = in_csr_->get_generic_view(ts).get_edges(dst_lid);
    ie_iter = physicalSlot(ie_edges, ie_offset, src_lid);
  }
  // Validate at the current transaction timestamp before writing either side.
  // Undo restores the independent timestamp preimages of the physical slots.
  accessor.set_data(oe_iter, prop, restore_timestamps ? restore_timestamps->first : ts);
  if (meta_->is_bundled() || restore_timestamps) {
    accessor.set_data(ie_iter, prop, restore_timestamps ? restore_timestamps->second : ts);
  }
}

namespace {

constexpr size_t kConstantUpdateMinEdgesPerWorker = 65536;

size_t constant_update_worker_count(size_t edge_num, size_t vertex_num,
                                    size_t requested_worker_count) {
  if (edge_num == 0 || vertex_num == 0 || requested_worker_count <= 1) {
    return 1;
  }
  const size_t useful_workers = std::max<size_t>(
      1, (edge_num + kConstantUpdateMinEdgesPerWorker - 1) /
             kConstantUpdateMinEdgesPerWorker);
  return std::min({requested_worker_count, useful_workers, vertex_num});
}

void update_constant_in_view(
    const GenericView& view, size_t vertex_num,
    std::vector<EdgeDataAccessor>& accessors,
    const std::vector<Property>& values, timestamp_t ts,
    size_t worker_count) {
  const size_t chunk_size = std::clamp<size_t>(
      vertex_num / std::max<size_t>(1, worker_count * 8), 32, 1024);
  std::atomic<size_t> next_vertex{0};
  std::atomic<bool> failed{false};
  std::mutex error_mutex;
  std::exception_ptr first_error;

  auto worker = [&]() {
    try {
      while (!failed.load(std::memory_order_relaxed)) {
        const size_t begin =
            next_vertex.fetch_add(chunk_size, std::memory_order_relaxed);
        if (begin >= vertex_num) {
          break;
        }
        const size_t end = std::min(vertex_num, begin + chunk_size);
        for (size_t raw_vid = begin; raw_vid < end; ++raw_vid) {
          auto edges = view.get_edges(static_cast<vid_t>(raw_vid));
          for (auto it = edges.begin(); it != edges.end(); ++it) {
            for (size_t property = 0; property < accessors.size(); ++property) {
              accessors[property].set_data(it, values[property], ts);
            }
          }
        }
      }
    } catch (...) {
      failed.store(true, std::memory_order_relaxed);
      std::lock_guard<std::mutex> guard(error_mutex);
      if (first_error == nullptr) {
        first_error = std::current_exception();
      }
    }
  };

  if (worker_count == 1) {
    worker();
  } else {
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
      workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
      thread.join();
    }
  }
  if (first_error != nullptr) {
    std::rethrow_exception(first_error);
  }
}

}  // namespace

void EdgeTable::BatchUpdatePropertiesConstant(
    const std::vector<int32_t>& col_ids,
    const std::vector<Property>& values, timestamp_t ts,
    size_t requested_worker_count) {
  if (col_ids.empty() || col_ids.size() != values.size()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Constant edge property update requires matching non-empty columns "
        "and values");
  }

  std::vector<EdgeDataAccessor> accessors;
  accessors.reserve(col_ids.size());
  for (size_t i = 0; i < col_ids.size(); ++i) {
    const auto col_id = col_ids[i];
    if (col_id < 0 || static_cast<size_t>(col_id) >= meta_->properties.size()) {
      THROW_INVALID_ARGUMENT_EXCEPTION("Invalid edge property column id");
    }
    if (meta_->properties[col_id].id() != values[i].type()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Constant edge property value type does not match schema");
    }
    if (values[i].type() == DataTypeId::kVarchar) {
      THROW_NOT_SUPPORTED_EXCEPTION(
          "Parallel constant update does not support string edge properties");
    }
    if (meta_->is_bundled() && col_id != 0) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Bundled edge storage supports only its inline property");
    }
    accessors.emplace_back(get_edge_data_accessor(col_id));
  }

  const size_t edge_num = EdgeNum();
  const size_t outgoing_vertices = out_csr_->size();
  const size_t outgoing_workers = constant_update_worker_count(
      edge_num, outgoing_vertices, requested_worker_count);
  update_constant_in_view(get_outgoing_view(ts), outgoing_vertices, accessors,
                          values, ts, outgoing_workers);

  if (meta_->is_bundled()) {
    const size_t incoming_vertices = in_csr_->size();
    const size_t incoming_workers = constant_update_worker_count(
        edge_num, incoming_vertices, requested_worker_count);
    update_constant_in_view(get_incoming_view(ts), incoming_vertices,
                            accessors, values, ts, incoming_workers);
  }
}

void EdgeTable::EnsureCapacity(size_t capacity) {
  if (!meta_->is_bundled()) {
    if (capacity <= capacity_.load()) {
      return;
    }
    capacity = std::max(capacity, 4096UL);
    table_->resize(capacity, meta_->default_property_values);
    capacity_.store(capacity);
  }
}

void EdgeTable::EnsureCapacity(vid_t src_v_cap, vid_t dst_v_cap,
                               size_t capacity) {
  if (src_v_cap > out_csr_->size()) {
    out_csr_->resize(src_v_cap);
  }
  if (dst_v_cap > in_csr_->size()) {
    in_csr_->resize(dst_v_cap);
  }
  EnsureCapacity(capacity);
}

size_t EdgeTable::EdgeNum() const {
  if (out_csr_) {
    return out_csr_->edge_num();
  } else if (in_csr_) {
    return in_csr_->edge_num();
  } else {
    return 0;
  }
}

size_t EdgeTable::PropertyNum() const { return table_->col_num(); }

GenericView EdgeTable::get_outgoing_view(timestamp_t ts) const {
  return out_csr_->get_generic_view(ts);
}

GenericView EdgeTable::get_incoming_view(timestamp_t ts) const {
  return in_csr_->get_generic_view(ts);
}

EdgeDataAccessor EdgeTable::get_edge_data_accessor(int col_id) const {
  if (!meta_->is_bundled()) {
    return EdgeDataAccessor(meta_->properties[col_id].id(),
                            table_->get_column_by_id(col_id).get());
  } else {
    if (meta_->properties.empty()) {
      return EdgeDataAccessor(DataTypeId::kEmpty, nullptr);
    } else {
      return EdgeDataAccessor(meta_->properties[0].id(), nullptr);
    }
  }
}

EdgeDataAccessor EdgeTable::get_edge_data_accessor(
    const std::string& col_name) const {
  auto prop_ind = meta_->get_property_index(col_name);
  if (prop_ind == -1) {
    THROW_INVALID_ARGUMENT_EXCEPTION("property " + col_name +
                                     " not found in edge table, or deleted");
  }
  return get_edge_data_accessor(static_cast<int>(prop_ind));
}

void EdgeTable::AddProperties(const std::vector<std::string>& prop_names,
                              const std::vector<DataType>& prop_types,
                              const std::vector<Property>& default_values,
                              const std::vector<StorageStrategy>& strategies) {
  if (prop_names.empty()) {
    return;
  }

  if (table_->col_num() == 0) {
    // NOTE: Rather than check meta_->is_bundled(),we check whether the table
    // is empty.
    if (meta_->properties.size() == 1 &&
        meta_->properties[0].id() != DataTypeId::kVarchar) {
      dropAndCreateNewBundledCSR(nullptr);
    } else {
      dropAndCreateNewUnbundledCSR(false);
    }
  } else {
    size_t property_size = table_->get_column_by_id(0)->size();
    table_->add_columns(prop_names, prop_types, default_values, property_size,
                        strategies, memory_level_);
  }
}

void EdgeTable::RenameProperties(const std::vector<std::string>& old_names,
                                 const std::vector<std::string>& new_names) {
  CHECK_EQ(old_names.size(), new_names.size());
  for (size_t i = 0; i < old_names.size(); ++i) {
    if (!meta_->is_bundled()) {
      table_->rename_column(old_names[i], new_names[i]);
    }
  }
}

void EdgeTable::DeleteProperties(const std::vector<std::string>& col_names) {
  if (meta_->is_bundled()) {
    if (meta_->property_names.size() <= 0) {
      return;
    }
    bool found = false;
    for (auto col : col_names) {
      if (col == meta_->property_names[0]) {
        found = true;
        break;
      }
    }
    if (found) {
      dropAndCreateNewUnbundledCSR(true);
    }
  } else {
    for (const auto& col : col_names) {
      table_->delete_column(col);
      VLOG(1) << "delete column " << col;
    }
    if (table_->col_num() == 0) {
      dropAndCreateNewUnbundledCSR(true);
    } else if (table_->col_num() == 1) {
      auto remaining_col = table_->get_column_by_id(0);
      if (remaining_col->type() != DataTypeId::kVarchar) {
        dropAndCreateNewBundledCSR(remaining_col);
      }
    }
  }
}

void EdgeTable::ValidateSingleMultiplicity(
    std::span<const vid_t> sources,
    std::span<const vid_t> destinations) const {
  if (sources.size() != destinations.size()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Source and destination counts differ");
  }
  const auto checkDirection = [](const CsrBase& csr,
                                 std::span<const vid_t> endpoints,
                                 const char* direction) {
    std::unordered_set<vid_t> seen;
    const auto view = csr.get_generic_view(MAX_TIMESTAMP);
    for (const auto endpoint : endpoints) {
      if (!seen.insert(endpoint).second) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            std::string("ONE relationship multiplicity violated by repeated ") +
            direction + " endpoint in one batch");
      }
      if (endpoint < csr.size()) {
        const auto edges = view.get_edges(endpoint);
        if (edges.begin() != edges.end()) {
          THROW_INVALID_ARGUMENT_EXCEPTION(
              std::string("ONE relationship multiplicity violated: ") +
              direction + " endpoint already has a relationship");
        }
      }
    }
  };
  if (meta_->oe_strategy == EdgeStrategy::kSingle) {
    checkDirection(*out_csr_, sources, "source");
  }
  if (meta_->ie_strategy == EdgeStrategy::kSingle) {
    checkDirection(*in_csr_, destinations, "destination");
  }
}

int32_t EdgeTable::AddEdge(vid_t src_lid, vid_t dst_lid,
                           const std::vector<Property>& edge_data,
                           timestamp_t ts, Allocator& alloc, bool insert_safe) {
  return AddEdgeWithOffsets(src_lid, dst_lid, edge_data, ts, alloc, insert_safe)
      .first;
}

std::pair<int32_t, int32_t> EdgeTable::AddEdgeWithOffsets(
    vid_t src_lid, vid_t dst_lid, const std::vector<Property>& edge_data,
    timestamp_t ts, Allocator& alloc, bool insert_safe) {
  std::unique_lock singleLock(single_multiplicity_mutex_, std::defer_lock);
  if (meta_->oe_strategy == EdgeStrategy::kSingle ||
      meta_->ie_strategy == EdgeStrategy::kSingle) {
    singleLock.lock();
    ValidateSingleMultiplicity(std::span(&src_lid, 1), std::span(&dst_lid, 1));
  }
  return AddEdgeUnchecked(src_lid, dst_lid, edge_data, ts, alloc, insert_safe);
}

std::pair<int32_t, int32_t> EdgeTable::AddEdgeUnchecked(
    vid_t src_lid, vid_t dst_lid, const std::vector<Property>& edge_data,
    timestamp_t ts, Allocator& alloc, bool insert_safe) {
  Property csr_data;
  if (meta_->is_bundled()) {
    if (edge_data.size() != meta_->properties.size() &&
        !(edge_data.empty() && meta_->properties.size() == 1 &&
          meta_->properties[0] == DataTypeId::kEmpty)) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "edge data size not match edge table property size");
    }
    csr_data = edge_data.empty() ? Property() : edge_data[0];
  } else {
    if (meta_->properties.size() != edge_data.size()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "edge data size not match edge table property size");
    }
    size_t row_id = table_idx_.fetch_add(1);
    // Populate the row before publishing either CSR entry. A failed property
    // write must not leave a visible relationship that no transaction can undo.
    table_->insert(row_id, edge_data, insert_safe);
    csr_data.set_uint64(row_id);
  }
  int32_t ie_offset = -1;
  int32_t oe_offset = -1;
  try {
    ie_offset = in_csr_->put_generic_edge(dst_lid, src_lid, csr_data, ts, alloc);
    oe_offset = out_csr_->put_generic_edge(src_lid, dst_lid, csr_data, ts, alloc);
  } catch (...) {
    // Insertion has not returned, so the caller cannot have installed undo for
    // these entries yet. Remove any direction that was already published.
    if (oe_offset >= 0) {
      out_csr_->delete_edge(src_lid, oe_offset, ts);
    }
    if (ie_offset >= 0) {
      in_csr_->delete_edge(dst_lid, ie_offset, ts);
    }
    throw;
  }
  return {oe_offset, ie_offset};
}

void EdgeTable::BatchAddEdges(
    const std::vector<std::tuple<vid_t, vid_t, std::vector<Property>>>& edges,
    timestamp_t ts, Allocator& alloc) {
  if (edges.empty()) return;

  size_t n = edges.size();
  std::vector<vid_t> src_list, dst_list;
  src_list.reserve(n);
  dst_list.reserve(n);
  for (auto& [src, dst, props] : edges) {
    src_list.push_back(src);
    dst_list.push_back(dst);
  }
  std::unique_lock singleLock(single_multiplicity_mutex_, std::defer_lock);
  if (meta_->oe_strategy == EdgeStrategy::kSingle ||
      meta_->ie_strategy == EdgeStrategy::kSingle) {
    singleLock.lock();
    ValidateSingleMultiplicity(src_list, dst_list);
  }

  if (meta_->is_bundled()) {
    // Bundled storage: the CSR stores the property value inline.
    // Dispatch by property type for batch insertion.
    auto property_type = meta_->properties.empty() ? DataTypeId::kEmpty
                                                    : meta_->properties[0].id();
    if (property_type == DataTypeId::kEmpty) {
      insert_edges_empty_impl(
          dynamic_cast<TypedCsrBase<EmptyType>*>(out_csr_.get()),
          dynamic_cast<TypedCsrBase<EmptyType>*>(in_csr_.get()),
          src_list, dst_list, alloc);
      return;
    }
    std::vector<Property> bundled_data;
    bundled_data.reserve(n);
    for (auto& [src, dst, props] : edges) {
      bundled_data.push_back(props.empty() ? Property() : props[0]);
    }
    switch (property_type) {
#define TYPE_DISPATCHER(enum_val, type)                                        \
  case DataTypeId::enum_val:                                                   \
    insert_edges_bundled_typed_impl(                                           \
        dynamic_cast<TypedCsrBase<type>*>(out_csr_.get()),                     \
        dynamic_cast<TypedCsrBase<type>*>(in_csr_.get()), src_list, dst_list,  \
        bundled_data, alloc);                                                         \
    break;
    FOR_EACH_DATA_TYPE_NO_STRING(TYPE_DISPATCHER)
#undef TYPE_DISPATCHER
    default:
      for (size_t i = 0; i < n; ++i) {
        AddEdgeUnchecked(src_list[i], dst_list[i], std::get<2>(edges[i]), ts,
                          alloc, true);
      }
    }
    return;
  }

  // Unbundled: CSR stores uint64 row_id. Pre-allocate row IDs and batch-insert.
  size_t offset = table_idx_.fetch_add(n);
  insert_edges_separated_impl(
      dynamic_cast<TypedCsrBase<uint64_t>*>(out_csr_.get()),
      dynamic_cast<TypedCsrBase<uint64_t>*>(in_csr_.get()),
      src_list, dst_list, offset, alloc);

  for (size_t i = 0; i < n; ++i) {
    table_->insert(offset + i, std::get<2>(edges[i]), true);
  }
}

void EdgeTable::BatchAddEdges(const IndexerType& src_indexer,
                              const IndexerType& dst_indexer,
                              std::shared_ptr<IRecordBatchSupplier> supplier) {
  in_csr_->resize(dst_indexer.size());
  out_csr_->resize(src_indexer.size());
  std::vector<vid_t> src_lid, dst_lid;
  std::vector<std::shared_ptr<arrow::RecordBatch>> data_batches;
  while (true) {
    auto batch = supplier->GetNextBatch();
    if (batch == nullptr) {
      break;
    }
    auto src_array = batch->column(0);
    auto dst_array = batch->column(1);
    parse_endpoint_column(src_indexer, src_array, src_lid);
    parse_endpoint_column(dst_indexer, dst_array, dst_lid);
    if (batch->num_columns() > 2) {
      batch = batch->RemoveColumn(0).ValueOrDie()->RemoveColumn(0).ValueOrDie();
      data_batches.push_back(batch);
    }
  }
  std::vector<bool> valid_flags;  // true for valid edges
  std::tie(src_lid, dst_lid, valid_flags) =
      filterInvalidEdges(src_lid, dst_lid);
  std::unique_lock singleLock(single_multiplicity_mutex_, std::defer_lock);
  if (meta_->oe_strategy == EdgeStrategy::kSingle ||
      meta_->ie_strategy == EdgeStrategy::kSingle) {
    singleLock.lock();
    ValidateSingleMultiplicity(src_lid, dst_lid);
  }
  size_t new_size = table_idx_.load() + src_lid.size();
  if (new_size > Capacity()) {
    auto new_cap = new_size;
    while (new_size >= new_cap) {
      new_cap = new_cap < 4096 ? 4096 : new_cap + (new_cap + 4) / 5;
    }
    EnsureCapacity(new_cap);
  }
  if (meta_->is_bundled()) {
    auto edges = extract_bundled_edge_data_from_batches(meta_, data_batches,
                                                        valid_flags);
    batch_add_bundled_edges_impl(out_csr_.get(), in_csr_.get(),
                                 meta_->properties, src_lid, dst_lid, edges);
  } else {
    auto oe_csr = dynamic_cast<TypedCsrBase<uint64_t>*>(out_csr_.get());
    auto ie_csr = dynamic_cast<TypedCsrBase<uint64_t>*>(in_csr_.get());
    assert(oe_csr != nullptr && ie_csr != nullptr);
    batch_add_unbundled_edges_impl(
        src_lid, dst_lid, oe_csr, ie_csr, table_.get(), table_idx_, capacity_,
        meta_->properties, data_batches, valid_flags);
  }
}

void EdgeTable::AccumulateEdgeDegrees(
    const IndexerType& src_indexer, const IndexerType& dst_indexer,
    std::shared_ptr<IRecordBatchSupplier> supplier,
    std::vector<uint32_t>& out_degrees, std::vector<uint32_t>& in_degrees,
    size_t& edge_count) const {
  if (out_degrees.size() != src_indexer.size() ||
      in_degrees.size() != dst_indexer.size()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Degree vector size does not match vertex indexer size");
  }

  std::vector<vid_t> src_lids;
  std::vector<vid_t> dst_lids;
  while (auto batch = supplier->GetNextBatch()) {
    src_lids.clear();
    dst_lids.clear();
    src_lids.reserve(batch->num_rows());
    dst_lids.reserve(batch->num_rows());
    parse_endpoint_column(src_indexer, batch->column(0), src_lids);
    parse_endpoint_column(dst_indexer, batch->column(1), dst_lids);
    if (src_lids.size() != dst_lids.size()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Source and destination endpoint counts do not match");
    }

    for (size_t i = 0; i < src_lids.size(); ++i) {
      auto src = src_lids[i];
      auto dst = dst_lids[i];
      if (src == std::numeric_limits<vid_t>::max() ||
          dst == std::numeric_limits<vid_t>::max()) {
        continue;
      }
      if (out_degrees[src] ==
              static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          in_degrees[dst] ==
              static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Vertex degree exceeds CSR capacity limit");
      }
      if (edge_count == std::numeric_limits<size_t>::max()) {
        THROW_INVALID_ARGUMENT_EXCEPTION("Edge count overflow");
      }
      ++out_degrees[src];
      ++in_degrees[dst];
      ++edge_count;
    }
  }
}

void EdgeTable::PrepareBatchAddEdges(
    const std::vector<uint32_t>& out_degrees,
    const std::vector<uint32_t>& in_degrees, size_t edge_count) {
  if ((meta_->oe_strategy == EdgeStrategy::kSingle &&
       std::any_of(out_degrees.begin(), out_degrees.end(),
                    [](auto degree) { return degree > 1; })) ||
      (meta_->ie_strategy == EdgeStrategy::kSingle &&
       std::any_of(in_degrees.begin(), in_degrees.end(),
                    [](auto degree) { return degree > 1; }))) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ONE relationship multiplicity violated by batch endpoint degrees");
  }
  if (table_idx_.load() != 0 || out_csr_->edge_num() != 0 ||
      in_csr_->edge_num() != 0) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "PrepareBatchAddEdges requires an empty edge table");
  }
  out_csr_->resize(static_cast<vid_t>(out_degrees.size()));
  in_csr_->resize(static_cast<vid_t>(in_degrees.size()));
  out_csr_->batch_reserve(out_degrees);
  in_csr_->batch_reserve(in_degrees);
  EnsureCapacity(edge_count);
}

void EdgeTable::BatchAddEdges(
    const std::vector<vid_t>& src_lid_list,
    const std::vector<vid_t>& dst_lid_list,
    const std::vector<std::vector<Property>>& edge_data_list) {
  std::unique_lock singleLock(single_multiplicity_mutex_, std::defer_lock);
  if (meta_->oe_strategy == EdgeStrategy::kSingle ||
      meta_->ie_strategy == EdgeStrategy::kSingle) {
    singleLock.lock();
    ValidateSingleMultiplicity(src_lid_list, dst_lid_list);
  }
  size_t new_size = table_idx_.load() + src_lid_list.size();
  if (new_size > Capacity()) {
    auto new_cap = new_size;
    while (new_size >= new_cap) {
      new_cap = new_cap < 4096 ? 4096 : new_cap + (new_cap + 4) / 5;
    }
    EnsureCapacity(new_cap);
  }
  if (meta_->is_bundled()) {
    std::vector<Property> flat_edge_data;
    assert(meta_->properties.size() == 1);
    if (meta_->properties[0] == DataTypeId::kEmpty) {
    } else {
      flat_edge_data.reserve(edge_data_list.size());
      for (const auto& edata : edge_data_list) {
        assert(edata.size() == 1);
        flat_edge_data.push_back(edata[0]);
      }
    }
    batch_add_bundled_edges_impl(out_csr_.get(), in_csr_.get(),
                                 meta_->properties, src_lid_list, dst_lid_list,
                                 flat_edge_data);
  } else {
    auto oe_csr = dynamic_cast<TypedCsrBase<uint64_t>*>(out_csr_.get());
    auto ie_csr = dynamic_cast<TypedCsrBase<uint64_t>*>(in_csr_.get());
    assert(oe_csr != nullptr && ie_csr != nullptr);
    size_t offset = table_idx_.fetch_add(src_lid_list.size());
    insert_edges_separated_impl(oe_csr, ie_csr, src_lid_list, dst_lid_list,
                                offset);
    for (size_t i = 0; i < edge_data_list.size(); ++i) {
      table_->insert(offset + i, edge_data_list[i], true);
    }
  }
}

namespace {

constexpr size_t kMinEdgesPerBuildWorker = 65536;
constexpr size_t kMaxEdgeBuildWorkers = 2;

size_t edge_build_worker_count(size_t edge_count,
                               size_t requested_worker_count) {
  if (edge_count == 0 || requested_worker_count <= 1) {
    return 1;
  }
  const size_t useful_workers =
      (edge_count + kMinEdgesPerBuildWorker - 1) /
      kMinEdgesPerBuildWorker;
  return std::min({requested_worker_count, useful_workers,
                   kMaxEdgeBuildWorkers});
}

void count_edge_degrees_parallel(
    const std::vector<vid_t>& source_lids,
    const std::vector<vid_t>& destination_lids, size_t source_vertex_count,
    size_t destination_vertex_count, size_t worker_count,
    std::vector<uint32_t>& out_degrees, std::vector<uint32_t>& in_degrees) {
  out_degrees.assign(source_vertex_count, 0);
  in_degrees.assign(destination_vertex_count, 0);
  if (source_lids.empty()) {
    return;
  }
  if (worker_count == 1) {
    for (size_t i = 0; i < source_lids.size(); ++i) {
      const auto source = source_lids[i];
      const auto destination = destination_lids[i];
      if (source >= source_vertex_count ||
          destination >= destination_vertex_count) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Internal edge endpoint is out of range");
      }
      if (out_degrees[source] ==
              static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          in_degrees[destination] ==
              static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Vertex degree exceeds CSR capacity limit");
      }
      ++out_degrees[source];
      ++in_degrees[destination];
    }
    return;
  }

  std::vector<std::atomic<uint32_t>> atomic_out_degrees(source_vertex_count);
  std::vector<std::atomic<uint32_t>> atomic_in_degrees(
      destination_vertex_count);
  for (auto& degree : atomic_out_degrees) {
    degree.store(0, std::memory_order_relaxed);
  }
  for (auto& degree : atomic_in_degrees) {
    degree.store(0, std::memory_order_relaxed);
  }
  std::atomic<bool> failed{false};
  auto worker = [&](size_t worker_index) {
    const size_t begin = source_lids.size() * worker_index / worker_count;
    const size_t end = source_lids.size() * (worker_index + 1) / worker_count;
    for (size_t i = begin; i < end && !failed.load(); ++i) {
      const auto source = source_lids[i];
      const auto destination = destination_lids[i];
      if (source >= source_vertex_count ||
          destination >= destination_vertex_count) {
        failed.store(true);
        break;
      }
      const auto old_out = atomic_out_degrees[source].fetch_add(
          1, std::memory_order_relaxed);
      const auto old_in = atomic_in_degrees[destination].fetch_add(
          1, std::memory_order_relaxed);
      if (old_out >= static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          old_in >= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        failed.store(true);
        break;
      }
    }
  };
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
    workers.emplace_back(worker, worker_index);
  }
  for (auto& thread : workers) {
    thread.join();
  }
  if (failed.load()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Internal endpoint or degree validation failed");
  }
  for (size_t i = 0; i < source_vertex_count; ++i) {
    out_degrees[i] = atomic_out_degrees[i].load(std::memory_order_relaxed);
  }
  for (size_t i = 0; i < destination_vertex_count; ++i) {
    in_degrees[i] = atomic_in_degrees[i].load(std::memory_order_relaxed);
  }
}

}  // namespace

void EdgeTable::BatchAddEmptyEdges(
    const std::vector<vid_t>& src_lid_list,
    const std::vector<vid_t>& dst_lid_list,
    const std::vector<size_t>* edge_chunk_ends,
    const std::vector<std::vector<uint32_t>>* out_degree_chunks,
    const std::vector<std::vector<uint32_t>>* in_degree_chunks,
    size_t requested_worker_count) {
  if (src_lid_list.size() != dst_lid_list.size()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Source and destination endpoint counts do not match");
  }
  if (!meta_->is_bundled() ||
      (!meta_->properties.empty() &&
       meta_->properties[0].id() != DataTypeId::kEmpty)) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "BatchAddEmptyEdges requires a property-free edge table");
  }
  auto* mutable_out = dynamic_cast<MutableCsr<EmptyType>*>(out_csr_.get());
  auto* mutable_in = dynamic_cast<MutableCsr<EmptyType>*>(in_csr_.get());
  const size_t worker_count = edge_build_worker_count(
      src_lid_list.size(), requested_worker_count);
  if (mutable_out != nullptr && mutable_in != nullptr &&
      out_csr_->edge_num() == 0 && in_csr_->edge_num() == 0) {
    for (size_t edge = 0; edge < src_lid_list.size(); ++edge) {
      if (src_lid_list[edge] >= out_csr_->size() ||
          dst_lid_list[edge] >= in_csr_->size()) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Internal edge endpoint is out of range");
      }
    }
    std::vector<uint32_t> out_degrees;
    std::vector<uint32_t> in_degrees;
    const bool has_degree_chunks = edge_chunk_ends != nullptr ||
                                   out_degree_chunks != nullptr ||
                                   in_degree_chunks != nullptr;
    if (has_degree_chunks &&
        (edge_chunk_ends == nullptr || out_degree_chunks == nullptr ||
         in_degree_chunks == nullptr)) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Edge chunk boundaries and both degree chunk sets are required");
    }
    if (has_degree_chunks) {
      if (edge_chunk_ends->empty() ||
          edge_chunk_ends->size() != out_degree_chunks->size() ||
          edge_chunk_ends->size() != in_degree_chunks->size() ||
          edge_chunk_ends->back() != src_lid_list.size()) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Precomputed edge chunk metadata does not match edge input");
      }
      size_t previous_end = 0;
      for (size_t chunk = 0; chunk < edge_chunk_ends->size(); ++chunk) {
        if ((*edge_chunk_ends)[chunk] <= previous_end ||
            (*edge_chunk_ends)[chunk] > src_lid_list.size() ||
            (*out_degree_chunks)[chunk].size() > out_csr_->size() ||
            (*in_degree_chunks)[chunk].size() > in_csr_->size()) {
          THROW_INVALID_ARGUMENT_EXCEPTION(
              "Precomputed edge chunk metadata is invalid");
        }
        previous_end = (*edge_chunk_ends)[chunk];
      }
      if (table_idx_.load() != 0) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Chunked CSR reserve requires an empty edge table");
      }
      mutable_out->batch_reserve_from_degree_chunks(*out_degree_chunks,
                                                    src_lid_list.size());
      mutable_in->batch_reserve_from_degree_chunks(*in_degree_chunks,
                                                   src_lid_list.size());
    } else {
      count_edge_degrees_parallel(
          src_lid_list, dst_lid_list, out_csr_->size(), in_csr_->size(),
          worker_count, out_degrees, in_degrees);
      mutable_out->batch_reserve(out_degrees);
      mutable_in->batch_reserve(in_degrees);
    }
    mutable_out->batch_put_edges_reserved(src_lid_list, dst_lid_list, {},
                                          worker_count, edge_chunk_ends,
                                          out_degree_chunks);
    mutable_in->batch_put_edges_reserved(dst_lid_list, src_lid_list, {},
                                         worker_count, edge_chunk_ends,
                                         in_degree_chunks);

    // The QueryProcessor write lock keeps both directions invisible until the
    // complete FILL statement returns. Endpoint and degree metadata validation
    // above happens before the first endpoint is populated.
    EnsureCapacity(src_lid_list.size());
    return;
  }
  batch_add_bundled_edges_impl(out_csr_.get(), in_csr_.get(),
                               meta_->properties, src_lid_list, dst_lid_list,
                               {});
}

void EdgeTable::Compact(bool compact_csr, bool sort_on_compaction,
                        timestamp_t ts) {
  if (compact_csr) {
    out_csr_->compact();
    in_csr_->compact();
  }
  if (sort_on_compaction) {
    out_csr_->batch_sort_by_edge_data(ts);
    in_csr_->batch_sort_by_edge_data(ts);
  }
  out_csr_->reset_timestamp();
  in_csr_->reset_timestamp();
}

size_t EdgeTable::Size() const {
  if (meta_->is_bundled()) {
    if (out_csr_) {
      return out_csr_->edge_num();
    } else if (in_csr_) {
      return in_csr_->edge_num();
    } else {
      THROW_RUNTIME_ERROR("both csr are null");
    }
  }
  // TODO(zhanglei): the size may be inaccurate if some edges are deleted but
  // not compacted yet.
  return table_idx_.load();
}

size_t EdgeTable::Capacity() const {
  if (meta_->is_bundled()) {
    if (out_csr_) {
      return out_csr_->capacity();
    } else if (in_csr_) {
      return in_csr_->capacity();
    } else {
      THROW_RUNTIME_ERROR("both csr are null");
    }
  }
  return capacity_.load();
}

void EdgeTable::dropAndCreateNewBundledCSR(
    std::shared_ptr<ColumnBase> remaining_col) {
  DataTypeId property_type = (remaining_col == nullptr)
                                 ? meta_->properties[0].id()
                                 : remaining_col->type();
  auto suffix = get_next_csr_path_suffix();
  std::string next_oe_csr_path =
      tmp_dir(work_dir_) + "/" +
      oe_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name) +
      suffix;
  std::string next_ie_csr_path =
      tmp_dir(work_dir_) + "/" +
      ie_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name) +
      suffix;

  std::unique_ptr<CsrBase> new_out_csr, new_in_csr;
  new_out_csr =
      create_csr(meta_->oe_mutable, meta_->oe_strategy, property_type);
  new_in_csr = create_csr(meta_->ie_mutable, meta_->ie_strategy, property_type);

  new_out_csr->open_in_memory(next_oe_csr_path);
  new_in_csr->open_in_memory(next_ie_csr_path);
  new_out_csr->resize(out_csr_->size());
  new_in_csr->resize(in_csr_->size());

  if (remaining_col == nullptr) {
    auto edges = out_csr_->batch_export(nullptr);
    batch_put_edges_with_default_edata(
        std::get<0>(edges), std::get<1>(edges), property_type,
        meta_->default_property_values[0], new_out_csr.get());
    batch_put_edges_with_default_edata(
        std::get<1>(edges), std::get<0>(edges), property_type,
        meta_->default_property_values[0], new_in_csr.get());
  } else {
    auto row_id_col = std::make_shared<ULongColumn>(StorageStrategy::kMem);
    auto edges = out_csr_->batch_export(row_id_col);
    std::vector<Property> remaining_data;
    remaining_data.reserve(row_id_col->size());
    for (size_t i = 0; i < row_id_col->size(); ++i) {
      auto row_id = row_id_col->get_view(i);
      CHECK_LT(row_id, remaining_col->size());
      remaining_data.emplace_back(remaining_col->get_prop(row_id));
    }
    batch_put_edges_to_bundled_csr(std::get<0>(edges), std::get<1>(edges),
                                   property_type, remaining_data,
                                   new_out_csr.get());
    batch_put_edges_to_bundled_csr(std::get<1>(edges), std::get<0>(edges),
                                   property_type, remaining_data,
                                   new_in_csr.get());
  }

  table_->drop();
  table_ = std::make_unique<Table>();
  table_idx_.store(0);
  capacity_.store(0);
  out_csr_->close();
  in_csr_->close();
  out_csr_ = std::move(new_out_csr);
  in_csr_ = std::move(new_in_csr);
}

void EdgeTable::dropAndCreateNewUnbundledCSR(bool delete_property) {
  auto suffix = get_next_csr_path_suffix();
  std::string next_oe_csr_path =
      tmp_dir(work_dir_) + "/" +
      oe_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name) +
      suffix;
  std::string next_ie_csr_path =
      tmp_dir(work_dir_) + "/" +
      ie_prefix(meta_->src_label_name, meta_->dst_label_name,
                meta_->edge_label_name) +
      suffix;
  std::string next_table_prefix = edata_prefix(
      meta_->src_label_name, meta_->dst_label_name, meta_->edge_label_name);
  // In this method, the edge table must be bundled, so the table must be
  // opened opened. In open_in_memory method, table will try to read the
  // existing table file from checkpoint_dir, but it must not exist.
  if (!delete_property) {
    LOG(INFO) << "rebuild unbundled edge csr with edge properties: "
              << meta_->property_names.size();
    table_->open_in_memory(next_table_prefix, work_dir_, meta_->property_names,
                           meta_->properties, meta_->strategies);
  }

  std::shared_ptr<ColumnBase> prev_data_col = nullptr;

  if (!delete_property) {
    if (table_->col_num() >= 1 &&
        table_->get_column_by_id(0)->type() != DataTypeId::kVarchar &&
        table_->get_column_by_id(0)->type() != DataTypeId::kEmpty) {
      prev_data_col = table_->get_column_by_id(0);
    }
  } else {
    // delete_property == true, which means the EdgeTable will become use csr of
    // empty type. we need to reset capacity and table_idx to 0
    table_idx_.store(0);
    capacity_.store(0);
  }

  auto edges = out_csr_->batch_export(prev_data_col);
  if (prev_data_col && prev_data_col->size() > 0) {
    table_->resize(prev_data_col->size(), meta_->default_property_values);
    table_idx_.store(prev_data_col->size());
    EnsureCapacity(prev_data_col->size());
  } else if (!delete_property) {
    table_->resize(std::get<0>(edges).size(), meta_->default_property_values);
    table_idx_.store(std::get<0>(edges).size());
    EnsureCapacity(std::get<0>(edges).size());
  }
  std::vector<uint64_t> row_ids;
  for (size_t i = 0; i < std::get<0>(edges).size(); ++i) {
    row_ids.push_back(i);
  }
  std::unique_ptr<CsrBase> new_out_csr, new_in_csr;
  if (delete_property) {
    new_out_csr =
        create_csr(meta_->oe_mutable, meta_->oe_strategy, DataTypeId::kEmpty);
    new_in_csr =
        create_csr(meta_->ie_mutable, meta_->ie_strategy, DataTypeId::kEmpty);
  } else {
    new_out_csr =
        create_csr(meta_->oe_mutable, meta_->oe_strategy, DataTypeId::kUInt64);
    new_in_csr =
        create_csr(meta_->ie_mutable, meta_->ie_strategy, DataTypeId::kUInt64);
  }

  new_out_csr->open_in_memory(next_oe_csr_path);
  new_in_csr->open_in_memory(next_ie_csr_path);
  new_out_csr->resize(out_csr_->size());
  new_in_csr->resize(in_csr_->size());
  if (delete_property) {
    dynamic_cast<TypedCsrBase<EmptyType>*>(new_out_csr.get())
        ->batch_put_edges(std::get<0>(edges), std::get<1>(edges), {});
    dynamic_cast<TypedCsrBase<EmptyType>*>(new_in_csr.get())
        ->batch_put_edges(std::get<1>(edges), std::get<0>(edges), {});
  } else {
    dynamic_cast<TypedCsrBase<uint64_t>*>(new_out_csr.get())
        ->batch_put_edges(std::get<0>(edges), std::get<1>(edges), row_ids);
    dynamic_cast<TypedCsrBase<uint64_t>*>(new_in_csr.get())
        ->batch_put_edges(std::get<1>(edges), std::get<0>(edges), row_ids);
  }
  out_csr_->close();
  in_csr_->close();
  out_csr_ = std::move(new_out_csr);
  in_csr_ = std::move(new_in_csr);
}

std::string EdgeTable::get_next_csr_path_suffix() {
  return std::string("_v_") + std::to_string(csr_alter_version_.fetch_add(1));
}

}  // namespace neug

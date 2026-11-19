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
#pragma once

#include "neug/transaction/transaction_utils.h"
#include "neug/utils/property/types.h"
#include "neug/utils/serialization/in_archive.h"
#include "neug/utils/serialization/out_archive.h"

#include <string>
#include <vector>

namespace neug {
class EdgeRecord;
class PropertyGraph;

struct IUndoLog {
  virtual ~IUndoLog() = default;
  virtual OpType GetType() const = 0;
  virtual void Undo(PropertyGraph& graph, timestamp_t ts) const = 0;
};

struct CreateVertexTypeUndo : public IUndoLog {
  std::string vertex_type;
  CreateVertexTypeUndo(const std::string& vt) : vertex_type(vt) {}
  OpType GetType() const override { return OpType::kCreateVertexType; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct CreateEdgeTypeUndo : public IUndoLog {
  std::string src_type;
  std::string dst_type;
  std::string edge_type;
  CreateEdgeTypeUndo(const std::string& s, const std::string& d,
                     const std::string& e)
      : src_type(s), dst_type(d), edge_type(e) {}
  OpType GetType() const override { return OpType::kCreateEdgeType; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct InsertVertexUndo : public IUndoLog {
  label_t v_label;
  vid_t vid;
  InsertVertexUndo(label_t v_label, vid_t vid) : v_label(v_label), vid(vid) {}
  OpType GetType() const override { return OpType::kInsertVertex; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct InsertEdgeUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  vid_t src_lid, dst_lid;
  int32_t oe_offset, ie_offset;
  InsertEdgeUndo(label_t src_label, label_t dst_label, label_t edge_label,
                 vid_t src_lid, vid_t dst_lid, int32_t oe_offset,
                 int32_t ie_offset)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        src_lid(src_lid),
        dst_lid(dst_lid),
        oe_offset(oe_offset),
        ie_offset(ie_offset) {}
  OpType GetType() const override { return OpType::kInsertEdge; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct UpdateVertexPropUndo : public IUndoLog {
  label_t v_label;
  vid_t vid;
  int32_t col_id;
  Property value;
  UpdateVertexPropUndo(label_t v_label, vid_t vid, int32_t col_id,
                       const Property& value)
      : v_label(v_label), vid(vid), col_id(col_id), value(value) {}
  OpType GetType() const override { return OpType::kUpdateVertexProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

// One undo entry covering a batch of updates to a single vertex property
// column. `old_values` pairs the vertex lid with the pre-update value, in
// application order; replay restores each element in reverse order.
struct UpdateVertexPropBatchUndo : public IUndoLog {
  label_t v_label;
  int32_t col_id;
  std::vector<std::pair<vid_t, Property>> old_values;
  UpdateVertexPropBatchUndo(
      label_t v_label, int32_t col_id,
      std::vector<std::pair<vid_t, Property>> old_values)
      : v_label(v_label),
        col_id(col_id),
        old_values(std::move(old_values)) {}
  OpType GetType() const override { return OpType::kUpdateVertexProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct UpdateEdgePropUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  vid_t src_lid, dst_lid;
  int32_t oe_offset, ie_offset;
  int32_t col_id;
  Property value;
  timestamp_t oe_timestamp, ie_timestamp;
  UpdateEdgePropUndo(label_t src_label, label_t dst_label, label_t edge_label,
                     vid_t src_lid, vid_t dst_lid, int32_t oe_offset,
                     int32_t ie_offset, int32_t col_id, const Property& value,
                     timestamp_t oe_timestamp, timestamp_t ie_timestamp)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        src_lid(src_lid),
        dst_lid(dst_lid),
        oe_offset(oe_offset),
        ie_offset(ie_offset),
        col_id(col_id),
        value(value),
        oe_timestamp(oe_timestamp),
        ie_timestamp(ie_timestamp) {}
  OpType GetType() const override { return OpType::kUpdateEdgeProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct UpdateEdgePropBatchUndo : public IUndoLog {
  using Row = std::tuple<vid_t, vid_t, int32_t, int32_t, Property>;
  label_t src_label, dst_label, edge_label;
  int32_t col_id;
  std::vector<Row> old_values;
  UpdateEdgePropBatchUndo(label_t src, label_t dst, label_t edge, int32_t column,
                          std::vector<Row> values)
      : src_label(src), dst_label(dst), edge_label(edge), col_id(column),
        old_values(std::move(values)) {}
  OpType GetType() const override { return OpType::kUpdateEdgeProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct RemoveVertexUndo : public IUndoLog {
  label_t v_label;
  vid_t lid;
  timestamp_t timestamp;
  // Incident edges have their own preceding RemoveEdgeUndo entries.
  RemoveVertexUndo(label_t v_label, vid_t lid, timestamp_t timestamp)
      : v_label(v_label), lid(lid), timestamp(timestamp) {}
  OpType GetType() const override { return OpType::kRemoveVertex; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct RemoveEdgeUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  vid_t src_lid, dst_lid;
  int32_t oe_offset, ie_offset;
  timestamp_t oe_timestamp, ie_timestamp;
  RemoveEdgeUndo(label_t src_label, label_t dst_label, label_t edge_label,
                 vid_t src_lid, vid_t dst_lid, int32_t oe_offset,
                 int32_t ie_offset, timestamp_t oe_timestamp,
                 timestamp_t ie_timestamp)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        src_lid(src_lid),
        dst_lid(dst_lid),
        oe_offset(oe_offset),
        ie_offset(ie_offset),
        oe_timestamp(oe_timestamp),
        ie_timestamp(ie_timestamp) {}
  OpType GetType() const override { return OpType::kRemoveEdge; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct AddVertexPropUndo : public IUndoLog {
  label_t label;
  std::vector<std::string> prop_names;
  AddVertexPropUndo(label_t label, const std::vector<std::string>& prop_names)
      : label(label), prop_names(prop_names) {}
  OpType GetType() const override { return OpType::kAddVertexProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct AddEdgePropUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  std::vector<std::string> prop_names;
  AddEdgePropUndo(label_t src_label, label_t dst_label, label_t edge_label,
                  const std::vector<std::string>& prop_names)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        prop_names(prop_names) {}
  OpType GetType() const override { return OpType::kAddEdgeProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};
struct RenameVertexPropUndo : public IUndoLog {
  label_t label;
  std::vector<std::pair<std::string, std::string>> old_names_to_new_names;
  RenameVertexPropUndo(label_t label,
                       const std::vector<std::pair<std::string, std::string>>&
                           old_names_to_new_names)
      : label(label), old_names_to_new_names(old_names_to_new_names) {}
  OpType GetType() const override { return OpType::kRenameVertexProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};
struct RenameEdgePropUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  std::vector<std::pair<std::string, std::string>> old_names_to_new_names;
  RenameEdgePropUndo(label_t src_label, label_t dst_label, label_t edge_label,
                     const std::vector<std::pair<std::string, std::string>>&
                         old_names_to_new_names)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        old_names_to_new_names(old_names_to_new_names) {}
  OpType GetType() const override { return OpType::kRenameEdgeProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct DeleteVertexPropUndo : public IUndoLog {
  label_t label;
  std::vector<std::string> prop_names;
  DeleteVertexPropUndo(label_t label,
                       const std::vector<std::string>& prop_names)
      : label(label), prop_names(prop_names) {}
  OpType GetType() const override { return OpType::kDeleteVertexProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct DeleteEdgePropUndo : public IUndoLog {
  label_t src_label, dst_label, edge_label;
  std::vector<std::string> prop_names;
  DeleteEdgePropUndo(label_t src_label, label_t dst_label, label_t edge_label,
                     const std::vector<std::string>& prop_names)
      : src_label(src_label),
        dst_label(dst_label),
        edge_label(edge_label),
        prop_names(prop_names) {}
  OpType GetType() const override { return OpType::kDeleteEdgeProp; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct DeleteVertexTypeUndo : public IUndoLog {
  std::string v_label;
  DeleteVertexTypeUndo(const std::string& v_label) : v_label(v_label) {}
  OpType GetType() const override { return OpType::kDeleteVertexType; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

struct DeleteEdgeTypeUndo : public IUndoLog {
  std::string src_label, dst_label, edge_label;
  DeleteEdgeTypeUndo(const std::string& s, const std::string& d,
                     const std::string& e)
      : src_label(s), dst_label(d), edge_label(e) {}
  OpType GetType() const override { return OpType::kDeleteEdgeType; }
  void Undo(PropertyGraph& graph, timestamp_t ts) const override;
};

}  // namespace neug

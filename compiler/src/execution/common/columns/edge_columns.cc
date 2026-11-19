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

#include "neug/execution/common/columns/edge_columns.h"

namespace neug {

namespace execution {

std::shared_ptr<IContextColumn> SDSLEdgeColumn::shuffle(
    const std::vector<size_t>& offsets) const {
  SDSLEdgeColumnBuilder builder(dir_, label_);
  builder.reserve(offsets.size());
  bool has_offsets = !oe_offsets_.empty();
  if (is_optional_) {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      if (std::get<0>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<1>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        if (has_offsets) {
          builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                                oe_offsets_[offset], ie_offsets_[offset]);
        } else {
          builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e));
        }
      }
    }
  } else {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      if (has_offsets) {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              oe_offsets_[offset], ie_offsets_[offset]);
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e));
      }
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> SDSLEdgeColumn::optional_shuffle(
    const std::vector<size_t>& offsets) const {
  SDSLEdgeColumnBuilder builder(dir_, label_);
  builder.reserve(offsets.size());
  bool has_offsets = !oe_offsets_.empty();
  for (auto offset : offsets) {
    if (offset == std::numeric_limits<size_t>::max()) {
      builder.push_back_null();
    } else {
      auto& e = edges_[offset];
      if (std::get<0>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<1>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        if (has_offsets) {
          builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                                oe_offsets_[offset], ie_offsets_[offset]);
        } else {
          builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e));
        }
      }
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> SDSLEdgeColumnBuilder::finish() {
  auto col = std::make_shared<SDSLEdgeColumn>(dir_, label_);
  col->edges_ = std::move(edges_);
  col->oe_offsets_ = std::move(oe_offsets_);
  col->ie_offsets_ = std::move(ie_offsets_);
  col->is_optional_ = is_optional_;
  return col;
}

namespace {
std::shared_ptr<IContextColumn> shuffle_segmented_edges(
    const MSEdgeColumn& column, const std::vector<size_t>& offsets) {
  // Preserve physical identities in a flat column. Rebuilding without CSR
  // offsets merges parallel edges, while rebuilding one segment per row can
  // make later lookups quadratic for alternating typed domains.
  BDMLEdgeColumnBuilder builder(column.get_labels());
  builder.reserve(offsets.size());
  for (auto offset : offsets) {
    if (offset == std::numeric_limits<size_t>::max() ||
        !column.has_value(offset)) {
      builder.push_back_null();
      continue;
    }
    const auto edge = column.get_edge(offset);
    builder.push_back_opt(edge.label, edge.src, edge.dst, edge.prop, edge.dir,
                          edge.oe_offset, edge.ie_offset);
  }
  return builder.finish();
}
}  // namespace

std::shared_ptr<IContextColumn> MSEdgeColumn::shuffle(
    const std::vector<size_t>& offsets) const {
  return shuffle_segmented_edges(*this, offsets);
}

std::shared_ptr<IContextColumn> MSEdgeColumn::optional_shuffle(
    const std::vector<size_t>& offsets) const {
  return shuffle_segmented_edges(*this, offsets);
}

std::shared_ptr<IContextColumn> BDSLEdgeColumn::shuffle(
    const std::vector<size_t>& offsets) const {
  BDSLEdgeColumnBuilder builder(label_);
  builder.reserve(offsets.size());
  if (is_optional_) {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      if (std::get<0>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<1>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e));
      }
    }
  } else {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                            std::get<3>(e));
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> BDSLEdgeColumn::optional_shuffle(
    const std::vector<size_t>& offsets) const {
  BDSLEdgeColumnBuilder builder(label_);
  builder.reserve(offsets.size());
  for (auto offset : offsets) {
    if (offset == std::numeric_limits<size_t>::max()) {
      builder.push_back_null();
    } else {
      auto& e = edges_[offset];
      if (std::get<0>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<1>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e));
      }
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> SDMLEdgeColumn::shuffle(
    const std::vector<size_t>& offsets) const {
  SDMLEdgeColumnBuilder builder(dir_, labels_);
  builder.reserve(offsets.size());
  if (is_optional_) {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      if (std::get<1>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<2>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e));
      }
    }
  } else {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                            std::get<3>(e));
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> SDMLEdgeColumn::optional_shuffle(
    const std::vector<size_t>& offsets) const {
  SDMLEdgeColumnBuilder builder(dir_, labels_);
  builder.reserve(offsets.size());
  for (auto offset : offsets) {
    if (offset == std::numeric_limits<size_t>::max()) {
      builder.push_back_null();
    } else {
      auto& e = edges_[offset];
      if (std::get<1>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<2>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e));
      }
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> BDMLEdgeColumn::shuffle(
    const std::vector<size_t>& offsets) const {
  BDMLEdgeColumnBuilder builder(labels_);
  builder.reserve(offsets.size());
  if (is_optional_) {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      if (std::get<1>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<2>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e), std::get<4>(e),
                              std::get<5>(e), std::get<6>(e));
      }
    }
  } else {
    for (auto offset : offsets) {
      auto& e = edges_[offset];
      builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                            std::get<3>(e), std::get<4>(e),
                            std::get<5>(e), std::get<6>(e));
    }
  }
  return builder.finish();
}

std::shared_ptr<IContextColumn> BDMLEdgeColumn::optional_shuffle(
    const std::vector<size_t>& offsets) const {
  BDMLEdgeColumnBuilder builder(labels_);
  builder.reserve(offsets.size());
  for (auto offset : offsets) {
    if (offset == std::numeric_limits<size_t>::max()) {
      builder.push_back_null();
    } else {
      auto& e = edges_[offset];
      if (std::get<1>(e) == std::numeric_limits<vid_t>::max() ||
          std::get<2>(e) == std::numeric_limits<vid_t>::max()) {
        builder.push_back_null();
      } else {
        builder.push_back_opt(std::get<0>(e), std::get<1>(e), std::get<2>(e),
                              std::get<3>(e), std::get<4>(e),
                              std::get<5>(e), std::get<6>(e));
      }
    }
  }
  return builder.finish();
}

}  // namespace execution

}  // namespace neug

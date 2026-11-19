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

#include "neug/storages/csr/mutable_csr.h"

#include <errno.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "neug/storages/file_names.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/file_utils.h"
#include "neug/utils/property/types.h"
#include "neug/utils/spinlock.h"

namespace neug {

template <typename EDATA_T>
size_t MutableCsr<EDATA_T>::count_valid_edges() const {
  size_t count = 0;
  for (size_t src = 0; src < adj_list_size_.size(); ++src) {
    const nbr_t* nbrs = adj_list_buffer_[src];
    const size_t degree =
        adj_list_size_[src].load(std::memory_order_relaxed);
    for (size_t offset = 0; offset < degree; ++offset) {
      if (nbrs[offset].timestamp.load(std::memory_order_relaxed) !=
          INVALID_TIMESTAMP) {
        ++count;
      }
    }
  }
  return count;
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::open(const std::string& name,
                               const std::string& snapshot_dir,
                               const std::string& work_dir) {
  mmap_array<int> degree_list;
  mmap_array<int>* cap_list = &degree_list;
  if (snapshot_dir != "") {
    degree_list.open(snapshot_dir + "/" + name + ".deg", false);
    if (std::filesystem::exists(snapshot_dir + "/" + name + ".cap")) {
      cap_list = new mmap_array<int>();
      cap_list->open(snapshot_dir + "/" + name + ".cap", false);
    }
    nbr_list_.open(snapshot_dir + "/" + name + ".nbr", false);
    load_meta(snapshot_dir + "/" + name);
  }
  if (std::filesystem::exists(tmp_dir(work_dir) + "/" + name + ".nbr")) {
    std::filesystem::remove(tmp_dir(work_dir) + "/" + name + ".nbr");
  }
  nbr_list_.touch(tmp_dir(work_dir) + "/" + name + ".nbr");
  adj_list_buffer_.open(tmp_dir(work_dir) + "/" + name + ".adj_buffer", true);
  adj_list_buffer_.resize(degree_list.size());
  adj_list_size_.open(tmp_dir(work_dir) + "/" + name + ".adj_size", true);
  adj_list_size_.resize(degree_list.size());
  adj_list_capacity_.open(tmp_dir(work_dir) + "/" + name + ".adj_cap", true);
  adj_list_capacity_.resize(degree_list.size());
  locks_ = new SpinLock[degree_list.size()];

  // Writable adjacency aliases must dirty the backing array for checkpoint.
  nbr_t* ptr = nbr_list_.data();
  for (size_t i = 0; i < degree_list.size(); ++i) {
    int degree = degree_list[i];
    int cap = (*cap_list)[i];
    adj_list_buffer_[i] = ptr;
    adj_list_capacity_[i] = cap;
    adj_list_size_[i] = degree;
    ptr += cap;
  }
  if (cap_list != &degree_list) {
    delete cap_list;
  }
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::open_in_memory(const std::string& prefix) {
  mmap_array<int> degree_list;
  degree_list.open(prefix + ".deg", false);
  load_meta(prefix);
  mmap_array<int>* cap_list = &degree_list;
  if (std::filesystem::exists(prefix + ".cap")) {
    cap_list = new mmap_array<int>();
    cap_list->open(prefix + ".cap", false);
  }

  nbr_list_.open(prefix + ".nbr", false);

  adj_list_buffer_.reset();
  adj_list_size_.reset();
  adj_list_capacity_.reset();
  auto v_cap = degree_list.size();
  adj_list_buffer_.resize(v_cap);
  adj_list_size_.resize(v_cap);
  adj_list_capacity_.resize(v_cap);
  locks_ = new SpinLock[v_cap];

  // Adjacency pointers are writable, including through EdgeDataAccessor.
  // Mark the private mapping dirty before publishing those pointers so a
  // later contiguous dump cannot hard-link the unchanged checkpoint file.
  nbr_t* ptr = nbr_list_.data();
  for (size_t i = 0; i < degree_list.size(); ++i) {
    int degree = degree_list[i];
    int cap = (*cap_list)[i];
    adj_list_buffer_[i] = ptr;
    adj_list_capacity_[i] = cap;
    adj_list_size_[i] = degree;
    ptr += cap;
  }

  if (cap_list != &degree_list) {
    delete cap_list;
  }
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::open_with_hugepages(const std::string& prefix) {
  mmap_array<int> degree_list;
  degree_list.open(prefix + ".deg", false);
  load_meta(prefix);
  mmap_array<int>* cap_list = &degree_list;
  if (std::filesystem::exists(prefix + ".cap")) {
    cap_list = new mmap_array<int>();
    cap_list->open(prefix + ".cap", false);
  }

  nbr_list_.open_with_hugepages(prefix + ".nbr");

  adj_list_buffer_.reset();
  adj_list_size_.reset();
  adj_list_capacity_.reset();
  auto v_cap = degree_list.size();
  adj_list_buffer_.open_with_hugepages("");
  adj_list_buffer_.resize(v_cap);
  adj_list_size_.open_with_hugepages("");
  adj_list_size_.resize(v_cap);
  adj_list_capacity_.open_with_hugepages("");
  adj_list_capacity_.resize(v_cap);
  locks_ = new SpinLock[v_cap];

  // Hugepage allocation can fall back to a private file mapping as well.
  nbr_t* ptr = nbr_list_.data();
  for (size_t i = 0; i < degree_list.size(); ++i) {
    int degree = degree_list[i];
    int cap = (*cap_list)[i];
    adj_list_buffer_[i] = ptr;
    adj_list_capacity_[i] = cap;
    adj_list_size_[i] = degree;
    ptr += cap;
  }

  if (cap_list != &degree_list) {
    delete cap_list;
  }
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::dump(const std::string& name,
                               const std::string& new_snapshot_dir) {
  size_t vnum = adj_list_buffer_.size();
  dump_meta(new_snapshot_dir + "/" + name);
  mmap_array<int> degree_list;
  std::vector<int> cap_list;
  degree_list.open("", false);
  degree_list.resize(vnum);
  cap_list.resize(vnum);
  bool need_cap_list = false;
  size_t offset = 0;
  for (size_t i = 0; i < vnum; ++i) {
    offset += adj_list_capacity_[i];

    degree_list[i] = adj_list_size_[i];
    cap_list[i] = adj_list_capacity_[i];
    if (degree_list[i] != cap_list[i]) {
      need_cap_list = true;
    }
  }

  if (need_cap_list) {
    write_file(new_snapshot_dir + "/" + name + ".cap", cap_list.data(),
               sizeof(int), cap_list.size());
  }

  degree_list.dump(new_snapshot_dir + "/" + name + ".deg");

  std::string filename = new_snapshot_dir + "/" + name + ".nbr";
  bool is_contiguous = offset == nbr_list_.size();
  size_t expected_offset = 0;
  const nbr_t* base =
      static_cast<const mmap_array<nbr_t>&>(nbr_list_).data();
  for (size_t i = 0; is_contiguous && i < vnum; ++i) {
    size_t capacity = static_cast<size_t>(adj_list_capacity_[i]);
    if (capacity != 0 &&
        adj_list_buffer_[i] != base + expected_offset) {
      is_contiguous = false;
      break;
    }
    expected_offset += capacity;
  }
  if (is_contiguous) {
    nbr_list_.dump(filename);
    return;
  }

  FILE* fout = fopen((new_snapshot_dir + "/" + name + ".nbr").c_str(), "wb");
  if (fout == nullptr) {
    std::stringstream ss;
    ss << "Failed to open nbr list " << filename << ", " << strerror(errno);
    LOG(ERROR) << ss.str();
    throw std::runtime_error(ss.str());
  }

  for (size_t i = 0; i < vnum; ++i) {
    size_t ret{};
    if ((ret = fwrite(adj_list_buffer_[i], sizeof(nbr_t), adj_list_capacity_[i],
                      fout)) != static_cast<size_t>(adj_list_capacity_[i])) {
      std::stringstream ss;
      ss << "Failed to write nbr list " << filename << ", expected "
         << adj_list_capacity_[i] << ", got " << ret << ", " << strerror(errno);
      LOG(ERROR) << ss.str();
      throw std::runtime_error(ss.str());
    }
  }
  int ret = 0;
  if ((ret = fflush(fout)) != 0) {
    std::stringstream ss;
    ss << "Failed to flush nbr list " << filename << ", error code: " << ret
       << " " << strerror(errno);
    LOG(ERROR) << ss.str();
    throw std::runtime_error(ss.str());
  }
  if ((ret = fclose(fout)) != 0) {
    std::stringstream ss;
    ss << "Failed to close nbr list " << filename << ", error code: " << ret
       << " " << strerror(errno);
    LOG(ERROR) << ss.str();
    throw std::runtime_error(ss.str());
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::reset_timestamp() {
  // Adjacency pointers may alias a MAP_PRIVATE mapping. Timestamp changes must
  // prevent dump() from hard-linking the unchanged checkpoint backing file.
  (void) nbr_list_.data();
  size_t vnum = adj_list_buffer_.size();
  for (size_t i = 0; i != vnum; ++i) {
    nbr_t* nbrs = adj_list_buffer_[i];
    size_t deg = adj_list_size_[i].load(std::memory_order_relaxed);
    for (size_t j = 0; j != deg; ++j) {
      if (nbrs[j].timestamp != INVALID_TIMESTAMP) {
        nbrs[j].timestamp.store(0, std::memory_order_relaxed);
      }
    }
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::compact() {
  // We don't shrink the capacity of each adjacency list, but just remove the
  // deleted edges.
  // adj_list_buffer_ can alias a privately mapped nbr_list_. Mark the backing
  // array dirty before modifying it so dump() does not reuse the stale file.
  (void) nbr_list_.data();
  size_t vnum = adj_list_buffer_.size();
  for (size_t i = 0; i != vnum; ++i) {
    int sz = adj_list_size_[i];
    nbr_t* read_ptr = adj_list_buffer_[i];
    nbr_t* read_end = read_ptr + sz;
    nbr_t* write_ptr = adj_list_buffer_[i];
    int removed = 0;
    while (read_ptr != read_end) {
      if (read_ptr->timestamp != INVALID_TIMESTAMP) {
        if (removed) {
          *write_ptr = *read_ptr;
        }
        ++write_ptr;
      } else {
        ++removed;
      }
      ++read_ptr;
    }
    adj_list_size_[i] -= removed;
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::resize(vid_t vnum) {
  if (vnum > adj_list_size_.size()) {
    size_t old_size = adj_list_size_.size();
    adj_list_buffer_.resize(vnum);
    adj_list_size_.resize(vnum);
    adj_list_capacity_.resize(vnum);
    for (size_t k = old_size; k != vnum; ++k) {
      adj_list_buffer_[k] = nullptr;
      adj_list_size_[k] = 0;
      adj_list_capacity_[k] = 0;
    }
    delete[] locks_;
    locks_ = new SpinLock[vnum];
  } else {
    size_t removed = 0;
    for (size_t src = vnum; src < adj_list_size_.size(); ++src) {
      const nbr_t* nbrs = adj_list_buffer_[src];
      const size_t degree =
          adj_list_size_[src].load(std::memory_order_relaxed);
      for (size_t offset = 0; offset < degree; ++offset) {
        removed += nbrs[offset].timestamp.load(std::memory_order_relaxed) !=
                   INVALID_TIMESTAMP;
      }
    }
    edge_count_.fetch_sub(removed, std::memory_order_relaxed);
    adj_list_buffer_.resize(vnum);
    adj_list_size_.resize(vnum);
    adj_list_capacity_.resize(vnum);
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_reserve(
    const std::vector<uint32_t>& degrees) {
  if (edge_num() != 0) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "batch_reserve can only be used on an empty CSR");
  }
  resize(static_cast<vid_t>(degrees.size()));

  size_t total_capacity = 0;
  for (auto degree : degrees) {
    if (degree > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Vertex degree exceeds MutableCsr capacity limit");
    }
    if (total_capacity > std::numeric_limits<size_t>::max() - degree) {
      THROW_INVALID_ARGUMENT_EXCEPTION("MutableCsr capacity overflow");
    }
    total_capacity += degree;
  }

  nbr_list_.resize(total_capacity);
  nbr_t* base = total_capacity == 0 ? nullptr : nbr_list_.data();
  size_t offset = 0;
  for (size_t i = 0; i < degrees.size(); ++i) {
    adj_list_buffer_[i] = base == nullptr ? nullptr : base + offset;
    adj_list_size_[i].store(0);
    adj_list_capacity_[i] = static_cast<int>(degrees[i]);
    offset += degrees[i];
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_reserve_from_degree_chunks(
    const std::vector<std::vector<uint32_t>>& degree_chunks,
    size_t expected_edge_count) {
  if (edge_num() != 0) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "batch_reserve can only be used on an empty CSR");
  }
  if (degree_chunks.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("At least one degree chunk is required");
  }
  for (const auto& chunk : degree_chunks) {
    if (chunk.size() > adj_list_size_.size()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Degree chunk size exceeds MutableCsr vertex capacity");
    }
  }

  size_t total_capacity = 0;
  for (size_t vertex = 0; vertex < adj_list_size_.size(); ++vertex) {
    uint64_t degree = 0;
    for (const auto& chunk : degree_chunks) {
      if (vertex < chunk.size()) {
        degree += chunk[vertex];
      }
    }
    if (degree > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Vertex degree exceeds MutableCsr capacity limit");
    }
    if (total_capacity > std::numeric_limits<size_t>::max() - degree) {
      THROW_INVALID_ARGUMENT_EXCEPTION("MutableCsr capacity overflow");
    }
    adj_list_capacity_[vertex] = static_cast<int>(degree);
    total_capacity += degree;
  }
  if (total_capacity != expected_edge_count) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Degree chunks do not match the expected edge count");
  }

  nbr_list_.resize(total_capacity);
  nbr_t* base = total_capacity == 0 ? nullptr : nbr_list_.data();
  size_t offset = 0;
  for (size_t vertex = 0; vertex < adj_list_size_.size(); ++vertex) {
    adj_list_buffer_[vertex] = base == nullptr ? nullptr : base + offset;
    adj_list_size_[vertex].store(0);
    offset += adj_list_capacity_[vertex];
  }
}

template <typename EDATA_T>
size_t MutableCsr<EDATA_T>::capacity() const {
  // We assume the capacity of each csr is INFINITE.
  return CsrBase::INFINITE_CAPACITY;
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::close() {
  if (locks_ != nullptr) {
    delete[] locks_;
    locks_ = nullptr;
  }
  adj_list_buffer_.reset();
  adj_list_size_.reset();
  adj_list_capacity_.reset();
  nbr_list_.reset();
  edge_count_.store(0, std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_sort_by_edge_data(timestamp_t ts) {
  size_t vnum = adj_list_buffer_.size();
  for (size_t i = 0; i != vnum; ++i) {
    std::sort(
        adj_list_buffer_[i],
        adj_list_buffer_[i] + adj_list_size_[i].load(std::memory_order_relaxed),
        [](const nbr_t& lhs, const nbr_t& rhs) { return lhs.data < rhs.data; });
  }
  unsorted_since_ = ts;
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_delete_vertices(
    const std::set<vid_t>& src_set, const std::set<vid_t>& dst_set) {
  vid_t vnum = adj_list_size_.size();
  size_t removed_valid = 0;
  for (vid_t src : src_set) {
    if (src < vnum) {
      const size_t degree =
          adj_list_size_[src].load(std::memory_order_relaxed);
      for (size_t offset = 0; offset < degree; ++offset) {
        removed_valid +=
            adj_list_buffer_[src][offset].timestamp.load(
                std::memory_order_relaxed) != INVALID_TIMESTAMP;
      }
      adj_list_size_[src] = 0;
    }
  }
  for (vid_t src = 0; src < vnum; ++src) {
    if (adj_list_size_[src] == 0) {
      continue;
    }
    const nbr_t* read_ptr = adj_list_buffer_[src];
    const nbr_t* read_end = read_ptr + adj_list_size_[src].load();
    nbr_t* write_ptr = adj_list_buffer_[src];
    int removed = 0;
    while (read_ptr != read_end) {
      vid_t nbr = read_ptr->neighbor;
      if (dst_set.find(nbr) == dst_set.end()) {
        if (removed) {
          *write_ptr = *read_ptr;
        }
        ++write_ptr;
      } else {
        removed_valid += read_ptr->timestamp.load(std::memory_order_relaxed) !=
                         INVALID_TIMESTAMP;
        ++removed;
      }
      ++read_ptr;
    }
    adj_list_size_[src] -= removed;
  }
  edge_count_.fetch_sub(removed_valid, std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_delete_edges(
    const std::vector<vid_t>& src_list, const std::vector<vid_t>& dst_list) {
  constexpr timestamp_t kDeleted = std::numeric_limits<timestamp_t>::max();
  vid_t vnum = adj_list_size_.size();

  // Group edges by source vertex using hash map for O(1) insert.
  std::unordered_map<vid_t, std::vector<vid_t>> src_dst_map;
  for (size_t i = 0; i < src_list.size(); ++i) {
    vid_t src = src_list[i];
    if (src < vnum) {
      src_dst_map[src].push_back(dst_list[i]);
    }
  }

  for (auto& [src, targets] : src_dst_map) {
    // Sort + deduplicate targets for efficient lookup.
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    nbr_t* neighbors = adj_list_buffer_[src];
    int degree = adj_list_size_[src].load();
    size_t target_cnt = targets.size();

    if (target_cnt >= static_cast<size_t>(degree) / 2) {
      // Endpoint deletion removes every physical occurrence, including
      // parallel edges. A neighbor->single-offset map loses all but one
      // occurrence and leaves the source graph and FILL retractions divergent.
      const std::unordered_set<vid_t> target_set(targets.begin(), targets.end());
      for (int32_t i = 0; i < degree; ++i) {
        if (neighbors[i].timestamp.load() != kDeleted &&
            target_set.contains(neighbors[i].neighbor)) {
          if (neighbors[i].timestamp.exchange(kDeleted) != kDeleted) {
            edge_count_.fetch_sub(1, std::memory_order_relaxed);
          }
        }
      }
    } else {
      // Few targets: scan adjacency list once, check each live neighbor
      // against sorted targets via binary search.
      for (int32_t i = 0; i < degree; ++i) {
        if (neighbors[i].timestamp.load() == kDeleted) continue;
        if (std::binary_search(targets.begin(), targets.end(),
                               neighbors[i].neighbor)) {
          if (neighbors[i].timestamp.exchange(kDeleted) != kDeleted) {
            edge_count_.fetch_sub(1, std::memory_order_relaxed);
          }
        }
      }
    }
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_delete_edges(
    const std::vector<std::pair<vid_t, int32_t>>& edges) {
  constexpr timestamp_t kDeleted = std::numeric_limits<timestamp_t>::max();
  vid_t vnum = adj_list_size_.size();
  // Direct marking: offsets are already known, no grouping or scanning needed.
  // Deduplicate by (src, offset) to avoid double-marking the same edge.
  // Use a lightweight hash set of packed (src << 32 | offset) for dedup.
  std::unordered_set<uint64_t> seen;
  seen.reserve(edges.size());
  for (const auto& edge : edges) {
    vid_t src = edge.first;
    int32_t offset = edge.second;
    if (src >= vnum || offset < 0 ||
        offset >= adj_list_size_[src].load()) {
      continue;
    }
    uint64_t key = (static_cast<uint64_t>(src) << 32) |
                   static_cast<uint32_t>(offset);
    if (seen.insert(key).second) {
      if (adj_list_buffer_[src][offset].timestamp.exchange(kDeleted) != kDeleted) {
        edge_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::delete_edge(vid_t src, int32_t offset,
                                      timestamp_t ts) {
  vid_t vnum = adj_list_size_.size();
  if (src >= vnum || offset >= adj_list_size_[src]) {
    THROW_INVALID_ARGUMENT_EXCEPTION("src out of bound or offset out of bound");
  }
  nbr_t* nbrs = adj_list_buffer_[src];
  auto old_ts = nbrs[offset].timestamp.load();
  if (old_ts <= ts) {
    if (nbrs[offset].timestamp.exchange(INVALID_TIMESTAMP) !=
        INVALID_TIMESTAMP) {
      edge_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  } else if (old_ts == std::numeric_limits<timestamp_t>::max()) {
    LOG(ERROR) << "Attempting to delete already deleted edge.";
  } else {
    LOG(ERROR) << "Attempting to delete edge with timestamp " << old_ts
               << " using older timestamp " << ts;
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::revert_delete_edge(vid_t src, vid_t nbr,
                                             int32_t offset, timestamp_t ts) {
  vid_t vnum = adj_list_size_.size();
  if (src >= vnum || offset >= adj_list_size_[src]) {
    THROW_INVALID_ARGUMENT_EXCEPTION("src out of bound or offset out of bound");
  }
  nbr_t* nbrs = adj_list_buffer_[src];
  if (nbrs[offset].neighbor != nbr) {
    THROW_INVALID_ARGUMENT_EXCEPTION("neighbor id not match");
  }
  auto old_ts = nbrs[offset].timestamp.load();
  if (old_ts == std::numeric_limits<timestamp_t>::max()) {
    assert(nbrs[offset].neighbor == nbr);
    nbrs[offset].timestamp.store(ts);
    edge_count_.fetch_add(1, std::memory_order_relaxed);
  } else {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Attempting to revert delete on edge that is not deleted.");
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_put_edges(const std::vector<vid_t>& src_list,
                                          const std::vector<vid_t>& dst_list,
                                          const std::vector<EDATA_T>& data_list,
                                          timestamp_t ts) {
  vid_t vnum = adj_list_size_.size();
  std::vector<int> degree(vnum, 0);
  size_t inserted = 0;
  for (auto src : src_list) {
    if (src < vnum) {
      degree[src]++;
      ++inserted;
    }
  }

  bool needs_reallocation = false;
  for (vid_t i = 0; i < vnum; ++i) {
    if (degree[i] >
        adj_list_capacity_[i] - adj_list_size_[i].load()) {
      needs_reallocation = true;
      break;
    }
  }
  if (!needs_reallocation) {
    for (size_t i = 0; i < src_list.size(); ++i) {
      vid_t src = src_list[i];
      if (src >= vnum) {
        continue;
      }
      auto& nbr = adj_list_buffer_[src][adj_list_size_[src].fetch_add(1)];
      nbr.neighbor = dst_list[i];
      nbr.data = data_list[i];
      nbr.timestamp.store(ts);
    }
    edge_count_.fetch_add(inserted, std::memory_order_relaxed);
    return;
  }

  size_t total_to_move = 0;
  size_t total_to_allocate = 0;
  for (vid_t i = 0; i < vnum; ++i) {
    int old_deg = adj_list_size_[i].load();
    total_to_move += old_deg;
    int new_degree = degree[i] + old_deg;
    int new_cap = std::ceil(new_degree * NeugDBConfig::DEFAULT_RESERVE_RATIO);
    adj_list_capacity_[i] = new_cap;
    total_to_allocate += new_cap;
  }

  std::vector<nbr_t> new_nbr_list(total_to_move);
  size_t offset = 0;
  for (vid_t i = 0; i < vnum; ++i) {
    int old_deg = adj_list_size_[i].load();
    memcpy(new_nbr_list.data() + offset, adj_list_buffer_[i],
           sizeof(nbr_t) * old_deg);
    offset += old_deg;
  }

  nbr_list_.resize(total_to_allocate);
  offset = 0;
  size_t new_offset = 0;
  for (vid_t i = 0; i < vnum; ++i) {
    nbr_t* new_buffer = nbr_list_.data() + offset;
    int old_deg = adj_list_size_[i].load();
    memcpy(new_buffer, new_nbr_list.data() + new_offset,
           sizeof(nbr_t) * old_deg);
    new_offset += old_deg;
    offset += adj_list_capacity_[i];
    adj_list_buffer_[i] = new_buffer;
    adj_list_size_[i].store(old_deg);
  }

  for (size_t i = 0; i < src_list.size(); ++i) {
    vid_t src = src_list[i];
    if (src >= vnum) {
      continue;
    }
    vid_t dst = dst_list[i];
    const EDATA_T& data = data_list[i];
    auto& nbr = adj_list_buffer_[src][adj_list_size_[src].fetch_add(1)];
    nbr.neighbor = dst;
    nbr.data = data;
    nbr.timestamp.store(ts);
  }
  edge_count_.fetch_add(inserted, std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_put_edges_reserved(
    const std::vector<vid_t>& src_list, const std::vector<vid_t>& dst_list,
    const std::vector<EDATA_T>& data_list, size_t requested_worker_count,
    const std::vector<size_t>* edge_chunk_ends,
    const std::vector<std::vector<uint32_t>>* degree_chunks,
    timestamp_t ts) {
  if (src_list.size() != dst_list.size() ||
      (!data_list.empty() && data_list.size() != src_list.size())) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Reserved CSR batch input sizes do not match");
  }
  const bool has_degree_chunks =
      edge_chunk_ends != nullptr || degree_chunks != nullptr;
  if (has_degree_chunks &&
      (edge_chunk_ends == nullptr || degree_chunks == nullptr)) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Reserved CSR degree chunks and edge boundaries are both required");
  }
  if (src_list.empty()) {
    if (has_degree_chunks &&
        (!edge_chunk_ends->empty() || !degree_chunks->empty())) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Reserved CSR received chunks for an empty edge input");
    }
    return;
  }
  size_t worker_count =
      std::min(std::max<size_t>(1, requested_worker_count), src_list.size());
  if (has_degree_chunks) {
    if (edge_chunk_ends->empty() ||
        edge_chunk_ends->size() != degree_chunks->size() ||
        edge_chunk_ends->back() != src_list.size()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Reserved CSR degree chunks do not match edge input");
    }
    worker_count = std::min(worker_count, edge_chunk_ends->size());
  }
  std::atomic<bool> failed{false};
  auto run_workers = [&](const auto& worker) {
    if (worker_count == 1) {
      worker(0);
      return;
    }
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker_index = 0; worker_index < worker_count;
         ++worker_index) {
      workers.emplace_back(worker, worker_index);
    }
    for (auto& thread : workers) {
      thread.join();
    }
  };

  std::vector<std::vector<uint32_t>> worker_offsets(
      worker_count,
      std::vector<uint32_t>(adj_list_size_.size(), static_cast<uint32_t>(0)));
  std::vector<size_t> worker_edge_begins(worker_count, 0);
  std::vector<size_t> worker_edge_ends(worker_count, src_list.size());
  for (size_t worker = 0; worker < worker_count; ++worker) {
    worker_edge_begins[worker] = src_list.size() * worker / worker_count;
    worker_edge_ends[worker] =
        src_list.size() * (worker + 1) / worker_count;
  }

  if (has_degree_chunks) {
    size_t previous_end = 0;
    for (size_t chunk = 0; chunk < edge_chunk_ends->size(); ++chunk) {
      if ((*edge_chunk_ends)[chunk] <= previous_end ||
          (*edge_chunk_ends)[chunk] > src_list.size() ||
          (*degree_chunks)[chunk].size() > adj_list_size_.size()) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Reserved CSR degree chunk metadata is invalid");
      }
      previous_end = (*edge_chunk_ends)[chunk];
    }
    for (size_t worker = 0; worker < worker_count; ++worker) {
      const size_t chunk_begin =
          edge_chunk_ends->size() * worker / worker_count;
      const size_t chunk_end =
          edge_chunk_ends->size() * (worker + 1) / worker_count;
      worker_edge_begins[worker] =
          chunk_begin == 0 ? 0 : (*edge_chunk_ends)[chunk_begin - 1];
      worker_edge_ends[worker] = (*edge_chunk_ends)[chunk_end - 1];
    }
    for (size_t worker = 1; worker < worker_count; ++worker) {
      const size_t preceding_chunk_end =
          edge_chunk_ends->size() * worker / worker_count;
      for (size_t src = 0; src < adj_list_size_.size(); ++src) {
        uint64_t offset = 0;
        for (size_t chunk = 0; chunk < preceding_chunk_end; ++chunk) {
          if (src < (*degree_chunks)[chunk].size()) {
            offset += (*degree_chunks)[chunk][src];
          }
        }
        if (offset > static_cast<uint64_t>(adj_list_capacity_[src])) {
          THROW_INVALID_ARGUMENT_EXCEPTION(
              "Reserved CSR degree exceeds vertex capacity");
        }
        worker_offsets[worker][src] = static_cast<uint32_t>(offset);
      }
    }
  } else {
    auto count_worker_degrees = [&](size_t worker_index) {
      const size_t begin = src_list.size() * worker_index / worker_count;
      const size_t end = src_list.size() * (worker_index + 1) / worker_count;
      auto& offsets = worker_offsets[worker_index];
      for (size_t i = begin; i < end && !failed.load(); ++i) {
        const vid_t src = src_list[i];
        if (src >= adj_list_size_.size()) {
          failed.store(true);
          break;
        }
        ++offsets[src];
      }
    };
    run_workers(count_worker_degrees);
    if (failed.load()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Reserved CSR endpoint validation failed");
    }
    for (size_t src = 0; src < adj_list_size_.size(); ++src) {
      uint64_t offset = 0;
      for (size_t worker = 0; worker < worker_count; ++worker) {
        const uint32_t degree = worker_offsets[worker][src];
        worker_offsets[worker][src] = static_cast<uint32_t>(offset);
        offset += degree;
      }
      if (offset > static_cast<uint64_t>(adj_list_capacity_[src])) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "Reserved CSR degree exceeds vertex capacity");
      }
    }
  }

  auto fill_worker = [&](size_t worker_index) {
    const size_t begin = worker_edge_begins[worker_index];
    const size_t end = worker_edge_ends[worker_index];
    auto& offsets = worker_offsets[worker_index];
    for (size_t i = begin; i < end; ++i) {
      const vid_t src = src_list[i];
      auto& nbr = adj_list_buffer_[src][offsets[src]++];
      nbr.neighbor = dst_list[i];
      nbr.data = data_list.empty() ? EDATA_T() : data_list[i];
      nbr.timestamp.store(ts);
    }
  };

  run_workers(fill_worker);
  for (size_t src = 0; src < adj_list_size_.size(); ++src) {
    adj_list_size_[src].store(worker_offsets.back()[src],
                              std::memory_order_relaxed);
  }
  edge_count_.fetch_add(src_list.size(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::batch_put_edges(
    const std::vector<vid_t>& src_list, const std::vector<vid_t>& dst_list,
    const std::vector<EDATA_T>& data_list, timestamp_t ts, Allocator& alloc) {
  vid_t vnum = adj_list_size_.size();
  std::vector<int> degree(vnum, 0);
  std::vector<vid_t> touched;
  touched.reserve(src_list.size());
  for (auto src : src_list) {
    if (src < vnum) {
      if (degree[src]++ == 0) touched.push_back(src);
    }
  }

  // Classify the TOUCHED vertices only — an O(vnum) scan here would dwarf
  // the actual batch work on tables with millions of vertices. Live size
  // does not shrink on delete (tombstones keep their slot counted), so free
  // tail slack can be zero even when the list holds deleted slots:
  //   - fits: degree <= capacity - size (plain append);
  //   - reuse: degree <= capacity - size + tombstones (overwrite tombstone
  //     slots in place — the common delete-then-reinsert pattern becomes a
  //     pure in-place overwrite with no allocation and no copy);
  //   - overflow: genuinely needs more room (per-vertex growth below).
  //
  // NOTE: slot reuse is only exercised on the embedded ts=0 batch path,
  // which has no undo log / WAL replay; the MVCC path inserts through
  // put_edge (append-only) so a tombstone slot is never overwritten while
  // a pending RemoveEdgeUndo still references it.
  const timestamp_t kTombstone = std::numeric_limits<timestamp_t>::max();
  std::unordered_map<vid_t, std::vector<int32_t>> free_slots;
  std::vector<vid_t> overflow_vertices;
  for (vid_t i : touched) {
    int slack = adj_list_capacity_[i] - adj_list_size_[i].load();
    if (degree[i] <= slack) continue;
    int tombstones = 0;
    std::vector<int32_t> free;
    for (int k = 0; k < adj_list_size_[i].load(); ++k) {
      if (adj_list_buffer_[i][k].timestamp.load() == kTombstone) {
        ++tombstones;
        free.push_back(k);
      }
    }
    if (degree[i] <= slack + tombstones) {
      if (!free.empty()) free_slots.emplace(i, std::move(free));
    } else {
      overflow_vertices.push_back(i);
    }
  }

  if (!overflow_vertices.empty()) {
    if (overflow_vertices.size() * 4 <= static_cast<size_t>(vnum)) {
      // Increment-friendly path: grow ONLY the overflowing vertices, each
      // into its own allocation (the same pattern as put_edge); all other
      // vertex buffers stay untouched. This keeps incremental maintenance
      // local instead of repacking the whole arena per batch.
      for (vid_t i : overflow_vertices) {
        int old_deg = adj_list_size_[i].load();
        int new_cap = std::max<int>(
            adj_list_capacity_[i] + (adj_list_capacity_[i] >> 1),
            old_deg + degree[i]);
        new_cap = std::max(new_cap, 8);
        nbr_t* new_buffer =
            static_cast<nbr_t*>(alloc.allocate(new_cap * sizeof(nbr_t)));
        if (old_deg > 0) {
          memcpy(new_buffer, adj_list_buffer_[i], sizeof(nbr_t) * old_deg);
        }
        adj_list_buffer_[i] = new_buffer;
        adj_list_capacity_[i] = new_cap;
      }
    } else {
      // Bulk-load path: most vertices overflow (initial load), so one
      // contiguous arena allocation has better locality and fewer small
      // allocations — rebuild the arena as before.
      size_t total_to_move = 0;
      size_t total_to_allocate = 0;
      for (vid_t i = 0; i < vnum; ++i) {
        int old_deg = adj_list_size_[i].load();
        total_to_move += old_deg;
        int new_degree = degree[i] + old_deg;
        int new_cap =
            std::ceil(new_degree * NeugDBConfig::DEFAULT_RESERVE_RATIO);
        adj_list_capacity_[i] = new_cap;
        total_to_allocate += new_cap;
      }

      std::vector<nbr_t> new_nbr_list(total_to_move);
      size_t offset = 0;
      for (vid_t i = 0; i < vnum; ++i) {
        int old_deg = adj_list_size_[i].load();
        memcpy(new_nbr_list.data() + offset, adj_list_buffer_[i],
               sizeof(nbr_t) * old_deg);
        offset += old_deg;
      }

      nbr_list_.resize(total_to_allocate);
      offset = 0;
      size_t new_offset = 0;
      for (vid_t i = 0; i < vnum; ++i) {
        nbr_t* new_buffer = nbr_list_.data() + offset;
        int old_deg = adj_list_size_[i].load();
        memcpy(new_buffer, new_nbr_list.data() + new_offset,
               sizeof(nbr_t) * old_deg);
        new_offset += old_deg;
        offset += adj_list_capacity_[i];
        adj_list_buffer_[i] = new_buffer;
        adj_list_size_[i].store(old_deg);
      }
    }
  }

  for (size_t i = 0; i < src_list.size(); ++i) {
    vid_t src = src_list[i];
    if (src >= vnum) {
      continue;
    }
    vid_t dst = dst_list[i];
    const EDATA_T& data = data_list[i];
    // Reuse a tombstone slot when the vertex was classified for reuse;
    // otherwise append (capacity is guaranteed by the classification and
    // the growth step above).
    auto it = free_slots.find(src);
    if (it != free_slots.end() && !it->second.empty()) {
      int32_t off = it->second.back();
      it->second.pop_back();
      auto& nbr = adj_list_buffer_[src][off];
      nbr.neighbor = dst;
      nbr.data = data;
      nbr.timestamp.store(ts);
    } else {
      auto& nbr = adj_list_buffer_[src][adj_list_size_[src].fetch_add(1)];
      nbr.neighbor = dst;
      nbr.data = data;
      nbr.timestamp.store(ts);
    }
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::load_meta(const std::string& prefix) {
  std::string meta_file_path = prefix + ".meta";
  if (std::filesystem::exists(meta_file_path)) {
    read_file(meta_file_path, &unsorted_since_, sizeof(timestamp_t), 1);

  } else {
    unsorted_since_ = 0;
  }
}

template <typename EDATA_T>
void MutableCsr<EDATA_T>::dump_meta(const std::string& prefix) const {
  std::string meta_file_path = prefix + ".meta";
  write_file(meta_file_path, &unsorted_since_, sizeof(timestamp_t), 1);
}

template <typename EDATA_T>
size_t SingleMutableCsr<EDATA_T>::count_valid_edges() const {
  size_t count = 0;
  for (size_t src = 0; src < nbr_list_.size(); ++src) {
    count += nbr_list_[src].timestamp.load(std::memory_order_relaxed) !=
             INVALID_TIMESTAMP;
  }
  return count;
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::open(const std::string& name,
                                     const std::string& snapshot_dir,
                                     const std::string& work_dir) {
  auto tmp_file = tmp_dir(work_dir) + "/" + name + ".snbr";
  auto snapshot_file = snapshot_dir + "/" + name + ".snbr";
  if (std::filesystem::exists(tmp_file)) {
    std::filesystem::remove(tmp_file);
  }
  if (!std::filesystem::exists(tmp_file)) {
    if (std::filesystem::exists(snapshot_file)) {
      copy_file(snapshot_file, tmp_file);
    }
  }
  nbr_list_.open(tmp_file, true);
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::open_in_memory(const std::string& prefix) {
  nbr_list_.open(prefix + ".snbr", false);
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::open_with_hugepages(const std::string& prefix) {
  nbr_list_.open_with_hugepages(prefix + ".snbr");
  edge_count_.store(count_valid_edges(), std::memory_order_relaxed);
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::dump(const std::string& name,
                                     const std::string& new_snapshot_dir) {
  // TODO: opt with mv
  write_file(new_snapshot_dir + "/" + name + ".snbr", nbr_list_.data(),
             sizeof(nbr_t), nbr_list_.size());
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::reset_timestamp() {
  size_t vnum = nbr_list_.size();
  for (size_t i = 0; i != vnum; ++i) {
    if (nbr_list_[i].timestamp != INVALID_TIMESTAMP) {
      nbr_list_[i].timestamp.store(0, std::memory_order_relaxed);
    }
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::compact() {}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::resize(vid_t vnum) {
  if (vnum > nbr_list_.size()) {
    size_t old_size = nbr_list_.size();
    nbr_list_.resize(vnum);
    for (size_t k = old_size; k != vnum; ++k) {
      nbr_list_[k].timestamp.store(std::numeric_limits<timestamp_t>::max());
    }
  } else {
    size_t removed = 0;
    for (size_t src = vnum; src < nbr_list_.size(); ++src) {
      removed += nbr_list_[src].timestamp.load(std::memory_order_relaxed) !=
                 INVALID_TIMESTAMP;
    }
    edge_count_.fetch_sub(removed, std::memory_order_relaxed);
    nbr_list_.resize(vnum);
  }
}

template <typename EDATA_T>
size_t SingleMutableCsr<EDATA_T>::capacity() const {
  return nbr_list_.size();
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::close() {
  nbr_list_.reset();
  edge_count_.store(0, std::memory_order_relaxed);
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::batch_sort_by_edge_data(timestamp_t ts) {}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::batch_delete_vertices(
    const std::set<vid_t>& src_set, const std::set<vid_t>& dst_set) {
  vid_t vnum = nbr_list_.size();
  for (auto src : src_set) {
    if (src < vnum) {
      if (nbr_list_[src].timestamp.exchange(INVALID_TIMESTAMP) !=
          INVALID_TIMESTAMP) {
        edge_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
  for (vid_t v = 0; v < vnum; ++v) {
    auto& nbr = nbr_list_[v];
    if (dst_set.find(nbr.neighbor) != dst_set.end()) {
      if (nbr.timestamp.exchange(INVALID_TIMESTAMP) != INVALID_TIMESTAMP) {
        edge_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::batch_delete_edges(
    const std::vector<vid_t>& src_list, const std::vector<vid_t>& dst_list) {
  vid_t vnum = nbr_list_.size();
  for (size_t i = 0; i != src_list.size(); ++i) {
    vid_t src = src_list[i];
    vid_t dst = dst_list[i];
    if (src >= vnum) {
      continue;
    }
    auto& nbr = nbr_list_[src];
    if (nbr.neighbor == dst) {
      if (nbr.timestamp.exchange(INVALID_TIMESTAMP) != INVALID_TIMESTAMP) {
        edge_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::batch_delete_edges(
    const std::vector<std::pair<vid_t, int32_t>>& edge_list) {
  vid_t vnum = nbr_list_.size();
  for (const auto& edge : edge_list) {
    vid_t src = edge.first;
    if (src >= vnum) {
      continue;
    }
    auto& nbr = nbr_list_[src];
    assert(edge.second == 0);
    if (nbr.timestamp.exchange(INVALID_TIMESTAMP) != INVALID_TIMESTAMP) {
      edge_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::delete_edge(vid_t src, int32_t offset,
                                            timestamp_t ts) {
  vid_t vnum = nbr_list_.size();
  if (src >= vnum) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "src out of bound: " + std::to_string(src) +
        " >= " + std::to_string(vnum));
  }
  auto& nbr = nbr_list_[src];
  assert(offset == 0);
  if (nbr.timestamp.load() <= ts) {
    if (nbr.timestamp.exchange(INVALID_TIMESTAMP) != INVALID_TIMESTAMP) {
      edge_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  } else if (nbr.timestamp.load() == std::numeric_limits<timestamp_t>::max()) {
    LOG(ERROR) << "Fail to delete edge, already deleted.";
  } else {
    LOG(ERROR) << "Fail to delete edge, timestamp not satisfied.";
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::revert_delete_edge(vid_t src, vid_t nbr_vid,
                                                   int32_t offset,
                                                   timestamp_t ts) {
  vid_t vnum = nbr_list_.size();
  if (src >= vnum || offset != 0) {
    THROW_INVALID_ARGUMENT_EXCEPTION("src out of bound or offset out of bound");
  }
  auto& nbr = nbr_list_[src];
  if (nbr.neighbor != nbr_vid) {
    THROW_INVALID_ARGUMENT_EXCEPTION("neighbor id not match");
  }
  if (nbr.timestamp.load() == std::numeric_limits<timestamp_t>::max()) {
    nbr.timestamp.store(ts);
    edge_count_.fetch_add(1, std::memory_order_relaxed);
  } else {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Attempting to revert delete on edge that is not deleted.");
  }
}

template <typename EDATA_T>
void SingleMutableCsr<EDATA_T>::batch_put_edges(
    const std::vector<vid_t>& src_list, const std::vector<vid_t>& dst_list,
    const std::vector<EDATA_T>& data_list, timestamp_t ts) {
  vid_t vnum = nbr_list_.size();
  for (size_t i = 0; i != src_list.size(); ++i) {
    vid_t src = src_list[i];
    if (src >= vnum) {
      continue;
    }
    auto& nbr = nbr_list_[src];
    const bool was_deleted =
        nbr.timestamp.load(std::memory_order_relaxed) == INVALID_TIMESTAMP;
    nbr.neighbor = dst_list[i];
    nbr.data = data_list[i];
    nbr.timestamp.store(ts);
    if (was_deleted) {
      edge_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

template class MutableCsr<EmptyType>;
template class MutableCsr<int32_t>;
template class MutableCsr<uint32_t>;
template class MutableCsr<Date>;
template class MutableCsr<int64_t>;
template class MutableCsr<uint64_t>;
template class MutableCsr<double>;
template class MutableCsr<float>;
template class MutableCsr<DateTime>;
template class MutableCsr<Interval>;
template class MutableCsr<bool>;

template class SingleMutableCsr<float>;
template class SingleMutableCsr<double>;
template class SingleMutableCsr<uint64_t>;
template class SingleMutableCsr<int64_t>;
template class SingleMutableCsr<Date>;
template class SingleMutableCsr<uint32_t>;
template class SingleMutableCsr<int32_t>;
template class SingleMutableCsr<EmptyType>;
template class SingleMutableCsr<DateTime>;
template class SingleMutableCsr<Interval>;
template class SingleMutableCsr<bool>;

}  // namespace neug

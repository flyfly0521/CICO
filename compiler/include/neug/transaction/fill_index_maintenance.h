#pragma once
#include <set>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/transaction/edge_change.h"
#include "neug/utils/allocators.h"
#include "neug/utils/api.h"
#include "neug/utils/property/property.h"
#include "neug/utils/property/types.h"

// Proto package `common` lives in the GLOBAL ::common namespace. Forward
// declare to keep the generated header out of this header; the maintainer's
// out-of-line destructor handles the incomplete type in the .cc.
namespace common {
class Expression;
}  // namespace common

namespace neug {

class PropertyGraph;
class QueryProcessor;
class Schema;
class UpdateTransaction;
struct GenericView;

namespace catalog {
class FillIndexAuxInfo;
class Catalog;
struct IndexColnumInfo;
}  // namespace catalog

namespace execution {
class ExprBase;
class BindedExprBase;
}  // namespace execution

namespace transaction {
class Transaction;
}  // namespace transaction

class FillIndexMaintenanceRegistry;
class IGraphPlanner;

// Sentinel: source property is the vertex OID (primary key).
constexpr int kSourcePropIsOid = -1;

// A single edge hop in a resolved linear pattern chain.
struct FillPathHop {
  label_t src_vertex_label;  // Schema vertex label of source
  label_t dst_vertex_label;  // Schema vertex label of destination
  label_t edge_label;        // Schema edge label
  bool is_outgoing;  // true: pattern direction matches storage direction
};

// A concrete physical edge sub-table watched by a FILL maintainer.  NeuG edge
// labels are relation groups and may contain several (src_label, dst_label)
// tables, so edge_label alone is not precise enough for sub-table FILLs.
struct EdgeTableSubscription {
  label_t edge_label;
  label_t src_label;
  label_t dst_label;

  bool operator==(const EdgeTableSubscription& other) const {
    return edge_label == other.edge_label && src_label == other.src_label &&
           dst_label == other.dst_label;
  }
};

// A staged materialization preserves ordered OPTIONAL/aggregation boundaries.
// Until those stages have an incremental algorithm, prevent changes to its
// actual sources instead of exposing a stale READY index. Unrelated tables and
// properties, including other FILL output columns, remain writable.
struct StagedFillDependencies {
  std::unordered_set<label_t> vertex_labels;
  std::vector<EdgeTableSubscription> edge_tables;
  std::vector<std::pair<label_t, int>> vertex_properties;
  std::vector<std::pair<EdgeTableSubscription, int>> edge_properties;
};

// ---------------------------------------------------------------------------
// Private implementation contract for execution strategies. Strategy classes
// implement this interface but do not implement PatternMaterializedIndex, so
// they cannot be registered as catalog-facing indexes.
// ---------------------------------------------------------------------------
namespace detail {

class MaintenanceStrategy {
 public:
  virtual ~MaintenanceStrategy() = default;

  // ---- Single-edge interface (original per-edge callbacks) ----
  // Generic interface: takes PropertyGraph directly (embedded / ts=0 path).
  // src_label / dst_label: Schema vertex labels of the changed edge's
  // endpoints. alloc: memory allocator for edge insertion (required when
  // writing shortcut edges).
  virtual void OnEdgeInserted(PropertyGraph& graph, timestamp_t ts,
                              label_t edge_label, label_t src_label,
                              label_t dst_label, vid_t src, vid_t dst,
                              Allocator& alloc) = 0;
  virtual void OnEdgeDeleted(PropertyGraph& graph, timestamp_t ts,
                             label_t edge_label, label_t src_label,
                             label_t dst_label, vid_t src, vid_t dst) = 0;

  // Transaction-aware overloads (server / MVCC path).
  // Default implementation unwraps the transaction and delegates to the
  // PropertyGraph virtuals above — subclasses that need undo-log/WAL support
  // (S2) should override these to route writes through
  // txn.UpdateVertexProperty.
  virtual void OnEdgeInserted(UpdateTransaction& txn, label_t edge_label,
                              label_t src_label, label_t dst_label, vid_t src,
                              vid_t dst);
  virtual void OnEdgeDeleted(UpdateTransaction& txn, label_t edge_label,
                             label_t src_label, label_t dst_label, vid_t src,
                             vid_t dst);

  // ---- Batch edge interface (Phase 1-3 of linearity improvement) ----
  // Subclasses may override these to aggregate deltas across multiple edges,
  // reducing per-anchor read-modify-write cycles and improving CSR locality.
  // Default implementations delegate to the per-edge callbacks above.
  //
  // Embedded (ts=0) path.
  virtual void OnEdgesBatchInserted(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      label_t src_label, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges, Allocator& alloc);
  virtual void OnEdgesBatchDeleted(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      label_t src_label, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges);

  // Server (MVCC) path.
  virtual void OnEdgesBatchInserted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label, const std::vector<std::pair<vid_t, vid_t>>& edges);
  virtual void OnEdgesBatchDeleted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label, const std::vector<std::pair<vid_t, vid_t>>& edges);

  // Returns the set of Schema edge labels this maintainer cares about.
  virtual const std::unordered_set<label_t>& watched_edge_labels() const = 0;

  // Exact physical edge tables watched by this maintainer. Registration and
  // dispatch always use the complete (edge, src, dst) tuple; relation-group
  // labels are never treated as wildcard subscriptions.
  virtual const std::vector<EdgeTableSubscription>& watched_edge_tables()
      const = 0;

  // Called when a vertex property changes. Maintainers that source properties
  // from vertices should recompute and push deltas to anchors (Issue 8).
  // Default: no-op.
  virtual void OnVertexPropertyChanged(PropertyGraph& graph, timestamp_t ts,
                                       label_t vertex_label, vid_t vid,
                                       int prop_id, const Property& old_val,
                                       const Property& new_val) {}
  virtual void OnVertexPropertyChanged(UpdateTransaction& txn,
                                       label_t vertex_label, vid_t vid,
                                       int prop_id, const Property& old_val,
                                       const Property& new_val);

  // Returns the set of (vertex_label, property_id) pairs this maintainer
  // cares about for property change notification. Default: empty.
  virtual std::vector<std::pair<label_t, int>> watched_vertex_properties()
      const {
    return {};
  }
};

}  // namespace detail

namespace detail {

// ---------------------------------------------------------------------------
// Internal vertex-property pushdown strategy (§1 in
// update/index_update.md). Supports linear-chain patterns where
// each edge has n:1 functional dependency.
//
// Example: FILL PERSON(countryName) FROM
//   (f:PERSON-[ISLOCATEDIN]->city:PLACE-[:ISPARTOF]->country:PLACE)
// ---------------------------------------------------------------------------
class VertexPushdownStrategy : public MaintenanceStrategy {
 public:
  // Construct from FillIndexAuxInfo, resolving PatternIR → linear path
  // and translating catalog table IDs → Schema label_t values.
  VertexPushdownStrategy(const catalog::FillIndexAuxInfo& aux,
                         const Schema& schema, const catalog::Catalog& catalog,
                         transaction::Transaction* txn);

  // Embedded (ts=0) path: writes directly to the PropertyGraph.
  void OnEdgeInserted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst, Allocator& alloc) override;
  void OnEdgeDeleted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst) override;

  // Server (MVCC) path: routes property writes through txn so they are
  // covered by the undo-log and WAL redo (S2 fix).
  void OnEdgeInserted(UpdateTransaction& txn, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst) override;
  void OnEdgeDeleted(UpdateTransaction& txn, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst) override;

  // Re-push source property changes to affected anchors (Issue 8).
  void OnVertexPropertyChanged(PropertyGraph& graph, timestamp_t ts,
                               label_t vertex_label, vid_t vid, int prop_id,
                               const Property& old_val,
                               const Property& new_val) override;

  void OnVertexPropertyChanged(UpdateTransaction& txn, label_t vertex_label,
                               vid_t vid, int prop_id, const Property& old_val,
                               const Property& new_val) override;

  // Batch edge interface: delegate to per-edge processing (no batch
  // optimization for vertex-pushdown in this phase — lower priority).
  void OnEdgesBatchInserted(PropertyGraph& graph, timestamp_t ts,
                            label_t edge_label, label_t src_label,
                            label_t dst_label,
                            const std::vector<std::pair<vid_t, vid_t>>& edges,
                            Allocator& alloc) override;
  void OnEdgesBatchDeleted(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      label_t src_label, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;
  void OnEdgesBatchInserted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;
  void OnEdgesBatchDeleted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;

  const std::unordered_set<label_t>& watched_edge_labels() const override {
    return watched_edge_labels_;
  }
  const std::vector<EdgeTableSubscription>& watched_edge_tables()
      const override {
    return watched_edge_tables_;
  }

  // Number of target columns this maintainer handles (may be 0 if all
  // columns were non-NODE_PROPERTY and were skipped).
  size_t target_column_count() const { return target_columns_.size(); }

  // Return (vertex_label, property_id) pairs for source properties that
  // trigger re-pushdown on change (Issue 8).
  std::vector<std::pair<label_t, int>> watched_vertex_properties()
      const override;

 private:
  void PropagateVertexPropertyChange(PropertyGraph& graph, timestamp_t ts,
      label_t vertex_label, vid_t vid, int prop_id, const Property& new_val,
      UpdateTransaction* txn);

  // Resolved linear path from anchor (index 0) to endpoint (index N-1).
  std::vector<FillPathHop> path_;

  // Schema vertex label for each node in the chain (size = path_.size() + 1).
  std::vector<label_t> path_vertex_labels_;

  // Anchor vertex label.
  label_t anchor_label_;

  // Target column names on the anchor table.
  std::vector<std::string> target_columns_;

  // Resolved property column IDs on the anchor table.
  std::vector<int> target_prop_ids_;

  // For each target column, chain position of the source node (0 = anchor).
  std::vector<size_t> source_node_positions_;

  // For each target column, property ID on the source node's vertex table.
  std::vector<int> source_prop_ids_;

  // Pure functional projections have no aggregate row-count strategy. Their
  // literal-true presence outputs follow the same complete-path existence.
  std::vector<int> presence_prop_ids_;
  std::vector<Property> presence_defaults_;

  // Watched edge labels.
  std::unordered_set<label_t> watched_edge_labels_;
  std::vector<EdgeTableSubscription> watched_edge_tables_;

  // Walk backward from a single edge source s through hops [j-1..0].
  std::vector<vid_t> ReverseMatchPrefix(PropertyGraph& graph, timestamp_t ts,
                                        size_t j, vid_t s);

  // Batch variant: walk backward from multiple source vertices at once.
  // Deduplicates anchors across the entire batch set.
  std::vector<vid_t> ReverseMatchPrefix(PropertyGraph& graph, timestamp_t ts,
                                        size_t j,
                                        const std::vector<vid_t>& src_vertices);

  // Batch variant with provenance: returns per-source anchor lists,
  // index-aligned with the (sorted, deduplicated) src_vertices input. Used by
  // the batch-insert path — under the n:1 FD, anchor sets of distinct sources
  // are disjoint, so updates can be applied per source without cross-talk
  // between batch edges. On conflict (prefix FD violated, two sources reach
  // the same node), the smaller source index wins deterministically and a
  // warning is logged.
  std::vector<std::vector<vid_t>> ReverseMatchPrefixPartitioned(
      PropertyGraph& graph, timestamp_t ts, size_t j,
      const std::vector<vid_t>& src_vertices);

  // Shared core of both OnEdgesBatchInserted overloads for hop position j.
  // sorted_sd: batch edges mapped to (s, d) orientation (s = node at path
  // position j), sorted by (s, d), exact duplicates removed. txn: if
  // non-null, writes are routed through txn.UpdateVertexProperty (undo-log +
  // WAL); if null, writes go directly to the PropertyGraph (embedded path).
  // Dispatches to the serial or parallel implementation by batch size.
  void ApplyBatchInsertAtHop(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      const std::vector<std::pair<vid_t, vid_t>>& sorted_sd, size_t j,
      UpdateTransaction* txn);

  // Serial / parallel implementations of ApplyBatchInsertAtHop.
  void ApplyBatchInsertAtHopSerial(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      const std::vector<std::pair<vid_t, vid_t>>& sorted_sd, size_t j,
      UpdateTransaction* txn);
  void ApplyBatchInsertAtHopParallel(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      const std::vector<std::pair<vid_t, vid_t>>& sorted_sd, size_t j,
      UpdateTransaction* txn);

  // Threshold: use the parallel path when the number of batch edges at one
  // hop is >= this value. Minimum edges per worker thread.
  static constexpr size_t kBatchParallelThreshold = 512;
  static constexpr size_t kBatchMinEdgesPerThread = 256;

  // Compute per-anchor per-column target values WITHOUT writing (the
  // parallel compute phase). Returns one entry per anchor in `anchors`
  // (index-aligned); each entry is a (propId, value) list where an empty
  // Property marks a nullify (resolved to get_default_value at write time).
  std::vector<std::vector<std::pair<int, Property>>> ComputeAnchorUpdates(
      PropertyGraph& graph, timestamp_t ts, const std::vector<vid_t>& anchors,
      const std::optional<std::vector<vid_t>>& suffix, size_t j, vid_t s_at_j);

  // Write one anchor's computed updates; empty Property = nullify via
  // get_default_value. Routes through txn if non-null (undo-log + WAL).
  void WriteAnchorUpdates(PropertyGraph& graph, timestamp_t ts, vid_t anchor,
                          const std::vector<std::pair<int, Property>>& vals,
                          UpdateTransaction* txn);

  // Walk forward from edge destination d through hops [j+1..N-1].
  // Returns nullopt if the path is broken.
  std::optional<std::vector<vid_t>> ForwardMatchSuffix(PropertyGraph& graph,
                                                       timestamp_t ts, size_t j,
                                                       vid_t d);

  // Walk forward from `start` (at path position from_pos) to to_pos.
  // Returns the vid at to_pos, or UINT64_MAX if the path is broken (S4).
  vid_t WalkForwardToPosition(PropertyGraph& graph, timestamp_t ts, vid_t start,
                              size_t from_pos, size_t to_pos);

  // Update each anchor's properties from suffix chain or nullify.
  // s_at_j: the node at path position j (= s from OnEdgeInserted), used to
  //   fill columns sourced from position j without an extra graph lookup (S4).
  // txn: if non-null, property writes are routed through
  // txn.UpdateVertexProperty
  //   so they are undo-logged and WAL-serialized (S2). If null, writes go
  //   directly to the PropertyGraph (embedded / ts=0 path).
  void ApplyUpdates(PropertyGraph& graph, timestamp_t ts,
                    const std::vector<vid_t>& anchors,
                    const std::optional<std::vector<vid_t>>& suffix, size_t j,
                    vid_t s_at_j = static_cast<vid_t>(-1),
                    UpdateTransaction* txn = nullptr);
};

// ---------------------------------------------------------------------------
// Internal variable-length single-relationship strategy
// (§3 in IncGQ_doc/update/索引更新.md, recursive REL strategy).
//
// Maintains a materialized shortcut-edge table (e.g. ROOT_POST) over a
// tree-structured variable-length base edge (e.g. REPLYOF). When a base edge
// is inserted/deleted, the maintainer cascades SetRoot updates over the entire
// affected subtree, respecting an optional depth interval [lower, upper].
//
// Algorithm:
//  Insert REPLYOF(c,p): climb to root Post from p; BFS c's subtree; for each
//    (x, relDepth) in subtree: if lower≤(depthC+relDepth)≤upper, upsert
//    ROOT_POST(x → root).
//  Delete REPLYOF(c,p): BFS c's subtree; delete ROOT_POST for all descendants.
//
// Assumptions: REPLYOF is n:1 (each comment ≤1 parent); the base graph is
// a forest rooted at Posts. See plan notes for multi-label endpoint handling.
// ---------------------------------------------------------------------------
class VariadicRelStrategy : public MaintenanceStrategy {
 public:
  VariadicRelStrategy(const catalog::FillIndexAuxInfo& aux,
                      const Schema& schema, const catalog::Catalog& catalog,
                      transaction::Transaction* txn,
                      FillIndexMaintenanceRegistry* fill_registry = nullptr);

  // Embedded (ts=0) path: writes materialized edges through the registry's
  // cascading batch gateway when a registry is available.
  void OnEdgeInserted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst, Allocator& alloc) override;
  void OnEdgeDeleted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst) override;

  // Server (MVCC) path: routes ROOT_POST edge writes through txn (undo-log +
  // WAL).
  void OnEdgeInserted(UpdateTransaction& txn, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst) override;
  void OnEdgeDeleted(UpdateTransaction& txn, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst) override;

  // Batch edge interface with subtree dedup (L2.5a):
  // Sorts edges by tree depth (shallow first), skips nodes whose subtree
  // is already covered by an ancestor's edge change.
  void OnEdgesBatchInserted(PropertyGraph& graph, timestamp_t ts,
                            label_t edge_label, label_t src_label,
                            label_t dst_label,
                            const std::vector<std::pair<vid_t, vid_t>>& edges,
                            Allocator& alloc) override;
  void OnEdgesBatchDeleted(
      PropertyGraph& graph, timestamp_t ts, label_t edge_label,
      label_t src_label, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;
  void OnEdgesBatchInserted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;
  void OnEdgesBatchDeleted(
      UpdateTransaction& txn, label_t edge_label, label_t src_label,
      label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges) override;

  const std::unordered_set<label_t>& watched_edge_labels() const override {
    return watched_edge_labels_;
  }
  const std::vector<EdgeTableSubscription>& watched_edge_tables()
      const override {
    return watched_edge_tables_;
  }

 private:
  // Base (e.g. REPLYOF) edge group label — the label this maintainer watches.
  label_t base_edge_label_;
  // Materialized (e.g. ROOT_POST) edge group label.
  label_t mat_edge_label_;
  // Vertex label of the child/subtree nodes (e.g. Comment = REPLYOF src).
  label_t comment_label_;
  // Vertex label of the root / destination (e.g. Post).
  label_t root_label_;
  // RETURN endpoints can reverse storage, or select only the root's
  // zero-length partition. These properties belong to this FILL definition.
  bool reversed_output_{false};
  bool has_nonzero_output_{false};
  // Sub-table existence flags for the base edge.
  bool reply_has_comment_parent_{false};  // base edge between two comment nodes
  bool reply_has_post_parent_{false};     // base edge comment→root node
  // Depth interval for the index: only nodes at absDepth ∈ [lower_, upper_]
  // get a ROOT_POST entry. 0..UINT32_MAX means "all depths" (BI3 case).
  uint32_t lower_;
  uint32_t upper_;

  std::unordered_set<label_t> watched_edge_labels_;
  std::vector<EdgeTableSubscription> watched_edge_tables_;
  // Enables cascading embedded materialized-edge writes to arbitrary
  // downstream FILL maintainers. The server transaction path already
  // cascades through UpdateTransaction.
  FillIndexMaintenanceRegistry* fill_registry_{nullptr};

  // ---- Batch processing (two-phase compute/apply, serial/parallel) ----
  // Shared cores of the OnEdgesBatch{Inserted,Deleted} overload pairs.
  // Dispatch to the serial or parallel implementation by batch size.
  // Compute phase: one ClimbToRoot per edge (cached), shallow-first +
  // src-ordered subtree BFS, collect mat-edge (ROOT_POST) changes into
  // upsert/delete lists WITHOUT writing. Apply phase: one batch
  // BatchAddEdges / BatchDeleteEdges on the materialized table (undo-log /
  // WAL written once per batch instead of per descendant).
  void ApplyBatchInsertAtOnce(PropertyGraph& graph, timestamp_t ts,
                              label_t dst_label,
                              const std::vector<std::pair<vid_t, vid_t>>& edges,
                              UpdateTransaction* txn, Allocator* alloc);
  void ApplyBatchDeleteAtOnce(PropertyGraph& graph, timestamp_t ts,
                              label_t dst_label,
                              const std::vector<std::pair<vid_t, vid_t>>& edges,
                              UpdateTransaction* txn);
  void ApplyBatchInsertSerial(PropertyGraph& graph, timestamp_t ts,
                              label_t dst_label,
                              const std::vector<std::pair<vid_t, vid_t>>& edges,
                              UpdateTransaction* txn, Allocator* alloc);
  void ApplyBatchInsertParallel(
      PropertyGraph& graph, timestamp_t ts, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges, UpdateTransaction* txn,
      Allocator* alloc);
  void ApplyBatchDeleteSerial(PropertyGraph& graph, timestamp_t ts,
                              label_t dst_label,
                              const std::vector<std::pair<vid_t, vid_t>>& edges,
                              UpdateTransaction* txn);
  void ApplyBatchDeleteParallel(
      PropertyGraph& graph, timestamp_t ts, label_t dst_label,
      const std::vector<std::pair<vid_t, vid_t>>& edges,
      UpdateTransaction* txn);

  // Write collected mat-edge changes once (shared apply phase). Lists are
  // deduplicated by the caller; deletes are applied before upserts.
  void ApplyMatEdgeChanges(PropertyGraph& graph, timestamp_t ts,
                           const std::vector<std::pair<vid_t, vid_t>>& upserts,
                           const std::vector<std::pair<vid_t, vid_t>>& deletes,
                           UpdateTransaction* txn, Allocator* alloc) const;

  // Always keyed by a child, even when RETURN stores root -> child.
  GenericView MaterializedRoots(PropertyGraph& graph, timestamp_t ts) const;

  // Threshold: use the parallel path when the batch edge count is >= this
  // value. Minimum edges per worker thread.
  static constexpr size_t kBatchParallelThreshold = 512;
  static constexpr size_t kBatchMinEdgesPerThread = 256;

  // Climb UP from node p (label p_label) following outgoing base edges until
  // reaching a root_label_ node. Returns {root_vid, depth_of_p} where
  // depth_of_p is the #hops from p to the root, or nullopt if not reachable.
  std::optional<std::pair<vid_t, uint32_t>> ClimbToRoot(PropertyGraph& graph,
                                                        timestamp_t ts, vid_t p,
                                                        label_t p_label) const;

  // BFS downward from c (label comment_label_) following incoming base edges.
  // Collects all descendants {(vid, relDepth)}. Includes (c, 0). No depth
  // truncation (shared-REPLYOF safety per §3.3.1).
  void CollectSubtree(PropertyGraph& graph, timestamp_t ts, vid_t c,
                      std::vector<std::pair<vid_t, uint32_t>>& out) const;

  // Functionally upsert/delete the materialized edge for node x.
  // root==nullopt → delete existing ROOT_POST(x→*) if any.
  // root==R       → ensure ROOT_POST(x→R) exists; delete old if different.
  // txn != nullptr → route edge writes through txn (undo-log + WAL).
  // alloc != nullptr → used for embedded-path edge inserts.
  void SetRoot(PropertyGraph& graph, timestamp_t ts, vid_t x,
               std::optional<vid_t> root, UpdateTransaction* txn,
               Allocator* alloc) const;

  // Count x's outgoing base edges across all known sub-tables (for n:1 check).
  int CountOutgoingBaseEdges(PropertyGraph& graph, timestamp_t ts,
                             vid_t x) const;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Helper: create and register maintainers for all FILL indexes in the
// catalog (vertex-pushdown, aggregate, variadic-rel, etc.).
// Call this after FILL DDL execution to enable incremental maintenance.
// ---------------------------------------------------------------------------
void RegisterFillIndexMaintainers(const catalog::Catalog& catalog,
                                  const Schema& schema,
                                  transaction::Transaction* txn,
                                  FillIndexMaintenanceRegistry& registry,
                                  size_t max_maintenance_workers = 1,
                                  IGraphPlanner* planner = nullptr);

// ---------------------------------------------------------------------------
// Aggregate function types supported by incremental maintenance.
// ---------------------------------------------------------------------------
enum class AggregateFuncType : uint8_t {
  kCount = 0,  // COUNT(*): 1; COUNT(expr): 1 only for a non-null argument
  kSum = 1,    // SUM: contribution is the source property value
};

// ---------------------------------------------------------------------------
// Data structures for general graph pattern traversal (§2, index_update.md).
// ---------------------------------------------------------------------------

// A single traversal step: from a bound source node at position srcNodePos,
// follow the edge at edgeIdx (in the pattern's relsInfo array) to discover
// candidate node vids for the node at dstNodePos.
struct TraversalStep {
  uint32_t edgeIdx;     // Index into PatternIR::relsInfo
  uint32_t srcNodePos;  // Known (bound) node position in pattern
  uint32_t dstNodePos;  // Target (unbound or verify) node position to discover
};

// Pre-computed traversal plan for one occurrence j of an edge label.
// Defines the ordered sequence of graph traversal steps to find all complete
// matches given a fixed event edge at position j.
struct CompiledTraversalPlan {
  uint32_t eventEdgeIdx;     // The edge occurrence in the pattern
  uint32_t eventSrcNodePos;  // Pattern node pos for edge source
  uint32_t eventDstNodePos;  // Pattern node pos for edge dest
  label_t edgeLabel;         // Schema edge label for this occurrence
  bool edgeIsOutgoing;       // Pattern direction matches storage direction
  std::vector<TraversalStep> steps;  // BFS-ordered expansion + verify steps
};

// Complete derivation from CompleteAnchoredMatch: one full binding of all
// pattern node positions to graph node vids.
struct DerivationBinding {
  std::vector<vid_t> nodeVids;  // indexed by pattern node position
  std::vector<label_t>
      nodeLabels;  // actual schema label of each matched vertex

  // Physical edge identity for every PatternIR relationship occurrence. New
  // anchored plans populate this vector so batch ownership can distinguish
  // parallel edges and repeated occurrences. Legacy traversal leaves it empty.
  std::vector<PhysicalEdgeIdentity> relationIdentities;

  // Edge offsets for the anchor edge (REL_FILL only; -1 for NODE_FILL or
  // when offsets are not available). Used for precise edge identity in
  // parallel-edge scenarios (Issue 13).
  int32_t anchorOeOffset = -1;
  int32_t anchorIeOffset = -1;
};

// One partially aggregated target row produced by the Phase-E pushdown
// pipeline: the pipeline's terminal Project + GroupBy computes per-target-key
// partial sums, so no per-derivation C++ materialization happens on this path.
// Repeated-label variants place their canonical ownership filter immediately
// before this aggregation tail.
struct AggregatedTargetRow {
  // Target key. REL FILL: derived-relationship endpoints; NODE FILL:
  // anchorSrc holds the anchor vertex and anchorDst stays at the invalid vid.
  vid_t anchorSrc = std::numeric_limits<vid_t>::max();
  vid_t anchorDst = std::numeric_limits<vid_t>::max();
  // Actual schema labels of the key vertices (REL FILL needs them to select
  // the physical target table).
  label_t anchorSrcLabel = std::numeric_limits<label_t>::max();
  label_t anchorDstLabel = std::numeric_limits<label_t>::max();
  // Indexed by aggregate_columns_ position. kEmpty means no contribution
  // (SUM over an all-null group). Finalizer columns are always kEmpty and
  // are recomputed by the writeback stage from the merged state.
  absl::InlinedVector<Property, 1> columnContribs;
};

// ---------------------------------------------------------------------------
// The only catalog-facing materialized-pattern index. It selects and composes
// execution strategies from PatternIR and output metadata. Its general
// strategy supports arbitrary node/rel aggregate patterns with COUNT and SUM,
// including label-based CASE expression contributions.
//
// Implements CompleteAnchoredMatch: for each edge occurrence j, substitute
// the event edge e=(s,d) and evaluate the remaining pattern against the
// storage graph to find all (anchor, source) derivation pairs.
// ---------------------------------------------------------------------------
class PatternMaterializedIndex {
  friend class FillIndexMaintenanceRegistry;

 public:
  using Derivation = DerivationBinding;

  // Construct from FillIndexAuxInfo, resolving PatternIR → labels and
  // building pre-computed traversal plans for every edge occurrence.
  PatternMaterializedIndex(const catalog::FillIndexAuxInfo& aux,
                           const Schema& schema,
                           const catalog::Catalog& catalog,
                           transaction::Transaction* txn,
                           size_t max_maintenance_workers = 1,
                           FillIndexMaintenanceRegistry* fill_registry = nullptr,
                           IGraphPlanner* planner = nullptr);

  // Out-of-line: ast_runtime_ holds execution::ExprBase (incomplete here).
  ~PatternMaterializedIndex();

  void OnEdgeInserted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst, Allocator& alloc);

  void OnEdgeDeleted(PropertyGraph& graph, timestamp_t ts, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst, Allocator* alloc = nullptr);

  // Server (MVCC) path: routes property writes through txn so they are
  // covered by the undo-log and WAL redo (Issue 10 fix).
  void OnEdgeInserted(UpdateTransaction& txn, label_t edge_label,
                      label_t src_label, label_t dst_label, vid_t src,
                      vid_t dst);
  void OnEdgeDeleted(UpdateTransaction& txn, label_t edge_label,
                     label_t src_label, label_t dst_label, vid_t src,
                     vid_t dst);

  // Identity-preserving server path used by UpdateTransaction. These are the
  // Phase-D entry points; endpoint-only overloads remain for embedded/legacy
  // callers and conservatively fall back when ownership needs exact identity.
  void OnEdgeInserted(UpdateTransaction& txn, const EdgeChange& change);
  void OnEdgeDeleted(UpdateTransaction& txn, const EdgeChange& change);

  // Batch edge interface with anchor aggregation (L2a) and source-VID
  // sorting for CSR locality (L2b).
  void OnEdgesBatchInserted(PropertyGraph& graph, timestamp_t ts,
                            label_t edge_label, label_t src_label,
                            label_t dst_label,
                            const std::vector<std::pair<vid_t, vid_t>>& edges,
                            Allocator& alloc);
  // Identity-preserving batch entry for the direct storage interface:
  // ownership-capable occurrences need the physical edge identities, which
  // the endpoint-only overload above cannot provide.
  void OnEdgesBatchInserted(PropertyGraph& graph, timestamp_t ts,
                            const EdgeChangeBatch& changes, Allocator& alloc);
  void OnEdgesBatchDeleted(PropertyGraph& graph, timestamp_t ts,
                           const EdgeChangeBatch& changes,
                           Allocator* alloc = nullptr);
  void OnEdgesBatchDeleted(PropertyGraph& graph, timestamp_t ts,
                           label_t edge_label, label_t src_label,
                           label_t dst_label,
                           const std::vector<std::pair<vid_t, vid_t>>& edges,
                           Allocator* alloc = nullptr);
  void OnEdgesBatchInserted(UpdateTransaction& txn, label_t edge_label,
                            label_t src_label, label_t dst_label,
                            const std::vector<std::pair<vid_t, vid_t>>& edges);
  void OnEdgesBatchDeleted(UpdateTransaction& txn, label_t edge_label,
                           label_t src_label, label_t dst_label,
                           const std::vector<std::pair<vid_t, vid_t>>& edges);
  void OnEdgesBatchInserted(UpdateTransaction& txn,
                            const EdgeChangeBatch& changes);
  void OnEdgesBatchDeleted(UpdateTransaction& txn,
                           const EdgeChangeBatch& changes);

  // Property changes evaluate the full predicate and aggregate argument in
  // both row images, then publish their combined per-group difference.
  void OnVertexPropertyChanged(PropertyGraph& graph, timestamp_t ts,
                               label_t vertex_label, vid_t vid, int prop_id,
                               const Property& old_val,
                               const Property& new_val, Allocator* alloc = nullptr);
  void OnVertexPropertyChanged(UpdateTransaction& txn, label_t vertex_label,
                               vid_t vid, int prop_id, const Property& old_val,
                               const Property& new_val);
  void OnEdgePropertyChanged(PropertyGraph& graph, timestamp_t ts,
                             const PhysicalEdgeIdentity& edge, int prop_id,
                             const Property& old_val, const Property& new_val,
                             Allocator* alloc = nullptr);
  void OnEdgePropertyChanged(UpdateTransaction& txn,
                             const PhysicalEdgeIdentity& edge, int prop_id,
                             const Property& old_val, const Property& new_val);
  std::vector<std::pair<label_t, int>> watched_vertex_properties() const;

  // Zero-length recursive witnesses are created/deleted with their vertex;
  // no source edge event exists to discover them.
  const std::vector<label_t>& watched_vertex_lifecycles() const {
    return zero_hop_vertex_labels_;
  }
  void OnVertexInserted(UpdateTransaction& txn, label_t label, vid_t vid);
  void OnVertexDeleted(UpdateTransaction& txn, label_t label, vid_t vid);
  void OnVertexInserted(PropertyGraph& graph, timestamp_t ts, label_t label,
                        vid_t vid, Allocator& alloc);
  void OnVertexDeleted(PropertyGraph& graph, timestamp_t ts, label_t label,
                       vid_t vid, Allocator* alloc = nullptr);
  bool OwnsVertexProperty(const Schema& schema, label_t label,
                          int property) const;
  bool OwnsEdgeProperty(const Schema& schema, label_t source,
                        label_t destination, label_t edge, int property) const;

  bool watches_edge_property(label_t edge_label, label_t src_label,
                             label_t dst_label, const std::string& name) const;

  const std::unordered_set<label_t>& watched_edge_labels() const {
    return watched_edge_labels_;
  }
  const std::vector<EdgeTableSubscription>& watched_edge_tables() const {
    return watched_edge_tables_;
  }
  // Edge group structurally/property-wise produced by this REL FILL. Used to
  // order callbacks when one base mutation reaches both a producer and one of
  // its downstream consumers through different dependency paths.
  std::optional<label_t> produced_edge_label() const {
    return produced_edge_label_;
  }
  // Whether this maintainer reads a relation membership/property changed by
  // producer. Merely sharing an existing carrier does not create a dependency
  // between independent property FILLs on that carrier.
  bool depends_on_relation_output(
      const PatternMaterializedIndex& producer) const;
  // Canonical materialization target, e.g. PERSON(COUNTRYNAME) or
  // ROOT_POST(). Used only to attribute benchmark timing to one FILL index.
  const std::string& target_name() const { return target_name_; }

  // Per-column aggregate metadata resolved from FillIndexAuxInfo.
  struct AggregateColumnInfo {
    AggregateFuncType funcType;  // COUNT or SUM
    int targetPropId;            // Property column ID on the anchor table
    std::string targetPropName;  // Column name (for edge property access)
    uint32_t sourceNodePos{UINT32_MAX};
    int sourcePropId{kSourcePropIsOid};
    label_t sourceLabel{std::numeric_limits<label_t>::max()};

    // DataTypes for type-safe arithmetic (Issue 4 fix).
    // targetType: DataType of the target column — determines the arithmetic
    //   and storage format for aggregate deltas.
    // sourceType: DataType of the source column (for SUM source property
    // reads).
    DataTypeId targetType = DataTypeId::kInt64;
    DataTypeId sourceType = DataTypeId::kInt64;
    // Only SUM obeys modular arithmetic. COUNT and the hidden group row
    // count retain checked, nonnegative cardinality semantics.
    bool wrapsSignedArithmetic{false};

    // Expression-based contribution (parsed from nameTemplate).
    // For label-check expressions like CASE label(m)='POST' THEN 1.0 ELSE 0.5.
    // Each checked node may have multiple possible labels (multi-label nodes).
    // labelCheckNodePos[i] identifies the pattern node;
    // labelCheckAllLabels[i] lists all label_t candidates for that node;
    // labelCheckValue[i] is the expected label name (e.g. "POST").
    // At runtime, we iterate over candidates and test vertex membership.
    std::vector<uint32_t>
        labelCheckNodePos;  // Pattern positions of checked nodes
    std::vector<std::vector<label_t>>
        labelCheckAllLabels;                   // All possible labels
    std::vector<std::string> labelCheckValue;  // Label name strings to match
    double labelMatchContrib = 1.0;    // Contribution when label matches
    double labelDefaultContrib = 0.5;  // Contribution when no label matches

    // Serialized aggregate argument AST (::common::Expression), non-null for
    // indexes created after the AST migration. Null → legacy string-parsed
    // path above. COUNT columns may carry the argument AST for rewrite
    // matching and contribute one exactly when the argument is non-null.
    std::shared_ptr<::common::Expression> astExpr;
    // Non-null for a visible finalized column f(state). The additive state is
    // persisted in finalizerStatePropName and updated by a separate aggregate
    // column in the same FILL target.
    std::shared_ptr<::common::Expression> finalizerExpr;
    std::string finalizerStatePropName;
    DataTypeId finalizerStateType = DataTypeId::kInt64;
    bool isFinalizer{false};
    // Variable tags (pattern-node positions) referenced by the AST — the row
    // layout for record evaluation. Filled by buildAstRuntime().
    std::vector<uint32_t> astTags;
    // Property references extracted from the AST: (pattern node position,
    // concrete vertex label, schema property id). Property IDs may differ
    // between labels of the same pattern variable. Drives vertex-property
    // change propagation. Filled by buildAstRuntime().
    std::vector<std::tuple<uint32_t, label_t, int>> astPropRefs;
  };

 private:
  using RelDelta = std::tuple<std::string, Property, DataTypeId>;
  using RelDeltaMap = std::unordered_map<uint64_t, std::vector<RelDelta>>;

  std::string target_name_;
  std::string row_count_column_;
  std::optional<size_t> row_count_column_index_;
  std::vector<label_t> zero_hop_vertex_labels_;
  void MaintainZeroHopVertex(PropertyGraph& graph, timestamp_t ts,
                             label_t label, vid_t vid, bool insert,
                             UpdateTransaction* txn, Allocator* alloc);
  bool wrapsNodeAggregate(int propertyId) const;
  bool wrapsRelAggregate(const std::string& propertyName) const;
  // One catalog index is represented by one maintainer. Specialized
  // algorithms remain internal strategies and are composed here for pure or
  // mixed indexes (for example NODE_PROPERTY + NODE_AGGREGATE).
  bool general_pattern_strategy_enabled_{false};
  size_t max_maintenance_workers_{1};
  // Embedded derived-edge writes use this registry as a cascading batch
  // mutation gateway. UpdateTransaction owns the equivalent server path.
  FillIndexMaintenanceRegistry* fill_registry_{nullptr};
  std::optional<label_t> produced_edge_label_;
  std::vector<std::unique_ptr<detail::MaintenanceStrategy>> strategy_delegates_;

  // Fill type: NODE (=0) or REL (=1).
  uint8_t fill_type_;

  // Resolved schema vertex label for each pattern node position.
  std::vector<label_t> node_labels_;
  // All possible schema vertex labels per node (for multi-label nodes like
  // COMMENT:POST).
  std::vector<std::vector<label_t>> node_all_labels_;

  // --- NODE FILL ---
  uint32_t anchor_pos_;   // Anchor node position in pattern
  label_t anchor_label_;  // Schema label of the anchor vertex

  // --- REL FILL ---
  // The materialized relation need not be an edge in PatternIR. BI4, for
  // example, derives BI4_MESSAGE_COUNT(FORUM, PERSON) from a three-edge
  // pattern. anchor_edge_pos_ is therefore optional, while the endpoint node
  // positions and physical label pair are always resolved independently.
  uint32_t anchor_edge_pos_{UINT32_MAX};
  uint32_t anchor_src_pos_{UINT32_MAX};
  uint32_t anchor_dst_pos_{UINT32_MAX};
  label_t anchor_src_label_{std::numeric_limits<label_t>::max()};
  label_t anchor_dst_label_{std::numeric_limits<label_t>::max()};
  label_t anchor_edge_label_{std::numeric_limits<label_t>::max()};
  std::vector<std::pair<label_t, label_t>> anchor_label_pairs_;

  // Watched edge labels.
  std::unordered_set<label_t> watched_edge_labels_;
  std::vector<EdgeTableSubscription> watched_edge_tables_;

  // Resolved edge metadata for each pattern edge occurrence.
  // Supports multi-label source/destination via label pair combinations.
  struct PathEdgeInfo {
    std::vector<std::pair<label_t, label_t>>
        label_pairs;  // (src_label, dst_label) combos
    // Concrete stored-direction sub-tables. Recursive relations can traverse
    // intermediate label pairs absent from their logical endpoint labels.
    std::vector<std::pair<label_t, label_t>> physical_label_pairs;
    // Distinct storage labels for this logical relation occurrence; catalog
    // endpoint variants may share a label but retain their physical pairs.
    std::vector<label_t> edge_labels;
    label_t edge_label;
    bool is_outgoing;  // true: pattern direction matches storage direction
  };
  std::vector<PathEdgeInfo> path_edges_;

  // Correlated one-hop existential tests are Boolean filters over complete
  // outer bindings, not additional multiplicative pattern relationships.
  struct ExistsPredicate {
    uint32_t srcNodePos;
    uint32_t dstNodePos;
    bool negated;
    bool directed;
    std::vector<label_t> srcLabels;
    std::vector<label_t> dstLabels;
    std::vector<EdgeTableSubscription> tables;
  };
  std::vector<ExistsPredicate> exists_predicates_;
  std::vector<int> presence_prop_ids_;
  std::vector<Property> presence_defaults_;

  // The merged PatternIR used for matching.
  common::PatternIR pattern_ir_;

  // Per-occurrence traversal plans.
  std::vector<CompiledTraversalPlan> traversal_plans_;

  // Aggregate column metadata, one entry per target column.
  std::vector<AggregateColumnInfo> aggregate_columns_;
  // MAX of COUNT grouped by (anchor, this node). Re-evaluate only affected
  // anchor neighborhoods; no process-local aggregate cache can outlive a
  // transaction rollback or become stale after reopening a catalog.
  std::optional<uint32_t> grouped_count_node_pos_;
  std::optional<uint32_t> distinct_count_node_pos_;
  std::unordered_set<PhysicalEdgeIdentity, PhysicalEdgeIdentityHash> recompute_excluded_edges_;
  std::unordered_set<PhysicalEdgeIdentity, PhysicalEdgeIdentityHash> recompute_excluded_incoming_;
  void ApplyBatchDerivedDistinctCount(PropertyGraph& graph, timestamp_t ts,
                                      const EdgeChangeBatch& changes, int64_t sign,
                                      UpdateTransaction* txn, Allocator* alloc);
  void ApplyBatchGroupedCountMax(PropertyGraph& graph, timestamp_t ts,
                                 const EdgeChangeBatch& changes, int64_t sign,
                                 UpdateTransaction* txn);

  // MATCH-WHERE predicate evaluated after a structural derivation is
  // complete. Variable tags use node positions followed by relationship
  // positions; relationship values bind their exact physical edge identity.
  std::shared_ptr<::common::Expression> predicate_expr_;
  std::vector<uint32_t> predicate_ast_tags_;
  std::vector<std::tuple<uint32_t, label_t, int>> predicate_prop_refs_;

  // Parsed (unbound) expression trees for the AST columns, index-aligned
  // with aggregate_columns_. Defined in fill_index_maintenance.cc so this
  // header stays free of execution-layer includes. Null when no column
  // carries an AST (legacy catalog).
  struct AstRuntime;
  std::unique_ptr<AstRuntime> ast_runtime_;

  // Cached read-only query pipelines for event-anchored matching. Kept behind
  // a pimpl so transaction headers do not depend on execution/planner types.
  // Unsupported definitions and legacy catalog entries leave this empty and
  // continue to use CompleteAnchoredMatch.
  struct AnchoredRuntime;
  std::unique_ptr<AnchoredRuntime> anchored_runtime_;

  // Per-pattern-node traversal plans for property-change propagation: for
  // node position p, the ordered expand+verify step sequence that completes
  // a partial binding {p → v} into all full derivations.
  std::vector<std::vector<TraversalStep>> node_traversal_plans_;

  // ---- Methods ----

  void ApplyPropertyChange(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<Derivation>& candidates,
      const std::function<std::vector<uint32_t>(const Derivation&)>& tags,
      const std::string& property_name, const Property& old_value,
      const Property& new_value, UpdateTransaction* txn, Allocator* alloc);
  void OnVertexPropertyChangedImpl(
      PropertyGraph& graph, timestamp_t ts, label_t vertex_label, vid_t vid,
      int prop_id, const Property& old_val, const Property& new_val,
      UpdateTransaction* txn, Allocator* alloc);
  void OnEdgePropertyChangedImpl(
      PropertyGraph& graph, timestamp_t ts, const PhysicalEdgeIdentity& edge,
      int prop_id, const Property& old_val, const Property& new_val,
      UpdateTransaction* txn, Allocator* alloc);

  // Build BFS-based traversal plans for all edge occurrences.
  void buildTraversalPlans();

  // Build node-centric traversal plans (see node_traversal_plans_).
  void buildNodeTraversalPlans();

  // Run a compiled step sequence over an initial partial-binding layer.
  std::vector<Derivation> RunTraversalSteps(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<TraversalStep>& steps,
      std::vector<Derivation> initLayer);

  // One-hop traversal for a normal relation, or reflexive transitive closure
  // for a recursive relation, in the requested logical direction.
  std::vector<std::pair<label_t, vid_t>> TraversePatternEdge(
      PropertyGraph& graph, timestamp_t ts, uint32_t edgeIdx,
      label_t startLabel, vid_t startVid, bool logicalForward) const;

  // All complete derivations containing vertex v (with actual label
  // v_label) at pattern node position nodePos.
  std::vector<Derivation> CompleteAnchoredMatchFromNode(
      PropertyGraph& graph, timestamp_t ts, size_t nodePos, vid_t v,
      label_t v_label, bool applyPredicate = true);

  // Build ast_runtime_: parse each AST column's serialized ::common::Expression
  // into an execution::ExprBase (record mode, ContextMeta keyed by the AST's
  // variable tags) and record the referenced tags + property references.
  // Throws when a stored AST fails to parse (created indexes are validated
  // at DDL; a parse failure here indicates corruption).
  void buildAstRuntime(const Schema& schema);

  // Compile one cached anchored query pipeline per supported relationship
  // occurrence. Compilation happens during registry population, outside the
  // measured maintenance callback.
  void buildAnchoredRuntime(const catalog::FillIndexAuxInfo& aux,
                            const Schema& schema, IGraphPlanner* planner);
  bool CanUseUnsortedNodeBatch(label_t edge_label, size_t batch_size) const;

  // Execute a cached anchored plan over a complete physical event batch. A
  // disengaged optional means no compatible cached plan (or a read-only
  // execution failure), so callers can safely fall back before writeback.
  std::optional<std::vector<Derivation>> MatchAnchoredBatchWithPipeline(
      PropertyGraph& graph, timestamp_t ts, size_t planIdx,
      const EdgeChangeBatch& changes);

  // Phase-E aggregated variant of the above: the cached pipeline ends with a
  // contribution Project + GroupBy, returning one row per distinct target
  // key. A disengaged optional means the cached plan for planIdx is not in
  // aggregated mode (or failed), so callers fall back to the derivation path.
  std::optional<std::vector<AggregatedTargetRow>>
  MatchAnchoredBatchAggregatedWithPipeline(PropertyGraph& graph,
                                           timestamp_t ts, size_t planIdx,
                                           const EdgeChangeBatch& changes,
                                           std::shared_ptr<const EdgeOwnershipMap>
                                               batchOwnership = nullptr);

  // Build the final (vid, new value) pairs for the aggregated NODE writeback
  // in parallel over the merged per-key sums, then flush one batched
  // transaction write per target column. Runs instead of the serial
  // anchor_deltas accumulation on the aggregated path.
  void FlushAggregatedNodeWriteback(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<AggregatedTargetRow>& rows, int64_t sign,
      UpdateTransaction* txn);

  // Snapshot this before writing any target column: absent groups must start
  // all additive state at zero, regardless of user column defaults.
  bool StartsNewNodeAggregateGroup(PropertyGraph& graph, vid_t anchor,
                                   const Property& rowCountDelta,
                                   int64_t sign) const;

  // Bind the parsed AST trees against `graph` at `ts`, producing one bound
  // evaluator per aggregate column (null entries for legacy/COUNT columns).
  // Callers invoke this once per notification/batch — and once per worker on
  // the parallel path: bound evaluators pre-resolve per-label property
  // columns and are read-only at eval time.
  void BindAstColumns(
      PropertyGraph& graph, timestamp_t ts,
      std::vector<std::unique_ptr<execution::BindedExprBase>>& out) const;
  void BindFinalizers(
      PropertyGraph& graph, timestamp_t ts,
      std::vector<std::unique_ptr<execution::BindedExprBase>>& out) const;
  Property EvalFinalizer(const AggregateColumnInfo& col, const Property& state,
                         const execution::BindedExprBase& bound) const;

  std::unique_ptr<execution::BindedExprBase> BindPredicate(
      PropertyGraph& graph, timestamp_t ts) const;

  // Remove structurally complete tuples for which MATCH-WHERE is false or
  // null. A missing predicate is a no-op.
  void FilterDerivationsByPredicate(PropertyGraph& graph, timestamp_t ts,
                                    std::vector<Derivation>& derivations) const;

  bool EvaluateExistsPredicates(PropertyGraph& graph, timestamp_t ts,
                                const Derivation& deriv,
                                const EdgeOwnershipMap* excluded = nullptr) const;

  // Insert notifications see the new graph, delete notifications the old
  // graph. Excluding the event batch reconstructs the opposite row image for
  // affected outer bindings and existential witnesses, without rebuilding
  // the materialized view.
  void ApplyExistentialAggregateDeltas(
      PropertyGraph& graph, timestamp_t ts, const EdgeChangeBatch& changes,
      int64_t sign, UpdateTransaction* txn, Allocator* alloc);

  bool EvaluatePredicateWithPropertyOverride(
      PropertyGraph& graph, timestamp_t ts, const Derivation& deriv,
      const std::vector<uint32_t>& overriddenTags,
      const std::string& propertyName, const Property& value) const;

  // Contribution for one column/derivation pair: AST evaluation when `bound`
  // carries an evaluator for `colIdx`; legacy string-parsed path otherwise.
  Property ComputeContributionWithAst(
      PropertyGraph& graph, timestamp_t ts, const AggregateColumnInfo& col,
      const Derivation& deriv,
      const std::vector<std::unique_ptr<execution::BindedExprBase>>* bound,
      size_t colIdx);

  // Evaluate the full pattern given a fixed event edge e=(s,d) at planIdx.
  // src_label / dst_label are the actual Schema labels of the event edge's
  // endpoints, as reported by the change notification.  They are assigned
  // directly to the event nodes — scanning label-pair tables to re-derive
  // them is unreliable because local vertex IDs are reused across label
  // tables (e.g. a POST vid collides with a COMMENT vid and gets
  // mislabeled), which silently drops derivations (Issue 14).
  // Returns all complete derivations.
  std::vector<Derivation> CompleteAnchoredMatch(PropertyGraph& graph,
                                                timestamp_t ts, size_t planIdx,
                                                vid_t src, vid_t dst,
                                                label_t src_label,
                                                label_t dst_label,
                                                const PhysicalEdgeIdentity* event = nullptr);

  // Compute the aggregate contribution for a single derivation.
  Property ComputeContribution(PropertyGraph& graph, timestamp_t ts,
                               const AggregateColumnInfo& col,
                               const Derivation& deriv);

  // Apply computed deltas to anchor nodes (NODE FILL).
  // txn: if non-null, writes are routed through txn for undo-log/WAL (Issue
  // 10).
  void ApplyNodeAggregateDeltas(PropertyGraph& graph, timestamp_t ts,
                                const std::vector<Derivation>& derivations,
                                int64_t sign, UpdateTransaction* txn = nullptr);

  // Apply computed deltas to anchor edges (REL FILL).
  // txn: if non-null, writes are routed through txn for undo-log/WAL (Issue
  // 10).
  void ApplyRelAggregateDeltas(PropertyGraph& graph, timestamp_t ts,
                               const std::vector<Derivation>& derivations,
                               int64_t sign, UpdateTransaction* txn = nullptr);

  // Batched node-aggregate delta application: collects derivations across
  // multiple edges, aggregates deltas per anchor, and writes each anchor
  // once (L2a).
  void ApplyBatchNodeAggregateDeltas(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn = nullptr);

  // Batched rel-aggregate delta application (REL FILL variant of above).
  void ApplyBatchRelAggregateDeltas(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn = nullptr, Allocator* alloc = nullptr);

  bool IsExistingTargetRelTable(label_t edge_label, label_t src_label,
                                label_t dst_label) const;
  bool TryApplyExistingTargetRelInsert(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, UpdateTransaction* txn);

  // Deduplicate derivations by sorting and removing adjacent duplicates.
  static void deduplicateDerivations(std::vector<Derivation>& derivations);

  // Parse nameTemplate to extract label-based CASE expression info.
  void parseNameTemplateForLabelCheck(
      const std::string& nameTemplate,
      const std::vector<std::string>& nodeUniqueNames,
      const catalog::IndexColnumInfo& colInfo, AggregateColumnInfo& aggCol);

  // ---- Parallel batch processing ----

  // Threshold: use parallel path when the number of edges >= this value.
  static constexpr size_t kParallelThreshold = 512;
  // Minimum edges per worker thread to amortize thread creation overhead.
  static constexpr size_t kMinEdgesPerThread = 256;

  // Serial path (original single-threaded implementation).
  void ApplyBatchNodeAggregateDeltasSerial(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn);

  // Fast path for directed one-hop COUNT NODE FILL patterns. A changed edge
  // contributes exactly +/-1 to the endpoint that is the FILL anchor.
  bool TryApplyOneHopCountNodeFastPath(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn);

  // Fast path for directed two-hop COUNT NODE FILL when the changed edge is
  // incident to the anchor. The contribution is the degree of the middle
  // vertex on the other relationship, e.g. BI6 HASCREATOR maintenance.
  bool TryApplyDirectedTwoHopDegreeCountNodeFastPath(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn);

  // Factor a predicate-free, single-label, undirected two-hop COUNT into
  // middle-vertex degrees and anchor multiplicities. Repeated relationships
  // use the cross term once; physical parallel edges retain their multiplicity.
  bool TryApplyTwoHopCountNodeFastPath(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn);

  void ApplyBatchRelAggregateDeltasSerial(
      PropertyGraph& graph, timestamp_t ts,
      const EdgeChangeBatch& sortedChanges, int64_t sign,
      UpdateTransaction* txn, Allocator* alloc);

  // Derived REL FILL pipeline:
  //   1. collect derivations in parallel;
  //   2. globally deduplicate on the caller thread;
  //   3. aggregate anchor deltas in parallel;
  //   4. write derived edges on the caller thread.
  void ApplyBatchDerivedRelAggregateDeltasParallel(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<std::pair<vid_t, vid_t>>& sortedEdges,
      label_t edge_label, label_t src_label, label_t dst_label, int64_t sign,
      UpdateTransaction* txn, Allocator* alloc);

  // Shared serial writeback. In particular, this owns the create/update/delete
  // lifecycle of derived aggregate edges.
  void ApplyMergedRelAggregateDeltas(PropertyGraph& graph, timestamp_t ts,
                                     RelDeltaMap& anchor_deltas, int64_t sign,
                                     UpdateTransaction* txn, Allocator* alloc);

  // Parallel path (multi-threaded CompleteAnchoredMatch + local aggregation).
  void ApplyBatchNodeAggregateDeltasParallel(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<std::pair<vid_t, vid_t>>& sortedEdges,
      label_t edge_label, label_t src_label, label_t dst_label, int64_t sign,
      UpdateTransaction* txn);

  void ApplyBatchRelAggregateDeltasParallel(
      PropertyGraph& graph, timestamp_t ts,
      const std::vector<std::pair<vid_t, vid_t>>& sortedEdges,
      label_t edge_label, label_t src_label, label_t dst_label, int64_t sign,
      UpdateTransaction* txn);

 public:
  bool RefreshFromSource(PropertyGraph&, timestamp_t, Allocator&,
                         UpdateTransaction* = nullptr, bool force = false);

 private:
  void MarkSourceAffected(PropertyGraph& graph, timestamp_t ts, label_t label,
                          vid_t vertex);
  // Only the registry's explicit no-DML schema barrier may transfer this
  // semantic state. Physical pipelines and subscriptions are always rebuilt.
  bool ReuseCleanSourceInitialization(const PatternMaterializedIndex& previous);
  std::optional<std::pair<uint64_t, uint64_t>> source_catalog_identity_;
  void MarkSourceEdgesAffected(PropertyGraph& graph, timestamp_t ts,
                               const EdgeChangeBatch& changes);
  struct SourceRuntime;
  std::unique_ptr<SourceRuntime> source_runtime_;
  struct DirectedCountCache;
  std::unique_ptr<DirectedCountCache> directed_count_cache_;
  bool RefreshCarrierPathCounts(PropertyGraph&, timestamp_t, Allocator&,
                                UpdateTransaction*, size_t& evaluated_rows);
  bool CollectCarrierPathDeltas(PropertyGraph&, timestamp_t,
                                const EdgeChangeBatch&);
  bool RefreshRecursiveDistinctCounts(PropertyGraph&, timestamp_t,
                                      UpdateTransaction*, size_t&);
  // Semantic capability, derived from the same catalog used by every query.
  std::unordered_set<int> distinct_neighbor_columns_;
  bool RefreshDistinctNeighborCounts(PropertyGraph&, timestamp_t,
      const EdgeChangeBatch&, int64_t sign, UpdateTransaction*);

};

// ---------------------------------------------------------------------------
// Maintenance-time accounting (benchmark instrumentation).
//
// The notify dispatchers (UpdateTransaction / StorageAPUpdateInterface)
// measure wall time around the maintainer-callback loop and accumulate it
// into the registry, so benchmarks can read the time spent in incremental
// index maintenance alone — excluding the DML itself — and compare it with
// the FILL build time.
// ---------------------------------------------------------------------------
struct FillMaintenanceTiming {
  int64_t insert_ns = 0;      // ns spent in insert-side maintainer callbacks
  int64_t delete_ns = 0;      // ns spent in delete-side maintainer callbacks
  int64_t insert_events = 0;  // insert dispatch events (single-edge or batch)
  int64_t delete_events = 0;  // delete dispatch events (single-edge or batch)
  int64_t source_refresh_ns = 0;
  int64_t source_refreshes = 0;
  int64_t source_full_refreshes = 0;
  int64_t source_initialization_reuses = 0;
  int64_t source_keys_evaluated = 0;
  int64_t source_rows_evaluated = 0;
  int64_t anchored_degree_batches = 0;
  int64_t anchored_aggregate_batches = 0;
  int64_t anchored_derivation_batches = 0;
  int64_t legacy_batches = 0;
  int64_t affected_anchor_recompute_batches = 0;
  int64_t target_edge_initialize_batches = 0;
  int64_t target_edge_remove_batches = 0;
  int64_t carrier_count_delta_batches = 0;
  int64_t carrier_count_recompute_batches = 0;
  int64_t recursive_distinct_batches = 0;
  int64_t factorized_two_hop_batches = 0;
  int64_t path_input_edges = 0;
  int64_t path_output_rows = 0;
};

// ---------------------------------------------------------------------------
// Registry: maps Schema edge labels → maintainers watching those edges.
//
// Thread safety: All public methods are internally serialized by a mutex.
// GetMaintainers / GetMaintainersCached return a VALUE (copy of the shared_ptr
// vector) so callers can iterate safely after the lock is released.
//
// Register() is NOT mutex-protected and must only be called from within the
// PopulateFn callback (which is invoked under the internal mutex).
// ---------------------------------------------------------------------------
class FillIndexMaintenanceRegistry {
 public:
  // Callback type for population of the registry from the catalog.
  // The callback runs under the internal mutex.
  using PopulateFn = std::function<void(FillIndexMaintenanceRegistry&)>;

  // Set a callback that populates this registry from the catalog.
  // Existing entries are invalidated; call Rebuild() to populate immediately.
  void SetPopulateCallback(PopulateFn fn);
  void RefreshSourceQueries(PropertyGraph&, timestamp_t, Allocator&,
                            UpdateTransaction* = nullptr, bool force = false);
  void InvalidateSourceQueries();
  bool HasSourceIndexes();

  // Register a maintainer under its exact catalog entry name. Called from
  // within PopulateFn — NOT
  // mutex-protected. Callers outside the populate callback must hold the
  // internal mutex explicitly (there are none in the current codebase).
  void Register(const std::string& indexName,
                std::shared_ptr<PatternMaterializedIndex> maintainer,
                // OID and catalog generation timestamp. Omitting this proof
                // keeps externally registered maintainers ineligible for reuse.
                std::optional<std::pair<uint64_t, uint64_t>> catalogIdentity =
                    std::nullopt);

  // Called under the populate lock, just like Register().
  void RegisterStaged(const std::string& indexName,
                      StagedFillDependencies dependencies);
  // Cached checks: startup/FILL/schema refresh populates these before DML.
  // A negative property id denotes a topology/schema mutation.
  void AssertStagedVertexMutationAllowed(label_t label, int property_id = -1) const;
  void AssertStagedEdgeMutationAllowed(label_t edge_label, label_t src_label,
                                      label_t dst_label, int property_id = -1) const;
  void AssertStagedVertexPropertyAllowed(const Schema& schema, label_t label,
                                         const std::string& property) const;
  void AssertStagedEdgePropertyAllowed(const Schema& schema, label_t edge_label,
                                       label_t src_label, label_t dst_label,
                                       const std::string& property) const;

  // Synchronously remove one catalog index's maintainer from every edge-table
  // and vertex-property subscription. Thread-safe. Returns false when the
  // index was not registered in memory.
  bool Unregister(const std::string& indexName);

  // Drop all registered maintainers and mark for re-population on next use
  // (e.g. after FILL DDL creates new indexes).  Thread-safe.
  void Clear();

  // Eagerly rebuild subscriptions from the catalog while outside execution
  // locks. AP DELETE uses GetMaintainersCached(), which cannot populate while
  // holding the graph/planner locks. Call after startup and FILL/schema DDL
  // so a first DELETE sees all registered definitions, including after reopen.
  void ScheduleRefresh();

  // Compatibility entry point for callers that explicitly rebuild subscriptions.
  void Rebuild() { ScheduleRefresh(); }

  // (Legacy alias kept for external callers.)
  void Refresh() { Rebuild(); }

  // Incremental refresh: call the populate callback to pick up newly-created
  // indexes from the catalog. Already-registered indexes are skipped.
  // Thread-safe.
  void RefreshNew();

  // Track which catalog index entries have already been registered.
  bool IsIndexRegistered(const std::string& indexName) const {
    return registered_index_names_.count(indexName) > 0;
  }
  void MarkIndexRegistered(const std::string& indexName) {
    registered_index_names_.insert(indexName);
  }

  // Return exact physical-table subscribers for this edge event.
  // Auto-populates from catalog if needed. Thread-safe.
  std::vector<std::shared_ptr<PatternMaterializedIndex>> GetMaintainers(
      label_t edge_label, label_t src_label, label_t dst_label);

  // Return a snapshot WITHOUT triggering population from catalog.
  // Use this in contexts where catalog access would deadlock (e.g. delete
  // plan). Thread-safe.
  std::vector<std::shared_ptr<PatternMaterializedIndex>> GetMaintainersCached(
      label_t edge_label, label_t src_label, label_t dst_label);

  // Topologically order callbacks that are reached by the same mutation.
  // INSERT evaluates consumers before derived-edge producers; DELETE uses the
  // opposite order. This is the sequential-delta ordering that counts an
  // event affecting multiple levels of one pattern exactly once.
  std::vector<std::shared_ptr<PatternMaterializedIndex>>
  OrderMaintainersForMutation(
      const std::vector<std::shared_ptr<PatternMaterializedIndex>>& maintainers,
      bool is_insert) const;

  // Delete source tables before their materialized carriers during a vertex
  // cascade. Uses the current registry snapshot without catalog access.
  std::vector<EdgeTableSubscription> OrderEdgeTablesForDeletion(
      const std::vector<EdgeTableSubscription>& tables) const;

  // Apply an embedded batch edge mutation and synchronously propagate it to
  // every dependent FILL maintainer. Downstream maintainers write their own
  // derived batches through the same gateway, producing a general cascade.
  // Delete callbacks run before the physical delete so they can still match
  // the old graph; insert callbacks run after the physical insert so they can
  // match the new graph. These methods intentionally use cached subscribers:
  // the outer DML has already populated the registry, and a nested catalog
  // refresh would be re-entrant.
  void CascadeBatchAddEdges(
      PropertyGraph& graph, timestamp_t ts, label_t src_label,
      label_t dst_label, label_t edge_label,
      const std::vector<
          std::tuple<vid_t, vid_t, std::vector<Property>>>& edges,
      Allocator& alloc);
  void CascadeBatchDeleteEdges(
      PropertyGraph& graph, timestamp_t ts, label_t src_label,
      label_t dst_label, label_t edge_label,
      const std::vector<std::tuple<vid_t, vid_t>>& edges,
      Allocator* alloc = nullptr);

  void NotifyVertexMutation(PropertyGraph& graph, timestamp_t ts, label_t label,
                            vid_t vertex);
  bool OwnsVertexProperty(const Schema& schema, label_t label, int property);
  bool OwnsEdgeProperty(const Schema& schema, label_t source,
                        label_t destination, label_t edge, int property);
  bool empty() const;

  // Return a snapshot of maintainers watching a specific vertex property.
  // Thread-safe — returns by value.
  std::vector<std::shared_ptr<PatternMaterializedIndex>>
  GetVertexPropMaintainers(label_t vertex_label, int prop_id);

  // Cached lifecycle subscriptions; outer DML refreshes the catalog before
  // graph mutation, so cascading callbacks never re-enter catalog locks.
  std::vector<std::shared_ptr<PatternMaterializedIndex>>
  GetVertexMaintainersCached(label_t vertex_label);
  void OnVertexInserted(UpdateTransaction& txn, label_t label, vid_t vid);
  void OnVertexDeleted(UpdateTransaction& txn, label_t label, vid_t vid);
  void OnVertexInserted(PropertyGraph& graph, timestamp_t ts, label_t label,
                        vid_t vid, Allocator& alloc);
  void OnVertexDeleted(PropertyGraph& graph, timestamp_t ts, label_t label,
                       vid_t vid, Allocator* alloc = nullptr);

  // ---- Maintenance timing (benchmark instrumentation) ----
  // Accumulate nanoseconds spent inside one maintainer callback. target_name
  // identifies exactly one FILL materialization. Thread-safe.
  void RecordMaintenanceTime(const std::string& target_name, bool is_insert,
                             int64_t elapsed_ns);
  // Observations of paths actually consumed by NODE maintenance. Failed
  // pipeline attempts are not counted as successful execution paths.
  void RecordSourceScope(const std::string&, bool, size_t, size_t);
  void RecordSourceRefreshTime(const std::string&, int64_t);
  FillMaintenanceTiming GetMaintenanceTimingForIndex(const std::string&) const;
  void RecordMaintenancePath(const std::string& target_name,
                             const std::string& path, size_t input_edges,
                             size_t output_rows);

  // Snapshot of the accumulated timing counters.
  FillMaintenanceTiming GetMaintenanceTiming() const;

  // Snapshot timing for exactly one FILL materialization target.
  FillMaintenanceTiming GetMaintenanceTiming(
      const std::string& target_name) const;

  // Zero all timing counters. Call before a measured DML phase.
  void ResetMaintenanceTiming();

 private:
  friend class QueryProcessor;
  // The caller must hold the exclusive graph/schema lock and prove that the
  // completed statement was pure schema DDL, with no DML, FILL, procedure or
  // transaction effect. No other Rebuild/Refresh/reopen path opts into reuse.
  void RebuildPreservingCleanSourceInitialization();
  void rebuild(bool preserve_clean_source_initialization);
  void RecordSourceInitializationReuse(const std::string& target);
  void registerSubscriptionsLocked(
      const std::shared_ptr<PatternMaterializedIndex>& maintainer);
  void rebuildSubscriptionsLocked();

  // Must be called with mutex_ held. Runs populateFn_ if registry is empty.
  void ensurePopulatedLocked();

  mutable std::mutex mutex_;
  std::unordered_map<
      label_t,
      std::unordered_map<
          label_t, std::unordered_map<label_t, std::vector<std::shared_ptr<
                                                   PatternMaterializedIndex>>>>>
      edge_table_maintainers_;
  PopulateFn populateFn_;
  // Set to true once populateFn_ has been called (even if the result was
  // empty). Prevents checking the catalog on every DML when there are 0
  // entries.
  bool checked_{false};
  // Deferred refresh flag retained for the lazy-population implementation;
  // ScheduleRefresh() currently refreshes eagerly and clears this flag.
  bool needs_refresh_{false};

  // Track which catalog index entries have been registered, so incremental
  // RefreshNew() only creates maintainers for newly-added indexes.
  std::unordered_set<std::string> registered_index_names_;
  std::unordered_map<std::string, std::shared_ptr<PatternMaterializedIndex>>
      maintainers_by_index_;
  std::unordered_map<std::string, StagedFillDependencies> staged_dependencies_;
  std::atomic<bool> has_staged_dependencies_{false};

  // Vertex-property-based index: (vertex_label, property_id) → maintainers.
  // Populated during Register() when a maintainer reports watched vertex
  // properties via watched_vertex_properties() (Issue 8).
  std::unordered_map<
      label_t, std::unordered_map<
                   int, std::vector<std::shared_ptr<PatternMaterializedIndex>>>>
      vertex_prop_maintainers_;

  // Accumulated maintainer-callback wall time and dispatch counts, keyed by
  // insert vs delete (benchmark instrumentation; see FillMaintenanceTiming).
  std::atomic<int64_t> maint_insert_ns_{0};
  std::atomic<int64_t> maint_delete_ns_{0};
  std::atomic<int64_t> maint_insert_events_{0};
  std::atomic<int64_t> maint_delete_events_{0};
  mutable std::mutex timing_mutex_;
  std::unordered_map<std::string, FillMaintenanceTiming> timing_by_target_;
};

}  // namespace neug

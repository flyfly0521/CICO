// this class will inheritate from graph_planner.h. and it implement the
// function of compilePlan by using the GDatabase, just like the behaviors in
// GOpt.Query test
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>
#include "neug/compiler/common/case_insensitive_map.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/main/metadata_manager.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/compiler/planner/graph_planner.h"

namespace neug {

/**
 * @brief GOptPlanner is an implementation of IGraphPlanner that uses the GOpt
 * optimization framework to compile Cypher queries into executable physical
 * plans.
 * @note GOptPlanner is not thread-safe. Concurrent access to its methods
 * should be synchronized externally.
 * compilePlan: need read-lock.
 * update_meta/update_statistics: need write-lock.
 */
class GOptPlanner : public neug::IGraphPlanner {
 public:
  GOptPlanner() : IGraphPlanner() {
    database = std::make_unique<neug::main::MetadataManager>();
    ctx = std::make_unique<neug::main::ClientContext>(database.get());
    neug::main::MetadataRegistry::registerMetadata(database.get());
  }

  ~GOptPlanner() override {
    if (neug::main::MetadataRegistry::getMetadataQuietly() == database.get())
      neug::main::MetadataRegistry::clearMetadata();
  }

  // Must be set by the embedding runtime (NeugDB) so that compiler-side catalog
  // metadata (e.g. indexes) can be recovered across restarts.
  void set_catalog_directory(std::string directory) {
    database->setCatalogDirectory(std::move(directory));
  }

  main::MetadataManager* getMetadataManager() const { return database.get(); }
  main::ClientContext* getClientContext() const { return ctx.get(); }

  inline std::string type() const override { return "gopt"; }

  result<std::pair<physical::PhysicalPlan, std::string>> compilePlan(
      const std::string& query) override;

  result<physical::PhysicalPlan> compileMaterializedSource(
      const std::string& query, bool bagProof = false,
      const std::vector<std::string>& keyLabels = {}) override;

  struct SourceDependencies {
    common::PatternIR pattern;
    size_t positiveNodes = 0;
    size_t positiveRels = 0;
    bool singleQueryPart = false;
    // Proven independent carrier groups: a direct property SUM/COUNT, or a
    // recursive source whose DISTINCT boundaries all retain the carrier key.
    bool partitionedNodeAggregate = false;
    // A two-key aggregate whose complete groups are owned by the first key.
    bool partitionedPairAggregate = false;
    // Proven bag-preserving carrier MATCH, transparent WITH, then a plain
    // path COUNT. The runtime additionally checks topology and storage types.
    std::optional<size_t> carrierCountRel;
    // Aggregating before the carrier join does not multiply the value by
    // parallel carrier edges. Label-only positive dyadic SUMs use exact
    // integer units and the same path-delta machinery as COUNT.
    bool carrierAfterAggregate = false;
    struct LabelWeight {
      std::vector<common::table_id_t> labels;
      int64_t units;
    };
    std::vector<LabelWeight> carrierWeights;
    double carrierWeightUnit = 1.0;
    struct RecursiveDistinctCount { size_t node, relationship; uint32_t upper; };
    std::optional<RecursiveDistinctCount> recursiveDistinctCount;
    // Proven single directed hop, COUNT(mandatory endpoint), with one
    // endpoint timestamp property strictly before a constant timestamp.
    struct FilteredNodeCount {
      size_t node;
      std::string property;
      std::string before;
    };
    std::optional<FilteredNodeCount> filteredNodeCount;
    std::vector<size_t> keys;
    std::unordered_set<std::string> properties;
  };
  SourceDependencies materializedSourceDependencies(const std::string& query,
                                                    size_t keyCount);

  result<physical::PhysicalPlan> compileKeyedMaterializedSource(
      const std::string& query, bool bagProof,
      const std::vector<std::string>& keyLabels);

  result<AnchoredMatchPlan> compileAnchoredMatchPlan(
      const std::string& sourceQuery,
      const std::string& anchorRelUniqueName,
      uint64_t anchorCardinality = 1) override;

  void update_meta(const YAML::Node& schema_yaml_node) override;

  void update_statistics(const std::string& graph_statistic_json) override;

  AccessMode analyzeMode(const std::string& query) const override;

 private:
  result<physical::PhysicalPlan> compileSourceImpl(
      const std::string&, bool, const std::vector<std::string>&, bool);
  std::unique_ptr<neug::main::MetadataManager> database;
  std::unique_ptr<neug::main::ClientContext> ctx;

 private:
  // return string pattern of update operators
  const common::case_insensitve_set_t& getUpdateOpTokens() const;
  const common::case_insensitve_set_t& getSchemaOpTokens() const;
};

}  // namespace neug

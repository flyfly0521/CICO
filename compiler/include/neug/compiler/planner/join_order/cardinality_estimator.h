#pragma once

#include <optional>

#include "neug/compiler/binder/query/query_graph.h"
#include "neug/compiler/common/types/types.h"
#include "neug/compiler/planner/operator/extend/logical_recursive_extend.h"
#include "neug/compiler/planner/operator/logical_plan.h"
#include "neug/compiler/storage/stats/table_stats.h"

namespace neug {
namespace catalog {
class IndexCatalogEntry;
class TableCatalogEntry;
}  // namespace catalog
namespace main {
class ClientContext;
}  // namespace main

namespace transaction {
class Transaction;
}  // namespace transaction

namespace planner {

class LogicalAggregate;
class LogicalGetV;

// Lookup data belongs to one ordinary planning pass, after binding has finished
// registering its catalog entries. It never changes Catalog's dynamic APIs or
// keeps a cached pointer across a new getBestPlan() call. Standalone estimators
// remain uncached unless their caller explicitly starts a planning pass.
class NEUG_API PlanningCatalogLookup {
 public:
  explicit PlanningCatalogLookup(main::ClientContext* context = nullptr)
      : context{context} {}
  main::ClientContext* operator->() const { return context; }
  explicit operator bool() const { return context != nullptr; }
  main::ClientContext* getClientContext() const { return context; }
  void beginPlanning();
  catalog::TableCatalogEntry* getTableCatalogEntry(
      const transaction::Transaction* transaction, common::table_id_t table) const;
  // Like nodeTableStats and relTableRows, scalar observations are a snapshot
  // within one planning pass. Unknown observations and observed zero remain
  // distinct. Degree/property statistics and READY checks are not cached here.
  std::optional<uint64_t> getObservedTableCardinality(
      const transaction::Transaction* transaction, common::table_id_t table) const;
  std::vector<catalog::IndexCatalogEntry*> getExtendedIndexEntriesForTable(
      common::table_id_t table) const;
  // Outer plans feeding correlated subqueries in this planning pass. An
  // EXPRESSIONS_SCAN of a correlated vertex inherits its arrival distribution
  // from the outer operator that produced it.
  void addCorrelatedSource(std::shared_ptr<LogicalOperator> op) const {
    if (op) correlatedSources.push_back(std::move(op));
  }
  const std::vector<std::shared_ptr<LogicalOperator>>& getCorrelatedSources() const {
    return correlatedSources;
  }

 private:
  void checkTransaction(const transaction::Transaction* transaction) const;
  main::ClientContext* context;
  bool enabled = false;
  mutable const transaction::Transaction* transaction = nullptr;
  mutable common::transaction_t transactionID = 0;
  mutable common::transaction_t startTS = 0;
  mutable bool indexesLoaded = false;
  mutable std::unordered_map<common::table_id_t, catalog::TableCatalogEntry*> tables;
  mutable std::unordered_map<common::table_id_t, std::optional<uint64_t>> observedTableRows;
  mutable std::unordered_map<common::table_id_t,
      std::vector<catalog::IndexCatalogEntry*>> indexes;
  mutable std::vector<std::shared_ptr<LogicalOperator>> correlatedSources;
};

class CardinalityEstimator {
 public:
  CardinalityEstimator() : context{nullptr} {};
  explicit CardinalityEstimator(main::ClientContext* context)
      : context{context} {}
  DELETE_COPY_DEFAULT_MOVE(CardinalityEstimator);

  void addCorrelatedSource(std::shared_ptr<LogicalOperator> op) {
    context.addCorrelatedSource(std::move(op));
  }
  void beginPlanning() {
    context.beginPlanning();
    nodeTableStats.clear();
    relTableRows.clear();
    nodeIDName2dom.clear();
    perQueryGraphNodeIDName2dom.clear();
  }

  // TODO(Xiyang): revisit this init at some point. Maybe we should init while
  // enumerating.
  void initNodeIDDom(const transaction::Transaction* transaction,
                     const binder::QueryGraph& queryGraph);
  NEUG_API void addNodeIDDomAndStats(
      const transaction::Transaction* transaction,
      const binder::Expression& nodeID,
      const std::vector<common::table_id_t>& tableIDs);
  void addPerQueryGraphNodeIDDom(const binder::Expression& nodeID,
                                 cardinality_t numNodes);
  void clearPerQueryGraphStats();

  cardinality_t estimateScanNode(const LogicalOperator& op) const;
  cardinality_t estimateExtend(const LogicalExtend& op, bool applyPredicate = true) const;
  cardinality_t estimateRecursiveExtend(const LogicalRecursiveExtend& op) const;
  cardinality_t estimateHashJoin(
      const std::vector<binder::expression_pair>& joinConditions,
      const LogicalOperator& probeOp, const LogicalOperator& buildOp) const;
  cardinality_t estimateCrossProduct(const LogicalOperator& probeOp,
                                     const LogicalOperator& buildOp) const;
  cardinality_t estimateIntersect(
      const binder::expression_vector& joinNodeIDs,
      const LogicalOperator& probeOp,
      const std::vector<LogicalOperator*>& buildOps) const;
  cardinality_t estimateFlatten(const LogicalOperator& childOp,
                                f_group_pos groupPosToFlatten) const;
  cardinality_t estimateFilter(const LogicalOperator& childOp,
                               const binder::Expression& predicate) const;
  cardinality_t estimateAggregate(const LogicalAggregate& op) const;
  cardinality_t estimateGetV(
      const planner::LogicalRecursiveExtend& extend) const;
  cardinality_t estimateGetV(const planner::LogicalExtend& extend) const;
  cardinality_t estimateGetV(const LogicalGetV& getV) const;

  double getExtensionRate(const binder::RelExpression& rel,
                          const binder::NodeExpression& boundNode,
                          const transaction::Transaction* transaction) const;
  double getExtensionRate(const binder::RelExpression& rel,
                          const common::table_id_vector_t& tableIDs,
                          const binder::NodeExpression& boundNode,
                          const transaction::Transaction* transaction) const;

  cardinality_t multiply(double extensionRate, cardinality_t card) const;

  cardinality_t getNumNodes(
      const transaction::Transaction* transaction,
      const std::vector<common::table_id_t>& tableIDs) const;

 private:
  __uint128_t getDirectionalRelationshipRows(
      const common::table_id_vector_t& relTables,
      const common::table_id_vector_t& boundTables,
      const common::table_id_vector_t& neighborTables, common::ExtendDirection direction,
      const transaction::Transaction* transaction) const;
  double getDirectionalExtensionRate(
      const binder::RelExpression& rel, const common::table_id_vector_t& relTables,
      const binder::NodeExpression& boundNode,
      const common::table_id_vector_t& neighborTables, common::ExtendDirection direction,
      const transaction::Transaction* transaction) const;
  cardinality_t getNodeIDDom(const std::string& nodeIDName) const;
  cardinality_t getNumRels(
      const transaction::Transaction* transaction,
      const std::vector<common::table_id_t>& tableIDs) const;
  struct RelationshipRows {
    cardinality_t rows;
    bool observed;
    common::table_id_t sourceTable;
    common::table_id_t destinationTable;
  };
  const RelationshipRows& getRelationshipRows(
      const transaction::Transaction* transaction, common::table_id_t tableID) const;

 private:
  PlanningCatalogLookup context;
  std::unordered_map<common::table_id_t, storage::TableStats> nodeTableStats;
  // Like node statistics, relationship row counts are a snapshot local to
  // this planner. Join enumeration asks for the same table thousands of
  // times; repeatedly resolving it through the catalog adds no information.
  // A new preparation constructs a new estimator and observes fresh counts.
  mutable std::unordered_map<common::table_id_t, RelationshipRows> relTableRows;
  // The domain of nodeID is defined as the number of unique value of nodeID,
  // i.e. num nodes.
  std::unordered_map<std::string, cardinality_t> nodeIDName2dom;
  // additional per-query-graph statistics for nodeIDName2dom (e.g. from
  // correlated expressions) when estimating the cardinality of a node we will
  // use the minimum of the estimations from nodeIDName2dom and
  // perQueryGraphNodeIDName2dom when we start enumerating a new query graph we
  // will reset this map
  std::unordered_map<std::string, cardinality_t> perQueryGraphNodeIDName2dom;
};

}  // namespace planner
}  // namespace neug

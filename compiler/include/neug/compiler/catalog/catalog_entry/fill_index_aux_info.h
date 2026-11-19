#pragma once
#include <atomic>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/common/serializer/deserializer.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/utils/api.h"

// Proto package `common` lives in the GLOBAL ::common namespace (not
// neug::common). Forward declare to keep the heavy generated header out of
// this widely-included header; serialize/deserialize/copy handle the
// complete type in the .cpp.
namespace common {
class Expression;
}  // namespace common

namespace neug {

namespace main {
class ClientContext;
}  // namespace main
namespace binder {
class BoundBaseScanSource;
class Expression;
class RewriterV4;
class NormalizedSingleQuery;
}  // namespace binder
}  // namespace neug
namespace neug {
namespace catalog {

// The catalog records only where the pattern result is materialized.  Value 2
// is retained solely for deserializing catalogs written before recursive
// relationship indexes were represented as ordinary REL materializations.
enum class FillEntryType : uint8_t {
  NODE = 0,
  REL = 1,
  LEGACY_VARIADIC_REL = 2,
  VARIADIC_REL = LEGACY_VARIADIC_REL
};
enum class IndexColumnType : uint8_t {
  NODE_PROPERTY = 0,
  NODE_AGGREGATE = 1,
  REL_PROPERTY = 2,
  REL_AGGREGATE = 3,
  REL_COMPUATION_WITH_AGGREGATION = 4,
  // A typed output evaluated by the persisted source query. It has no
  // additive aggregate or property-substitution rewrite contract.
  SOURCE_QUERY_VALUE = 5
};

// Arithmetic contracts are versioned separately from expression support.
// A readable legacy definition never acquires a new rewrite contract merely
// because its stored field happens to be an integer.
enum class FillAggregateNumericContract : uint8_t {
  LEGACY_UNSPECIFIED = 0,
  SIGNED_INTEGER_CHECKED = 1,
  SIGNED_INTEGER_WRAP = 2,
  SIGNED_INTEGER_DISTINCT_COUNT = 3,
  SOURCE_QUERY_RECOMPUTE = 4,
};

enum class FillAggregateForm : uint8_t {
  LEGACY_COMPUTED = 0,
  CANONICAL_BASE = 1,
};

// A semijoin/antijoin condition over one fixed-hop edge whose endpoints are
// both bound by the outer PatternIR. These edges constrain an outer tuple;
// their witness multiplicity must never multiply its aggregate contribution.
struct NEUG_API FillExistsPredicate {
  common::idx_t srcNodePos{0};
  common::idx_t dstNodePos{0};
  bool negated{false};
  bool directed{true};
  std::vector<common::table_id_t> srcTableIDs;
  std::vector<common::table_id_t> dstTableIDs;
  std::vector<common::table_id_t> relTableIDs;

  void serialize(common::Serializer& writer) const;
  static FillExistsPredicate deserialize(common::Deserializer& reader);
};

// Shared by FILL registration and automatic matching. Unsupported graph
// predicates throw; non-EXISTS scalar terms return nullopt. Node positions
// refer to the supplied outer pattern, never to printable variable names.
NEUG_API std::optional<FillExistsPredicate> extractFillExistsPredicate(
    const binder::Expression& expression, const common::PatternIR& pattern);
NEUG_API std::string fillExistsPredicateKey(FillExistsPredicate predicate);

struct NEUG_API IndexColnumInfo {
  IndexColumnType columnType;
  std::string nodeUnique;
  std::string propertyName;
  IndexColnumInfo() = default;
  IndexColnumInfo(IndexColumnType columnType, std::string nodeUnique,
                  std::string propertyName)
      : columnType{columnType},
        nodeUnique{std::move(nodeUnique)},
        propertyName{std::move(propertyName)} {}
  virtual void serialize(neug::common::Serializer& writer) const;
  // hasAstExpr: whether the parent FillIndexAuxInfo header byte carries
  // INDEX_COL_HAS_AST_FLAG; when false (legacy buffers) the serialized AST
  // block must be skipped because old per-column records do not contain it.
  static std::shared_ptr<IndexColnumInfo> deserialize(
      std::unique_ptr<common::Deserializer>& deserializer,
      bool hasAstExpr = false, bool hasFinalizedAggregate = false);
  virtual IndexColnumInfo* copy() {
    return new IndexColnumInfo(columnType, nodeUnique, propertyName);
  }
};

// Node aggregation index column: stores the aggregate function name, a
// name template for matching, and the unique names of nodes referenced by
// the aggregate (used during rewrite to verify the matched subgraph).
struct NEUG_API NodeAggregationIndexColnumInfo : public IndexColnumInfo {
  std::string aggregateFuncName;
  std::string nameTemplate;
  // Unique names of the pattern nodes that the aggregate expression
  // references.  Compared against the index PatternIR's node uniqNames
  // during rewrite (via matchedIdx mapping) instead of fragile per-pattern
  // position indices.
  std::vector<std::string> nodeUniqueNames;
  // Serialized aggregate-argument AST (::common::Expression). Variable tags
  // are pattern-node positions in the stored PatternIR. Consumed by
  //   - the rewrite matcher (structural alpha-equivalence), and
  //   - PatternMaterializedIndex (parse_expression + eval per
  //     derivation).
  // nullptr for legacy catalogs: consumers fall back to nameTemplate paths.
  std::shared_ptr<::common::Expression> aggregateArgExpr;
  NodeAggregationIndexColnumInfo() = default;
  NodeAggregationIndexColnumInfo(IndexColumnType columnType,
                                 std::string nodeUnique,
                                 std::string propertyName, std::string aggName,
                                 std::vector<std::string> nodeUniqNames,
                                 std::string namepTemplate)
      : IndexColnumInfo{columnType, std::move(nodeUnique),
                        std::move(propertyName)},
        aggregateFuncName{aggName},
        nameTemplate{std::move(namepTemplate)},
        nodeUniqueNames{std::move(nodeUniqNames)} {}
  void serialize(neug::common::Serializer& writer) const override;
  IndexColnumInfo* copy() override {
    auto* result = new NodeAggregationIndexColnumInfo(
        columnType, nodeUnique, propertyName, aggregateFuncName,
        nodeUniqueNames, nameTemplate);
    result->aggregateArgExpr = aggregateArgExpr;
    return result;
  }
};

// Rel aggregation index column: stores a name template and the unique names
// of nodes referenced by the aggregate expression.
struct NEUG_API RelIndexColnumInfo : public IndexColnumInfo {
  std::vector<std::string> nodeUniqueNames;
  std::string nameTemplate;
  // Aggregate function name ("COUNT"/"SUM"). Historically derived by
  // string-searching nameTemplate; stored explicitly once the AST is
  // serialized (empty for legacy catalogs → keep the old derivation).
  std::string aggregateFuncName;
  // Serialized aggregate-argument AST; same contract as
  // NodeAggregationIndexColnumInfo::aggregateArgExpr.
  std::shared_ptr<::common::Expression> aggregateArgExpr;
  RelIndexColnumInfo() = default;
  RelIndexColnumInfo(IndexColumnType columnType, std::string nodeUnique,
                     std::string propertyName,
                     std::vector<std::string> nodeUniqNames,
                     std::string nameTemeplate)
      : IndexColnumInfo{columnType, std::move(nodeUnique),
                        std::move(propertyName)},
        nodeUniqueNames{std::move(nodeUniqNames)},
        nameTemplate{std::move(nameTemeplate)} {}
  void serialize(neug::common::Serializer& writer) const override;
  IndexColnumInfo* copy() override {
    auto* result = new RelIndexColnumInfo(columnType, nodeUnique, propertyName,
                                          nodeUniqueNames, nameTemplate);
    result->aggregateFuncName = aggregateFuncName;
    result->aggregateArgExpr = aggregateArgExpr;
    return result;
  }
};

// Rel computation with aggregation column: like RelIndexColnumInfo but for
// scalar-function expressions that embed aggregates.
struct NEUG_API RelComputationWithAggreColumnInfo : public IndexColnumInfo {
  std::vector<std::string> nodeUniqueNames;
  std::string nameTemplate;
  // Serialized scalar-expression AST; same contract as
  // NodeAggregationIndexColnumInfo::aggregateArgExpr. Conversion throws at
  // FILL creation when the expression embeds aggregate functions (not
  // representable per-tuple), so non-null implies an aggregate-free tree.
  std::shared_ptr<::common::Expression> astExpr;
  // Finalized aggregate support. The additive aggregate is persisted in
  // stateTargetColumn; finalizerExpr references that state through the
  // reserved kFillAggregateStateTag record column.
  std::string aggregateFuncName;
  std::string stateTargetColumn;
  std::shared_ptr<::common::Expression> aggregateArgExpr;
  std::shared_ptr<::common::Expression> finalizerExpr;
  RelComputationWithAggreColumnInfo() = default;
  RelComputationWithAggreColumnInfo(IndexColumnType columnType,
                                    std::string nodeUnique,
                                    std::string propertyName,
                                    std::vector<std::string> nodeUniqNames,
                                    std::string nameTemeplate)
      : IndexColnumInfo{columnType, nodeUnique, propertyName},
        nodeUniqueNames{std::move(nodeUniqNames)},
        nameTemplate{std::move(nameTemeplate)} {}
  void serialize(neug::common::Serializer& writer) const override;
  IndexColnumInfo* copy() override {
    auto* result = new RelComputationWithAggreColumnInfo(
        columnType, nodeUnique, propertyName, nodeUniqueNames, nameTemplate);
    result->astExpr = astExpr;
    result->aggregateFuncName = aggregateFuncName;
    result->stateTargetColumn = stateTargetColumn;
    result->aggregateArgExpr = aggregateArgExpr;
    result->finalizerExpr = finalizerExpr;
    return result;
  }
};

// Stable origin of one local graph-pattern element in the merged PatternIR.
// The tuple of query-part/clause/graph/local positions survives re-binding of
// the persisted source query even when binder-generated unique names change.
struct NEUG_API FillPatternElementOrigin {
  uint32_t queryPartIdx{UINT32_MAX};
  uint32_t readingClauseIdx{UINT32_MAX};
  uint32_t queryGraphIdx{UINT32_MAX};
  uint32_t localElementIdx{UINT32_MAX};
  uint32_t mergedElementIdx{UINT32_MAX};
  std::string originalUniqueName;

  void serialize(common::Serializer& writer) const;
  static FillPatternElementOrigin deserialize(common::Deserializer& reader);
};

// FillIndexAuxInfo stores a single merged PatternIR that captures the
// complete graph pattern across all QueryParts. Rewriters match this full
// pattern against the current query graph after each mutation.
class NEUG_API FillIndexAuxInfo final : public IndexAuxInfo {
 public:
  friend class binder::RewriterV4;
  FillIndexAuxInfo() = default;
  FillIndexAuxInfo(
      FillEntryType fillType, std::string tableName, std::string tableUniqName,
      std::string tableVarName, std::vector<std::string> targetColumns,
      std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos,
      common::PatternIR patternIR, std::string sourceQuery = {},
      std::vector<std::string> predicateTemplates = {})
      : fillType{fillType},
        tableName{std::move(tableName)},
        tableUniqName{std::move(tableUniqName)},
        tableVariableName{std::move(tableVarName)},
        targetColumns{std::move(targetColumns)},
        indexColumnInfos{std::move(indexColumnInfos)},
        patternIR{std::move(patternIR)},
        sourceQuery{std::move(sourceQuery)} {
    setPredicateTemplates(std::move(predicateTemplates));
  }

  std::shared_ptr<common::BufferedSerializer> serialize() const override;
  // Empty for older catalogs that predate independent group-existence state.
  // The state is encoded as a normal hidden aggregate column, so the catalog
  // wire format and existing copy/serialization paths remain compatible.
  std::string getRowCountColumn() const;
  bool isRewriteReady() const { return rewriteReady->load(); }
  void setRewriteReady(bool ready) { rewriteReady->store(ready); }
  std::shared_ptr<std::atomic<bool>> getRewriteState() const { return rewriteReady; }
  void shareRewriteState(const FillIndexAuxInfo& other) { rewriteReady = other.rewriteReady; }
  bool requiresSourceRecompute() const {
    return aggregateNumericContract == FillAggregateNumericContract::SOURCE_QUERY_RECOMPUTE;
  }
  FillAggregateNumericContract getAggregateNumericContract() const {
    return aggregateNumericContract;
  }
  void setAggregateNumericContract(FillAggregateNumericContract value) {
    aggregateNumericContract = value;
  }
  FillAggregateForm getAggregateForm() const { return aggregateForm; }
  void setAggregateForm(FillAggregateForm value) { aggregateForm = value; }
  static FillIndexAuxInfo* deserialize(
      std::unique_ptr<common::BufferReader> reader);
  std::unique_ptr<IndexAuxInfo> copy() override;
  std::string toCypher(const IndexCatalogEntry& indexEntry,
                       const ToCypherInfo& info) const override;

  FillEntryType getFillType() const {
    return fillType == FillEntryType::LEGACY_VARIADIC_REL ? FillEntryType::REL
                                                          : fillType;
  }
  // Strategy inference, not a third materialization kind. A variadic shortcut
  // is a property-free REL materialization over one recursive PatternIR edge.
  bool isVariadicRelIndex() const;
  const std::vector<std::string>& getTargetColumns() const {
    return targetColumns;
  }
  const std::vector<std::shared_ptr<IndexColnumInfo>>& getIndexColumnInfos()
      const {
    return indexColumnInfos;
  }
  auto getTableName() const { return tableName; }
  const auto& getPatternIR() const { return patternIR; }
  auto& getPatternIR() { return patternIR; }
  const std::string& getTableUniqName() const { return tableUniqName; }
  const std::string& getTableVariableName() const { return tableVariableName; }
  const std::string& getSourceQuery() const { return sourceQuery; }
  const std::vector<std::string>& getPredicateTemplates() const {
    return predicateTemplates;
  }
  void setPredicateTemplates(std::vector<std::string> values);
  void setSourceQuery(std::string value) { sourceQuery = std::move(value); }
  bool usesWorkloadCompatibility() const { return workloadCompatibility; }
  void setWorkloadCompatibility(bool value) { workloadCompatibility = value; }
  // A complete, ordered computation pipeline. Its merged graph is only a
  // matching aid; it must never enter flat-pattern rewriting or maintenance.
  // The marker is persisted by the existing predicate-template wire format.
  bool isStagedPipeline() const {
    for (const auto& value : predicateTemplates)
      if (value == "@staged-count-sum-fill-v1") return true;
    return false;
  }
  std::vector<std::pair<common::table_id_t, std::string>>
  getStagedSourceProperties() const;
  const std::vector<FillPatternElementOrigin>& getNodeOrigins() const {
    return nodeOrigins;
  }
  const std::vector<FillPatternElementOrigin>& getRelOrigins() const {
    return relOrigins;
  }
  void setPatternElementOrigins(
      std::vector<FillPatternElementOrigin> nodeValues,
      std::vector<FillPatternElementOrigin> relValues) {
    nodeOrigins = std::move(nodeValues);
    relOrigins = std::move(relValues);
  }
  const std::shared_ptr<::common::Expression>& getPredicateExpr() const {
    return predicateExpr;
  }
  void setPredicateExpr(std::shared_ptr<::common::Expression> value) {
    predicateExpr = std::move(value);
  }
  const std::vector<std::shared_ptr<::common::Expression>>&
  getPredicateConjuncts() const {
    return predicateConjuncts;
  }
  void setPredicateConjuncts(
      std::vector<std::shared_ptr<::common::Expression>> values) {
    predicateConjuncts = std::move(values);
  }
  const std::vector<FillExistsPredicate>& getExistsPredicates() const {
    return existsPredicates;
  }
  void setExistsPredicates(std::vector<FillExistsPredicate> values) {
    existsPredicates = std::move(values);
  }
  const std::vector<std::string>& getPresenceColumns() const {
    return presenceColumns;
  }
  void setPresenceColumns(std::vector<std::string> values) {
    presenceColumns = std::move(values);
  }

  void setSrcTableId(uint64_t id) { srcTableId = id; }
  void setDstTableId(uint64_t id) { dstTableId = id; }
  uint32_t getSrcTableId() const { return srcTableId; }
  uint32_t getDstTableId() const { return dstTableId; }
  void setEndpointUniqueNames(std::string src, std::string dst) {
    srcNodeUniqName = std::move(src);
    dstNodeUniqName = std::move(dst);
  }
  const std::string& getSrcNodeUniqName() const { return srcNodeUniqName; }
  const std::string& getDstNodeUniqName() const { return dstNodeUniqName; }
  const std::string& getSrcNodeUniqueName() const { return srcNodeUniqName; }
  const std::string& getDstNodeUniqueName() const { return dstNodeUniqName; }
  void setEndpointNodeUniqueNames(std::string src, std::string dst) {
    setEndpointUniqueNames(std::move(src), std::move(dst));
  }
  void setRecursiveRelBounds(uint32_t lowerBound, uint32_t upperBound) {
    recursiveRelLowerBound = lowerBound;
    recursiveRelUpperBound = upperBound;
  }
  uint32_t getRecursiveRelLowerBound() const { return recursiveRelLowerBound; }
  uint32_t getRecursiveRelUpperBound() const { return recursiveRelUpperBound; }
  void setFillUndirectedRel(bool value) { fillUndirectedRel = value; }
  bool isFillUndirectedRel() const { return fillUndirectedRel; }

 private:
  FillEntryType fillType;
  bool fillUndirectedRel{
      false};  // only used for rel index, if true, the src and dst of
               // the rel are the same, and we need to fill both directions.
  uint32_t srcTableId{UINT32_MAX}, dstTableId{UINT32_MAX};
  // For derived REL FILLs, the materialized edge is not present in PatternIR.
  // These optional uniqNames identify the RETURN source/destination nodes.
  std::string srcNodeUniqName;
  std::string dstNodeUniqName;
  // Used by recursive REL strategies. Keep this separate from src/dst table
  // ids so target positions and recursive bounds remain independent.
  uint32_t recursiveRelLowerBound{UINT32_MAX};
  uint32_t recursiveRelUpperBound{UINT32_MAX};
  // Table metadata for the node/rel that stores the filled index.
  std::string tableName;
  std::string tableUniqName;
  std::string tableVariableName;
  // Column names on tableName that hold the filled values.
  std::vector<std::string> targetColumns;
  // Per-column metadata: what each column represents (property, aggregate, …).
  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;
  // Single merged PatternIR spanning all QueryParts — used directly for
  // full-graph matching during rewrite.
  neug::common::PatternIR patternIR;
  // Serialized MATCH-WHERE predicate. Variable tags are positions in
  // patternIR.nodesInfo, identical to aggregate argument expressions. Null
  // means that every structurally complete tuple is eligible.
  std::shared_ptr<::common::Expression> predicateExpr;
  // Individually serialized top-level AND terms. The combined expression
  // above remains the runtime-maintenance predicate; this list lets the
  // rewriter consume only the terms covered by an index and preserve any
  // residual query predicates.
  std::vector<std::shared_ptr<::common::Expression>> predicateConjuncts;
  // Additional graph predicates, combined with predicateExpr by AND. Kept
  // separate from the scalar AST so edge-table dependencies remain explicit.
  std::vector<FillExistsPredicate> existsPredicates;
  // BOOL outputs that represent whether the group has any source tuples.
  std::vector<std::string> presenceColumns;
  // Exact query inside FILL's FROM (...), captured from the original ANTLR
  // character interval. Empty only for legacy catalog entries.
  std::string sourceQuery;
  // Workload rewrites retain predicate templates and DISTINCT boundaries.
  std::vector<std::string> predicateTemplates;
  bool workloadCompatibility{false};
  std::shared_ptr<std::atomic<bool>> rewriteReady = std::make_shared<std::atomic<bool>>(true);
  // Older metadata does not grant new algebraic-rewrite capabilities merely
  // because it can still be read. Persisted in source metadata version 3.
  FillAggregateNumericContract aggregateNumericContract{
      FillAggregateNumericContract::LEGACY_UNSPECIFIED};
  FillAggregateForm aggregateForm{FillAggregateForm::LEGACY_COMPUTED};
  // Every local node/relationship occurrence and its position in patternIR.
  // Relationship origins distinguish repeated uses of the same type and are
  // the stable anchor keys for future event-driven maintenance planning.
  std::vector<FillPatternElementOrigin> nodeOrigins;
  std::vector<FillPatternElementOrigin> relOrigins;
};

std::string createNodeIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::TableCatalogEntry* tableEntry,
    neug::main::ClientContext* ctx, std::vector<std::string> columnNames,
    const std::string& sourceQuery = {});
class RelTableCatalogEntry;
std::string createRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    std::vector<std::string> fillColumnNames,
    neug::catalog::RelTableCatalogEntry* relGroupEntry,
    main::ClientContext* ctx, const std::string& sourceQuery = {});
std::string createVaradicRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::RelTableCatalogEntry* tableEntry, main::ClientContext* ctx,
    std::string indexTableName = {}, const std::string& sourceQuery = {});
// User writes must not invalidate a still-registered materialization. Internal
// maintenance writes bypass query binding and therefore do not call these.
void validateFillUserPropertyWrite(main::ClientContext* context,
                                   common::table_id_t tableID,
                                   const std::string& propertyName);
void validateFillUserRelationshipWrite(main::ClientContext* context,
                                       common::table_id_t tableID);
// Conservative for required MATCH bindings; callers must separately reject
// OPTIONAL bindings and prove that the covered pattern is actually present.
NEUG_API bool isFillExpressionNonNull(const binder::Expression& expression);
NEUG_API bool isFillSourceExpressionNonNull(
    const binder::Expression& expression,
    const binder::NormalizedSingleQuery& source);
// Determines whether a public aggregate is a base aggregate, without
// rewriting its value. Aliases retain their expression tree and provenance.
NEUG_API bool isCanonicalFillAggregate(const binder::Expression& expression);
// Enforce the strict aggregate profile before choosing compatibility metadata.
NEUG_API void validateCanonicalFillProjection(const binder::Expression& expression);
// A nonnegative CASE whose leaves are exactly 0, 0.5 or 1 admits an exact
// binary-scaled state. General floating SUM remains unsupported.
NEUG_API bool isExactHalfIntegerFillExpression(const binder::Expression& expression);
NEUG_API bool isMaintainableFillFinalizer(const binder::Expression& expression);
// Fold an identity-grouped aggregate's pure output/label-dispatch wrapper.
// This preserves the original source text and sends the proven bound source
// through the ordinary canonical registration and maintenance checks.
bool normalizeFillAggregateProjection(
    std::unique_ptr<binder::BoundBaseScanSource>& source,
    TableCatalogEntry* table, main::ClientContext* context);
void validateWorkloadFillStateOwnership(const FillIndexAuxInfo& info,
    common::table_id_t targetTable, main::ClientContext* context);
bool requiresSourceRecompute(const binder::BoundBaseScanSource&, const TableCatalogEntry&);
bool fillSourceReadsMaterializedProperty(const binder::BoundBaseScanSource&, main::ClientContext*);
bool requiresWorkloadFillCompatibility(
    const std::unique_ptr<binder::BoundBaseScanSource>& source,
    const TableCatalogEntry* table, const std::vector<std::string>& columns);
std::vector<std::string> collectWorkloadFillPredicates(
    const binder::NormalizedSingleQuery& query, const common::PatternIR& pattern);
// Shared by canonical FILLs and proven bag-preserving workload flattening.
// Rejects graph predicates without a supported incremental dependency model.
void setFillPredicateMetadata(FillIndexAuxInfo& info,
                              const binder::NormalizedSingleQuery& query);
void validateFillStateOwnership(
    const FillIndexAuxInfo& info, common::table_id_t targetTable,
    Catalog* catalog, const transaction::Transaction* transaction);
// Staged source guards have no transitive maintenance closure. Reject a
// derived carrier dependency in either registration order before FILL writes.
void validateStagedFillDependencies(
    const FillIndexAuxInfo& info, common::table_id_t targetTable,
    Catalog* catalog, const transaction::Transaction* transaction);
bool normalizeFillSingleEdgeDistinctCount(
    std::unique_ptr<binder::BoundBaseScanSource>& source, TableCatalogEntry* table,
    const std::shared_ptr<binder::Expression>& rowCount);
std::optional<std::vector<std::pair<std::string, common::LogicalType>>>
tryCreateFlattenedCountIndexEntry(
    std::unique_ptr<binder::BoundBaseScanSource>& source, TableCatalogEntry* table,
    main::ClientContext* context, std::vector<std::string> columns,
    const std::string& sourceQuery,
    std::shared_ptr<binder::Expression> presenceRowCount = nullptr);
std::string createWorkloadNodeIndexEntry(
    std::unique_ptr<binder::BoundBaseScanSource>& source, TableCatalogEntry* table,
    main::ClientContext* context, std::vector<std::string> columns,
    const std::string& sourceQuery);
std::string createWorkloadRelIndexEntry(
    std::unique_ptr<binder::BoundBaseScanSource>& source,
    std::vector<std::string> columns, RelTableCatalogEntry* table,
    main::ClientContext* context, const std::string& sourceQuery,
    std::string indexTableName = {});
std::string createWorkloadVaradicRelIndexEntry(
    std::unique_ptr<binder::BoundBaseScanSource>& source,
    RelTableCatalogEntry* table, main::ClientContext* context,
    std::string indexTableName, const std::string& sourceQuery);
}  // namespace catalog
}  // namespace neug

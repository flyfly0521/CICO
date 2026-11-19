#pragma once

#include <sys/types.h>
#include "neug/compiler/binder/bound_statement_visitor.h"
#include "neug/compiler/binder/query/normalized_query_part.h"
#include "neug/compiler/common/pattern_ir.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace neug::common;
namespace neug {
namespace main {
class ClientContext;
}
namespace common {
class BufferWriter;
class BufferReader;
class NodeInfo;

}  // namespace common
namespace catalog {
class IndexCatalogEntry;
class FillIndexAuxInfo;
class TableCatalogEntry;
}  // namespace catalog
namespace binder {
class Binder;
class BoundMatchClause;
class QueryGraph;
class Expression;
class PropertyExpression;
class CaseExpression;
class RelExpression;
class NodeExpression;


// RewriterV4: Full-graph matching rewriter.
//
// The FILL index stores a single merged PatternIR (spanning all QueryParts).
// Rewrite matching uses a bounded fixpoint: build a merged query IR from all
// QueryParts, apply one matching index, then rebuild the merged IR before
// looking for another match. Matched
// node/edge indices are propagated to per-QueryPart IRs for graph and
// expression rewriting.  No gate-check round-trip or per-QueryPart pattern
// selection needed.
class RewriterV4 final : public BoundStatementVisitor {
 public:
  explicit RewriterV4(main::ClientContext* clientContext,
                      neug::binder::Binder* binder = nullptr)
      : clientContext(clientContext), binder(binder) {}

 private:
  void visitRegularQueryUnsafe(BoundStatement& statement) override;
  bool completeAggregateInputIsClosed = false;
  size_t committedMatchRewrites = 0;
  bool completePairDistinctCountCandidate = false;
  bool aggregateEndpointPresenceOnlyCandidate = false;
  // Scoped to one regular-query visit; never reuse a source proof across
  // catalog updates, contexts, or differently typed FILL entries.
  std::unordered_map<const catalog::FillIndexAuxInfo*, bool> nodePresenceSetSourceProofs;
  bool provesNodePresenceSetSourceCached(const catalog::FillIndexAuxInfo& info);
  std::unordered_map<const catalog::FillIndexAuxInfo*, bool> endpointSetSourceProofs;
  bool provesSingleStageEndpointSetSourceCached(const catalog::FillIndexAuxInfo& info);
  bool tryRewriteCompleteAggregate(NormalizedSingleQuery& query);
  struct PropertyRewriteTarget {
    std::string newVarUniqueName;
    std::string newVarRawName;
    std::string newPropertyName;
    std::string forwardPropertyName;
    std::string reversePropertyName;
    std::shared_ptr<NodeExpression> targetNode;
    std::string directionReferenceNodeUniqueName;
    std::string directionReferenceNodeRawName;
    std::shared_ptr<RelExpression> directionTargetRel;
    bool directionAwareSameLabelRel = false;
    bool completePairDistinctCount = false;
    // A public COUNT(*) supplies removed-row multiplicity for SUM/COUNT, or
    // source presence for MIN/MAX/COUNT(DISTINCT). Preserve the retained operand.
    std::shared_ptr<Expression> countAggregateOperand;
  };

  struct FillTableContext {
    catalog::TableCatalogEntry* entry;
    table_id_t tableID;
  };

  struct IndexRewriteCandidate {
    catalog::IndexCatalogEntry* entry;
    catalog::FillIndexAuxInfo* info;
    common::PatternIR indexIR;  // owned copy of the matched index pattern
    QueryGraph* queryGraph;
    bool weightedRecursiveRelCoverageOnly = false;
    bool splitRelAnchor = false;
  };

  struct AggregateIndexGroup {
    common::PatternIR pattern;
    std::vector<FillTableContext> fillTables;
    bool allAggregateEndpointSetSources = false;
  };

  struct RetainedPatternVariables {
    std::unordered_set<std::string> nodeUniqueNames;
    std::unordered_set<std::string> relUniqueNames;
  };

  std::optional<FillTableContext> getFillTableContext(
      const catalog::FillIndexAuxInfo& indexInfo) const;
  std::shared_ptr<PropertyExpression> makeFillPropertyExpression(
      const FillTableContext& fillTable,
      const PropertyRewriteTarget& target) const;

  // Build a single merged PatternIR from all QueryParts' MATCH-clause graphs.
  common::PatternIR buildMergedQueryIR(
      const NormalizedSingleQuery& query) const;

  // Propagate matchedIdx from a full-graph merged match result to a
  // per-QueryPart PatternIR using uniqName as the bridging key.
  void propagateMatchedIndices(const common::PatternIR& mergedQueryIR,
                               common::PatternIR& localIR) const;

  // Create a rewrite candidate after the merged graph gate has matched. Graph
  // mutation is allowed only when the complete index pattern can be rematched
  // inside this MATCH clause; merged positions are not local graph positions.
  std::optional<IndexRewriteCandidate> makeRewriteCandidate(
      BoundMatchClause& matchClause, PatternIR& nodesIR,
      catalog::IndexCatalogEntry* entry,
      const common::PatternIR& preMatchedIndexIR);
  bool matchesVariadicRelationshipIndex(const IndexRewriteCandidate& candidate,
                                        PatternIR& nodesIR) const;
  bool matchesWeightedRecursiveRelPropertyIndex(
      const IndexRewriteCandidate& candidate) const;
  RetainedPatternVariables collectRetainedPatternVariables(
      BoundMatchClause& matchClause,
      const catalog::FillIndexAuxInfo& indexInfo,
      const common::PatternIR& matchedIndexIR) const;
  bool validateRewriteCandidate(BoundMatchClause& matchClause,
                                NormalizedQueryPart& queryPart,
                                PatternIR& nodesIR,
                                IndexRewriteCandidate& candidate);

  void rewritePropertyExpression(catalog::IndexCatalogEntry* entry);
  void applyIndexExpressionRewrites(catalog::IndexCatalogEntry* entry,
                                    BoundMatchClause& matchClause,
                                    NormalizedQueryPart& queryPart);
  void rewriteReferencesFromPreviousQueryPart(NormalizedQueryPart& queryPart);

  bool validateProjectionExpressionForIndexRewrite(
      const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
      PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
      common::PatternIR& indexIR,
      const std::unordered_set<std::string>* retainedNodeUniqueNames = nullptr,
      const std::unordered_set<std::string>* retainedRelUniqueNames = nullptr);
  bool registerAggregateRewriteTargets(
      const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
      PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
      common::PatternIR& indexIR);
  bool registerCountBackedAggregateTarget(
      const std::shared_ptr<Expression>& expr, BoundMatchClause& matchClause,
      PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
      common::PatternIR& indexIR,
      const std::unordered_set<std::string>* retainedNodeUniqueNames,
      const std::unordered_set<std::string>* retainedRelUniqueNames);
  std::optional<PropertyRewriteTarget> getTargetForPropertyExpression(
      PropertyExpression& propExpr, PatternIR& nodesIR,
      catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR);
  std::optional<PropertyRewriteTarget> getTargetForNodeProperty(
      size_t queryNodePos, const std::string& propertyName, PatternIR& nodesIR,
      catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR);
  std::optional<PropertyRewriteTarget> getTargetForRelProperty(
      PropertyExpression& propExpr, const std::string& propertyName,
      PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo,
      common::PatternIR& indexIR);

  bool validatePropertyCoverageForIndexRewrite(
      const std::shared_ptr<Expression>& rootExpr, PatternIR& nodesIR,
      catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR,
      const std::unordered_set<std::string>* retainedNodeUniqueNames = nullptr,
      const std::unordered_set<std::string>* retainedRelUniqueNames = nullptr);
  bool validatePropertyCoverageForIndexRewrite(const expression_vector& exprs, PatternIR& nodesIR, catalog::FillIndexAuxInfo& indexInfo, common::PatternIR& indexIR);

  /***************** rewrite ************** */
  bool rewriteMatchClause(BoundMatchClause& matchClause,
                          NormalizedQueryPart& queryPart, PatternIR& nodesIR,
                          IndexRewriteCandidate& candidate);
  bool acceptRewriteChoice(const IndexRewriteCandidate& candidate,
                           const PatternIR& current);
  void rewritePatternGraph(NormalizedQueryPart& queryPart,
                           BoundMatchClause& matchClause, PatternIR& nodesIR,
                           IndexRewriteCandidate& candidate);
  void nodeIndexRewrite(NormalizedQueryPart& queryPart, BoundMatchClause& matchClause,
                        PatternIR& nodesIR, catalog::IndexCatalogEntry& indexEntry,
                        QueryGraph& matchedGraph);
  void relIndexRewrite(NormalizedQueryPart& queryPart, BoundMatchClause& matchClause, PatternIR& nodesIR, catalog::IndexCatalogEntry& indexEntry);
  void variadicRelIndexRewrite(NormalizedQueryPart& queryPart, BoundMatchClause& matchClause, PatternIR& nodesIR, catalog::IndexCatalogEntry& indexEntry);
  void stageSelectiveVariadicAnchorSplit(
      NormalizedQueryPart& queryPart, BoundMatchClause& matchClause,
      QueryGraph& queryGraph, const std::shared_ptr<RelExpression>& bridgeRel);

  void tryRewriteMatchClause(BoundMatchClause& matchClause, NormalizedQueryPart& queryPart);
  bool tryRewriteIndependentNodePresence(
      BoundMatchClause& matchClause, NormalizedQueryPart& queryPart,
      const std::vector<catalog::IndexCatalogEntry*>& indexEntries);
  bool tryRewriteQueryPart(NormalizedQueryPart& queryPart);
  void visitSingleQueryUnsafe(NormalizedSingleQuery& query) override;
  void enumerateGlobalCountPresenceVariants(NormalizedSingleQuery& query);

 private:
  struct DeferredRebindPredicate {
    std::string oldUniqueName;
    std::string rawVarName;
    std::vector<std::shared_ptr<Expression>> conjuncts;
  };

  void applyDeferredRebindPredicates(NormalizedQueryPart& queryPart);
  void dropProjectionsForRemovedPatterns(
      NormalizedQueryPart& queryPart,
      const std::unordered_set<std::string>& removedNodeUniqueNames);
  void recordDeferredPredicatesFromMatch(
      const BoundMatchClause& matchClause,
      const std::unordered_map<std::string, std::string>& uniqueToRawVar,
      const std::unordered_set<std::string>& removedNodeUniqueNames,
      const std::unordered_set<std::string>& projectedPatternUniqueNames);

 private:
  main::ClientContext* clientContext;
  neug::binder::Binder* binder;
  std::unordered_set<std::string> visitedNodeUniqNames;
  std::vector<std::shared_ptr<neug::binder::RelExpression>> relExpressions;

  std::unordered_map<std::string, std::shared_ptr<Expression>> uniqueNameRewriteMap;
  std::unordered_map<std::string, std::shared_ptr<Expression>>
      deferredPropertyRewriteMap;
  std::unordered_set<std::string> projectionPredicateAnchorNodes;
  std::unordered_map<std::string, std::shared_ptr<Expression>> expressionsToRewrite;
  std::unordered_map<const Expression*, PropertyRewriteTarget> propertyRewriteTargets;
  // Additional physical tables covered by one logical multi-label node
  // aggregate rewrite. The selected index entry remains the primary table.
  std::vector<FillTableContext> additionalFillTables;
  // Catalog-only metadata, rebuilt inside every timed single-query visit.
  std::unordered_map<catalog::IndexCatalogEntry*,
                     std::optional<AggregateIndexGroup>> aggregateGroups;
  std::unordered_map<table_id_t, std::string> relLabels;

  // Local proof carried only through successful substitutions of an original
  // single-part global COUNT(*). It is never inferred from user alias text.
  struct GlobalCountProductProof {
    std::string aggregateUniqueName;
    std::vector<std::shared_ptr<Expression>> factors;
    std::unordered_set<std::string> introducedPresencePredicates;
  };
  std::optional<GlobalCountProductProof> globalCountProductProof;

  std::optional<NormalizedQueryPart> pendingPreQueryPart;
  std::vector<DeferredRebindPredicate> deferredRebindPredicates;

  // Full merged query IR for the current NormalizedSingleQuery. It is rebuilt
  // after each graph mutation so dependent indexes see the rewritten graph.
  common::PatternIR mergedQueryIR_;
  NormalizedSingleQuery* currentQuery_ = nullptr;
};

}  // namespace binder
}  // namespace neug

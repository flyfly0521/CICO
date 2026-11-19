#pragma once

#include <cstdint>
#include <functional>
#include "neug/compiler/common/serializer/deserializer.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/compiler/common/enums/query_rel_type.h"
#include "neug/compiler/common/types/types.h"
#include "neug/utils/api.h"
namespace neug {

namespace binder {
class QueryGraph;
}
namespace common {

enum class EdgeType {
  OUTGOING = 0,
  INCOMING,
  UNDIRECTED,
};

struct NEUG_API AdjacentInfo {
  // EdgeType edgeType;
  size_t dstNodeIdx;  // index of the destination node in the pattern graph
  std::vector<neug::common::oid_t>
      edgeLabels;  // relation label name //TODO
                   // 后期使用boost::dynamic_bitset优化
  AdjacentInfo() = default;
  AdjacentInfo(size_t dstNodeIdx, std::vector<neug::common::oid_t> edgeLabels)
      : dstNodeIdx(dstNodeIdx), edgeLabels(edgeLabels) {}
  void serialize(common::Serializer& writer) const;
  static AdjacentInfo deserialize(common::Deserializer& reader);
};

// MATCH (person:Person)
// lableName: Person
// varName: person
// uniqName: _0_person
struct NEUG_API NodeInfo {
  uint64_t matchedIdx{
      UINT64_MAX};        // matched node index in the query pattern graph
  std::string varName;    // variable name in the query pattern graph
  std::string uniqName;   // unique name in the query graph
  std::string indexName;  // original node index in the query pattern graph
                          // before rewriting
  std::vector<neug::common::oid_t> nodeLabels;  //  node label name
  std::vector<AdjacentInfo> undirected;
  std::vector<AdjacentInfo> outgoing;
  std::vector<AdjacentInfo> incoming;
  NodeInfo() = default;
  explicit NodeInfo(std::vector<common::oid_t> nodeLabels)
      : nodeLabels{std::move(nodeLabels)} {}
  static NodeInfo deserialize(common::Deserializer& reader);
  void serialize(common::Serializer& writer) const;
};

struct NEUG_API RelInfo {
  uint64_t matchedIdx{
      UINT64_MAX};        // matched rel index in the query pattern graph
  std::string varName;    // variable name in the query pattern graph
  std::string uniqName;   // unique name in the query graph
  std::string indexName;  // original rel index in the query pattern graph
                          // before rewriting
  size_t srcNodeIdx{UINT64_MAX};
  size_t dstNodeIdx{UINT64_MAX};
  EdgeType edgeType{EdgeType::OUTGOING};
  common::QueryRelType relType{common::QueryRelType::NON_RECURSIVE};
  std::vector<neug::common::oid_t> relLabels;
  RelInfo() = default;
  static RelInfo deserialize(common::Deserializer& reader);
  void serialize(common::Serializer& writer) const;
};

struct NEUG_API PatternIR {
  std::vector<NodeInfo> nodesInfo;
  std::vector<RelInfo> relsInfo;
  std::unordered_set<size_t> matchedNodeStarts;
  std::unordered_map<std::string, size_t> nodeUniqNameToPos;
  std::unordered_map<std::string, size_t> relUniqueNameToPos;
  PatternIR() = default;

  PatternIR(std::vector<NodeInfo> nodesInfo,
            std::vector<RelInfo> relsInfo,
            std::unordered_map<std::string, size_t> nodeUniqNameToPos,
            std::unordered_map<std::string, size_t> relUniqueNameToPos)
      : nodesInfo{std::move(nodesInfo)},
        relsInfo{std::move(relsInfo)},
        nodeUniqNameToPos{std::move(nodeUniqNameToPos)},
        relUniqueNameToPos{std::move(relUniqueNameToPos)} {}
  static int64_t findSource(std::vector<NodeInfo>& nodesInfo);
  static std::vector<uint32_t> findSources(std::vector<NodeInfo>& nodesInfo);
  static bool _findMatch(std::vector<NodeInfo>& nodesInfo,
                  std::vector<NodeInfo>& indexNodesInfo,
                  std::vector<bool>& nodesInfoVisited,
                  std::vector<bool>& indexNodesInfoVisited, size_t i, size_t j,
                  const std::string& indexName);
  static bool labelsEqual(const std::vector<common::oid_t>& nodeLabels,
                   const std::vector<common::oid_t>& otherNodeLabels);
  static std::pair<bool, size_t> findMatch(std::vector<NodeInfo>& nodesInfo,
                                    std::vector<NodeInfo>& indexNodesInfo,
                                    const std::string& indexName);
  static std::pair<bool, size_t> findMatch(PatternIR& patternIR,
                                           PatternIR& indexPatternIR,
                                           const std::string& indexName,
                                           bool allowDirectionCompensation = false);
  // Enumerate structural matches until the semantic guard accepts one.
  // A rejected mapping must not hide another valid automorphism.
  static std::pair<bool, size_t> findMatch(
      PatternIR& patternIR, PatternIR& indexPatternIR,
      const std::string& indexName, const std::function<bool()>& accept,
      bool allowDirectionCompensation = false);
  void serialize(common::Serializer& writer,
                 bool includeRelInfo = true) const;
  static PatternIR deserialize(common::Deserializer& reader,
                               bool includeRelInfo = true);

  // Merge multiple PatternIRs into a single combined PatternIR.
  // Nodes are deduplicated by uniqName (primary) or varName (fallback).
  // outNodeRemaps[patternIdx][oldNodeIdx] = newNodeIdx in the merged pattern.
  // Adjacency lists on shared nodes are merged so that findMatch works
  // correctly on the result.
  static PatternIR merge(const std::vector<PatternIR>& patterns,
                         std::vector<std::vector<uint32_t>>& outNodeRemaps);
};

NEUG_API PatternIR
convertGraphPatternToIR(const binder::QueryGraph& queryGraph);
}  // namespace common
}  // namespace neug

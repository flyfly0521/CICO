#include "neug/compiler/common/pattern_ir.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <utility>
#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/query/query_graph.h"
#include "neug/compiler/common/serializer/serializer.h"
namespace neug {
namespace common {
void AdjacentInfo::serialize(common::Serializer& writer) const {
  writer.write(dstNodeIdx);
  writer.serializeVector(edgeLabels);
}

AdjacentInfo AdjacentInfo::deserialize(common::Deserializer& reader) {
  AdjacentInfo adjInfo;
  reader.deserializeValue(adjInfo.dstNodeIdx);
  reader.deserializeVector(adjInfo.edgeLabels);
  return adjInfo;
}

void NodeInfo::serialize(common::Serializer& writer) const {
  writer.serializeValue(matchedIdx);
  writer.serializeValue(varName);
  writer.serializeValue(uniqName);
  writer.serializeValue(indexName);
  writer.serializeVector(nodeLabels);
  writer.serializeValue(undirected.size());
  for (auto& adjInfo : undirected) {
    adjInfo.serialize(writer);
  }
  writer.serializeValue(outgoing.size());
  for (auto& adjInfo : outgoing) {
    adjInfo.serialize(writer);
  }
  writer.serializeValue(incoming.size());
  for (auto& adjInfo : incoming) {
    adjInfo.serialize(writer);
  }
}

NodeInfo NodeInfo::deserialize(common::Deserializer& reader) {
  NodeInfo nodeInfo;
  reader.deserializeValue(nodeInfo.matchedIdx);
  reader.deserializeValue(nodeInfo.varName);
  reader.deserializeValue(nodeInfo.uniqName);
  reader.deserializeValue(nodeInfo.indexName);
  reader.deserializeVector(nodeInfo.nodeLabels);
  uint64_t undirectedSize;
  reader.deserializeValue(undirectedSize);
  for (size_t i = 0; i < undirectedSize; i++) {
    nodeInfo.undirected.emplace_back(AdjacentInfo::deserialize(reader));
  }
  uint64_t outgoingSize;
  reader.deserializeValue(outgoingSize);
  for (size_t i = 0; i < outgoingSize; i++) {
    nodeInfo.outgoing.emplace_back(AdjacentInfo::deserialize(reader));
  }
  uint64_t incomingSize;
  reader.deserializeValue(incomingSize);
  for (size_t i = 0; i < incomingSize; i++) {
    nodeInfo.incoming.emplace_back(AdjacentInfo::deserialize(reader));
  }
  return nodeInfo;
}

void RelInfo::serialize(common::Serializer& writer) const {
  writer.serializeValue(matchedIdx);
  writer.serializeValue(varName);
  writer.serializeValue(uniqName);
  writer.serializeValue(indexName);
  writer.serializeValue(srcNodeIdx);
  writer.serializeValue(dstNodeIdx);
  writer.serializeValue(edgeType);
  writer.serializeValue(relType);
  writer.serializeVector(relLabels);
}

RelInfo RelInfo::deserialize(common::Deserializer& reader) {
  RelInfo relInfo;
  reader.deserializeValue(relInfo.matchedIdx);
  reader.deserializeValue(relInfo.varName);
  reader.deserializeValue(relInfo.uniqName);
  reader.deserializeValue(relInfo.indexName);
  reader.deserializeValue(relInfo.srcNodeIdx);
  reader.deserializeValue(relInfo.dstNodeIdx);
  reader.deserializeValue(relInfo.edgeType);
  reader.deserializeValue(relInfo.relType);
  reader.deserializeVector(relInfo.relLabels);
  return relInfo;
}

void PatternIR::serialize(common::Serializer& writer,
                          bool includeRelInfo) const {
  writer.serializeValue(nodesInfo.size());
  for (auto& nodeInfo : nodesInfo) {
    nodeInfo.serialize(writer);
  }
  if (includeRelInfo) {
    writer.serializeVector(relsInfo);
  }
  writer.serializeUnorderedSet(matchedNodeStarts);
  writer.serializeUnorderedMap(nodeUniqNameToPos);
  writer.serializeUnorderedMap(relUniqueNameToPos);
}
PatternIR PatternIR::deserialize(common::Deserializer& reader,
                                 bool includeRelInfo) {
  PatternIR patternIR;
  uint64_t infoSize{0};
  reader.deserializeValue(infoSize);
  for (size_t i = 0; i < infoSize; i++) {
    patternIR.nodesInfo.emplace_back(NodeInfo::deserialize(reader));
  }
  if (includeRelInfo) {
    reader.deserializeVector(patternIR.relsInfo);
  }
  reader.deserializeUnorderedSet(patternIR.matchedNodeStarts);
  reader.deserializeUnorderedMap(patternIR.nodeUniqNameToPos);
  reader.deserializeUnorderedMap(patternIR.relUniqueNameToPos);
  return patternIR;
}

using neug::binder::NodeExpression;
using neug::binder::QueryGraph;
PatternIR convertGraphPatternToIR(const QueryGraph& queryGraph) {
  std::unordered_map<uint64_t*, size_t> nodeOffsets;
  std::unordered_map<std::string, size_t> nodeUniqNameToPos;
  std::vector<NodeInfo> nodesInfo(queryGraph.getQueryNodes().size());
  auto fillNodeInfo = [](std::shared_ptr<NodeExpression> node,
                         NodeInfo& nodeInfo) {
    for (auto& label : node->getEntries()) {
      nodeInfo.nodeLabels.emplace_back(label->getTableID());
    }
    nodeInfo.varName = node->getVariableName();
    nodeInfo.uniqName = node->getUniqueName();
  };
  {
    size_t k{0};
    for (auto node : queryGraph.getQueryNodes()) {
      auto ptr = reinterpret_cast<uint64_t*>(node.get());
      nodeOffsets[ptr] = k;
      nodeUniqNameToPos[node->getUniqueName()] = k;
      fillNodeInfo(node, nodesInfo[k]);
      ++k;
    }
  }

  std::vector<RelInfo> relsInfo(queryGraph.getNumQueryRels());
  std::unordered_map<std::string, size_t> relUniqueNameToPos;

  for (auto i = 0u; i < queryGraph.getNumQueryRels(); i++) {
    auto rel = queryGraph.getQueryRel(i);

    relUniqueNameToPos[rel->getUniqueName()] = i;
    std::vector<common::oid_t> relLabel;
    for (auto& label : rel->getEntries()) {
      relLabel.emplace_back(label->getTableID());
    }
    auto srcNode = rel->getSrcNode();
    auto dstNode = rel->getDstNode();
    auto srcIdx = nodeUniqNameToPos[srcNode->getUniqueName()];
    auto dstIdx = nodeUniqNameToPos[dstNode->getUniqueName()];
    auto& relInfo = relsInfo[i];
    relInfo.varName = rel->getVariableName();
    relInfo.uniqName = rel->getUniqueName();
    relInfo.srcNodeIdx = srcIdx;
    relInfo.dstNodeIdx = dstIdx;
    relInfo.relType = rel->getRelType();
    relInfo.relLabels = relLabel;
    switch (rel->getDirectionType()) {
    case binder::RelDirectionType::SINGLE: {
      relInfo.edgeType = EdgeType::OUTGOING;
      nodesInfo[srcIdx].outgoing.push_back(AdjacentInfo{dstIdx, relLabel});
      nodesInfo[dstIdx].incoming.push_back(AdjacentInfo{srcIdx, relLabel});
    } break;
    case binder::RelDirectionType::BOTH: {
      relInfo.edgeType = EdgeType::UNDIRECTED;
      // undirected edge type
      nodesInfo[srcIdx].undirected.push_back(AdjacentInfo{dstIdx, relLabel});
      nodesInfo[dstIdx].undirected.push_back(AdjacentInfo{srcIdx, relLabel});
    } break;
    default:
      NEUG_UNREACHABLE;
      break;
    }
  }
  return {std::move(nodesInfo), std::move(relsInfo), std::move(nodeUniqNameToPos),
          std::move(relUniqueNameToPos)};
}

int64_t PatternIR::findSource(std::vector<NodeInfo>& nodesInfo) {
    for (size_t i = 0; i < nodesInfo.size(); i++) {
        if (nodesInfo[i].incoming.empty()) {
            return i;
        }
    }
    return -1;
}

std::vector<uint32_t> PatternIR::findSources(std::vector<NodeInfo>& nodesInfo) {
  std::vector<uint32_t> sources;
  for (size_t i = 0; i < nodesInfo.size(); i++) {
      if (nodesInfo[i].incoming.empty()) {
          sources.push_back(i);
      }
  }
  return sources;
}

bool PatternIR::_findMatch(std::vector<NodeInfo>& nodesInfo,
    std::vector<NodeInfo>& indexNodesInfo, std::vector<bool>& nodesInfoVisited,
    std::vector<bool>& indexNodesInfoVisited, size_t i,
    size_t j, const std::string& indexName) { // TODO add marker to avoid revisit
    // if (nodesInfo[i].labelName != indexNodesInfo[j].labelName) {
    if (!labelsEqual(nodesInfo[i].nodeLabels, indexNodesInfo[j].nodeLabels)) {
        return false;
    }
    // check adjacency
    auto& info = indexNodesInfo[j];
    indexNodesInfoVisited[j] = true;
    nodesInfoVisited[i] = true;
    std::vector<std::pair<uint32_t, uint32_t>> undirected_offsets;
    for (auto& adj : info.undirected) {
        bool found{false};
        if (indexNodesInfoVisited[adj.dstNodeIdx]) {
            continue;
        }
        for (auto& nodeAdj : nodesInfo[i].undirected) {
            if (!nodesInfoVisited[nodeAdj.dstNodeIdx] && labelsEqual(nodeAdj.edgeLabels, adj.edgeLabels)) {
                found = true;
                undirected_offsets.push_back({nodeAdj.dstNodeIdx, adj.dstNodeIdx});
                nodesInfoVisited[nodeAdj.dstNodeIdx] = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> outgoing_offsets;
    for (auto& adj : info.outgoing) {
        bool found{false};
        if (indexNodesInfoVisited[adj.dstNodeIdx]) {
            continue;
        }
        for (auto& nodeAdj : nodesInfo[i].outgoing) {
            if (!nodesInfoVisited[nodeAdj.dstNodeIdx] && labelsEqual(nodeAdj.edgeLabels, adj.edgeLabels)) {
                found = true;
                outgoing_offsets.push_back({nodeAdj.dstNodeIdx, adj.dstNodeIdx});
                // nodesInfoVisited[nodeAdj.dstNodeIdx] = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> incoming_offsets;
    for (auto& adj : info.incoming) {
        bool found{false};
        if (indexNodesInfoVisited[adj.dstNodeIdx]) {
            continue;
        }
        for (auto& nodeAdj : nodesInfo[i].incoming) {
            if (!nodesInfoVisited[nodeAdj.dstNodeIdx] && labelsEqual(nodeAdj.edgeLabels, adj.edgeLabels)) {
                found = true;
                incoming_offsets.push_back({nodeAdj.dstNodeIdx, adj.dstNodeIdx});
                // nodesInfoVisited[nodeAdj.dstNodeIdx] = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    
    
    for (auto& pr : undirected_offsets) {
        if (!_findMatch(nodesInfo, indexNodesInfo, nodesInfoVisited, indexNodesInfoVisited,
                pr.first, pr.second, indexName)) {
            return false;
        }
    }
    
    for (auto& pr : outgoing_offsets) {
        if (!_findMatch(nodesInfo, indexNodesInfo, nodesInfoVisited, indexNodesInfoVisited,
                pr.first, pr.second, indexName)) {
            return false;
        }
    }
    for (auto& pr : incoming_offsets) {
        if (!_findMatch(nodesInfo, indexNodesInfo, nodesInfoVisited, indexNodesInfoVisited,
                pr.first, pr.second, indexName)) {
            return false;
        }
    }

    indexNodesInfo[j].matchedIdx = i;
    nodesInfo[i].matchedIdx = j;
    nodesInfo[i].indexName = indexName; // 记录下匹配的索引名称，后续在rewrite的时候会用到
    return true;
}

std::pair<bool, size_t> PatternIR::findMatch(std::vector<NodeInfo>& nodesInfo,
    std::vector<NodeInfo>& indexNodesInfo, const std::string& indexName) {
    // find index pattern's source node
    auto start = findSource(indexNodesInfo);
    if (start == -1) {
        // invalid index (almost impossible)
        return {false, 0};
    }
    std::vector<bool> nodesInfoVisited(nodesInfo.size(), false);
    std::vector<bool> indexNodesInfoVisited(indexNodesInfo.size(), false);
    for (size_t i = 0; i < nodesInfo.size(); i++) {
        // TODO record whether nodes in nodesInfo have been visited
        if (_findMatch(nodesInfo, indexNodesInfo, nodesInfoVisited, indexNodesInfoVisited, i,
                static_cast<size_t>(start), indexName)) {
            return {true, i};
        }
        std::fill(nodesInfoVisited.begin(), nodesInfoVisited.end(), false);
        std::fill(indexNodesInfoVisited.begin(), indexNodesInfoVisited.end(), false);
    }
    return {false, 0};
}

namespace {

bool endpointsMatch(const RelInfo& relInfo, const RelInfo& indexRelInfo,
                    const std::vector<NodeInfo>& indexNodesInfo) {
  auto matchedSrcIdx = indexNodesInfo[indexRelInfo.srcNodeIdx].matchedIdx;
  auto matchedDstIdx = indexNodesInfo[indexRelInfo.dstNodeIdx].matchedIdx;
  if (matchedSrcIdx == UINT64_MAX || matchedDstIdx == UINT64_MAX) {
    return false;
  }
  if (relInfo.edgeType == EdgeType::UNDIRECTED ||
      indexRelInfo.edgeType == EdgeType::UNDIRECTED) {
    return (relInfo.srcNodeIdx == matchedSrcIdx &&
            relInfo.dstNodeIdx == matchedDstIdx) ||
           (relInfo.srcNodeIdx == matchedDstIdx &&
            relInfo.dstNodeIdx == matchedSrcIdx);
  }
  return relInfo.srcNodeIdx == matchedSrcIdx &&
         relInfo.dstNodeIdx == matchedDstIdx;
}

bool relsCompatible(const RelInfo& relInfo, const RelInfo& indexRelInfo,
                    const std::vector<NodeInfo>& indexNodesInfo) {
  const auto directionCompatible =
      relInfo.edgeType == indexRelInfo.edgeType ||
      (relInfo.edgeType == EdgeType::UNDIRECTED &&
       indexRelInfo.edgeType == EdgeType::OUTGOING);
  return directionCompatible &&
         relInfo.relType == indexRelInfo.relType &&
         PatternIR::labelsEqual(relInfo.relLabels, indexRelInfo.relLabels) &&
         endpointsMatch(relInfo, indexRelInfo, indexNodesInfo);
}

bool matchRels(PatternIR& patternIR, PatternIR& indexPatternIR,
               const std::string& indexName) {
  if (indexPatternIR.relsInfo.empty()) {
    return true;
  }
  if (patternIR.relsInfo.size() < indexPatternIR.relsInfo.size()) {
    return false;
  }
  std::vector<bool> relsInfoVisited(patternIR.relsInfo.size(), false);
  for (size_t indexRelIdx = 0; indexRelIdx < indexPatternIR.relsInfo.size();
       ++indexRelIdx) {
    auto& indexRelInfo = indexPatternIR.relsInfo[indexRelIdx];
    bool found = false;
    for (size_t relIdx = 0; relIdx < patternIR.relsInfo.size(); ++relIdx) {
      if (relsInfoVisited[relIdx]) {
        continue;
      }
      auto& relInfo = patternIR.relsInfo[relIdx];
      if (!relsCompatible(relInfo, indexRelInfo, indexPatternIR.nodesInfo)) {
        continue;
      }
      relInfo.matchedIdx = indexRelIdx;
      relInfo.indexName = indexName;
      indexRelInfo.matchedIdx = relIdx;
      relsInfoVisited[relIdx] = true;
      found = true;
      break;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

void resetRelMatches(PatternIR& patternIR) {
  for (auto& relInfo : patternIR.relsInfo) {
    relInfo.matchedIdx = UINT64_MAX;
    relInfo.indexName.clear();
  }
}

void resetNodeMatches(PatternIR& patternIR) {
  for (auto& nodeInfo : patternIR.nodesInfo) {
    nodeInfo.matchedIdx = UINT64_MAX;
    nodeInfo.indexName.clear();
  }
}

bool isConnectedPattern(const PatternIR& patternIR) {
  if (patternIR.nodesInfo.empty()) {
    return false;
  }
  std::vector<std::vector<size_t>> adjacency(patternIR.nodesInfo.size());
  for (const auto& relInfo : patternIR.relsInfo) {
    if (relInfo.srcNodeIdx >= adjacency.size() ||
        relInfo.dstNodeIdx >= adjacency.size()) {
      return false;
    }
    adjacency[relInfo.srcNodeIdx].push_back(relInfo.dstNodeIdx);
    adjacency[relInfo.dstNodeIdx].push_back(relInfo.srcNodeIdx);
  }
  std::vector<bool> visited(adjacency.size(), false);
  std::vector<size_t> pending{0};
  visited[0] = true;
  while (!pending.empty()) {
    auto nodeIdx = pending.back();
    pending.pop_back();
    for (auto adjacentIdx : adjacency[nodeIdx]) {
      if (!visited[adjacentIdx]) {
        visited[adjacentIdx] = true;
        pending.push_back(adjacentIdx);
      }
    }
  }
  return std::all_of(visited.begin(), visited.end(),
                     [](bool value) { return value; });
}

bool nodeProfilesEqual(const NodeInfo& nodeInfo,
                       const NodeInfo& indexNodeInfo) {
  return PatternIR::labelsEqual(nodeInfo.nodeLabels,
                                indexNodeInfo.nodeLabels);
}

bool relCompatibleWithNodeMap(const RelInfo& relInfo,
                              const RelInfo& indexRelInfo,
                              const std::vector<size_t>& nodeMap) {
  const auto directionCompatible =
      relInfo.edgeType == indexRelInfo.edgeType ||
      (relInfo.edgeType == EdgeType::UNDIRECTED &&
       indexRelInfo.edgeType == EdgeType::OUTGOING);
  if (!directionCompatible ||
      relInfo.relType != indexRelInfo.relType ||
      !PatternIR::labelsEqual(relInfo.relLabels, indexRelInfo.relLabels)) {
    return false;
  }
  auto mappedSrcIdx = nodeMap[indexRelInfo.srcNodeIdx];
  auto mappedDstIdx = nodeMap[indexRelInfo.dstNodeIdx];
  if (relInfo.edgeType == EdgeType::UNDIRECTED ||
      indexRelInfo.edgeType == EdgeType::UNDIRECTED) {
    return (relInfo.srcNodeIdx == mappedSrcIdx &&
            relInfo.dstNodeIdx == mappedDstIdx) ||
           (relInfo.srcNodeIdx == mappedDstIdx &&
            relInfo.dstNodeIdx == mappedSrcIdx);
  }
  return relInfo.srcNodeIdx == mappedSrcIdx &&
         relInfo.dstNodeIdx == mappedDstIdx;
}

bool partialNodeMapIsFeasible(const PatternIR& patternIR,
                              const PatternIR& indexPatternIR,
                              const std::vector<size_t>& nodeMap) {
  for (const auto& indexRelInfo : indexPatternIR.relsInfo) {
    if (nodeMap[indexRelInfo.srcNodeIdx] == UINT64_MAX ||
        nodeMap[indexRelInfo.dstNodeIdx] == UINT64_MAX) {
      continue;
    }
    auto found = std::any_of(
        patternIR.relsInfo.begin(), patternIR.relsInfo.end(),
        [&](const RelInfo& relInfo) {
          return relCompatibleWithNodeMap(relInfo, indexRelInfo, nodeMap);
        });
    if (!found) {
      return false;
    }
  }
  return true;
}

bool findRelMapping(const PatternIR& patternIR,
                    const PatternIR& indexPatternIR,
                    const std::vector<size_t>& nodeMap,
                    size_t indexRelIdx, std::vector<bool>& relUsed,
                    std::vector<size_t>& relMap,
                    const std::function<bool()>& accept) {
  if (indexRelIdx == indexPatternIR.relsInfo.size()) {
    return accept();
  }
  const auto& indexRelInfo = indexPatternIR.relsInfo[indexRelIdx];
  for (size_t relIdx = 0; relIdx < patternIR.relsInfo.size(); ++relIdx) {
    if (relUsed[relIdx] ||
        !relCompatibleWithNodeMap(patternIR.relsInfo[relIdx], indexRelInfo,
                                  nodeMap)) {
      continue;
    }
    relUsed[relIdx] = true;
    relMap[indexRelIdx] = relIdx;
    if (findRelMapping(patternIR, indexPatternIR, nodeMap, indexRelIdx + 1,
                       relUsed, relMap, accept)) {
      return true;
    }
    relMap[indexRelIdx] = UINT64_MAX;
    relUsed[relIdx] = false;
  }
  return false;
}

bool findExactGraphMatch(PatternIR& patternIR, PatternIR& indexPatternIR,
                         const std::string& indexName, size_t& startIdx,
                         const std::function<bool()>& accept = {}) {
  if (patternIR.nodesInfo.size() < indexPatternIR.nodesInfo.size() ||
      patternIR.relsInfo.size() < indexPatternIR.relsInfo.size() ||
      !isConnectedPattern(indexPatternIR)) {
    return false;
  }

  const auto numNodes = indexPatternIR.nodesInfo.size();
  std::vector<size_t> indexNodeOrder(numNodes);
  std::iota(indexNodeOrder.begin(), indexNodeOrder.end(), 0);
  std::stable_sort(indexNodeOrder.begin(), indexNodeOrder.end(),
                   [&](size_t lhs, size_t rhs) {
                     const auto degree = [&](size_t idx) {
                       const auto& node = indexPatternIR.nodesInfo[idx];
                       return node.undirected.size() + node.outgoing.size() +
                              node.incoming.size();
                     };
                     return degree(lhs) > degree(rhs);
                   });

  std::vector<size_t> nodeMap(numNodes, UINT64_MAX);
  std::vector<bool> queryNodeUsed(patternIR.nodesInfo.size(), false);
  std::vector<bool> relUsed(patternIR.relsInfo.size(), false);
  std::vector<size_t> relMap(indexPatternIR.relsInfo.size(), UINT64_MAX);
  const auto acceptMapping = [&]() {
    resetNodeMatches(patternIR);
    resetNodeMatches(indexPatternIR);
    resetRelMatches(patternIR);
    resetRelMatches(indexPatternIR);
    for (size_t indexNodeIdx = 0; indexNodeIdx < numNodes; ++indexNodeIdx) {
      auto nodeIdx = nodeMap[indexNodeIdx];
      indexPatternIR.nodesInfo[indexNodeIdx].matchedIdx = nodeIdx;
      patternIR.nodesInfo[nodeIdx].matchedIdx = indexNodeIdx;
      patternIR.nodesInfo[nodeIdx].indexName = indexName;
    }
    for (size_t indexRelIdx = 0; indexRelIdx < relMap.size(); ++indexRelIdx) {
      auto relIdx = relMap[indexRelIdx];
      indexPatternIR.relsInfo[indexRelIdx].matchedIdx = relIdx;
      patternIR.relsInfo[relIdx].matchedIdx = indexRelIdx;
      patternIR.relsInfo[relIdx].indexName = indexName;
    }
    auto sourceIdx = PatternIR::findSource(indexPatternIR.nodesInfo);
    startIdx = sourceIdx >= 0 ? nodeMap[static_cast<size_t>(sourceIdx)]
                             : nodeMap.front();
    if (!accept || accept()) return true;
    resetNodeMatches(patternIR);
    resetNodeMatches(indexPatternIR);
    resetRelMatches(patternIR);
    resetRelMatches(indexPatternIR);
    return false;
  };
  std::function<bool(size_t)> mapNodes = [&](size_t orderIdx) {
    if (orderIdx == indexNodeOrder.size()) {
      return findRelMapping(patternIR, indexPatternIR, nodeMap, 0,
                            relUsed, relMap, acceptMapping);
    }
    auto indexNodeIdx = indexNodeOrder[orderIdx];
    const auto& indexNodeInfo = indexPatternIR.nodesInfo[indexNodeIdx];
    for (size_t nodeIdx = 0; nodeIdx < patternIR.nodesInfo.size(); ++nodeIdx) {
      if (queryNodeUsed[nodeIdx] ||
          !nodeProfilesEqual(patternIR.nodesInfo[nodeIdx], indexNodeInfo)) {
        continue;
      }
      queryNodeUsed[nodeIdx] = true;
      nodeMap[indexNodeIdx] = nodeIdx;
      if (partialNodeMapIsFeasible(patternIR, indexPatternIR, nodeMap) &&
          mapNodes(orderIdx + 1)) {
        return true;
      }
      nodeMap[indexNodeIdx] = UINT64_MAX;
      queryNodeUsed[nodeIdx] = false;
    }
    return false;
  };
  return mapNodes(0);
}

}  // namespace

std::pair<bool, size_t> PatternIR::findMatch(PatternIR& patternIR,
                                             PatternIR& indexPatternIR,
                                             const std::string& indexName,
                                             bool allowDirectionCompensation) {
  const auto directionsMatch = [&]() {
    return allowDirectionCompensation ||
        std::all_of(indexPatternIR.relsInfo.begin(), indexPatternIR.relsInfo.end(),
                    [&](const auto& rel) {
                      return rel.matchedIdx < patternIR.relsInfo.size() &&
                          rel.edgeType == patternIR.relsInfo[rel.matchedIdx].edgeType;
                    });
  };
  resetNodeMatches(patternIR);
  resetNodeMatches(indexPatternIR);
  resetRelMatches(patternIR);
  resetRelMatches(indexPatternIR);
  // Reject impossible matches before allocating greedy/backtracking state.
  if (patternIR.nodesInfo.size() < indexPatternIR.nodesInfo.size() ||
      patternIR.relsInfo.size() < indexPatternIR.relsInfo.size()) {
    return {false, 0};
  }
  for (const auto& indexNode : indexPatternIR.nodesInfo) {
    if (std::none_of(patternIR.nodesInfo.begin(), patternIR.nodesInfo.end(),
                     [&](const auto& node) {
                       return labelsEqual(node.nodeLabels, indexNode.nodeLabels);
                     })) {
      return {false, 0};
    }
  }
  auto [nodesMatched, startIdx] =
      findMatch(patternIR.nodesInfo, indexPatternIR.nodesInfo, indexName);
  // The greedy traversal can reserve two directed neighbors before either
  // recursive call marks them visited. Never accept a non-injective mapping
  // (or stale partial assignments from an earlier starting node).
  if (nodesMatched) {
    std::vector<bool> used(patternIR.nodesInfo.size(), false);
    for (size_t indexPos = 0; indexPos < indexPatternIR.nodesInfo.size();
         ++indexPos) {
      const auto queryPos = indexPatternIR.nodesInfo[indexPos].matchedIdx;
      if (queryPos >= used.size() || used[queryPos] ||
          patternIR.nodesInfo[queryPos].matchedIdx != indexPos) {
        nodesMatched = false;
        break;
      }
      used[queryPos] = true;
    }
    for (size_t queryPos = 0; queryPos < used.size(); ++queryPos) {
      if (!used[queryPos] &&
          patternIR.nodesInfo[queryPos].matchedIdx != UINT64_MAX) {
        nodesMatched = false;
      }
    }
  }
  if (!nodesMatched) {
    resetNodeMatches(patternIR);
    resetNodeMatches(indexPatternIR);
    if (findExactGraphMatch(patternIR, indexPatternIR, indexName, startIdx,
                           directionsMatch)) {
      return {true, startIdx};
    }
    return {false, startIdx};
  }
  if (!matchRels(patternIR, indexPatternIR, indexName) || !directionsMatch()) {
    resetNodeMatches(patternIR);
    resetNodeMatches(indexPatternIR);
    resetRelMatches(patternIR);
    resetRelMatches(indexPatternIR);
    if (findExactGraphMatch(patternIR, indexPatternIR, indexName, startIdx,
                           directionsMatch)) {
      return {true, startIdx};
    }
    return {false, startIdx};
  }
  return {true, startIdx};
}

std::pair<bool, size_t> PatternIR::findMatch(
    PatternIR& patternIR, PatternIR& indexPatternIR,
    const std::string& indexName, const std::function<bool()>& accept,
    bool allowDirectionCompensation) {
  resetNodeMatches(patternIR);
  resetNodeMatches(indexPatternIR);
  resetRelMatches(patternIR);
  resetRelMatches(indexPatternIR);
  size_t startIdx = 0;
  const auto guarded = [&]() {
    if (!allowDirectionCompensation)
      for (const auto& rel : indexPatternIR.relsInfo)
        if (rel.edgeType != patternIR.relsInfo[rel.matchedIdx].edgeType)
          return false;
    return accept();
  };
  const auto found = findExactGraphMatch(patternIR, indexPatternIR, indexName,
                                        startIdx, guarded);
  return {found, startIdx};
}

PatternIR PatternIR::merge(const std::vector<PatternIR>& patterns,
                            std::vector<std::vector<uint32_t>>& outNodeRemaps) {
  NEUG_ASSERT(!patterns.empty());
  outNodeRemaps.clear();
  outNodeRemaps.resize(patterns.size());

  // Start with the first pattern as the base.
  PatternIR merged = patterns[0];
  // Initialize node remap for patterns[0] as identity.
  outNodeRemaps[0].resize(merged.nodesInfo.size());
  for (size_t i = 0; i < merged.nodesInfo.size(); ++i) {
    outNodeRemaps[0][i] = static_cast<uint32_t>(i);
  }

  for (size_t pi = 1; pi < patterns.size(); ++pi) {
    const auto& other = patterns[pi];

    // --- Pass 1: Determine node remapping ---
    std::vector<uint32_t> nodeRemap(other.nodesInfo.size(), UINT32_MAX);
    for (size_t nj = 0; nj < other.nodesInfo.size(); ++nj) {
      const auto& onode = other.nodesInfo[nj];
      bool found = false;
      // Try uniqName match first (deterministic unique identifier).
      if (!onode.uniqName.empty()) {
        for (size_t mk = 0; mk < merged.nodesInfo.size(); ++mk) {
          if (merged.nodesInfo[mk].uniqName == onode.uniqName) {
            nodeRemap[nj] = static_cast<uint32_t>(mk);
            found = true;
            break;
          }
        }
      }
      // Fall back to varName match.
      if (!found && !onode.varName.empty()) {
        for (size_t mk = 0; mk < merged.nodesInfo.size(); ++mk) {
          if (merged.nodesInfo[mk].varName == onode.varName) {
            nodeRemap[nj] = static_cast<uint32_t>(mk);
            found = true;
            break;
          }
        }
      }
      if (!found) {
        nodeRemap[nj] =
            static_cast<uint32_t>(merged.nodesInfo.size()) +
            static_cast<uint32_t>(nj);  // Placeholder; will be corrected in pass 2.
      }
    }

    // Compute final positions for new nodes (compact assignment).
    uint32_t nextNewPos = static_cast<uint32_t>(merged.nodesInfo.size());
    for (size_t nj = 0; nj < other.nodesInfo.size(); ++nj) {
      if (nodeRemap[nj] >= merged.nodesInfo.size()) {
        // This is a new node; assign final position.
        nodeRemap[nj] = nextNewPos++;
      }
    }

    // --- Pass 2: Merge nodes with adjacency remapping ---
    // Pre-allocate space for new nodes.
    size_t numNewNodes = nextNewPos - merged.nodesInfo.size();
    merged.nodesInfo.reserve(merged.nodesInfo.size() + numNewNodes);

    for (size_t nj = 0; nj < other.nodesInfo.size(); ++nj) {
      const auto& onode = other.nodesInfo[nj];
      uint32_t newPos = nodeRemap[nj];
      bool isNewNode = (newPos >= merged.nodesInfo.size());

      if (isNewNode) {
        NodeInfo newNode = onode;
        newNode.matchedIdx = UINT64_MAX;
        newNode.indexName.clear();
        // Remap adjacency lists using the fully-populated nodeRemap.
        for (auto& adj : newNode.undirected) {
          NEUG_ASSERT(nodeRemap[adj.dstNodeIdx] != UINT32_MAX);
          adj.dstNodeIdx = nodeRemap[adj.dstNodeIdx];
        }
        for (auto& adj : newNode.outgoing) {
          NEUG_ASSERT(nodeRemap[adj.dstNodeIdx] != UINT32_MAX);
          adj.dstNodeIdx = nodeRemap[adj.dstNodeIdx];
        }
        for (auto& adj : newNode.incoming) {
          NEUG_ASSERT(nodeRemap[adj.dstNodeIdx] != UINT32_MAX);
          adj.dstNodeIdx = nodeRemap[adj.dstNodeIdx];
        }
        merged.nodesInfo.push_back(std::move(newNode));
      } else {
        // Shared node: merge adjacency lists from other pattern.
        NodeInfo& existingNode = merged.nodesInfo[newPos];
        auto mergeAdjs = [&](const std::vector<AdjacentInfo>& src,
                              std::vector<AdjacentInfo>& dst) {
          for (const auto& adj : src) {
            NEUG_ASSERT(nodeRemap[adj.dstNodeIdx] != UINT32_MAX);
            AdjacentInfo newAdj = adj;
            newAdj.dstNodeIdx = nodeRemap[adj.dstNodeIdx];
            bool dup = false;
            for (const auto& eadj : dst) {
              if (eadj.dstNodeIdx == newAdj.dstNodeIdx &&
                  labelsEqual(eadj.edgeLabels, newAdj.edgeLabels)) {
                dup = true;
                break;
              }
            }
            if (!dup) {
              dst.push_back(std::move(newAdj));
            }
          }
        };
        mergeAdjs(onode.undirected, existingNode.undirected);
        mergeAdjs(onode.outgoing, existingNode.outgoing);
        mergeAdjs(onode.incoming, existingNode.incoming);
      }
    }

    // --- Merge rels from the other pattern ---
    for (size_t rj = 0; rj < other.relsInfo.size(); ++rj) {
      RelInfo newRel = other.relsInfo[rj];
      newRel.matchedIdx = UINT64_MAX;
      newRel.indexName.clear();
      NEUG_ASSERT(nodeRemap[newRel.srcNodeIdx] != UINT32_MAX &&
                  nodeRemap[newRel.dstNodeIdx] != UINT32_MAX);
      newRel.srcNodeIdx = nodeRemap[newRel.srcNodeIdx];
      newRel.dstNodeIdx = nodeRemap[newRel.dstNodeIdx];
      merged.relsInfo.push_back(std::move(newRel));
    }

    outNodeRemaps[pi] = std::move(nodeRemap);
  }

  // Rebuild lookup maps. Keep every original node unique name as an alias to
  // its merged position: when two QueryParts carry the same logical variable
  // through WITH, merge may retain only the first NodeInfo object while
  // expressions in a later part still reference that part's binder-generated
  // unique name.
  merged.nodeUniqNameToPos.clear();
  merged.relUniqueNameToPos.clear();
  for (size_t i = 0; i < merged.nodesInfo.size(); ++i) {
    if (!merged.nodesInfo[i].uniqName.empty()) {
      merged.nodeUniqNameToPos[merged.nodesInfo[i].uniqName] = i;
    }
  }
  for (size_t i = 0; i < merged.relsInfo.size(); ++i) {
    if (!merged.relsInfo[i].uniqName.empty()) {
      merged.relUniqueNameToPos[merged.relsInfo[i].uniqName] = i;
    }
  }
  for (size_t patternIdx = 0; patternIdx < patterns.size(); ++patternIdx) {
    const auto& pattern = patterns[patternIdx];
    for (size_t nodeIdx = 0; nodeIdx < pattern.nodesInfo.size(); ++nodeIdx) {
      const auto& uniqueName = pattern.nodesInfo[nodeIdx].uniqName;
      if (!uniqueName.empty()) {
        merged.nodeUniqNameToPos[uniqueName] =
            outNodeRemaps[patternIdx][nodeIdx];
      }
    }
  }

  return merged;
}

bool PatternIR::labelsEqual(const std::vector<common::oid_t>& nodeLabels, const std::vector<common::oid_t>& otherNodeLabels) {
    if (nodeLabels.size() != otherNodeLabels.size()) {
        return false;
    }
    for (auto otherLabel : otherNodeLabels) {
        if (std::find(nodeLabels.begin(), nodeLabels.end(), otherLabel) ==
            nodeLabels.end()) {
            return false;
        }
    }
    return true;
}


}  // namespace common
}  // namespace neug

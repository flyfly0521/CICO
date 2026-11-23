#include "neug/compiler/binder/visitor/property_collector.h"
#include "neug/compiler/binder/rewriter/distinct_fill_shape.h"
#include "neug/compiler/binder/rewriter/fill_predicate_signature.h"

#include "neug/compiler/binder/bound_scan_source.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/index_predicate_key.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/normalized_single_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"

#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_expr_converter.h"
#include "neug/compiler/catalog/catalog_entry/rel_group_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/generated/proto/plan/expr.pb.h"

#include "neug/compiler/common/assert.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/common/enums/scan_source_type.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/common/serializer/buffered_reader.h"
#include "neug/compiler/common/serializer/buffered_serializer.h"
#include "neug/compiler/common/serializer/deserializer.h"
#include "neug/compiler/common/serializer/serializer.h"
#include "neug/compiler/common/string_format.h"
#include "neug/compiler/common/string_utils.h"

#include "neug/compiler/main/client_context.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neug {
namespace catalog {

std::string FillIndexAuxInfo::getRowCountColumn() const {
  for (const auto& column : targetColumns) {
    if (column.starts_with(common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) {
      return column;
    }
  }
  return {};
}

static constexpr uint64_t FILL_RUNTIME_CACHE_MAGIC =
    0x4B555A555F46494CULL;  // "NEUG_FILL"
static constexpr uint32_t FILL_RUNTIME_CACHE_VERSION =
    2;  // bumped: single merged PatternIR
static constexpr uint8_t PATTERN_IR_WITH_REL_INFO_FLAG = 0x80;
static constexpr uint8_t FILL_UNDIRECTED_REL_VALUE_MASK = 0x01;
// Bit flag set in the header byte to indicate the new single-PatternIR format.
static constexpr uint8_t SINGLE_PATTERN_IR_FORMAT_FLAG = 0x40;
// Bit flag set in the header byte when each aggregate index column carries a
// serialized ::common::Expression AST after its legacy string fields. Old
// catalog buffers have this bit clear and must deserialize without the AST
// block (the per-column records have no length prefix, so the flag is the
// only forward-compat gate).
static constexpr uint8_t INDEX_COL_HAS_AST_FLAG = 0x20;
// The buffer contains a predicate-presence boolean (and optional serialized
// MATCH-WHERE AST) after the recursive bounds.
static constexpr uint8_t PREDICATE_AST_FLAG = 0x10;
// REL_COMPUATION_WITH_AGGREGATION records carry persisted-state/finalizer
// metadata after their legacy scalar AST block.
static constexpr uint8_t FINALIZED_AGGREGATE_FLAG = 0x08;
// A count-prefixed list of top-level AND terms follows the legacy combined
// predicate block. Keeping the combined block preserves runtime-maintenance
// and old-reader compatibility, while the list supports residual predicates
// during query rewrite.
static constexpr uint8_t PREDICATE_CONJUNCTS_FLAG = 0x04;
// A versioned block containing the original FILL source query follows all
// legacy fields. Keeping it at the tail preserves the byte layout consumed by
// older readers, which ignore unconsumed bytes inside the length-delimited
// IndexAuxInfo buffer.
static constexpr uint8_t SOURCE_QUERY_BLOCK_FLAG = 0x02;
static constexpr uint32_t SOURCE_QUERY_BLOCK_VERSION = 8;

namespace {

template <typename T>
void appendVectorSignature(std::ostringstream& out,
                           const std::vector<T>& values) {
  out << values.size() << '[';
  for (const auto& value : values) {
    out << value << ',';
  }
  out << ']';
}

void appendAdjSignature(std::ostringstream& out,
                        const std::vector<common::AdjacentInfo>& adjs) {
  out << adjs.size() << '[';
  for (const auto& adj : adjs) {
    out << adj.dstNodeIdx << ':';
    appendVectorSignature(out, adj.edgeLabels);
    out << ';';
  }
  out << ']';
}

void appendPatternSignature(std::ostringstream& out,
                            const common::PatternIR& patternIR) {
  out << "nodes=" << patternIR.nodesInfo.size() << '[';
  for (const auto& node : patternIR.nodesInfo) {
    out << node.varName << '|' << node.uniqName << '|';
    appendVectorSignature(out, node.nodeLabels);
    appendAdjSignature(out, node.undirected);
    appendAdjSignature(out, node.outgoing);
    appendAdjSignature(out, node.incoming);
    out << ';';
  }
  out << ']';
  out << "rels=" << patternIR.relsInfo.size() << '[';
  for (const auto& rel : patternIR.relsInfo) {
    out << rel.varName << '|' << rel.uniqName << '|' << rel.srcNodeIdx << "->"
        << rel.dstNodeIdx << '|' << static_cast<uint32_t>(rel.edgeType) << '|'
        << static_cast<uint32_t>(rel.relType) << '|';
    appendVectorSignature(out, rel.relLabels);
    out << ';';
  }
  out << ']';
}

void appendIndexColumnSignature(
    std::ostringstream& out,
    const std::vector<std::shared_ptr<IndexColnumInfo>>& indexColumnInfos) {
  out << "indexColumns=" << indexColumnInfos.size() << '{';
  for (const auto& info : indexColumnInfos) {
    if (info == nullptr) {
      out << "null;";
      continue;
    }
    out << static_cast<uint32_t>(info->columnType) << '|' << info->nodeUnique
        << '|' << info->propertyName << '|';
    if (auto* nodeAgg =
            dynamic_cast<NodeAggregationIndexColnumInfo*>(info.get())) {
      out << nodeAgg->aggregateFuncName << '|' << nodeAgg->nameTemplate << '|';
      appendVectorSignature(out, nodeAgg->nodeUniqueNames);
    } else if (auto* rel = dynamic_cast<RelIndexColnumInfo*>(info.get())) {
      out << rel->nameTemplate << '|';
      appendVectorSignature(out, rel->nodeUniqueNames);
    } else if (auto* relAgg = dynamic_cast<RelComputationWithAggreColumnInfo*>(
                   info.get())) {
      out << relAgg->nameTemplate << '|';
      appendVectorSignature(out, relAgg->nodeUniqueNames);
      out << relAgg->aggregateFuncName << '|' << relAgg->stateTargetColumn
          << '|';
      if (relAgg->aggregateArgExpr) {
        out << relAgg->aggregateArgExpr->SerializeAsString();
      }
      out << '|';
      if (relAgg->finalizerExpr) {
        out << relAgg->finalizerExpr->SerializeAsString();
      }
    }
    out << ';';
  }
  out << '}';
}

std::string makeFillIndexBaseName(
    const std::string& tableName, const std::vector<std::string>& columnNames,
    FillEntryType fillType, const common::PatternIR& patternIR,
    const std::vector<std::shared_ptr<IndexColnumInfo>>& indexColumnInfos,
    const std::string& extraSignature = "") {
  std::string baseName = "fill_" + tableName;
  for (const auto& columnName : columnNames) {
    baseName += "_" + columnName;
  }
  if (columnNames.empty()) {
    baseName += "_no_columns";
  }

  std::ostringstream signature;
  signature << tableName << '|' << static_cast<uint32_t>(fillType) << '|';
  appendVectorSignature(signature, columnNames);
  appendPatternSignature(signature, patternIR);
  appendIndexColumnSignature(signature, indexColumnInfos);
  signature << extraSignature;

  std::ostringstream hash;
  hash << std::hex << std::setw(16) << std::setfill('0')
       << std::hash<std::string>{}(signature.str());
  return baseName + "_" + hash.str().substr(0, 12);
}

std::string makeUniqueExtendedIndexName(Catalog* catalog,
                                        transaction::Transaction* transaction,
                                        const std::string& baseName) {
  auto indexName = baseName;
  uint64_t suffix = 1;
  while (catalog->containsExtendedIndex(transaction, indexName)) {
    indexName = baseName + "_" + std::to_string(suffix++);
  }
  return indexName;
}

}  // namespace

// --- IndexColnumInfo serialization ---

void IndexColnumInfo::serialize(neug::common::Serializer& writer) const {
  writer.write(static_cast<uint8_t>(columnType));
  writer.serializeValue(nodeUnique);
  writer.serializeValue(propertyName);
}

void NodeAggregationIndexColnumInfo::serialize(
    neug::common::Serializer& writer) const {
  IndexColnumInfo::serialize(writer);
  writer.serializeValue(aggregateFuncName);
  writer.serializeVector(
      nodeUniqueNames);  // uniqName list instead of patternIdx
  writer.serializeValue(nameTemplate);
  // Serialized AST block (gated by INDEX_COL_HAS_AST_FLAG on the reader side).
  writer.serializeValue(aggregateArgExpr != nullptr);
  if (aggregateArgExpr) {
    writer.serializeValue(aggregateArgExpr->SerializeAsString());
  }
}

void RelIndexColnumInfo::serialize(neug::common::Serializer& writer) const {
  IndexColnumInfo::serialize(writer);
  writer.serializeVector(
      nodeUniqueNames);  // uniqName list instead of patternIdx
  writer.serializeValue(nameTemplate);
  writer.serializeValue(aggregateFuncName);
  writer.serializeValue(aggregateArgExpr != nullptr);
  if (aggregateArgExpr) {
    writer.serializeValue(aggregateArgExpr->SerializeAsString());
  }
}

void RelComputationWithAggreColumnInfo::serialize(
    neug::common::Serializer& writer) const {
  IndexColnumInfo::serialize(writer);
  writer.serializeVector(
      nodeUniqueNames);  // uniqName list instead of patternIdx
  writer.serializeValue(nameTemplate);
  writer.serializeValue(astExpr != nullptr);
  if (astExpr) {
    writer.serializeValue(astExpr->SerializeAsString());
  }
  writer.serializeValue(finalizerExpr != nullptr);
  if (finalizerExpr) {
    writer.serializeValue(aggregateFuncName);
    writer.serializeValue(stateTargetColumn);
    writer.serializeValue(aggregateArgExpr != nullptr);
    if (aggregateArgExpr) {
      writer.serializeValue(aggregateArgExpr->SerializeAsString());
    }
    writer.serializeValue(finalizerExpr->SerializeAsString());
  }
}

void FillPatternElementOrigin::serialize(common::Serializer& writer) const {
  writer.write(queryPartIdx);
  writer.write(readingClauseIdx);
  writer.write(queryGraphIdx);
  writer.write(localElementIdx);
  writer.write(mergedElementIdx);
  writer.serializeValue(originalUniqueName);
}

FillPatternElementOrigin FillPatternElementOrigin::deserialize(
    common::Deserializer& reader) {
  FillPatternElementOrigin result;
  reader.deserializeValue(result.queryPartIdx);
  reader.deserializeValue(result.readingClauseIdx);
  reader.deserializeValue(result.queryGraphIdx);
  reader.deserializeValue(result.localElementIdx);
  reader.deserializeValue(result.mergedElementIdx);
  reader.deserializeValue(result.originalUniqueName);
  return result;
}

// --- FillIndexAuxInfo serialization ---

void FillExistsPredicate::serialize(common::Serializer& writer) const {
  writer.serializeValue(srcNodePos);
  writer.serializeValue(dstNodePos);
  writer.serializeValue(negated);
  writer.serializeValue(directed);
  writer.serializeVector(srcTableIDs);
  writer.serializeVector(dstTableIDs);
  writer.serializeVector(relTableIDs);
}

FillExistsPredicate FillExistsPredicate::deserialize(
    common::Deserializer& reader) {
  FillExistsPredicate result;
  reader.deserializeValue(result.srcNodePos);
  reader.deserializeValue(result.dstNodePos);
  reader.deserializeValue(result.negated);
  reader.deserializeValue(result.directed);
  reader.deserializeVector(result.srcTableIDs);
  reader.deserializeVector(result.dstTableIDs);
  reader.deserializeVector(result.relTableIDs);
  return result;
}

std::shared_ptr<common::BufferedSerializer> FillIndexAuxInfo::serialize()
    const {
  auto bufferedSerializer = std::make_shared<common::BufferedSerializer>();
  auto writer = std::make_shared<common::Serializer>(bufferedSerializer);
  // fillType + flags
  writer->write(static_cast<uint8_t>(getFillType()));
  writer->write(static_cast<uint8_t>(
      (fillUndirectedRel ? FILL_UNDIRECTED_REL_VALUE_MASK : 0) |
      PATTERN_IR_WITH_REL_INFO_FLAG | SINGLE_PATTERN_IR_FORMAT_FLAG |
      INDEX_COL_HAS_AST_FLAG | PREDICATE_AST_FLAG | FINALIZED_AGGREGATE_FLAG |
      PREDICATE_CONJUNCTS_FLAG | SOURCE_QUERY_BLOCK_FLAG));
  // srcTableId, dstTableId (these remain for rel indexes)
  writer->write(srcTableId);
  writer->write(dstTableId);
  // table metadata
  writer->serializeValue(tableName);
  writer->serializeValue(tableUniqName);
  writer->serializeValue(tableVariableName);
  // Target columns
  writer->serializeVector(targetColumns);
  // Index column infos
  uint32_t numIndexCols = static_cast<uint32_t>(indexColumnInfos.size());
  writer->write(numIndexCols);
  for (const auto& colInfo : indexColumnInfos) {
    colInfo->serialize(*writer);
  }

  // Single merged PatternIR
  patternIR.serialize(*writer, true);

  // recursive rel bounds
  writer->write(recursiveRelLowerBound);
  writer->write(recursiveRelUpperBound);
  writer->serializeValue(predicateExpr != nullptr);
  if (predicateExpr) {
    writer->serializeValue(predicateExpr->SerializeAsString());
  }
  writer->serializeValue(
      static_cast<uint32_t>(predicateConjuncts.size()));
  for (const auto& conjunct : predicateConjuncts) {
    writer->serializeValue(conjunct->SerializeAsString());
  }
  auto sourceBlock = std::make_shared<common::BufferedSerializer>();
  common::Serializer sourceWriter{sourceBlock};
  sourceWriter.serializeValue(sourceQuery);
  sourceWriter.serializeVector(nodeOrigins);
  sourceWriter.serializeVector(relOrigins);
  sourceWriter.serializeValue(srcNodeUniqName);
  sourceWriter.serializeValue(dstNodeUniqName);
  sourceWriter.serializeValue(static_cast<uint8_t>(aggregateNumericContract));
  sourceWriter.serializeValue(static_cast<uint8_t>(aggregateForm));
  sourceWriter.serializeVector(predicateTemplates);
  sourceWriter.serializeValue(workloadCompatibility);
  sourceWriter.serializeValue(isRewriteReady());
  sourceWriter.serializeVector(existsPredicates);
  sourceWriter.serializeVector(presenceColumns);
  writer->write(SOURCE_QUERY_BLOCK_VERSION);
  writer->write(sourceBlock->getSize());
  writer->write(sourceBlock->getBlobData(), sourceBlock->getSize());
  return bufferedSerializer;
}

// --- Deserialization ---

std::shared_ptr<IndexColnumInfo> IndexColnumInfo::deserialize(
    std::unique_ptr<common::Deserializer>& deserializer, bool hasAstExpr,
    bool hasFinalizedAggregate) {
  uint8_t IndexColumnTypeByte = 0;
  deserializer->read(reinterpret_cast<uint8_t*>(&IndexColumnTypeByte),
                     sizeof(uint8_t));
  auto columnType = static_cast<IndexColumnType>(IndexColumnTypeByte);
  std::string nodeUnique, propertyName;
  deserializer->deserializeValue(nodeUnique);
  deserializer->deserializeValue(propertyName);
  switch (columnType) {
  case IndexColumnType::NODE_PROPERTY: {
    return std::make_shared<IndexColnumInfo>(IndexColnumInfo{
        columnType, std::move(nodeUnique), std::move(propertyName)});
  }
  case IndexColumnType::REL_PROPERTY:
  case IndexColumnType::SOURCE_QUERY_VALUE: {
    return std::make_shared<IndexColnumInfo>(IndexColnumInfo{
        columnType, std::move(nodeUnique), std::move(propertyName)});
  }
  case IndexColumnType::NODE_AGGREGATE: {
    std::vector<std::string> nodeUniqueNames;
    std::string nameTemplate, aggName;
    deserializer->deserializeValue(aggName);
    deserializer->deserializeVector(nodeUniqueNames);
    deserializer->deserializeValue(nameTemplate);
    auto info = std::make_shared<NodeAggregationIndexColnumInfo>(
        NodeAggregationIndexColnumInfo{
            columnType, std::move(nodeUnique), std::move(propertyName), aggName,
            std::move(nodeUniqueNames), nameTemplate});
    if (hasAstExpr) {
      bool hasAst = false;
      std::string astBytes;
      deserializer->deserializeValue(hasAst);
      if (hasAst) {
        deserializer->deserializeValue(astBytes);
        info->aggregateArgExpr = std::make_shared<::common::Expression>();
        if (!info->aggregateArgExpr->ParseFromString(astBytes)) {
          THROW_RUNTIME_ERROR(
              "Failed to deserialize FILL aggregate argument AST");
        }
      }
    }
    return info;
  }
  case IndexColumnType::REL_AGGREGATE: {
    std::vector<std::string> nodeUniqueNames;
    std::string nameTemplate;
    deserializer->deserializeVector(nodeUniqueNames);
    deserializer->deserializeValue(nameTemplate);
    auto info = std::make_shared<RelIndexColnumInfo>(RelIndexColnumInfo{
        columnType, std::move(nodeUnique), std::move(propertyName),
        std::move(nodeUniqueNames), std::move(nameTemplate)});
    if (hasAstExpr) {
      std::string astBytes;
      bool hasAst = false;
      deserializer->deserializeValue(info->aggregateFuncName);
      deserializer->deserializeValue(hasAst);
      if (hasAst) {
        deserializer->deserializeValue(astBytes);
        info->aggregateArgExpr = std::make_shared<::common::Expression>();
        if (!info->aggregateArgExpr->ParseFromString(astBytes)) {
          THROW_RUNTIME_ERROR(
              "Failed to deserialize FILL aggregate argument AST");
        }
      }
    }
    return info;
  }
  case IndexColumnType::REL_COMPUATION_WITH_AGGREGATION: {
    std::vector<std::string> nodeUniqueNames;
    std::string nameTemplate;
    deserializer->deserializeVector(nodeUniqueNames);
    deserializer->deserializeValue(nameTemplate);
    auto info = std::make_shared<RelComputationWithAggreColumnInfo>(
        RelComputationWithAggreColumnInfo{
            columnType, std::move(nodeUnique), std::move(propertyName),
            std::move(nodeUniqueNames), std::move(nameTemplate)});
    if (hasAstExpr) {
      bool hasAst = false;
      std::string astBytes;
      deserializer->deserializeValue(hasAst);
      if (hasAst) {
        deserializer->deserializeValue(astBytes);
        info->astExpr = std::make_shared<::common::Expression>();
        if (!info->astExpr->ParseFromString(astBytes)) {
          THROW_RUNTIME_ERROR(
              "Failed to deserialize FILL scalar expression AST");
        }
      }
    }
    if (hasFinalizedAggregate) {
      bool hasFinalizer = false;
      deserializer->deserializeValue(hasFinalizer);
      if (hasFinalizer) {
        bool hasAggregateArg = false;
        std::string aggregateBytes, finalizerBytes;
        deserializer->deserializeValue(info->aggregateFuncName);
        deserializer->deserializeValue(info->stateTargetColumn);
        deserializer->deserializeValue(hasAggregateArg);
        if (hasAggregateArg) {
          deserializer->deserializeValue(aggregateBytes);
          info->aggregateArgExpr = std::make_shared<::common::Expression>();
          if (!info->aggregateArgExpr->ParseFromString(aggregateBytes)) {
            THROW_RUNTIME_ERROR(
                "Failed to deserialize finalized FILL aggregate argument AST");
          }
        }
        deserializer->deserializeValue(finalizerBytes);
        info->finalizerExpr = std::make_shared<::common::Expression>();
        if (!info->finalizerExpr->ParseFromString(finalizerBytes)) {
          THROW_RUNTIME_ERROR(
              "Failed to deserialize finalized FILL expression AST");
        }
      }
    }
    return info;
  }
  default:
    NEUG_UNREACHABLE;
  }
  return nullptr;
}

FillIndexAuxInfo* FillIndexAuxInfo::deserialize(
    std::unique_ptr<common::BufferReader> reader) {
  FillIndexAuxInfo* info = new FillIndexAuxInfo();
  auto deserilizer = std::make_unique<common::Deserializer>(std::move(reader));

  deserilizer->read(reinterpret_cast<uint8_t*>(&info->fillType),
                    sizeof(uint8_t));
  const bool legacyVariadicType =
      info->fillType == FillEntryType::LEGACY_VARIADIC_REL;
  uint8_t fillUndirectedRelByte = 0;
  deserilizer->read(reinterpret_cast<uint8_t*>(&fillUndirectedRelByte),
                    sizeof(uint8_t));
  const bool patternIRHasRelInfo =
      (fillUndirectedRelByte & PATTERN_IR_WITH_REL_INFO_FLAG) != 0;
  const bool isSinglePatternIRFormat =
      (fillUndirectedRelByte & SINGLE_PATTERN_IR_FORMAT_FLAG) != 0;
  info->fillUndirectedRel =
      (fillUndirectedRelByte & FILL_UNDIRECTED_REL_VALUE_MASK) != 0;
  const bool hasAstExpr = (fillUndirectedRelByte & INDEX_COL_HAS_AST_FLAG) != 0;
  const bool hasPredicateBlock =
      (fillUndirectedRelByte & PREDICATE_AST_FLAG) != 0;
  const bool hasFinalizedAggregate =
      (fillUndirectedRelByte & FINALIZED_AGGREGATE_FLAG) != 0;
  const bool hasPredicateConjuncts =
      (fillUndirectedRelByte & PREDICATE_CONJUNCTS_FLAG) != 0;
  const bool hasSourceQueryBlock =
      (fillUndirectedRelByte & SOURCE_QUERY_BLOCK_FLAG) != 0;
  deserilizer->deserializeValue(info->srcTableId);
  deserilizer->deserializeValue(info->dstTableId);
  // table metadata
  deserilizer->deserializeValue(info->tableName);
  deserilizer->deserializeValue(info->tableUniqName);
  deserilizer->deserializeValue(info->tableVariableName);

  deserilizer->deserializeVector(info->targetColumns);

  // Index column infos
  uint32_t numIndexCols{};
  deserilizer->deserializeValue(numIndexCols);
  info->indexColumnInfos.reserve(numIndexCols);
  for (uint32_t i = 0; i < numIndexCols; ++i) {
    auto colInfo = IndexColnumInfo::deserialize(deserilizer, hasAstExpr,
                                                hasFinalizedAggregate);
    info->indexColumnInfos.emplace_back(std::move(colInfo));
  }

  if (isSinglePatternIRFormat) {
    // New format: single merged PatternIR.
    info->patternIR =
        common::PatternIR::deserialize(*deserilizer, patternIRHasRelInfo);
  } else {
    // Backward compat: old format stored a vector of per-QueryPart patternIRs,
    // optionally followed by a merged PatternIR for gate-check.
    // Read the count-prefixed vector of sub-patterns, then the merged one.
    uint32_t numPatternIRs{};
    deserilizer->deserializeValue(numPatternIRs);
    std::vector<common::PatternIR> patternIRs;
    patternIRs.reserve(numPatternIRs);
    for (uint32_t i = 0; i < numPatternIRs; ++i) {
      patternIRs.emplace_back(
          common::PatternIR::deserialize(*deserilizer, patternIRHasRelInfo));
    }
    // Try to read the merged PatternIR (preceded by a flag byte = 1 if
    // present).
    if (!deserilizer->finished()) {
      uint8_t mergedFlag = 0;
      deserilizer->read(&mergedFlag, sizeof(uint8_t));
      if (mergedFlag == 1 && !deserilizer->finished()) {
        info->patternIR =
            common::PatternIR::deserialize(*deserilizer, patternIRHasRelInfo);
      } else {
        // No valid merged flag — merge on the fly.
        std::vector<std::vector<uint32_t>> remaps;
        info->patternIR = common::PatternIR::merge(patternIRs, remaps);
      }
    } else {
      std::vector<std::vector<uint32_t>> remaps;
      info->patternIR = common::PatternIR::merge(patternIRs, remaps);
    }
  }

  if (!deserilizer->finished()) {
    deserilizer->deserializeValue(info->recursiveRelLowerBound);
    deserilizer->deserializeValue(info->recursiveRelUpperBound);
  } else if (legacyVariadicType) {
    // Backward compatibility for older aux buffers that overloaded src/dst
    // table ids to store variable-length lower/upper bounds.
    info->recursiveRelLowerBound = info->srcTableId;
    info->recursiveRelUpperBound = info->dstTableId;
    info->srcTableId = UINT32_MAX;
    info->dstTableId = UINT32_MAX;
  }
  if (hasPredicateBlock && !deserilizer->finished()) {
    bool hasPredicate = false;
    deserilizer->deserializeValue(hasPredicate);
    if (hasPredicate) {
      std::string astBytes;
      deserilizer->deserializeValue(astBytes);
      info->predicateExpr = std::make_shared<::common::Expression>();
      if (!info->predicateExpr->ParseFromString(astBytes)) {
        THROW_RUNTIME_ERROR("Failed to deserialize FILL WHERE predicate AST");
      }
    }
  }
  if (hasPredicateConjuncts && !deserilizer->finished()) {
    uint32_t numConjuncts = 0;
    deserilizer->deserializeValue(numConjuncts);
    info->predicateConjuncts.reserve(numConjuncts);
    for (uint32_t i = 0; i < numConjuncts; ++i) {
      std::string astBytes;
      deserilizer->deserializeValue(astBytes);
      auto conjunct = std::make_shared<::common::Expression>();
      if (!conjunct->ParseFromString(astBytes)) {
        THROW_RUNTIME_ERROR(
            "Failed to deserialize FILL WHERE predicate conjunct AST");
      }
      info->predicateConjuncts.push_back(std::move(conjunct));
    }
  } else if (info->predicateExpr != nullptr) {
    // Catalogs produced by the first predicate-AST format only stored the
    // combined expression. Exact single-predicate rewrite remains available.
    info->predicateConjuncts.push_back(info->predicateExpr);
  }
  if (hasSourceQueryBlock && !deserilizer->finished()) {
    uint32_t sourceQueryVersion = 0;
    deserilizer->deserializeValue(sourceQueryVersion);
    uint64_t sourceBlockSize = 0;
    deserilizer->deserializeValue(sourceBlockSize);
    if (sourceBlockSize > deserilizer->remainingBytes()) {
      THROW_RUNTIME_ERROR(common::stringFormat(
          "FILL source query metadata block size {} exceeds {} remaining "
          "bytes",
          sourceBlockSize, deserilizer->remainingBytes()));
    }
    std::vector<uint8_t> sourceBlock(sourceBlockSize);
    deserilizer->read(sourceBlock.data(), sourceBlock.size());
    if (sourceQueryVersion == 0 ||
        sourceQueryVersion > SOURCE_QUERY_BLOCK_VERSION) {
      THROW_RUNTIME_ERROR(
          "Unsupported FILL source metadata version; graph predicates cannot "
          "be safely ignored.");
    }
    if (sourceQueryVersion >= 1 &&
        sourceQueryVersion <= SOURCE_QUERY_BLOCK_VERSION) {
      auto sourceReader = std::make_unique<common::BufferReader>(
          sourceBlock.data(), sourceBlock.size());
      auto sourceDeserializer =
          std::make_unique<common::Deserializer>(std::move(sourceReader));
      sourceDeserializer->deserializeValue(info->sourceQuery);
      sourceDeserializer->deserializeVector(info->nodeOrigins);
      sourceDeserializer->deserializeVector(info->relOrigins);
      if (sourceQueryVersion >= 2) {
        sourceDeserializer->deserializeValue(info->srcNodeUniqName);
        sourceDeserializer->deserializeValue(info->dstNodeUniqName);
      }
      if (sourceQueryVersion >= 3) {
        uint8_t contract = 0;
        sourceDeserializer->deserializeValue(contract);
        if (contract > static_cast<uint8_t>(
                FillAggregateNumericContract::SOURCE_QUERY_RECOMPUTE)) {
          THROW_RUNTIME_ERROR("Unsupported FILL aggregate numeric contract");
        }
        info->aggregateNumericContract =
            static_cast<FillAggregateNumericContract>(contract);
      }
      if (sourceQueryVersion >= 4) {
        uint8_t form = 0;
        sourceDeserializer->deserializeValue(form);
        if (form > static_cast<uint8_t>(FillAggregateForm::CANONICAL_BASE)) {
          THROW_RUNTIME_ERROR("Unsupported FILL aggregate expression form");
        }
        info->aggregateForm = static_cast<FillAggregateForm>(form);
      }
      if (sourceQueryVersion == 6) {
        // Both branches used version 6: this branch stored correlated
        // predicates/presence, while the source runtime stored a snapshot
        // flag and readiness. Probe the complete bounded local tail first.
        const auto offset = sourceBlock.size() - sourceDeserializer->remainingBytes();
        auto local = std::make_unique<common::Deserializer>(
            std::make_unique<common::BufferReader>(sourceBlock.data() + offset,
                                                   sourceBlock.size() - offset));
        std::vector<std::string> templates, presence;
        std::vector<FillExistsPredicate> exists;
        bool compatibility = false;
        bool localLayout = false;
        try {
          local->deserializeVector(templates);
          local->deserializeValue(compatibility);
          local->deserializeVector(exists);
          local->deserializeVector(presence);
          localLayout = local->finished();
        } catch (const std::exception&) {
          // The upstream format has a different field order.
        }
        if (localLayout) {
          info->predicateTemplates = std::move(templates);
          info->workloadCompatibility = compatibility;
          info->existsPredicates = std::move(exists);
          info->presenceColumns = std::move(presence);
          sourceDeserializer = std::move(local);
        } else {
          bool snapshotOnly = false, ready = true;
          sourceDeserializer->deserializeValue(snapshotOnly);
          sourceDeserializer->deserializeVector(info->predicateTemplates);
          sourceDeserializer->deserializeValue(ready);
          if (snapshotOnly)
            THROW_RUNTIME_ERROR("Snapshot-only FILL metadata requires rebuilding its source definition");
          info->setRewriteReady(ready);
        }
      } else if (sourceQueryVersion >= 5) {
        sourceDeserializer->deserializeVector(info->predicateTemplates);
        sourceDeserializer->deserializeValue(info->workloadCompatibility);
        if (sourceQueryVersion >= 7) {
          bool ready = true;
          sourceDeserializer->deserializeValue(ready);
          info->setRewriteReady(ready);
        }
        if (sourceQueryVersion >= 8) {
          sourceDeserializer->deserializeVector(info->existsPredicates);
          sourceDeserializer->deserializeVector(info->presenceColumns);
        }
      }
      {
        for (const auto& predicate : info->existsPredicates) {
          if (predicate.srcNodePos >= info->patternIR.nodesInfo.size() ||
              predicate.dstNodePos >= info->patternIR.nodesInfo.size() ||
              predicate.srcTableIDs.empty() || predicate.dstTableIDs.empty() ||
              predicate.relTableIDs.empty()) {
            THROW_RUNTIME_ERROR("Invalid correlated FILL predicate metadata");
          }
        }
        for (const auto& column : info->presenceColumns) {
          if (std::find(info->targetColumns.begin(), info->targetColumns.end(),
                        column) == info->targetColumns.end()) {
            THROW_RUNTIME_ERROR("FILL presence column is not an owned target");
          }
        }
      }
      if (!sourceDeserializer->finished()) {
        THROW_RUNTIME_ERROR(
            "Unexpected trailing bytes in FILL source query metadata block");
      }
    }
  }
  if (!hasSourceQueryBlock && !hasPredicateBlock && !deserilizer->finished()) {
    // workload catalogs appended templates and endpoint identities directly.
    deserilizer->deserializeVector(info->predicateTemplates);
    if (!deserilizer->finished()) {
      deserilizer->deserializeValue(info->srcNodeUniqName);
      deserilizer->deserializeValue(info->dstNodeUniqName);
    }
    info->workloadCompatibility = true;
  }
  if (info->isVariadicRelIndex()) {
    info->fillType = FillEntryType::VARIADIC_REL;
  }
  info->setPredicateTemplates(std::move(info->predicateTemplates));
  return info;
}

void FillIndexAuxInfo::setPredicateTemplates(std::vector<std::string> values) {
  for (auto& value : values) value = binder::normalizeIndexPredicateKey(value);
  predicateTemplates = std::move(values);
}

bool FillIndexAuxInfo::isVariadicRelIndex() const {
  if (fillType == FillEntryType::LEGACY_VARIADIC_REL) {
    return true;
  }
  if (getFillType() != FillEntryType::REL || !targetColumns.empty() ||
      !indexColumnInfos.empty() || patternIR.relsInfo.size() != 1) {
    return false;
  }
  return patternIR.relsInfo.front().relType !=
         common::QueryRelType::NON_RECURSIVE;
}

std::unique_ptr<IndexAuxInfo> FillIndexAuxInfo::copy() {
  auto copied = std::make_unique<FillIndexAuxInfo>(
      getFillType(), tableName, tableUniqName, tableVariableName, targetColumns,
      indexColumnInfos, patternIR, sourceQuery);
  copied->fillUndirectedRel = fillUndirectedRel;
  copied->srcTableId = srcTableId;
  copied->dstTableId = dstTableId;
  copied->srcNodeUniqName = srcNodeUniqName;
  copied->dstNodeUniqName = dstNodeUniqName;
  copied->recursiveRelLowerBound = recursiveRelLowerBound;
  copied->recursiveRelUpperBound = recursiveRelUpperBound;
  copied->predicateExpr = predicateExpr;
  copied->predicateConjuncts = predicateConjuncts;
  copied->existsPredicates = existsPredicates;
  copied->presenceColumns = presenceColumns;
  copied->nodeOrigins = nodeOrigins;
  copied->relOrigins = relOrigins;
  copied->aggregateNumericContract = aggregateNumericContract;
  copied->aggregateForm = aggregateForm;
  copied->setPredicateTemplates(predicateTemplates);
  copied->workloadCompatibility = workloadCompatibility;
  copied->rewriteReady = rewriteReady;
  if (isVariadicRelIndex()) copied->fillType = FillEntryType::VARIADIC_REL;
  return copied;
}

std::string FillIndexAuxInfo::toCypher(const IndexCatalogEntry& indexEntry,
                                       const ToCypherInfo&) const {
  if (sourceQuery.empty()) {
    return "";
  }
  const auto quoteIdentifier = [](const std::string& identifier) {
    std::string result{"`"};
    for (const auto ch : identifier) {
      result += ch;
      if (ch == '`') {
        result += '`';
      }
    }
    result += '`';
    return result;
  };
  std::ostringstream result;
  result << "FILL " << quoteIdentifier(tableName) << '(';
  const bool derivedRelation = getFillType() == FillEntryType::REL &&
      std::none_of(patternIR.relsInfo.begin(), patternIR.relsInfo.end(),
                   [&](const auto& rel) {
                     return std::find(rel.relLabels.begin(), rel.relLabels.end(),
                                      indexEntry.getTableID()) != rel.relLabels.end();
                   });
  bool first = true;
  for (const auto& column : targetColumns) {
    if (derivedRelation || column.starts_with(
            common::InternalKeyword::FILL_AGGREGATE_STATE_PREFIX)) {
      continue;
    }
    if (!first) {
      result << ", ";
    }
    result << quoteIdentifier(column);
    first = false;
  }
  result << ") FROM (\n" << sourceQuery << "\n);";
  return result.str();
}

using binder::BoundQueryScanSource;
using binder::BoundRegularQuery;
using binder::NormalizedSingleQuery;

struct FillMatchGraphRef {
  const binder::QueryGraph* graph;
  uint32_t queryPartIdx;
  uint32_t readingClauseIdx;
  uint32_t queryGraphIdx;
};

// Return every QueryGraph belonging to every MATCH reading clause. A
// QueryPart may contain multiple MATCH clauses, and each MATCH may contain
// multiple disconnected QueryGraphs. Catalog extraction must mirror the
// complete query consumed by Planner::planQuery rather than assuming one
// graph at readingClause(0).
namespace {

class FillDependencyCollector final : public binder::ExpressionVisitor {
 public:
  binder::expression_vector getPropertyExprs() const {
    return properties_.getPropertyExprs();
  }

 protected:
  void visitChildren(const binder::Expression& expression) override {
    if (expression.expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      const auto& aggregate =
          expression.constCast<binder::AggregateFunctionExpression>();
      if (aggregate.getFunction().name == "COUNT" &&
          aggregate.getNumChildren() == 1) {
        const auto& child = aggregate.getChild(0);
        const auto type = child->getDataType().getLogicalTypeID();
        // COUNT of a bound node/relationship reads its presence, not its
        // properties. Keep traversing computed arguments (including CASE),
        // which may actually depend on a previously materialized property.
        if (child->expressionType == common::ExpressionType::PATTERN &&
            (type == common::LogicalTypeID::NODE ||
             type == common::LogicalTypeID::REL)) {
          return;
        }
      }
    }
    binder::ExpressionVisitor::visitChildren(expression);
  }

  void visitPropertyExpr(std::shared_ptr<binder::Expression> expression) override {
    properties_.visit(std::move(expression));
  }

  void visitNodeRelExpr(std::shared_ptr<binder::Expression> expression) override {
    properties_.visit(std::move(expression));
  }

  void visitSubqueryExpr(std::shared_ptr<binder::Expression> expression) override {
    properties_.visit(std::move(expression));
  }

 private:
  binder::PropertyExprCollector properties_;
};

}  // namespace

bool fillSourceReadsMaterializedProperty(const binder::BoundBaseScanSource& source,
                                        main::ClientContext* context) {
  FillDependencyCollector properties;
  const auto& regular = source.constCast<BoundQueryScanSource>().statement->constCast<BoundRegularQuery>();
  for (uint32_t qi = 0; qi < regular.getNumSingleQueries(); ++qi) {
    const auto* single = regular.getSingleQuery(qi);
    for (uint32_t pi = 0; pi < single->getNumQueryParts(); ++pi) {
      const auto* part = single->getQueryPart(pi);
      if (part->hasProjectionBody())
        for (const auto& expression : part->getProjectionBody()->getProjectionExpressions())
          properties.visit(expression);
      if (part->hasProjectionBodyPredicate()) properties.visit(part->getProjectionBodyPredicate());
      for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci)
        if (part->getReadingClause(ci)->hasPredicate()) properties.visit(part->getReadingClause(ci)->getPredicate());
    }
  }
  for (const auto* entry : context->getCatalog()->getExtendedIndexEntries(context->getTransaction())) {
    if (entry->getIndexType() != "FILL") continue;
    const auto& previous = entry->getAuxInfo().cast<FillIndexAuxInfo>();
    for (const auto& expression : properties.getPropertyExprs()) {
      const auto& property = expression->constCast<binder::PropertyExpression>();
      if (property.hasProperty(entry->getTableID()) &&
          std::find(previous.getTargetColumns().begin(), previous.getTargetColumns().end(),
                    property.getPropertyName()) != previous.getTargetColumns().end()) return true;
    }
  }
  return false;
}

static bool isComputedBooleanFillValue(const binder::Expression& expression) {
  if (expression.getDataType().getLogicalTypeID() != common::LogicalTypeID::BOOL)
    return false;
  using common::ExpressionType;
  switch (expression.expressionType) {
  case ExpressionType::OR:
  case ExpressionType::XOR:
  case ExpressionType::AND:
  case ExpressionType::NOT:
  case ExpressionType::EQUALS:
  case ExpressionType::NOT_EQUALS:
  case ExpressionType::GREATER_THAN:
  case ExpressionType::GREATER_THAN_EQUALS:
  case ExpressionType::LESS_THAN:
  case ExpressionType::LESS_THAN_EQUALS:
  case ExpressionType::IS_NULL:
  case ExpressionType::IS_NOT_NULL:
  case ExpressionType::FUNCTION:
  case ExpressionType::CASE_ELSE:
    return true;
  default:
    return false;  // Literal presence and property projections keep their routes.
  }
}

bool requiresSourceRecompute(const binder::BoundBaseScanSource& source,
                             const TableCatalogEntry& target) {
  const auto& query = source.constCast<BoundQueryScanSource>()
                          .statement->constCast<BoundRegularQuery>();
  const bool node = target.getType() == CatalogEntryType::NODE_TABLE_ENTRY;
  const auto dataStart = node ? 1u : 2u;
  const auto columns =
      const_cast<binder::BoundBaseScanSource&>(source).getColumns();
  if (columns.size() == dataStart)
    return true;
  // An ordinary Boolean projection is a value, including when it is false.
  // It has no additive/property delegate. Evaluate the unchanged source and
  // persist one independent row state; the REL runtime preserves its bag.
  if (!node && std::any_of(columns.begin() + dataStart, columns.end(),
                          [](const auto& expression) {
                            return isComputedBooleanFillValue(*expression);
                          }))
    return true;
  const bool hasPublicPresence = std::any_of(
      columns.begin() + dataStart, columns.end(), [](const auto& expression) {
        return expression->expressionType == common::ExpressionType::LITERAL &&
            expression->getDataType().getLogicalTypeID() == common::LogicalTypeID::BOOL;
      });
  // A relationship's Boolean-only projection has no aggregate or property
  // strategy to maintain it. Retain the complete source (including DISTINCT,
  // direction and predicates) and the regular binder's private presence state.
  // This helper is called again after that INT64 state has been appended, so
  // recognize the private literal as well. NODE existence and value+presence
  // projections keep their separately proved workload routes below.
  if (!node && hasPublicPresence &&
      std::all_of(columns.begin() + dataStart, columns.end(),
                  [](const auto& expression) {
        if (expression->expressionType != common::ExpressionType::LITERAL)
          return false;
        const auto& literal = expression->template constCast<binder::LiteralExpression>();
        if (literal.isNull()) return false;
        if (expression->getDataType().getLogicalTypeID() == common::LogicalTypeID::BOOL)
          return literal.getValue().template getValue<bool>();
        return expression->getDataType().getLogicalTypeID() == common::LogicalTypeID::INT64 &&
            expression->getUniqueName().starts_with(
                common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX);
      }))
    return true;
  const std::function<bool(const binder::Expression&)> containsAggregate =
      [&](const binder::Expression& expression) {
        if (expression.expressionType == common::ExpressionType::AGGREGATE_FUNCTION)
          return true;
        for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(expression))
          if (containsAggregate(*child)) return true;
        return false;
      };
  // A public presence flag does not turn a computed value into additive state.
  // In particular, generated MAX(COUNT(...)) views must retain their full
  // source query so deleting the winning subgroup can reveal the next one.
  // Select that maintenance route before the native presence interface below.
  for (size_t i = dataStart; i < columns.size(); ++i) {
    if (hasPublicPresence &&
        (columns[i]->expressionType == common::ExpressionType::FUNCTION ||
         columns[i]->expressionType == common::ExpressionType::CASE_ELSE))
      return true;
    if (columns[i]->expressionType != common::ExpressionType::AGGREGATE_FUNCTION)
      continue;
    const auto& aggregate =
        columns[i]->constCast<binder::AggregateFunctionExpression>();
    // A SUM can be zero while its group still exists. The workload route
    // does not own an independent row count for SUM + public presence; use
    // the typed source runtime and its private state instead. A SUM over a
    // prior aggregate remains eligible for the separate proved COUNT/SUM
    // pipeline normalization, which already creates that private state.
    const auto arguments = aggregate.getChildren();
    if (hasPublicPresence && aggregate.getFunction().name == "SUM" &&
        std::none_of(arguments.begin(), arguments.end(),
                     [&](const auto& child) { return containsAggregate(*child); }))
      return true;
    if (aggregate.getFunction().name == "MIN" ||
        aggregate.getFunction().name == "MAX")
      return true;
  }
  // Presence outputs select the proven native workload interface, including
  // staged-count guards and declared functional projections.
  for (size_t i = dataStart; i < columns.size(); ++i)
    if (columns[i]->expressionType == common::ExpressionType::LITERAL &&
        columns[i]->getDataType().getLogicalTypeID() == common::LogicalTypeID::BOOL)
      return false;
  for (size_t i = dataStart; i < columns.size(); ++i) {
    const auto& expr = columns[i];
    if (expr->expressionType == common::ExpressionType::LITERAL &&
        expr->getUniqueName().starts_with(
            common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX))
      return true;
    const auto type = expr->getDataType().getLogicalTypeID();
    if (expr->expressionType == common::ExpressionType::PROPERTY)
      return true;
    if ((type == common::LogicalTypeID::DOUBLE || type == common::LogicalTypeID::FLOAT) &&
        !isMaintainableFillFinalizer(*expr) &&
        !(expr->expressionType == common::ExpressionType::AGGREGATE_FUNCTION &&
          expr->getNumChildren() == 1 && isExactHalfIntegerFillExpression(*expr->getChild(0))))
      return true;
    if (expr->expressionType == common::ExpressionType::AGGREGATE_FUNCTION &&
        expr->constCast<binder::AggregateFunctionExpression>().isDistinct())
      return true;
    if (expr->expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      const auto& aggregate=expr->constCast<binder::AggregateFunctionExpression>();
      if (aggregate.getFunction().name == "MIN" ||
          aggregate.getFunction().name == "MAX")
        return true;
      if(aggregate.getFunction().name=="SUM"&&aggregate.getNumChildren()==1&&
         aggregate.getChild(0)->getDataType()!=aggregate.getDataType())
        return true;  // Preserve the binder's widening via typed source evaluation.
    }
  }
  std::unordered_set<std::string> unionVariables;
  for (uint32_t qi = 0; qi < query.getNumSingleQueries(); ++qi) {
    const auto* single = query.getSingleQuery(qi);
    for (uint32_t pi = 0; pi < single->getNumQueryParts(); ++pi)
      for (uint32_t ci = 0;
           ci < single->getQueryPart(pi)->getNumReadingClause(); ++ci) {
        const auto* clause = single->getQueryPart(pi)->getReadingClause(ci);
        if (clause->getClauseType() != common::ClauseType::MATCH)
          continue;
        const auto* graphs = clause->constCast<binder::BoundMatchClause>()
                                 .getQueryGraphCollection();
        if (graphs->getQueryRels().empty())
          return true;
        for (const auto& node : graphs->getQueryNodes())
          if (node->getNumEntries() > 1)
            unionVariables.insert(node->getUniqueName());
        for (const auto& rel : graphs->getQueryRels())
          if (rel->getNumEntries() > 1)
            unionVariables.insert(rel->getUniqueName());
      }
  }
  auto readsUnionProperty = [&](const std::shared_ptr<binder::Expression>&
                                    expression) {
    binder::PropertyExprCollector properties;
    properties.visit(expression);
    for (const auto& item : properties.getPropertyExprs())
      if (unionVariables.contains(
              item->constCast<binder::PropertyExpression>().getVariableName()))
        return true;
    return false;
  };
  for (const auto& column : columns)
    if (readsUnionProperty(column))
      return true;
  for (uint32_t q = 0; q < query.getNumSingleQueries(); ++q) {
    const auto* single = query.getSingleQuery(q);
    for (uint32_t p = 0; p < single->getNumQueryParts(); ++p) {
      const auto* part = single->getQueryPart(p);
      if (part->hasProjectionBody() && part->getProjectionBody()->isDistinct())
        return true;
      for (uint32_t c = 0; c < part->getNumReadingClause(); ++c) {
        const auto* clause = part->getReadingClause(c);
        if (clause->getClauseType() != common::ClauseType::MATCH)
          continue;
        if (clause->hasPredicate() && readsUnionProperty(clause->getPredicate()))
          return true;
        const auto* graphs = clause->constCast<binder::BoundMatchClause>()
                                 .getQueryGraphCollection();
        if (graphs->getQueryRels().empty()) return true;
        for (const auto& rel : graphs->getQueryRels()) {
          if (rel->isRecursive())
            return true;
          if (!node &&
              std::any_of(rel->getEntries().begin(), rel->getEntries().end(),
                          [&](const auto* entry) {
                            return entry->getTableID() == target.getTableID();
                          })) {
            const auto& edge = target.constCast<RelTableCatalogEntry>();
            if (!edge.isSingleMultiplicity(common::RelDataDirection::FWD) &&
                !edge.isSingleMultiplicity(common::RelDataDirection::BWD))
              return true;
          }
        }
      }
    }
  }
  return false;
}

static std::vector<FillMatchGraphRef> collectFillMatchGraphs(
    const NormalizedSingleQuery& singleQuery, bool recompute = false) {
  std::vector<FillMatchGraphRef> result;
  bool sawAggregation = false;
  std::unordered_map<std::string, std::string> variableIdentities;
  auto rememberIdentity = [&](const auto& element) {
    const auto& name = element->getVariableName();
    if (name.empty()) {
      return;
    }
    const auto [found, inserted] =
        variableIdentities.emplace(name, element->getUniqueName());
    if (!inserted && found->second != element->getUniqueName()) {
      THROW_BINDER_EXCEPTION(
          "FILL cannot reuse a variable name for a different graph object "
          "across query parts until scoped lineage matching is supported.");
    }
  };
  for (uint32_t pi = 0; pi < singleQuery.getNumQueryParts(); ++pi) {
    const auto* queryPart = singleQuery.getQueryPart(pi);
    if (queryPart->hasUpdatingClause()) {
      THROW_BINDER_EXCEPTION("FILL source queries must be read-only.");
    }
    if (queryPart->hasProjectionBody()) {
      const auto* body = queryPart->getProjectionBody();
      if ((!recompute && body->isDistinct()) || body->hasSkipOrLimit()) {
        THROW_BINDER_EXCEPTION(
            "DISTINCT, SKIP and LIMIT are not supported in incremental FILL "
            "source queries.");
      }
      if (!recompute && sawAggregation && queryPart->hasReadingClause()) {
        THROW_BINDER_EXCEPTION(
            "MATCH after aggregation is not supported in incremental FILL "
            "source queries.");
      }
      if (body->hasAggregateExpressions()) {
        if (sawAggregation && !recompute) {
          THROW_BINDER_EXCEPTION(
              "Multiple aggregation stages are not supported in FILL sources.");
        }
        sawAggregation = true;
      }
    }
    for (uint32_t ri = 0; ri < queryPart->getNumReadingClause(); ++ri) {
      const auto* clause = queryPart->getReadingClause(ri);
      if (clause->getClauseType() != common::ClauseType::MATCH) {
        THROW_BINDER_EXCEPTION(
            "Incremental FILL currently supports MATCH reading clauses only.");
      }
      const auto* matchClause =
          clause->constPtrCast<binder::BoundMatchClause>();
      if (!recompute &&
          matchClause->getMatchClauseType() != common::MatchClauseType::MATCH) {
        THROW_BINDER_EXCEPTION(
            "OPTIONAL MATCH is not supported in incremental FILL source queries.");
      }
      const auto* collection = matchClause->getQueryGraphCollection();
      for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi) {
        const auto* graph = collection->getQueryGraph(gi);
        for (uint32_t ni = 0; ni < graph->getNumQueryNodes(); ++ni) {
          rememberIdentity(graph->getQueryNode(ni));
        }
        for (uint32_t ei = 0; ei < graph->getNumQueryRels(); ++ei) {
          rememberIdentity(graph->getQueryRel(ei));
          if (!recompute && graph->getQueryRel(ei)->isRecursive()) {
            THROW_BINDER_EXCEPTION(
                "Recursive FILL is not supported until maintenance preserves "
                "TRAIL path witness multiplicity.");
          }
        }
        result.push_back(
            {graph, pi, ri, gi});
      }
    }
  }
  return result;
}

// Helper: collect all QueryGraphs from all QueryParts, convert each to a
// PatternIR, and merge them into a single full PatternIR.
static common::PatternIR buildFullPatternIR(
    const NormalizedSingleQuery& singleQuery,
    std::vector<common::PatternIR>& outPerGraphPatterns,
    std::vector<FillPatternElementOrigin>& outNodeOrigins,
    std::vector<FillPatternElementOrigin>& outRelOrigins,
    bool recompute = false) {
  outPerGraphPatterns.clear();
  outNodeOrigins.clear();
  outRelOrigins.clear();
  const auto graphRefs = collectFillMatchGraphs(singleQuery, recompute);
  for (const auto& ref : graphRefs) {
    outPerGraphPatterns.emplace_back(
        common::convertGraphPatternToIR(*ref.graph));
  }
  if (outPerGraphPatterns.empty()) {
    return common::PatternIR{};
  }

  std::vector<std::vector<uint32_t>> nodeRemaps;
  common::PatternIR merged;
  if (outPerGraphPatterns.size() == 1) {
    merged = outPerGraphPatterns[0];
    nodeRemaps.resize(1);
    nodeRemaps[0].resize(merged.nodesInfo.size());
    for (uint32_t i = 0; i < merged.nodesInfo.size(); ++i) {
      nodeRemaps[0][i] = i;
    }
  } else {
    merged = common::PatternIR::merge(outPerGraphPatterns, nodeRemaps);
  }

  uint32_t relBase = 0;
  for (uint32_t patternIdx = 0; patternIdx < outPerGraphPatterns.size();
       ++patternIdx) {
    const auto& pattern = outPerGraphPatterns[patternIdx];
    const auto& ref = graphRefs[patternIdx];
    for (uint32_t localIdx = 0; localIdx < pattern.nodesInfo.size();
         ++localIdx) {
      outNodeOrigins.push_back(
          {ref.queryPartIdx, ref.readingClauseIdx, ref.queryGraphIdx, localIdx,
           nodeRemaps[patternIdx][localIdx],
           pattern.nodesInfo[localIdx].uniqName});
    }
    // PatternIR::merge preserves every relationship occurrence and appends
    // each pattern's rels in input order, so the merged relationship remap is
    // the cumulative local offset.
    for (uint32_t localIdx = 0; localIdx < pattern.relsInfo.size();
         ++localIdx) {
      outRelOrigins.push_back(
          {ref.queryPartIdx, ref.readingClauseIdx, ref.queryGraphIdx, localIdx,
           relBase + localIdx, pattern.relsInfo[localIdx].uniqName});
    }
    relBase += static_cast<uint32_t>(pattern.relsInfo.size());
  }
  for (size_t ni = 0; ni < merged.nodesInfo.size(); ++ni) {
    if (recompute && merged.nodesInfo.size() == 1 && merged.relsInfo.empty())
      break;
    if (std::none_of(merged.relsInfo.begin(), merged.relsInfo.end(),
                     [&](const auto& rel) {
                       return rel.srcNodeIdx == ni || rel.dstNodeIdx == ni;
                     })) {
      THROW_BINDER_EXCEPTION(
          "FILL sources with isolated pattern nodes require vertex insertion "
          "and deletion maintenance, which is not yet supported.");
    }
  }
  return merged;
}

// Convert every MATCH-WHERE predicate in the FILL source to the same tagged
// expression representation used by aggregate arguments, then combine them
// with AND. Predicates after WITH are deliberately rejected: they can refer
// to projected aggregates and do not have per-candidate-tuple semantics.
struct FillPredicateAsts {
  std::shared_ptr<::common::Expression> combined;
  std::vector<std::shared_ptr<::common::Expression>> conjuncts;
  std::vector<FillExistsPredicate> existsPredicates;
};

std::optional<FillExistsPredicate> extractFillExistsPredicate(
    const binder::Expression& expression,
    const common::PatternIR& fullPatternIR) {
  auto* term = &expression;
  bool negated = false;
  while (term->expressionType == common::ExpressionType::NOT &&
         term->getNumChildren() == 1) {
    negated = !negated;
    term = term->getChild(0).get();
  }
  if (term->expressionType != common::ExpressionType::SUBQUERY) {
    return std::nullopt;
  }
  const auto& subquery = term->constCast<binder::SubqueryExpression>();
  const auto* graph = subquery.getQueryGraphCollection();
  const auto rels = graph->getQueryRels();
  if (subquery.getSubqueryType() != common::SubqueryType::EXISTS ||
      subquery.hasWhereExpression() || rels.size() != 1 ||
      rels.front()->isRecursive() || rels.front()->getEntries().empty() ||
      (rels.front()->getDirectionType() != binder::RelDirectionType::SINGLE &&
       rels.front()->getDirectionType() != binder::RelDirectionType::BOTH)) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL EXISTS requires one fixed-hop relationship with "
        "both endpoints bound by the outer MATCH and no subquery WHERE.");
  }
  const auto& rel = rels.front();
  const auto subqueryNodes = graph->getQueryNodes();
  const auto src = fullPatternIR.nodeUniqNameToPos.find(rel->getSrcNodeName());
  const auto dst = fullPatternIR.nodeUniqNameToPos.find(rel->getDstNodeName());
  if (src == fullPatternIR.nodeUniqNameToPos.end() ||
      dst == fullPatternIR.nodeUniqNameToPos.end() ||
      std::any_of(subqueryNodes.begin(), subqueryNodes.end(),
                  [&](const auto& node) {
        return node->getUniqueName() != rel->getSrcNodeName() &&
               node->getUniqueName() != rel->getDstNodeName();
      })) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL EXISTS requires both relationship endpoints to be "
        "bound by the outer MATCH; local subquery vertices are unsupported.");
  }
  if (fullPatternIR.relsInfo.empty() ||
      std::any_of(fullPatternIR.relsInfo.begin(),
                  fullPatternIR.relsInfo.end(), [](const auto& outerRel) {
        return outerRel.relType != common::QueryRelType::NON_RECURSIVE;
      })) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL EXISTS requires fixed-hop outer relationships.");
  }
  std::vector<bool> reachable(fullPatternIR.nodesInfo.size(), false);
  reachable[src->second] = true;
  bool changed;
  do {
    changed = false;
    for (const auto& outerRel : fullPatternIR.relsInfo) {
      if (reachable[outerRel.srcNodeIdx] != reachable[outerRel.dstNodeIdx]) {
        reachable[outerRel.srcNodeIdx] = true;
        reachable[outerRel.dstNodeIdx] = true;
        changed = true;
      }
    }
  } while (changed);
  if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL EXISTS requires a connected outer MATCH pattern; "
        "disconnected cross products are unsupported.");
  }
  FillExistsPredicate predicate;
  predicate.srcNodePos = src->second;
  predicate.dstNodePos = dst->second;
  predicate.negated = negated;
  predicate.directed =
      rel->getDirectionType() == binder::RelDirectionType::SINGLE;
  predicate.srcTableIDs = rel->getSrcNode()->getTableIDs();
  predicate.dstTableIDs = rel->getDstNode()->getTableIDs();
  predicate.relTableIDs = rel->getTableIDs();
  return predicate;
}

std::string fillExistsPredicateKey(FillExistsPredicate predicate) {
  if (!predicate.directed && predicate.dstNodePos < predicate.srcNodePos) {
    std::swap(predicate.srcNodePos, predicate.dstNodePos);
    std::swap(predicate.srcTableIDs, predicate.dstTableIDs);
  }
  std::ostringstream out;
  out << "@exists-fill-v1:" << predicate.negated << ':' << predicate.directed
      << ':' << predicate.srcNodePos << ':' << predicate.dstNodePos;
  for (auto* ids : {&predicate.srcTableIDs, &predicate.dstTableIDs,
                    &predicate.relTableIDs}) {
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    out << '|';
    for (auto id : *ids) out << id << ',';
  }
  return out.str();
}

static FillPredicateAsts buildFillPredicateAsts(
    const NormalizedSingleQuery& singleQuery,
    const common::PatternIR& fullPatternIR) {
  auto resolveTag = [&](const std::string& name) -> std::optional<uint32_t> {
    auto uniqueIt = fullPatternIR.nodeUniqNameToPos.find(name);
    if (uniqueIt != fullPatternIR.nodeUniqNameToPos.end()) {
      return static_cast<uint32_t>(uniqueIt->second);
    }
    std::optional<uint32_t> match;
    for (size_t i = 0; i < fullPatternIR.nodesInfo.size(); ++i) {
      if (fullPatternIR.nodesInfo[i].varName != name)
        continue;
      if (match.has_value() && *match != i) {
        THROW_BINDER_EXCEPTION(
            "Ambiguous node variable '" + name +
            "' in FILL WHERE predicate across multiple MATCH clauses");
      }
      match = static_cast<uint32_t>(i);
    }
    if (match.has_value())
      return match;

    auto relUniqueIt = fullPatternIR.relUniqueNameToPos.find(name);
    if (relUniqueIt != fullPatternIR.relUniqueNameToPos.end()) {
      return static_cast<uint32_t>(fullPatternIR.nodesInfo.size() +
                                   relUniqueIt->second);
    }
    for (size_t i = 0; i < fullPatternIR.relsInfo.size(); ++i) {
      if (fullPatternIR.relsInfo[i].varName != name)
        continue;
      if (match.has_value() &&
          *match != fullPatternIR.nodesInfo.size() + i) {
        THROW_BINDER_EXCEPTION(
            "Ambiguous rel variable '" + name +
            "' in FILL WHERE predicate across multiple MATCH clauses");
      }
      match = static_cast<uint32_t>(fullPatternIR.nodesInfo.size() + i);
    }
    return match;
  };

  FillPredicateAsts result;
  for (uint32_t pi = 0; pi < singleQuery.getNumQueryParts(); ++pi) {
    const auto* queryPart = singleQuery.getQueryPart(pi);
    if (queryPart->hasProjectionBodyPredicate()) {
      THROW_BINDER_EXCEPTION(
          "WHERE after WITH is not supported by incremental FILL maintenance; "
          "place tuple predicates on MATCH");
    }
    for (uint32_t ri = 0; ri < queryPart->getNumReadingClause(); ++ri) {
      const auto* clause = queryPart->getReadingClause(ri);
      if (clause->getClauseType() != common::ClauseType::MATCH ||
          !clause->hasPredicate()) {
        continue;
      }
      for (const auto& conjunct : clause->getConjunctivePredicates()) {
        if (auto exists = extractFillExistsPredicate(*conjunct, fullPatternIR)) {
          result.existsPredicates.push_back(std::move(*exists));
          continue;
        }
        result.conjuncts.push_back(
            ConvertFillIndexExprToAst(*conjunct, resolveTag));
      }
    }
  }
  if (result.conjuncts.empty()) {
    // A non-null predicate disables degree-only/count-only maintenance paths
    // that cannot account for semijoin and antijoin transitions.
    if (!result.existsPredicates.empty()) {
      const binder::LiteralExpression truth{common::Value{true}, "true"};
      result.combined = ConvertFillIndexExprToAst(truth, resolveTag);
    }
    return result;
  }

  auto combined = result.conjuncts.front();
  for (size_t i = 1; i < result.conjuncts.size(); ++i) {
    auto next = std::make_shared<::common::Expression>();
    next->add_operators()->set_brace(
        ::common::ExprOpr::Brace::ExprOpr_Brace_LEFT_BRACE);
    for (const auto& opr : combined->operators()) {
      next->add_operators()->CopyFrom(opr);
    }
    next->add_operators()->set_logical(::common::Logical::AND);
    for (const auto& opr : result.conjuncts[i]->operators()) {
      next->add_operators()->CopyFrom(opr);
    }
    next->add_operators()->set_brace(
        ::common::ExprOpr::Brace::ExprOpr_Brace_RIGHT_BRACE);
    combined = std::move(next);
  }
  result.combined = std::move(combined);
  return result;
}

void setFillPredicateMetadata(FillIndexAuxInfo& info,
                              const NormalizedSingleQuery& query) {
  auto predicates = buildFillPredicateAsts(query, info.getPatternIR());
  info.setPredicateExpr(std::move(predicates.combined));
  info.setPredicateConjuncts(std::move(predicates.conjuncts));
  info.setExistsPredicates(std::move(predicates.existsPredicates));
}

static std::string fillPredicateSignature(const FillIndexAuxInfo& info) {
  std::ostringstream signature;
  if (info.getPredicateExpr()) {
    signature << "|where=" << info.getPredicateExpr()->SerializeAsString();
  }
  for (const auto& predicate : info.getExistsPredicates()) {
    signature << "|exists=" << predicate.srcNodePos << ','
              << predicate.dstNodePos << ',' << predicate.negated << ','
              << predicate.directed << ',';
    appendVectorSignature(signature, predicate.srcTableIDs);
    appendVectorSignature(signature, predicate.dstTableIDs);
    appendVectorSignature(signature, predicate.relTableIDs);
  }
  return signature.str();
}

std::vector<std::string> collectPredicateTemplates(
    const binder::NormalizedSingleQuery& singleQuery,
    const common::PatternIR& patternIR, catalog::Catalog* catalog,
    const transaction::Transaction* transaction) {
  auto templates = collectWorkloadFillPredicates(singleQuery, patternIR);
  for (auto partIdx = 0u; partIdx < singleQuery.getNumQueryParts(); ++partIdx) {
    const auto* queryPart = singleQuery.getQueryPart(partIdx);
    if (queryPart->hasProjectionBodyPredicate())
      templates.push_back("@post-projection:" +
                          queryPart->getProjectionBodyPredicate()->toString());
  }

  std::sort(templates.begin(), templates.end());
  templates.erase(std::unique(templates.begin(), templates.end()),
                  templates.end());
  return templates;
}

static std::optional<uint32_t> resolvePatternAstTag(
    const common::PatternIR& pattern,
    const std::unordered_map<std::string, std::string>& nodeVarToUnique,
    const std::unordered_map<std::string, std::string>& relVarToUnique,
    const std::string& name) {
  std::string uniqName = name;
  auto nodeVarIt = nodeVarToUnique.find(name);
  if (nodeVarIt != nodeVarToUnique.end())
    uniqName = nodeVarIt->second;
  auto nodePosIt = pattern.nodeUniqNameToPos.find(uniqName);
  if (nodePosIt != pattern.nodeUniqNameToPos.end()) {
    return static_cast<uint32_t>(nodePosIt->second);
  }

  uniqName = name;
  auto relVarIt = relVarToUnique.find(name);
  if (relVarIt != relVarToUnique.end())
    uniqName = relVarIt->second;
  auto relPosIt = pattern.relUniqueNameToPos.find(uniqName);
  if (relPosIt != pattern.relUniqueNameToPos.end()) {
    return static_cast<uint32_t>(pattern.nodesInfo.size() + relPosIt->second);
  }
  return std::nullopt;
}

static std::string normalizeFillAggregateFunctionName(std::string funcName) {
  std::transform(funcName.begin(), funcName.end(), funcName.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return funcName == "COUNT_STAR" ? "COUNT" : funcName;
}

namespace {

struct FillAffineShape {
  // A null base denotes a numeric constant. Nonlinear/multi-input row
  // expressions remain opaque base values; only their outer affine shell
  // is classified here, with no algebraic or NULL-changing distribution.
  const binder::Expression* base;
  bool transformed;
};

FillAffineShape getFillAffineShape(const binder::Expression& expression) {
  using common::ExpressionType;
  if (expression.expressionType == ExpressionType::LITERAL &&
      !expression.constCast<binder::LiteralExpression>().isNull()) {
    const auto type = expression.getDataType().getLogicalTypeID();
    if (type == common::LogicalTypeID::INT8 ||
        type == common::LogicalTypeID::INT16 ||
        type == common::LogicalTypeID::INT32 ||
        type == common::LogicalTypeID::INT64 ||
        type == common::LogicalTypeID::FLOAT ||
        type == common::LogicalTypeID::DOUBLE) {
      return {nullptr, false};
    }
  }
  if (expression.expressionType != ExpressionType::FUNCTION)
    return {&expression, false};
  const auto& function =
      expression.constCast<binder::ScalarFunctionExpression>();
  const auto& name = function.getFunction().name;
  if (name != "+" && name != "-" && name != "*")
    return {&expression, false};
  if (expression.getNumChildren() == 1 && (name == "+" || name == "-")) {
    auto child = getFillAffineShape(*expression.getChild(0));
    return {child.base, child.base != nullptr};
  }
  if (expression.getNumChildren() != 2)
    return {&expression, false};
  const auto left = getFillAffineShape(*expression.getChild(0));
  const auto right = getFillAffineShape(*expression.getChild(1));
  if (!left.base && !right.base)
    return {nullptr, false};
  if (!left.base || !right.base)
    return {left.base ? left.base : right.base, true};
  if (name != "*" &&
      left.base->getUniqueName() == right.base->getUniqueName())
    return {left.base, true};
  return {&expression, false};
}

FillAggregateForm getBoundFillAggregateForm(
    const binder::expression_vector& columns, size_t dataStart) {
  for (size_t i = dataStart; i < columns.size(); ++i) {
    if (!isCanonicalFillAggregate(*columns[i]))
      return FillAggregateForm::LEGACY_COMPUTED;
  }
  return FillAggregateForm::CANONICAL_BASE;
}

}  // namespace

bool isCanonicalFillAggregate(const binder::Expression& expression) {
  if (expression.expressionType != common::ExpressionType::AGGREGATE_FUNCTION)
    return false;
  const auto& aggregate =
      expression.constCast<binder::AggregateFunctionExpression>();
  const auto functionName =
      normalizeFillAggregateFunctionName(aggregate.getFunction().name);
  if (aggregate.isDistinct() && functionName != "COUNT")
    return false;
  if (functionName == "COUNT")
    return true;
  if (functionName != "SUM" || aggregate.getNumChildren() != 1)
    return false;
  return !getFillAffineShape(*aggregate.getChild(0)).transformed;
}

bool isFillExpressionNonNull(const binder::Expression& expression) {
  switch (expression.expressionType) {
  case common::ExpressionType::LITERAL:
    return !expression.constCast<binder::LiteralExpression>().isNull();
  case common::ExpressionType::PROPERTY: {
    // Stored properties are nonnullable. Missing properties on heterogeneous
    // labels and OPTIONAL bindings are excluded separately by source checks.
    const auto& property = expression.constCast<binder::PropertyExpression>();
    return property.isSingleLabel() &&
           property.hasProperty(property.getSingleTableID());
  }
  case common::ExpressionType::AGGREGATE_FUNCTION: {
    // A preceding query stage may pass its aggregate expression directly to
    // a later SUM. COUNT always produces an integer (zero on empty/all-NULL
    // input), irrespective of its operand's nullability. Other aggregates,
    // especially SUM, must retain their possibly-NULL result contract.
    const auto& aggregate =
        expression.constCast<binder::AggregateFunctionExpression>();
    return normalizeFillAggregateFunctionName(aggregate.getFunction().name) ==
           "COUNT";
  }
  case common::ExpressionType::IS_NULL:
  case common::ExpressionType::IS_NOT_NULL:
    return true;
  case common::ExpressionType::CASE_ELSE: {
    const auto& conditional = expression.constCast<binder::CaseExpression>();
    if (!conditional.getElseExpression() ||
        !isFillExpressionNonNull(*conditional.getElseExpression())) {
      return false;
    }
    for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i) {
      if (!isFillExpressionNonNull(
              *conditional.getCaseAlternative(i)->thenExpression)) {
        return false;
      }
    }
    return true;
  }
  case common::ExpressionType::FUNCTION: {
    const auto& scalar = expression.constCast<binder::ScalarFunctionExpression>();
    const auto& name = scalar.getFunction().name;
    if (name != "+" && name != "-" && name != "*") {
      return false;
    }
    const auto children = expression.getChildren();
    return std::all_of(children.begin(), children.end(), [](const auto& child) {
                         return isFillExpressionNonNull(*child);
                       });
  }
  default:
    return false;
  }
}

namespace {

std::optional<long double> fillNumericLiteral(const binder::Expression& expression) {
  if (expression.expressionType != common::ExpressionType::LITERAL)
    return std::nullopt;
  const auto& literal = expression.constCast<binder::LiteralExpression>();
  if (literal.isNull()) return std::nullopt;
  const auto& value = literal.getValue();
  switch (expression.getDataType().getLogicalTypeID()) {
  case common::LogicalTypeID::INT32: return value.getValue<int32_t>();
  case common::LogicalTypeID::INT64: return value.getValue<int64_t>();
  case common::LogicalTypeID::FLOAT: return value.getValue<float>();
  case common::LogicalTypeID::DOUBLE: return value.getValue<double>();
  default: return std::nullopt;
  }
}

using FillNumericBounds = std::pair<long double, long double>;

// Conservative domain proof for finalizers. It only consumes aggregate state
// and constants, and excludes zero divisors / negative square-root inputs.
std::optional<FillNumericBounds> fillFinalizerBounds(
    const binder::Expression& expression) {
  using common::ExpressionType;
  if (const auto literal = fillNumericLiteral(expression)) {
    if (!std::isfinite(*literal)) return std::nullopt;
    return FillNumericBounds{*literal, *literal};
  }
  if (expression.expressionType == ExpressionType::AGGREGATE_FUNCTION) {
    const auto& aggregate = expression.constCast<binder::AggregateFunctionExpression>();
    if (aggregate.isDistinct()) return std::nullopt;
    const auto name = normalizeFillAggregateFunctionName(aggregate.getFunction().name);
    if (name == "COUNT")
      return FillNumericBounds{0, std::numeric_limits<int64_t>::max()};
    if (name == "SUM" && aggregate.getNumChildren() == 1 &&
        isExactHalfIntegerFillExpression(*aggregate.getChild(0)))
      return FillNumericBounds{0, 0x1p52L};
    if (name == "SUM" && aggregate.getNumChildren() == 1 &&
        aggregate.getDataType().getLogicalTypeID() == common::LogicalTypeID::INT64)
      return FillNumericBounds{std::numeric_limits<int64_t>::min(),
                               std::numeric_limits<int64_t>::max()};
    return std::nullopt;
  }
  if (expression.expressionType == ExpressionType::CASE_ELSE) {
    const auto& conditional = expression.constCast<binder::CaseExpression>();
    if (!conditional.getElseExpression()) return std::nullopt;
    auto bounds = fillFinalizerBounds(*conditional.getElseExpression());
    if (!bounds) return std::nullopt;
    for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i) {
      const auto* branch = conditional.getCaseAlternative(i);
      if (!fillFinalizerBounds(*branch->whenExpression)) return std::nullopt;
      const auto branchBounds = fillFinalizerBounds(*branch->thenExpression);
      if (!branchBounds) return std::nullopt;
      bounds->first = std::min(bounds->first, branchBounds->first);
      bounds->second = std::max(bounds->second, branchBounds->second);
    }
    return bounds;
  }
  const auto children = binder::ExpressionChildrenCollector::collectChildren(expression);
  if (expression.expressionType == ExpressionType::EQUALS ||
      expression.expressionType == ExpressionType::NOT_EQUALS ||
      expression.expressionType == ExpressionType::GREATER_THAN ||
      expression.expressionType == ExpressionType::GREATER_THAN_EQUALS ||
      expression.expressionType == ExpressionType::LESS_THAN ||
      expression.expressionType == ExpressionType::LESS_THAN_EQUALS ||
      expression.expressionType == ExpressionType::AND ||
      expression.expressionType == ExpressionType::OR) {
    for (const auto& child : children)
      if (!fillFinalizerBounds(*child)) return std::nullopt;
    return FillNumericBounds{0, 1};
  }
  if (expression.expressionType != ExpressionType::FUNCTION || children.empty())
    return std::nullopt;
  const auto& name = expression.constCast<binder::ScalarFunctionExpression>().getFunction().name;
  const auto left = fillFinalizerBounds(*children[0]);
  if (!left) return std::nullopt;
  if (name.starts_with("CAST") &&
      expression.getDataType().getLogicalTypeID() == common::LogicalTypeID::DOUBLE)
    return left;
  if (name == "SQRT" && children.size() == 1 && left->first >= 0)
    return FillNumericBounds{std::sqrt(left->first), std::sqrt(left->second)};
  if (name == "ROUND" && children.size() == 2) {
    const auto digits = fillNumericLiteral(*children[1]);
    if (!digits || *digits != 0) return std::nullopt;
    return FillNumericBounds{std::floor(left->first), std::ceil(left->second)};
  }
  if (children.size() != 2) return std::nullopt;
  const auto right = fillFinalizerBounds(*children[1]);
  if (!right) return std::nullopt;
  if (name == "+")
    return FillNumericBounds{left->first + right->first, left->second + right->second};
  if (name == "-")
    return FillNumericBounds{left->first - right->second, left->second - right->first};
  if (name == "*" || (name == "/" && (right->first > 0 || right->second < 0))) {
    const auto op = [&](long double a, long double b) { return name == "*" ? a*b : a/b; };
    const auto values = {op(left->first, right->first), op(left->first, right->second),
                         op(left->second, right->first), op(left->second, right->second)};
    return FillNumericBounds{*std::min_element(values.begin(), values.end()),
                             *std::max_element(values.begin(), values.end())};
  }
  return std::nullopt;
}

}  // namespace

bool isExactHalfIntegerFillExpression(const binder::Expression& expression) {
  if (const auto literal = fillNumericLiteral(expression))
    return *literal == 0 || *literal == 0.5L || *literal == 1;
  if (expression.expressionType != common::ExpressionType::CASE_ELSE) return false;
  const auto& conditional = expression.constCast<binder::CaseExpression>();
  if (!conditional.getElseExpression() ||
      !isExactHalfIntegerFillExpression(*conditional.getElseExpression())) return false;
  for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i)
    if (!isExactHalfIntegerFillExpression(*conditional.getCaseAlternative(i)->thenExpression))
      return false;
  return true;
}

bool isMaintainableFillFinalizer(const binder::Expression& expression) {
  if (expression.expressionType == common::ExpressionType::AGGREGATE_FUNCTION)
    return false;
  std::unordered_set<std::string> aggregates;
  std::function<void(const binder::Expression&)> collect = [&](const auto& value) {
    if (value.expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      aggregates.insert(value.getUniqueName());
      return;
    }
    for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(value))
      collect(*child);
  };
  collect(expression);
  return aggregates.size() == 1 && fillFinalizerBounds(expression).has_value();
}

static bool nonNullSourceValue(const binder::Expression& expression,
                               const NormalizedSingleQuery& source) {
  if (expression.expressionType == common::ExpressionType::PROPERTY) {
    const auto& property = expression.constCast<binder::PropertyExpression>();
    for (const auto& ref : collectFillMatchGraphs(source, true)) {
      const auto* clause = source.getQueryPart(ref.queryPartIdx)
                               ->getReadingClause(ref.readingClauseIdx);
      if (clause->constCast<binder::BoundMatchClause>().getMatchClauseType() !=
          common::MatchClauseType::MATCH)
        continue;  // An OPTIONAL-only binding can be NULL despite its schema.
      if (ref.graph->containsQueryNode(property.getVariableName())) {
        const auto& entries =
            ref.graph->getQueryNode(property.getVariableName())->getEntries();
        return std::all_of(entries.begin(), entries.end(),
                           [&](const auto* entry) {
                             return property.hasProperty(entry->getTableID());
                           });
      }
      if (ref.graph->containsQueryRel(property.getVariableName())) {
        const auto& entries =
            ref.graph->getQueryRel(property.getVariableName())->getEntries();
        return std::all_of(entries.begin(), entries.end(),
                           [&](const auto* entry) {
                             return property.hasProperty(entry->getTableID());
                           });
      }
    }
    return false;
  }
  if (expression.expressionType == common::ExpressionType::CASE_ELSE) {
    const auto& value = expression.constCast<binder::CaseExpression>();
    if (!value.getElseExpression() ||
        !nonNullSourceValue(*value.getElseExpression(), source))
      return false;
    for (size_t i = 0; i < value.getNumCaseAlternatives(); ++i)
      if (!nonNullSourceValue(*value.getCaseAlternative(i)->thenExpression,
                              source))
        return false;
    return true;
  }
  if (expression.expressionType == common::ExpressionType::FUNCTION) {
    const auto& function =
        expression.constCast<binder::ScalarFunctionExpression>()
            .getFunction()
            .name;
    if (function != "+" && function != "-" && function != "*")
      return false;
    for (const auto& child : expression.getChildren())
      if (!nonNullSourceValue(*child, source))
        return false;
    return true;
  }
  return isFillExpressionNonNull(expression);
}

bool isFillSourceExpressionNonNull(
    const binder::Expression& expression,
    const binder::NormalizedSingleQuery& source) {
  return nonNullSourceValue(expression, source);
}

// A stored Boolean has no NULL-validity bitmap. Prove only known total
// operators over non-NULL operands; unknown scalar functions remain rejected.
// This separate proof does not broaden the numeric SUM/finalizer contract.
static bool nonNullBooleanFillOperand(const binder::Expression& expression,
                                      const NormalizedSingleQuery& source) {
  using common::ExpressionType;
  switch (expression.expressionType) {
  case ExpressionType::IS_NULL:
  case ExpressionType::IS_NOT_NULL:
    return true;  // The operand itself may be NULL.
  case ExpressionType::OR:
  case ExpressionType::XOR:
  case ExpressionType::AND:
  case ExpressionType::NOT:
  case ExpressionType::EQUALS:
  case ExpressionType::NOT_EQUALS:
  case ExpressionType::GREATER_THAN:
  case ExpressionType::GREATER_THAN_EQUALS:
  case ExpressionType::LESS_THAN:
  case ExpressionType::LESS_THAN_EQUALS: {
    const auto children = expression.getChildren();
    return !children.empty() &&
        std::all_of(children.begin(), children.end(), [&](const auto& child) {
          return nonNullBooleanFillOperand(*child, source);
        });
  }
  case ExpressionType::CASE_ELSE: {
    const auto& value = expression.constCast<binder::CaseExpression>();
    if (!value.getElseExpression() ||
        !nonNullBooleanFillOperand(*value.getElseExpression(), source))
      return false;
    for (size_t i = 0; i < value.getNumCaseAlternatives(); ++i)
      if (!nonNullBooleanFillOperand(
              *value.getCaseAlternative(i)->thenExpression, source))
        return false;
    return true;  // A NULL WHEN selects another branch, not a NULL result.
  }
  default:
    return nonNullSourceValue(expression, source);
  }
}

static void validateFillAggregate(
    const binder::AggregateFunctionExpression& aggregate) {
  const auto functionName =
      normalizeFillAggregateFunctionName(aggregate.getFunction().name);
  if ((aggregate.isDistinct() && functionName != "COUNT") ||
      (functionName != "COUNT" && functionName != "SUM")) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL supports non-DISTINCT COUNT and SUM aggregates only.");
  }
  if (aggregate.getNumChildren() > 1 ||
      (functionName == "SUM" && aggregate.getNumChildren() != 1)) {
    THROW_BINDER_EXCEPTION(
        "FILL SUM requires one argument; FILL COUNT requires zero or one.");
  }
  if (functionName == "SUM" &&
      !isFillExpressionNonNull(*aggregate.getChild(0))) {
    THROW_BINDER_EXCEPTION(
        "FILL SUM currently requires a provably non-NULL argument. Nullable "
        "SUM needs separate non-NULL-count state; COUNT(nullable expression) "
        "is supported.");
  }
  if (functionName == "SUM") {
    const auto type = aggregate.getChild(0)->getDataType().getLogicalTypeID();
    if (type == common::LogicalTypeID::FLOAT ||
        (type == common::LogicalTypeID::DOUBLE &&
         !isExactHalfIntegerFillExpression(*aggregate.getChild(0)))) {
      THROW_BINDER_EXCEPTION(
          "FILL SUM over FLOAT/DOUBLE is not supported by exact incremental "
          "maintenance: floating addition loses contributions that cannot "
          "be recovered on deletion. Use an exact signed-integer expression "
          "or execute the original query without FILL.");
    }
    if (type == common::LogicalTypeID::UINT8 ||
        type == common::LogicalTypeID::UINT16 ||
        type == common::LogicalTypeID::UINT32 ||
        type == common::LogicalTypeID::UINT64) {
      THROW_BINDER_EXCEPTION(
          "Unsigned SUM is not supported by FILL signed retraction state.");
    }
  }
}

void validateCanonicalFillProjection(const binder::Expression& expression) {
  bool hasAggregate = false;
  std::function<void(const binder::Expression&)> visit = [&](const auto& value) {
    if (value.expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
      hasAggregate = true;
      return;
    }
    for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(value))
      visit(*child);
  };
  visit(expression);
  if (!hasAggregate) return;
  if (!isCanonicalFillAggregate(expression)) {
    THROW_BINDER_EXCEPTION(
        "FILL requires canonical base aggregates SUM(x), COUNT(x) or COUNT(*). "
        "Persist base aggregates and evaluate finalizers in the query; "
        "NEUG_ALLOW_LEGACY_FILL_EXPRESSIONS=1 explicitly enables legacy "
        "computed-value materialization.");
  }
  validateFillAggregate(expression.constCast<binder::AggregateFunctionExpression>());
}

static void validateFillGrouping(const NormalizedSingleQuery& query,
                                 const binder::expression_vector& columns,
                                 size_t keyCount) {
  std::unordered_set<std::string> keys;
  std::unordered_set<std::string> keyNodes;
  for (size_t i = 0; i < keyCount; ++i) {
    keys.insert(columns[i]->getUniqueName());
    if (columns[i]->expressionType == common::ExpressionType::PROPERTY) {
      keyNodes.insert(columns[i]->constCast<binder::PropertyExpression>()
                          .getVariableName());
    }
  }
  uint32_t lastAggregation = 0;
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi)
    if (query.getQueryPart(pi)->getProjectionBody()->hasAggregateExpressions())
      lastAggregation = pi;
  for (uint32_t pi = lastAggregation; pi < query.getNumQueryParts(); ++pi) {
    const auto* body = query.getQueryPart(pi)->getProjectionBody();
    if (!body->hasAggregateExpressions()) {
      continue;
    }
    for (const auto& key : body->getGroupByExpressions()) {
      // Constants do not split a carrier group. Native interfaces append a
      // public true flag and the source maintenance route appends private 1.
      if (key->expressionType == common::ExpressionType::LITERAL)
        continue;
      const bool carrierNode =
          key->expressionType == common::ExpressionType::PATTERN &&
          key->getDataType().getLogicalTypeID() == common::LogicalTypeID::NODE &&
          keyNodes.contains(key->getUniqueName());
      if (!carrierNode && !keys.contains(key->getUniqueName())) {
        THROW_BINDER_EXCEPTION(
            "Aggregate FILL must group only by its carrier primary key(s); "
            "use one vertex ID or two endpoint IDs without additional "
            "vertex, property, edge or witness grouping keys.");
      }
    }
  }
}

static void validateSourceRecomputeKeys(const NormalizedSingleQuery& query,
                                        const binder::expression_vector& columns,
                                        size_t keyCount) {
  // Key-restricted source maintenance starts at the original required graph.
  // OPTIONAL-only keys can be NULL, and a key introduced after aggregation
  // cannot safely be pushed through that earlier grouping stage.
  std::unordered_set<std::string> requiredNodes;
  for (uint32_t pi = 0; pi < query.getNumQueryParts(); ++pi) {
    const auto* part = query.getQueryPart(pi);
    for (uint32_t ci = 0; ci < part->getNumReadingClause(); ++ci) {
      const auto* clause = part->getReadingClause(ci);
      if (clause->getClauseType() != common::ClauseType::MATCH)
        continue;
      const auto& match = clause->constCast<binder::BoundMatchClause>();
      if (match.getMatchClauseType() != common::MatchClauseType::MATCH)
        continue;
      for (const auto& node : match.getQueryGraphCollection()->getQueryNodes())
        requiredNodes.insert(node->getUniqueName());
    }
    if (part->hasProjectionBody() &&
        part->getProjectionBody()->hasAggregateExpressions())
      break;
  }
  for (size_t i = 0; i < keyCount; ++i) {
    if (columns[i]->expressionType != common::ExpressionType::PROPERTY ||
        !requiredNodes.contains(columns[i]->constCast<binder::PropertyExpression>()
                                    .getVariableName()))
      THROW_BINDER_EXCEPTION(
          "Source-maintained FILL keys must be bound by a required MATCH "
          "before aggregation; OPTIONAL-only or later-stage keys are unsupported.");
  }
}

static void visitFillAstVariables(
    const ::common::Expression& expression,
    const std::function<void(const ::common::Variable&)>& visitor) {
  for (const auto& op : expression.operators()) {
    switch (op.item_case()) {
    case ::common::ExprOpr::kVar:
      visitor(op.var());
      break;
    case ::common::ExprOpr::kCase:
      for (const auto& branch : op.case_().when_then_expressions()) {
        visitFillAstVariables(branch.when_expression(), visitor);
        visitFillAstVariables(branch.then_result_expression(), visitor);
      }
      visitFillAstVariables(op.case_().else_result_expression(), visitor);
      break;
    case ::common::ExprOpr::kToTuple:
      for (const auto& field : op.to_tuple().fields()) {
        visitFillAstVariables(field, visitor);
      }
      break;
    case ::common::ExprOpr::kScalarFunc:
      for (const auto& parameter : op.scalar_func().parameters()) {
        visitFillAstVariables(parameter, visitor);
      }
      break;
    default:
      break;
    }
  }
}

using FillPropertyAccess = std::pair<common::table_id_t, std::string>;

std::vector<FillPropertyAccess> FillIndexAuxInfo::getStagedSourceProperties() const {
  std::vector<FillPropertyAccess> result;
  if (!isStagedPipeline()) return result;
  const std::string prefix = "@staged-source-property:";
  for (const auto& value : predicateTemplates) {
    if (!value.starts_with(prefix)) continue;
    const auto separator = value.find(':', prefix.size());
    if (separator == std::string::npos || separator + 1 == value.size())
      THROW_RUNTIME_ERROR("Invalid staged FILL property dependency metadata");
    const auto idText = value.substr(prefix.size(), separator - prefix.size());
    size_t consumed = 0;
    const auto id = std::stoull(idText, &consumed);
    if (consumed != idText.size())
      THROW_RUNTIME_ERROR("Invalid staged FILL property dependency table ID");
    result.emplace_back(id, value.substr(separator + 1));
  }
  return result;
}

static std::vector<FillPropertyAccess> collectFillPropertyAccesses(
    const FillIndexAuxInfo& info, bool validateNewDefinition) {
  std::vector<FillPropertyAccess> result = info.getStagedSourceProperties();
  const auto& pattern = info.getPatternIR();
  auto collect = [&](const std::shared_ptr<::common::Expression>& ast) {
    if (!ast) {
      return;
    }
    visitFillAstVariables(*ast, [&](const ::common::Variable& variable) {
      if (!variable.has_property() || !variable.property().has_key()) {
        return;
      }
      const auto tag = static_cast<size_t>(variable.tag().id());
      const auto& labels = tag < pattern.nodesInfo.size()
                               ? pattern.nodesInfo[tag].nodeLabels
                               : pattern.relsInfo.at(tag - pattern.nodesInfo.size())
                                     .relLabels;
      if (validateNewDefinition && !info.requiresSourceRecompute() &&
          labels.size() != 1 &&
          tag >= pattern.nodesInfo.size()) {
        THROW_BINDER_EXCEPTION(
            "FILL relationship property expressions currently require "
            "single-label source variables.");
      }
      for (const auto label : labels) {
        result.emplace_back(label, variable.property().key().name());
      }
    });
  };
  collect(info.getPredicateExpr());
  for (const auto& column : info.getIndexColumnInfos()) {
    switch (column->columnType) {
    case IndexColumnType::SOURCE_QUERY_VALUE:
      // Source-query dependencies are bound from the complete persisted
      // definition; this output is not a graph-property substitution.
      break;
    case IndexColumnType::NODE_AGGREGATE:
      collect(static_cast<const NodeAggregationIndexColnumInfo&>(*column)
                  .aggregateArgExpr);
      break;
    case IndexColumnType::REL_AGGREGATE:
      collect(static_cast<const RelIndexColnumInfo&>(*column).aggregateArgExpr);
      break;
    case IndexColumnType::REL_COMPUATION_WITH_AGGREGATION: {
      const auto& finalizer =
          static_cast<const RelComputationWithAggreColumnInfo&>(*column);
      collect(finalizer.astExpr);
      collect(finalizer.aggregateArgExpr);
      if (validateNewDefinition && finalizer.finalizerExpr) {
        visitFillAstVariables(*finalizer.finalizerExpr,
                              [](const ::common::Variable& variable) {
          if (variable.tag().id() != kFillAggregateStateTag) {
            THROW_BINDER_EXCEPTION(
                "FILL scalar finalizers may depend only on their aggregate "
                "state and constants.");
          }
        });
      }
      break;
    }
    case IndexColumnType::NODE_PROPERTY:
    case IndexColumnType::REL_PROPERTY: {
      // Include legacy property FILLs when checking newly introduced
      // dependencies. New property FILLs are rejected before this function.
      const auto node = pattern.nodeUniqNameToPos.find(column->nodeUnique);
      if (node != pattern.nodeUniqNameToPos.end()) {
        for (const auto label : pattern.nodesInfo[node->second].nodeLabels) {
          result.emplace_back(label, column->propertyName);
        }
      }
      break;
    }
    }
  }
  return result;
}

void validateStagedFillDependencies(
    const FillIndexAuxInfo& info, common::table_id_t targetTable,
    Catalog* catalog, const transaction::Transaction* transaction) {
  const auto readsRelation = [](const FillIndexAuxInfo& source, common::table_id_t table) {
    const auto& rels = source.getPatternIR().relsInfo;
    return std::any_of(rels.begin(), rels.end(), [&](const auto& relation) {
      return std::find(relation.relLabels.begin(), relation.relLabels.end(), table) !=
             relation.relLabels.end();
    });
  };
  const auto derived = [&](const FillIndexAuxInfo& source, common::table_id_t table) {
    return source.getFillType() == FillEntryType::REL && !readsRelation(source, table);
  };
  for (const auto* entry : catalog->getExtendedIndexEntries(transaction)) {
    if (entry->getIndexType() != "FILL") continue;
    const auto& previous = entry->getAuxInfo().cast<FillIndexAuxInfo>();
    if ((info.isStagedPipeline() && derived(previous, entry->getTableID()) &&
         readsRelation(info, entry->getTableID())) ||
        (previous.isStagedPipeline() && derived(info, targetTable) &&
         readsRelation(previous, targetTable))) {
      THROW_BINDER_EXCEPTION(
          "Staged COUNT/SUM FILL cannot depend on a FILL-owned derived relationship "
          "until transitive source-write protection is supported. DROP the dependent "
          "index before changing this carrier definition.");
    }
    if (previous.isStagedPipeline()) {
      for (const auto& [table, property] : previous.getStagedSourceProperties()) {
        if (table == targetTable && std::find(info.getTargetColumns().begin(),
              info.getTargetColumns().end(), property) != info.getTargetColumns().end()) {
          THROW_BINDER_EXCEPTION(
              "Staged COUNT/SUM FILL reads this target property. DROP its index "
              "before changing the source with another FILL, then rebuild it.");
        }
      }
    }
  }
}

void validateFillStateOwnership(
    const FillIndexAuxInfo& info, common::table_id_t targetTable,
    Catalog* catalog, const transaction::Transaction* transaction) {
  validateStagedFillDependencies(info, targetTable, catalog, transaction);
  const auto accesses = collectFillPropertyAccesses(info, true);
  auto isDerivedRelation = [](const FillIndexAuxInfo& candidate,
                              common::table_id_t table) {
    const auto& rels = candidate.getPatternIR().relsInfo;
    return (candidate.getFillType() == FillEntryType::REL ||
            candidate.getFillType() == FillEntryType::VARIADIC_REL) &&
           std::none_of(rels.begin(), rels.end(), [&](const auto& rel) {
             return std::find(rel.relLabels.begin(), rel.relLabels.end(), table) !=
                    rel.relLabels.end();
           });
  };
  auto writes = [](const FillIndexAuxInfo& owner, common::table_id_t table,
                   const FillPropertyAccess& access) {
    const auto& columns = owner.getTargetColumns();
    return access.first == table &&
           std::find(columns.begin(), columns.end(), access.second) !=
               columns.end();
  };
  // Edge existence is a topology dependency even when that edge does not
  // occur in the outer PatternIR. Check the complete derived-table graph
  // before registering a definition; a maintenance recursion limit cannot
  // make a cyclic (especially negative) definition well-defined.
  std::unordered_map<common::table_id_t,
                     std::unordered_set<common::table_id_t>> edgeDependencies;
  auto collectEdgeDependencies = [&](const FillIndexAuxInfo& definition,
                                      common::table_id_t target) {
    if (!isDerivedRelation(definition, target)) {
      return;
    }
    auto& dependencies = edgeDependencies[target];
    for (const auto& relation : definition.getPatternIR().relsInfo) {
      dependencies.insert(relation.relLabels.begin(), relation.relLabels.end());
    }
    for (const auto& predicate : definition.getExistsPredicates()) {
      dependencies.insert(predicate.relTableIDs.begin(),
                          predicate.relTableIDs.end());
    }
  };
  collectEdgeDependencies(info, targetTable);
  for (const auto& access : accesses) {
    if (writes(info, targetTable, access)) {
      THROW_BINDER_EXCEPTION(
          "FILL cannot read a property that it also materializes.");
    }
  }
  for (const auto* entry : catalog->getExtendedIndexEntries(transaction)) {
    if (entry->getIndexType() != "FILL") {
      continue;
    }
    std::unique_ptr<FillIndexAuxInfo> deserialized;
    const FillIndexAuxInfo* previous;
    if (entry->isLoaded()) {
      previous = &entry->getAuxInfo().cast<FillIndexAuxInfo>();
    } else {
      deserialized.reset(FillIndexAuxInfo::deserialize(entry->getAuxBufferReader()));
      previous = deserialized.get();
    }
    collectEdgeDependencies(*previous, entry->getTableID());
    if (targetTable == entry->getTableID() &&
        (isDerivedRelation(info, targetTable) ||
         isDerivedRelation(*previous, entry->getTableID()))) {
      THROW_BINDER_EXCEPTION(
          "A derived relationship table can be owned by only one FILL "
          "definition. Drop its index before rebuilding the table.");
    }
    for (const auto& column : info.getTargetColumns()) {
      if (writes(*previous, entry->getTableID(), {targetTable, column})) {
        THROW_BINDER_EXCEPTION(
            "A FILL already owns this target property. Drop its index before "
            "rebuilding or replacing the definition.");
      }
    }
    for (const auto& access : accesses) {
      if (writes(*previous, entry->getTableID(), access) && !info.requiresSourceRecompute()) {
        THROW_BINDER_EXCEPTION(
            "FILL cannot depend on another FILL's materialized property until "
            "cascaded property-change maintenance is supported.");
      }
    }
    for (const auto& access : collectFillPropertyAccesses(*previous, false)) {
      if (writes(info, targetTable, access)) {
        THROW_BINDER_EXCEPTION(
            "FILL cannot overwrite a property read by an existing FILL until "
            "cascaded property-change maintenance is supported.");
      }
    }
  }
  if (isDerivedRelation(info, targetTable)) {
    std::unordered_set<common::table_id_t> visiting, visited;
    std::function<bool(common::table_id_t)> hasCycle = [&](auto table) {
      if (visiting.contains(table)) {
        return true;
      }
      if (visited.contains(table)) {
        return false;
      }
      visiting.insert(table);
      const auto dependencies = edgeDependencies.find(table);
      if (dependencies != edgeDependencies.end()) {
        for (const auto source : dependencies->second) {
          if (hasCycle(source)) {
            return true;
          }
        }
      }
      visiting.erase(table);
      visited.insert(table);
      return false;
    };
    if (hasCycle(targetTable)) {
      THROW_BINDER_EXCEPTION(
          "FILL relationship dependencies contain a cycle, including an "
          "EXISTS/NOT EXISTS edge dependency.");
    }
  }
}

static void validateFillUserWrite(main::ClientContext* context,
                                  common::table_id_t tableID,
                                  const std::string* propertyName) {
  for (const auto* entry : context->getCatalog()->getExtendedIndexEntries(
           context->getTransaction())) {
    if (entry->getIndexType() != "FILL" || entry->getTableID() != tableID) {
      continue;
    }
    std::unique_ptr<FillIndexAuxInfo> deserialized;
    const FillIndexAuxInfo* info;
    if (entry->isLoaded()) {
      info = &entry->getAuxInfo().cast<FillIndexAuxInfo>();
    } else {
      deserialized.reset(FillIndexAuxInfo::deserialize(entry->getAuxBufferReader()));
      info = deserialized.get();
    }
    if (propertyName) {
      const auto& columns = info->getTargetColumns();
      const auto normalized = common::StringUtils::getUpper(*propertyName);
      if (std::any_of(columns.begin(), columns.end(), [&](const auto& column) {
            return common::StringUtils::getUpper(column) == normalized;
          })) {
        THROW_BINDER_EXCEPTION(
            "Cannot write a property owned by an active FILL index. Drop the "
            "index before assigning its materialized output.");
      }
    } else if (info->getFillType() == FillEntryType::REL || info->getFillType() == FillEntryType::VARIADIC_REL) {
      const auto& rels = info->getPatternIR().relsInfo;
      const bool derived = std::none_of(rels.begin(), rels.end(),
                                        [&](const auto& rel) {
        return std::find(rel.relLabels.begin(), rel.relLabels.end(), tableID) !=
               rel.relLabels.end();
      });
      if (derived) {
        THROW_BINDER_EXCEPTION(
            "Cannot directly create, delete or copy relationships owned by a "
            "derived FILL index. Modify its source graph or drop the index.");
      }
    }
  }
}

void validateFillUserPropertyWrite(main::ClientContext* context,
                                   common::table_id_t tableID,
                                   const std::string& propertyName) {
  validateFillUserWrite(context, tableID, &propertyName);
}

void validateFillUserRelationshipWrite(main::ClientContext* context,
                                       common::table_id_t tableID) {
  validateFillUserWrite(context, tableID, nullptr);
}

void validateWorkloadFillStateOwnership(const FillIndexAuxInfo& info,
    common::table_id_t targetTable, main::ClientContext* context) {
  validateFillStateOwnership(info, targetTable, context->getCatalog(), context->getTransaction());
}

std::string createNodeIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::TableCatalogEntry* tableEntry,
    neug::main::ClientContext* ctx, std::vector<std::string> columnNames,
    const std::string& sourceQuery) {

  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION(
        common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  if (boundQuerySource.getNumSingleQueries() != 1) {
    THROW_BINDER_EXCEPTION("UNION is not supported in incremental FILL sources.");
  }
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);
  const bool recompute = requiresSourceRecompute(*boundSource, *tableEntry);
  const auto matchGraphs = collectFillMatchGraphs(*singleQuery, recompute);
  if (matchGraphs.empty()) {
    THROW_BINDER_EXCEPTION("FILL requires a graph MATCH source.");
  }

  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();

  // Build the full merged PatternIR from all QueryGraphs.
  std::vector<common::PatternIR> perGraphPatterns;
  std::vector<FillPatternElementOrigin> nodeOrigins, relOrigins;
  auto fullPatternIR = buildFullPatternIR(*singleQuery, perGraphPatterns,
                                          nodeOrigins, relOrigins, recompute);
  NEUG_ASSERT(!fullPatternIR.nodesInfo.empty());
  if (!recompute && fullPatternIR.relsInfo.empty()) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL requires at least one fixed-hop relationship; "
        "node-only source insertion/deletion maintenance is not yet supported.");
  }
  FillPredicateAsts predicateAsts;
  try {
    predicateAsts = buildFillPredicateAsts(*singleQuery, fullPatternIR);
  } catch (const std::exception&) {
    if (!recompute)
      throw;
  }
  auto predicateExpr = predicateAsts.combined;

  // Build cross-graph variable-name → unique-name maps for aggregation
  // variable resolution. Relationship tags are offset by node count in the
  // serialized AST so they do not collide with node positions.
  std::unordered_map<std::string, std::string> varNameToNodeUniqName;
  std::unordered_map<std::string, std::string> varNameToRelUniqName;
  for (const auto& ref : matchGraphs) {
    const auto* graph = ref.graph;
    for (auto j = 0u; j < graph->getNumQueryNodes(); j++) {
      auto node = graph->getQueryNode(j);
      if (!node->getVariableName().empty()) {
        varNameToNodeUniqName[node->getVariableName()] = node->getUniqueName();
      }
    }
    for (uint32_t j = 0; j < graph->getNumQueryRels(); ++j) {
      auto rel = graph->getQueryRel(j);
      if (!rel->getVariableName().empty()) {
        varNameToRelUniqName[rel->getVariableName()] = rel->getUniqueName();
      }
    }
  }

  size_t startIdx{1};
  auto queryColumns = boundSource->getColumns();
  // The materialization interface is independent of its maintenance strategy.
  validateFillGrouping(*singleQuery, queryColumns, startIdx);
  if (recompute)
    validateSourceRecomputeKeys(*singleQuery, queryColumns, startIdx);
  const bool hasDistinctCount = std::any_of(queryColumns.begin() + startIdx,
      queryColumns.end(), [](const auto& expr) {
        return expr->expressionType == common::ExpressionType::AGGREGATE_FUNCTION &&
            expr->template constCast<binder::AggregateFunctionExpression>().isDistinct();
      });
  if (hasDistinctCount && !recompute) {
    if (fullPatternIR.nodesInfo.size() != 2 || fullPatternIR.relsInfo.size() != 1 ||
        predicateExpr || singleQuery->getNumQueryParts() != 1 ||
        fullPatternIR.relsInfo[0].relType != common::QueryRelType::NON_RECURSIVE ||
        fullPatternIR.relsInfo[0].edgeType == common::EdgeType::UNDIRECTED) {
      THROW_BINDER_EXCEPTION("DISTINCT FILL currently requires a directed one-hop NODE count without predicates.");
    }
    const auto carrier = queryColumns[0]->getUniqueName();
    const auto carrierName = carrier.substr(0, carrier.find('.'));
    for (size_t i = startIdx; i < queryColumns.size(); ++i) {
      const auto& expr = queryColumns[i];
      if (expr->expressionType != common::ExpressionType::AGGREGATE_FUNCTION)
        THROW_BINDER_EXCEPTION("DISTINCT NODE FILL supports COUNT outputs only.");
      const auto& aggregate = expr->constCast<binder::AggregateFunctionExpression>();
      if (normalizeFillAggregateFunctionName(aggregate.getFunction().name) != "COUNT" ||
          (aggregate.getNumChildren() && aggregate.getChild(0)->expressionType != common::ExpressionType::PATTERN))
        THROW_BINDER_EXCEPTION("DISTINCT NODE FILL supports counts of bound graph objects only.");
      if (aggregate.isDistinct() && (aggregate.getNumChildren() != 1 ||
          (aggregate.getChild(0)->getDataType().getLogicalTypeID() != common::LogicalTypeID::NODE &&
           aggregate.getChild(0)->getDataType().getLogicalTypeID() != common::LogicalTypeID::REL) ||
          aggregate.getChild(0)->getUniqueName() == carrierName))
        THROW_BINDER_EXCEPTION("DISTINCT NODE FILL must count the opposite endpoint node or its physical relationship.");
    }
  }
  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;
  for (size_t i = startIdx; i < queryColumns.size(); ++i) {
    auto& queryColumn = queryColumns[i];
    auto& columnName = columnNames[i - startIdx];
    auto uniqName = queryColumn->getUniqueName();

    if (recompute &&
        queryColumn->expressionType == common::ExpressionType::LITERAL &&
        columnName.starts_with(
            common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) {
      auto presence = std::make_shared<NodeAggregationIndexColnumInfo>();
      presence->columnType = IndexColumnType::NODE_AGGREGATE;
      presence->aggregateFuncName = "COUNT";
      presence->nameTemplate = "COUNT(*)";
      indexColumnInfos.emplace_back(std::move(presence));
      continue;
    }
    if (recompute &&
        ((queryColumn->expressionType == common::ExpressionType::LITERAL &&
          !queryColumn->constCast<binder::LiteralExpression>().isNull()) ||
         queryColumn->expressionType == common::ExpressionType::FUNCTION ||
         queryColumn->expressionType == common::ExpressionType::CASE_ELSE)) {
      indexColumnInfos.emplace_back(std::make_shared<IndexColnumInfo>(
          IndexColumnType::SOURCE_QUERY_VALUE, "", ""));
      continue;
    }
    if (queryColumn->expressionType ==
        common::ExpressionType::AGGREGATE_FUNCTION) {
      auto info = std::make_shared<NodeAggregationIndexColnumInfo>();
      info->columnType = IndexColumnType::NODE_AGGREGATE;

      auto aggFuncExpr =
          queryColumn->ptrCast<binder::AggregateFunctionExpression>();
      if (!recompute)
        validateFillAggregate(*aggFuncExpr);
      else if (normalizeFillAggregateFunctionName(
                   aggFuncExpr->getFunction().name) == "SUM" &&
               (aggFuncExpr->getNumChildren() != 1 ||
                !nonNullSourceValue(*aggFuncExpr->getChild(0), *singleQuery)))
        THROW_BINDER_EXCEPTION(
            "FILL SUM requires a provably non-NULL source value; nullable "
            "materialization needs explicit validity state");
      info->nameTemplate = aggFuncExpr->getParsedName();
      info->aggregateFuncName =
          normalizeFillAggregateFunctionName(aggFuncExpr->getFunction().name);

      // Validate: only COUNT and SUM are supported for incremental maintenance.
      {
        std::string funcUpper = info->aggregateFuncName;
        if (funcUpper != "COUNT" && funcUpper != "SUM" &&
            !(recompute && (funcUpper == "MIN" || funcUpper == "MAX"))) {
          THROW_BINDER_EXCEPTION(common::stringFormat(
              "FILL aggregate function '{}' is not supported for incremental "
              "maintenance. Only COUNT and SUM are currently supported.",
              aggFuncExpr->getFunction().name));
        }
      }

      // Collect the unique names of nodes referenced by this aggregate.
      for (auto var : aggFuncExpr->getRawNameVariables()) {
        auto it = varNameToNodeUniqName.find(var);
        if (it != varNameToNodeUniqName.end()) {
          info->nodeUniqueNames.push_back(it->second);
        } else if (auto rel = varNameToRelUniqName.find(var); rel != varNameToRelUniqName.end()) {
          info->nodeUniqueNames.push_back(rel->second);
        }
      }

      // Extract nodeUnique and propertyName from the aggregate's child
      // expression.
      if (aggFuncExpr->getNumChildren() == 1) {
        auto child = aggFuncExpr->getChild(0);
        if (child->expressionType == common::ExpressionType::PROPERTY) {
          auto& propExpr = child->constCast<binder::PropertyExpression>();
          info->nodeUnique = propExpr.getVariableName();
          info->propertyName = propExpr.getPropertyName();
        } else if (child->expressionType == common::ExpressionType::PATTERN &&
                   child->dataType.getLogicalTypeID() ==
                       common::LogicalTypeID::NODE) {
          info->nodeUnique = child->getUniqueName();
        }
      }

      // Serialize the aggregate argument into the ::common::Expression AST
      // (structural rewrite matching + incremental evaluation). Variable tags
      // are positions in fullPatternIR.nodesInfo — the same numbering the
      // rewrite matcher derives via matchedIdx. Fails fast on expressions
      // outside the supported grammar instead of silently degrading to the
      // legacy string-template heuristics.
      std::string aggregateFuncUpper = info->aggregateFuncName;
      if (aggFuncExpr->getNumChildren() == 1 &&
          aggFuncExpr->getChild(0)->expressionType !=
              common::ExpressionType::PATTERN) {
        auto resolveTag =
            [&](const std::string& name) -> std::optional<uint32_t> {
          return resolvePatternAstTag(fullPatternIR, varNameToNodeUniqName,
                                      varNameToRelUniqName, name);
        };
        try {
          info->aggregateArgExpr =
              ConvertFillIndexExprToAst(*aggFuncExpr->getChild(0), resolveTag);
        } catch (const std::exception&) {
          if (!recompute)
            throw;
        }
      } else if (aggregateFuncUpper == "SUM" &&
                 aggFuncExpr->getNumChildren() > 1) {
        THROW_BINDER_EXCEPTION(common::stringFormat(
            "FILL aggregate function '{}' has multiple arguments, which is "
            "not supported.",
            aggFuncExpr->getFunction().name));
      }
      // COUNT(*) and COUNT(pattern) contribute one per required MATCH witness.
      // COUNT(expr) retains its AST to count non-NULL arguments, including CASE.

      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType ==
               common::ExpressionType::PROPERTY) {
      auto pos = uniqName.find('.');
      NEUG_ASSERT(pos != std::string::npos);
      auto fir = uniqName.substr(0, pos);
      auto sec = uniqName.substr(pos + 1);
      bool found{false};
      bool isRelProperty{false};
      common::table_id_t tId{common::INVALID_TABLE_ID};

      for (const auto& ref : matchGraphs) {
        const auto* graph = ref.graph;

        if (graph->containsQueryNode(fir)) {
          auto nodeExpr = graph->getQueryNode(fir);
          tId = nodeExpr->getEntries()[0]->getTableID();
          found = true;
          break;
        } else if (graph->containsQueryRel(fir)) {
          auto relExpr = graph->getQueryRel(fir);
          tId = relExpr->getEntries()[0]->getTableID();
          found = true;
          isRelProperty = true;
          break;
        }
      }
      if (!found) {
        THROW_BINDER_EXCEPTION(common::stringFormat(
            "Cannot find pattern variable {} in FILL query for column {}.", fir,
            columnName));
      }
      auto tbEntry = catalog->getTableCatalogEntry(transaction, tId);
      NEUG_ASSERT(tbEntry->containsProperty(sec));
      auto info = std::make_shared<IndexColnumInfo>();
      info->columnType = isRelProperty ? IndexColumnType::REL_PROPERTY
                                       : IndexColumnType::NODE_PROPERTY;
      info->nodeUnique = fir;
      info->propertyName = tbEntry->getProperty(sec).getName();
      indexColumnInfos.emplace_back(std::move(info));
    } else {
      THROW_BINDER_EXCEPTION(
          "NODE FILL values must be direct properties or supported aggregates.");
    }
  }

  // The first RETURN expression identifies the node whose row receives the
  // materialized values.  Resolve the target position from that expression,
  // rather than comparing catalog table OIDs: DDL may recreate/extend a table
  // between binding the MATCH and creating this catalog entry, and catalog
  // OIDs are not storage label IDs.  A label-only lookup is also ambiguous
  // when the same vertex label occurs more than once in the pattern.
  auto labelName = tableEntry->getName();
  auto tableID = tableEntry->getTableID();
  size_t nodePos = UINT64_MAX;
  std::string tableUniq, tableVar;

  if (!queryColumns.empty() &&
      queryColumns[0]->expressionType == common::ExpressionType::PROPERTY) {
    const auto keyName = queryColumns[0]->getUniqueName();
    const auto separator = keyName.find('.');
    const auto keyVariable = keyName.substr(0, separator);
    for (const auto& ref : matchGraphs) {
      const auto* graph = ref.graph;
      if (!graph->containsQueryNode(keyVariable))
        continue;
      const auto node = graph->getQueryNode(keyVariable);
      if (node->getEntries().size() != 1) {
        THROW_BINDER_EXCEPTION("NODE FILL requires a single-label target node.");
      }
      tableUniq = node->getUniqueName();
      tableVar = node->getVariableName();
      const auto posIt = fullPatternIR.nodeUniqNameToPos.find(tableUniq);
      if (posIt != fullPatternIR.nodeUniqNameToPos.end())
        nodePos = posIt->second;
      break;
    }
  }

  // Carrier identity must be an actual primary key, not an arbitrary property
  // with the same type as the target key.
  if (queryColumns[0]->expressionType != common::ExpressionType::PROPERTY ||
      !queryColumns[0]->constCast<binder::PropertyExpression>()
           .isPrimaryKey(tableID)) {
    THROW_BINDER_EXCEPTION("NODE FILL must return the target node primary key first.");
  }

  if (nodePos == UINT64_MAX) {
    THROW_BINDER_EXCEPTION("Cannot resolve the NODE FILL target node.");
  }

  bool hasProperty = false, hasAggregate = false;
  for (const auto& info : indexColumnInfos) {
    hasAggregate |= info->columnType == IndexColumnType::NODE_AGGREGATE;
    hasProperty |= info->columnType != IndexColumnType::NODE_AGGREGATE;
  }
  if (!recompute && hasProperty && hasAggregate) {
    THROW_BINDER_EXCEPTION(
        "NODE FILL cannot combine property witnesses and aggregate groups.");
  }
  if (!recompute && hasProperty) {
    THROW_BINDER_EXCEPTION(
        "Property NODE FILL requires a proven single witness, explicit absence "
        "state and complete property-change maintenance; it is not yet "
        "supported.");
  }

  // If the uniqName from the pattern differs from what we resolved (e.g.
  // multiple QueryGraphs share the same node), use the resolved one so
  // rewriters can find it via the uniqName lookup.
  if (fullPatternIR.nodeUniqNameToPos.count(tableUniq)) {
    // OK — `tableUniq` already resolves in the merged pattern.
  } else {
    // Fallback: use the variable name to find it.
    tableUniq = fullPatternIR.nodesInfo[nodePos].uniqName;
  }

  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::NODE, labelName, tableUniq, tableVar, columnNames,
      indexColumnInfos, std::move(fullPatternIR), sourceQuery);
  aux->setAggregateNumericContract(
      recompute ? FillAggregateNumericContract::SOURCE_QUERY_RECOMPUTE
      : hasDistinctCount
          ? FillAggregateNumericContract::SIGNED_INTEGER_DISTINCT_COUNT
          : FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  aux->setAggregateForm(getBoundFillAggregateForm(queryColumns, 1));
  auto templates = collectPredicateTemplates(*singleQuery, aux->getPatternIR(),
                                             catalog, transaction);
  if (auto shape = binder::distinctFillShape(*singleQuery,
                                             singleQuery->getNumQueryParts(),
                                             aux->getPatternIR(), false))
    templates.push_back(*shape);
  aux->setPredicateTemplates(std::move(templates));
  aux->setPatternElementOrigins(std::move(nodeOrigins), std::move(relOrigins));
  aux->setPredicateExpr(predicateExpr);
  aux->setPredicateConjuncts(std::move(predicateAsts.conjuncts));
  aux->setExistsPredicates(std::move(predicateAsts.existsPredicates));
  validateFillStateOwnership(*aux, tableID, catalog, transaction);
  std::vector<common::property_id_t> propertyIDs;
  propertyIDs.reserve(columnNames.size());
  for (const auto& c : columnNames) {
    if (tableEntry->containsProperty(c)) {
      propertyIDs.push_back(tableEntry->getPropertyID(c));
    }
  }
  auto baseName = makeFillIndexBaseName(
      labelName, columnNames, catalog::FillEntryType::NODE, aux->getPatternIR(),
      indexColumnInfos,
      fillPredicateSignature(*aux));
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

std::string createRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    std::vector<std::string> fillColumnNames,
    neug::catalog::RelTableCatalogEntry* relGroupEntry,
    main::ClientContext* ctx, const std::string& sourceQuery) {
  // Endpoint-only materializations retain the workload route's typed keys,
  // label dispatch and per-witness storage contract.
  if (fillColumnNames.empty() &&
      requiresWorkloadFillCompatibility(boundSource, relGroupEntry, fillColumnNames)) {
    return createWorkloadRelIndexEntry(boundSource, std::move(fillColumnNames),
                                      relGroupEntry, ctx, sourceQuery);
  }
  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION(
        common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  if (boundQuerySource.getNumSingleQueries() != 1) {
    THROW_BINDER_EXCEPTION("UNION is not supported in incremental FILL sources.");
  }
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);
  const bool recompute = requiresSourceRecompute(*boundSource, *relGroupEntry);

  if (!recompute && fillColumnNames.empty()) {
    THROW_BINDER_EXCEPTION(
        "FILL without aggregate values requires per-witness relationship "
        "storage; endpoint-deduplicated shortcut maintenance is not supported.");
  }

  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();

  // Concrete rel-group children use internal catalog names such as
  // ROOT_FORUM_COMMENT_FORUM, while storage and maintenance address the
  // shared edge label ROOT_FORUM. Keep the concrete table ID below for the
  // endpoint pair, but store the public group label in FILL metadata.
  auto labelName = relGroupEntry->getLabel(catalog, transaction);
  std::string relUniqName, relVarName;
  bool fillUndirectedRel = false;

  // Collect every MATCH graph and build variable-name → node-position maps.
  std::vector<const binder::QueryGraph*> graphs;
  std::vector<std::unordered_map<std::string, uint32_t>> variableNameMaps;
  std::vector<std::unordered_map<std::string, uint32_t>> relVariableNameMaps;
  for (const auto& ref : collectFillMatchGraphs(*singleQuery, recompute)) {
    const auto* graph = ref.graph;
    graphs.push_back(graph);

    std::unordered_map<std::string, uint32_t> varMap;
    for (auto j = 0u; j < graph->getNumQueryNodes(); j++) {
      auto node = graph->getQueryNode(j);
      if (!node->getVariableName().empty()) {
        varMap[node->getVariableName()] = j;
      }
    }
    std::unordered_map<std::string, uint32_t> relVarMap;
    for (uint32_t j = 0; j < graph->getNumQueryRels(); ++j) {
      auto rel = graph->getQueryRel(j);
      if (!rel->getVariableName().empty()) {
        relVarMap[rel->getVariableName()] = j;
      }
      const bool isTargetRelation = std::any_of(
          rel->getEntries().begin(), rel->getEntries().end(),
          [&](const auto* entry) {
            return entry->getTableID() == relGroupEntry->getTableID();
          });
      if (isTargetRelation) {
        // MANY_MANY still permits a unique edge per endpoint pair. The
        // maintainer validates that concrete carrier condition on updates;
        // a global ONE direction is stronger than the index contract needs.
        relVarName = rel->getVariableName();
        relUniqName = rel->getUniqueName();
        fillUndirectedRel =
            rel->getDirectionType() == binder::RelDirectionType::BOTH;
      }
    }
    variableNameMaps.push_back(std::move(varMap));
    relVariableNameMaps.push_back(std::move(relVarMap));
  }

  // Build the full merged PatternIR.
  std::vector<common::PatternIR> perGraphPatterns;
  std::vector<FillPatternElementOrigin> nodeOrigins, relOrigins;
  auto fullPatternIR = buildFullPatternIR(*singleQuery, perGraphPatterns,
                                          nodeOrigins, relOrigins, recompute);
  if (!recompute && fullPatternIR.relsInfo.empty()) {
    THROW_BINDER_EXCEPTION(
        "Incremental FILL requires at least one fixed-hop relationship; "
        "node-only source insertion/deletion maintenance is not yet supported.");
  }
  FillPredicateAsts predicateAsts;
  try {
    predicateAsts = buildFillPredicateAsts(*singleQuery, fullPatternIR);
  } catch (const std::exception&) {
    if (!recompute)
      throw;
  }
  auto predicateExpr = predicateAsts.combined;

  auto startIdx = 2u;
  auto queryColumns = boundSource->getColumns();
  for (const auto& column : queryColumns) {
    if (!recompute &&
        column->expressionType == common::ExpressionType::AGGREGATE_FUNCTION &&
        column->constCast<binder::AggregateFunctionExpression>().isDistinct())
      THROW_BINDER_EXCEPTION("DISTINCT REL FILL maintenance is not supported.");
  }

  // The materialization interface is independent of its maintenance strategy.
  validateFillGrouping(*singleQuery, queryColumns, startIdx);
  if (recompute)
    validateSourceRecomputeKeys(*singleQuery, queryColumns, startIdx);
  std::string returnSrcNodeUniqName;
  std::string returnDstNodeUniqName;
  auto resolveReturnEndpoint = [&](size_t columnIdx) -> std::string {
    if (columnIdx >= queryColumns.size() ||
        queryColumns[columnIdx]->expressionType !=
            common::ExpressionType::PROPERTY) {
      return {};
    }
    const auto& propExpr =
        queryColumns[columnIdx]->constCast<binder::PropertyExpression>();
    const auto endpointTableID = columnIdx == 0
                                     ? relGroupEntry->getSrcTableID()
                                     : relGroupEntry->getDstTableID();
    if (!propExpr.isPrimaryKey(endpointTableID)) {
      THROW_BINDER_EXCEPTION(
          "REL FILL must return source and destination node primary keys first.");
    }
    const auto& varName = propExpr.getRawVariableName();
    for (size_t pi = 0; pi < variableNameMaps.size(); ++pi) {
      auto it = variableNameMaps[pi].find(varName);
      if (it != variableNameMaps[pi].end()) {
        const auto node = graphs[pi]->getQueryNode(it->second);
        if (node->getEntries().size() != 1) {
          THROW_BINDER_EXCEPTION(
              "REL FILL requires single-label source and destination nodes.");
        }
        return node->getUniqueName();
      }
    }
    return {};
  };
  returnSrcNodeUniqName = resolveReturnEndpoint(0);
  returnDstNodeUniqName = resolveReturnEndpoint(1);
  if (returnSrcNodeUniqName.empty() || returnDstNodeUniqName.empty()) {
    THROW_BINDER_EXCEPTION("Cannot resolve the REL FILL endpoint primary keys.");
  }

  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;
  std::unordered_map<std::string, std::string> aggregateStateColumns;
  // Discover every persisted aggregate output before classifying scalar
  // finalizers. Automatically appended internal states appear after the
  // visible finalizer in the projection, so a single left-to-right pass is
  // insufficient.
  for (size_t i = startIdx; i < queryColumns.size(); ++i) {
    if (queryColumns[i]->expressionType ==
        common::ExpressionType::AGGREGATE_FUNCTION) {
      aggregateStateColumns.emplace(queryColumns[i]->getUniqueName(),
                                    fillColumnNames[i - startIdx]);
    }
  }
  for (size_t i = startIdx; i < queryColumns.size(); ++i) {
    auto queryColumn = queryColumns[i];
    if (recompute && isComputedBooleanFillValue(*queryColumn)) {
      if (!nonNullBooleanFillOperand(*queryColumn, *singleQuery))
        THROW_BINDER_EXCEPTION(
            "FILL computed BOOL requires a provably non-NULL source value; "
            "nullable materialization needs explicit validity state");
      indexColumnInfos.emplace_back(std::make_shared<IndexColnumInfo>(
          IndexColumnType::SOURCE_QUERY_VALUE, "", ""));
      continue;
    }
    if (recompute &&
        queryColumn->expressionType == common::ExpressionType::LITERAL &&
        fillColumnNames[i - startIdx].starts_with(
            common::InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) {
      auto presence = std::make_shared<RelIndexColnumInfo>();
      presence->columnType = IndexColumnType::REL_AGGREGATE;
      presence->aggregateFuncName = "COUNT";
      presence->nameTemplate = "COUNT(*)";
      indexColumnInfos.emplace_back(std::move(presence));
      continue;
    }
    if (recompute &&
        queryColumn->expressionType == common::ExpressionType::LITERAL &&
        !queryColumn->constCast<binder::LiteralExpression>().isNull()) {
      indexColumnInfos.emplace_back(std::make_shared<IndexColnumInfo>(
          IndexColumnType::SOURCE_QUERY_VALUE, "", ""));
      continue;
    }
    if (queryColumn->expressionType ==
        common::ExpressionType::AGGREGATE_FUNCTION) {
      auto info = std::make_shared<RelIndexColnumInfo>();
      auto aggFuncExpr =
          queryColumn->ptrCast<binder::AggregateFunctionExpression>();
      if (!recompute)
        validateFillAggregate(*aggFuncExpr);
      else if (normalizeFillAggregateFunctionName(
                   aggFuncExpr->getFunction().name) == "SUM" &&
               (aggFuncExpr->getNumChildren() != 1 ||
                !nonNullSourceValue(*aggFuncExpr->getChild(0), *singleQuery)))
        THROW_BINDER_EXCEPTION(
            "FILL SUM requires a provably non-NULL source value; nullable "
            "materialization needs explicit validity state");
      info->columnType = IndexColumnType::REL_AGGREGATE;
      info->nameTemplate = aggFuncExpr->getParsedName();
      info->aggregateFuncName =
          normalizeFillAggregateFunctionName(aggFuncExpr->getFunction().name);

      // Validate: only COUNT and SUM are supported for incremental maintenance.
      {
        std::string funcUpper = info->aggregateFuncName;
        if (funcUpper != "COUNT" && funcUpper != "SUM" &&
            !(recompute && (funcUpper == "MIN" || funcUpper == "MAX"))) {
          THROW_BINDER_EXCEPTION(common::stringFormat(
              "FILL aggregate function '{}' is not supported for incremental "
              "maintenance. Only COUNT and SUM are currently supported.",
              aggFuncExpr->getFunction().name));
        }
      }

      // Resolve variable names to unique names using the variableNameMaps
      // and the full merged PatternIR's uniqName lookup.
      for (auto var : aggFuncExpr->getRawNameVariables()) {
        // Find which pattern this variable belongs to, then get its node's
        // uniqName. A relationship variable (COUNT(edge), SUM(edge.amount))
        // is recorded as well; otherwise the template can never be matched.
        bool found = false;
        for (size_t pi = 0; pi < variableNameMaps.size() && !found; ++pi) {
          auto it = variableNameMaps[pi].find(var);
          if (it != variableNameMaps[pi].end()) {
            // Get the node's uniqName from the graph.
            auto node = graphs[pi]->getQueryNode(it->second);
            info->nodeUniqueNames.push_back(node->getUniqueName());
            found = true;
          }
        }
        for (size_t pi = 0; pi < relVariableNameMaps.size() && !found; ++pi) {
          auto it = relVariableNameMaps[pi].find(var);
          if (it != relVariableNameMaps[pi].end()) {
            auto rel = graphs[pi]->getQueryRel(it->second);
            info->nodeUniqueNames.push_back(rel->getUniqueName());
            found = true;
          }
        }
      }

      // Serialize the aggregate argument AST (same contract as the node
      // path). Variable tags are positions in fullPatternIR.nodesInfo.
      std::string aggregateFuncUpper = info->aggregateFuncName;
      if (aggFuncExpr->getNumChildren() == 1 &&
          aggFuncExpr->getChild(0)->expressionType !=
              common::ExpressionType::PATTERN) {
        auto resolveTag =
            [&](const std::string& name) -> std::optional<uint32_t> {
          std::unordered_map<std::string, std::string> nodeVars;
          std::unordered_map<std::string, std::string> relVars;
          for (size_t pi = 0; pi < variableNameMaps.size(); ++pi) {
            auto it = variableNameMaps[pi].find(name);
            if (it != variableNameMaps[pi].end()) {
              nodeVars[name] =
                  graphs[pi]->getQueryNode(it->second)->getUniqueName();
            }
            auto relIt = relVariableNameMaps[pi].find(name);
            if (relIt != relVariableNameMaps[pi].end()) {
              relVars[name] =
                  graphs[pi]->getQueryRel(relIt->second)->getUniqueName();
            }
          }
          return resolvePatternAstTag(fullPatternIR, nodeVars, relVars, name);
        };
        try {
          info->aggregateArgExpr =
              ConvertFillIndexExprToAst(*aggFuncExpr->getChild(0), resolveTag);
        } catch (const std::exception&) {
          if (!recompute)
            throw;
        }
      } else if (aggregateFuncUpper == "SUM" &&
                 aggFuncExpr->getNumChildren() > 1) {
        THROW_BINDER_EXCEPTION(common::stringFormat(
            "FILL aggregate function '{}' has multiple arguments, which is "
            "not supported.",
            aggFuncExpr->getFunction().name));
      }

      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType ==
                   common::ExpressionType::FUNCTION ||
               queryColumn->expressionType == common::ExpressionType::CASE_ELSE) {
      auto scalarExpr = queryColumn;
      auto info = std::make_shared<RelComputationWithAggreColumnInfo>();
      info->columnType = IndexColumnType::REL_COMPUATION_WITH_AGGREGATION;
      info->nameTemplate = scalarExpr->getParsedName();
      for (auto var : scalarExpr->getRawNameVariables()) {
        // Each template placeholder names a node or a relationship of the
        // source pattern; record either, so the rewrite matcher can map every
        // placeholder (an unrecorded one makes the template unmatchable).
        bool found = false;
        for (size_t pi = 0; pi < variableNameMaps.size() && !found; ++pi) {
          auto it = variableNameMaps[pi].find(var);
          if (it != variableNameMaps[pi].end()) {
            auto node = graphs[pi]->getQueryNode(it->second);
            info->nodeUniqueNames.push_back(node->getUniqueName());
            found = true;
          }
        }
        for (size_t pi = 0; pi < relVariableNameMaps.size() && !found; ++pi) {
          auto it = relVariableNameMaps[pi].find(var);
          if (it != relVariableNameMaps[pi].end()) {
            auto rel = graphs[pi]->getQueryRel(it->second);
            info->nodeUniqueNames.push_back(rel->getUniqueName());
            found = true;
          }
        }
      }
      std::vector<std::shared_ptr<binder::Expression>> nestedAggregates;
      std::unordered_set<std::string> seenAggregates;
      std::function<void(const std::shared_ptr<binder::Expression>&)>
          collectAggregates =
              [&](const std::shared_ptr<binder::Expression>& e) {
                if (e->expressionType ==
                    common::ExpressionType::AGGREGATE_FUNCTION) {
                  if (seenAggregates.insert(e->getUniqueName()).second)
                    nestedAggregates.push_back(e);
                  return;
                }
                for (const auto& child : binder::ExpressionChildrenCollector::collectChildren(*e)) {
                  collectAggregates(child);
                }
              };
      collectAggregates(queryColumn);

      if (recompute) {
        indexColumnInfos.emplace_back(std::move(info));
        continue;
      }
      // Serialize either an ordinary per-tuple scalar, or a finalizer over a
      // separately persisted aggregate state column.
      {
        auto resolveTag =
            [&](const std::string& name) -> std::optional<uint32_t> {
          std::unordered_map<std::string, std::string> nodeVars;
          std::unordered_map<std::string, std::string> relVars;
          for (size_t pi = 0; pi < variableNameMaps.size(); ++pi) {
            auto it = variableNameMaps[pi].find(name);
            if (it != variableNameMaps[pi].end()) {
              nodeVars[name] =
                  graphs[pi]->getQueryNode(it->second)->getUniqueName();
            }
            auto relIt = relVariableNameMaps[pi].find(name);
            if (relIt != relVariableNameMaps[pi].end()) {
              relVars[name] =
                  graphs[pi]->getQueryRel(relIt->second)->getUniqueName();
            }
          }
          return resolvePatternAstTag(fullPatternIR, nodeVars, relVars, name);
        };
        if (nestedAggregates.empty()) {
          THROW_BINDER_EXCEPTION(
              "Non-aggregate scalar REL FILL values cannot currently be "
              "maintained per witness.");
        } else {
          if (!isMaintainableFillFinalizer(*scalarExpr)) {
            THROW_BINDER_EXCEPTION(
                "FILL scalar finalizers currently allow only aggregate state "
                "and non-NULL constants with a proven numeric domain. "
                "Division requires a nonzero denominator; SQRT requires "
                "nonnegative state.");
          }
          if (nestedAggregates.size() != 1) {
            THROW_BINDER_EXCEPTION(
                "A finalized FILL column must contain exactly one aggregate."
                " Persist multiple aggregates as separate state columns.");
          }
          auto aggregate = nestedAggregates.front()
                               ->ptrCast<binder::AggregateFunctionExpression>();
          std::string funcName =
              normalizeFillAggregateFunctionName(aggregate->getFunction().name);
          if (aggregate->isDistinct() ||
              (funcName != "SUM" && funcName != "COUNT")) {
            THROW_BINDER_EXCEPTION(
                "Finalized FILL columns currently require a non-DISTINCT SUM "
                "or COUNT state.");
          }
          auto state = aggregateStateColumns.find(
              nestedAggregates.front()->getUniqueName());
          if (state == aggregateStateColumns.end()) {
            THROW_BINDER_EXCEPTION(
                "Cannot associate a finalized FILL expression with its "
                "internal aggregate state.");
          }
          info->aggregateFuncName = funcName;
          info->stateTargetColumn = state->second;
          if (aggregate->getNumChildren() == 1 &&
              aggregate->getChild(0)->expressionType !=
                  common::ExpressionType::PATTERN) {
            info->aggregateArgExpr =
                ConvertFillIndexExprToAst(*aggregate->getChild(0), resolveTag);
          } else if (funcName != "COUNT") {
            THROW_BINDER_EXCEPTION(
                "Finalized SUM state requires exactly one argument.");
          }
          info->finalizerExpr =
              ConvertFillIndexFinalizerToAst(*scalarExpr, resolveTag);
        }
      }
      indexColumnInfos.emplace_back(std::move(info));
    } else if (queryColumn->expressionType ==
               common::ExpressionType::PROPERTY) {
      auto uniqName = queryColumn->getUniqueName();
      auto pos = uniqName.find('.');
      NEUG_ASSERT(pos != std::string::npos);
      auto varName = uniqName.substr(0, pos);
      auto propName = uniqName.substr(pos + 1);
      auto info = std::make_shared<IndexColnumInfo>();
      info->columnType = IndexColumnType::REL_PROPERTY;
      info->nodeUnique = varName;
      info->propertyName = propName;
      indexColumnInfos.emplace_back(std::move(info));
    } else {
      THROW_BINDER_EXCEPTION(
          "REL FILL values must be properties, supported aggregates, or "
          "supported scalar finalizers.");
    }
  }

  // Validate column type combination: reject unsupported mixed types.
  // Only pure aggregate or pure property columns are supported per index.
  {
    bool hasProperty = false, hasAggregate = false;
    for (auto& info : indexColumnInfos) {
      switch (info->columnType) {
      case IndexColumnType::REL_PROPERTY:
        hasProperty = true;
        break;
      case IndexColumnType::REL_AGGREGATE:
      case IndexColumnType::REL_COMPUATION_WITH_AGGREGATION:
        hasAggregate = true;
        break;
      default:
        break;
      }
    }
    if (!recompute && hasProperty && hasAggregate) {
      THROW_BINDER_EXCEPTION(
          "FILL index combining both REL_PROPERTY and aggregate columns is not "
          "supported for incremental maintenance.");
    }
    if (!recompute && hasProperty) {
      THROW_BINDER_EXCEPTION(
          "Property REL FILL requires per-witness value maintenance, which is "
          "not yet supported.");
    }
  }

  std::vector<common::property_id_t> propertyIDs;
  propertyIDs.reserve(fillColumnNames.size());
  for (const auto& c : fillColumnNames) {
    if (relGroupEntry->containsProperty(c)) {
      propertyIDs.push_back(relGroupEntry->getPropertyID(c));
    }
  }

  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::REL, labelName, relUniqName, relVarName,
      fillColumnNames, indexColumnInfos, std::move(fullPatternIR), sourceQuery);
  aux->setAggregateNumericContract(
      recompute ? FillAggregateNumericContract::SOURCE_QUERY_RECOMPUTE
                : FillAggregateNumericContract::SIGNED_INTEGER_WRAP);
  aux->setAggregateForm(getBoundFillAggregateForm(queryColumns, 2));
  auto templates = collectPredicateTemplates(*singleQuery, aux->getPatternIR(),
                                             catalog, transaction);
  if (auto shape = binder::distinctFillShape(*singleQuery,
                                             singleQuery->getNumQueryParts(),
                                             aux->getPatternIR(), false))
    templates.push_back(*shape);
  aux->setPredicateTemplates(std::move(templates));
  aux->setPatternElementOrigins(std::move(nodeOrigins), std::move(relOrigins));
  aux->setPredicateExpr(predicateExpr);
  aux->setPredicateConjuncts(std::move(predicateAsts.conjuncts));
  aux->setExistsPredicates(std::move(predicateAsts.existsPredicates));
  aux->setSrcTableId(relGroupEntry->getSrcTableID());
  aux->setDstTableId(relGroupEntry->getDstTableID());
  aux->setEndpointUniqueNames(std::move(returnSrcNodeUniqName),
                              std::move(returnDstNodeUniqName));
  aux->setFillUndirectedRel(fillUndirectedRel);
  auto tableID = relGroupEntry->getTableID();
  validateFillStateOwnership(*aux, tableID, catalog, transaction);
  auto baseName = makeFillIndexBaseName(
      relGroupEntry->getName(), fillColumnNames, catalog::FillEntryType::REL,
      aux->getPatternIR(), indexColumnInfos,
      fillPredicateSignature(*aux));
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

std::string createVaradicRelIndexEntry(
    std::unique_ptr<neug::binder::BoundBaseScanSource>& boundSource,
    neug::catalog::RelTableCatalogEntry* tableEntry, main::ClientContext* ctx,
    std::string indexTableName, const std::string& sourceQuery) {
  if (requiresWorkloadFillCompatibility(boundSource, tableEntry, {})) {
    return createWorkloadVaradicRelIndexEntry(boundSource, tableEntry, ctx,
                                             std::move(indexTableName), sourceQuery);
  }

  if (boundSource->type != common::ScanSourceType::QUERY) {
    THROW_BINDER_EXCEPTION(
        common::stringFormat("FILL source must be a query.."));
  }
  auto& boundQuerySource = boundSource->constCast<BoundQueryScanSource>()
                               .statement->constCast<BoundRegularQuery>();
  NEUG_ASSERT(boundQuerySource.getNumSingleQueries() == 1);
  const NormalizedSingleQuery* singleQuery = boundQuerySource.getSingleQuery(0);
  const bool recompute = requiresSourceRecompute(*boundSource, *tableEntry);
  const auto matchGraphs = collectFillMatchGraphs(*singleQuery, recompute);
  // Variadic materialization is deliberately a one-recursive-edge shortcut.
  // More complex sources fall back to the ordinary relationship FILL path,
  // which retains the complete merged pattern.
  if (matchGraphs.size() != 1) {
    return "";
  }

  auto catalog = ctx->getCatalog();
  auto transaction = ctx->getTransaction();
  const auto* graph = matchGraphs.front().graph;
  if (graph->getNumQueryNodes() != 2 || graph->getNumQueryRels() != 1) {
    return "";
  }
  std::vector<common::PatternIR> perGraphPatterns;
  std::vector<FillPatternElementOrigin> nodeOrigins, relOrigins;
  auto fullPatternIR = buildFullPatternIR(*singleQuery, perGraphPatterns,
                                          nodeOrigins, relOrigins, recompute);
  auto rel = graph->getQueryRel(0);
  auto relRecursiveInfo = rel->getRecursiveInfo();
  // This helper is also probed for ordinary no-column relationship FILLs.
  // A single-hop relationship has no RecursiveInfo; report "not variadic"
  // so bindCopyRelFrom() can fall back to createRelIndexEntry(). BI17's
  // ROOT_FORUM is one such materialization over ROOT_POST + CONTAINEROF.
  if (relRecursiveInfo == nullptr || relRecursiveInfo->bindData == nullptr) {
    return "";
  }

  auto queryColumns = boundSource->getColumns();
  NEUG_ASSERT(queryColumns.size() == 2);
  std::vector<std::string> columnNames;
  std::vector<std::shared_ptr<IndexColnumInfo>> indexColumnInfos;

  auto tableName = indexTableName.empty() ? tableEntry->getName()
                                          : std::move(indexTableName);
  auto aux = std::make_unique<catalog::FillIndexAuxInfo>(
      catalog::FillEntryType::REL, tableName, rel->getUniqueName(),
      rel->getVariableName(), columnNames, indexColumnInfos,
      std::move(fullPatternIR), sourceQuery);
  aux->setAggregateNumericContract(
      FillAggregateNumericContract::SOURCE_QUERY_RECOMPUTE);
  auto templates = collectPredicateTemplates(*singleQuery, aux->getPatternIR(),
                                             catalog, transaction);
  templates.push_back(
      binder::fillRecursiveSignature(*rel).value_or("@unproved-recursive"));
  if (auto shape = binder::distinctFillShape(*singleQuery,
                                             singleQuery->getNumQueryParts(),
                                             aux->getPatternIR(), false))
    templates.push_back(*shape);
  aux->setPredicateTemplates(std::move(templates));
  aux->setPatternElementOrigins(std::move(nodeOrigins), std::move(relOrigins));
  aux->setSrcTableId(tableEntry->getSrcTableID());
  aux->setDstTableId(tableEntry->getDstTableID());

  uint32_t lower = 0;
  uint32_t upper = UINT32_MAX;
  if (relRecursiveInfo->bindData->lowerBound > 0) {
    lower = relRecursiveInfo->bindData->lowerBound;
  }
  if (relRecursiveInfo->bindData->upperBound != UINT32_MAX) {
    upper = relRecursiveInfo->bindData->upperBound;
  }
  aux->setRecursiveRelBounds(lower, upper);
  aux->setEndpointUniqueNames(queryColumns[0]
                                  ->constCast<binder::PropertyExpression>()
                                  .getVariableName(),
                              queryColumns[1]
                                  ->constCast<binder::PropertyExpression>()
                                  .getVariableName());

  std::vector<common::property_id_t> propertyIDs;
  auto tableID = tableEntry->getTableID();
  validateFillStateOwnership(*aux, tableID, catalog, transaction);
  auto extraSignature = neug::common::stringFormat(
      "|bounds={}:{}", relRecursiveInfo->bindData->lowerBound,
      relRecursiveInfo->bindData->upperBound);
  auto baseName = makeFillIndexBaseName(
      tableName + "_variableLength", columnNames, catalog::FillEntryType::REL,
      aux->getPatternIR(), indexColumnInfos, extraSignature);
  auto indexName = makeUniqueExtendedIndexName(catalog, transaction, baseName);
  auto indexEntry = std::make_unique<catalog::IndexCatalogEntry>(
      "FILL", tableID, indexName, propertyIDs, std::move(aux));
  indexName = indexEntry->getName();
  catalog->createExtendedIndex(transaction, std::move(indexEntry));
  return indexName;
}

}  // namespace catalog
}  // namespace neug

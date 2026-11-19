#pragma once

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include "neug/compiler/binder/query/normalized_single_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/common/pattern_ir.h"

namespace neug::binder {

inline constexpr std::string_view DISTINCT_FILL_SHAPE_PREFIX =
    "@distinct-fill-v1:";

// Record the DISTINCT boundaries and recursive traversal semantics that a
// merged PatternIR alone cannot represent. Only a closed, mandatory fragment
// ending in one aggregate projection is eligible for whole-fragment rewrite.
inline std::optional<std::string> distinctFillShape(
    const NormalizedSingleQuery& query, uint32_t count,
    const common::PatternIR& ir, bool matchedPositions) {
  if (count < 2 || count > query.getNumQueryParts()) return std::nullopt;
  std::ostringstream out;
  out << DISTINCT_FILL_SHAPE_PREFIX << count;
  auto mappedNode = [&](const std::string& name) -> uint64_t {
    auto found = ir.nodeUniqNameToPos.find(name);
    if (found == ir.nodeUniqNameToPos.end()) return UINT64_MAX;
    return matchedPositions ? ir.nodesInfo[found->second].matchedIdx : found->second;
  };
  auto mappedRel = [&](const std::string& name) -> uint64_t {
    auto found = ir.relUniqueNameToPos.find(name);
    if (found == ir.relUniqueNameToPos.end()) return UINT64_MAX;
    return matchedPositions ? ir.relsInfo[found->second].matchedIdx : found->second;
  };
  auto canonical = [](const std::shared_ptr<Expression>& expression,
                      const std::string& variable, const char* token) {
    if (expression == nullptr) return std::string{};
    auto text = expression->toString();
    if (!variable.empty()) {
      auto needle = variable + ".";
      for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;) {
        text.replace(pos, needle.size(), std::string(token) + ".");
        pos += std::string(token).size() + 1;
      }
    }
    return text;
  };
  for (uint32_t partIdx = 0; partIdx < count; ++partIdx) {
    const auto* part = query.getQueryPart(partIdx);
    if (part->hasUpdatingClause() || !part->hasProjectionBody() ||
        part->hasProjectionBodyPredicate()) return std::nullopt;
    const auto* body = part->getProjectionBody();
    if (body->hasOrderByExpressions() || body->hasSkipOrLimit()) return std::nullopt;
    if (partIdx + 1 == count) {
      if (part->hasReadingClause() || body->isDistinct() ||
          !body->hasAggregateExpressions()) return std::nullopt;
      break;
    }
    if (!body->isDistinct() || body->hasAggregateExpressions() ||
        part->getNumReadingClause() != 1) return std::nullopt;
    const auto* clause = part->getReadingClause(0);
    if (clause->getClauseType() != common::ClauseType::MATCH) return std::nullopt;
    const auto* match = clause->constPtrCast<BoundMatchClause>();
    if (match->getMatchClauseType() != common::MatchClauseType::MATCH ||
        match->hasHint()) return std::nullopt;
    std::vector<uint64_t> keys;
    for (const auto& expression : body->getProjectionExpressions()) {
      if (expression == nullptr || expression->expressionType != common::ExpressionType::PATTERN ||
          expression->getDataType().getLogicalTypeID() != common::LogicalTypeID::NODE) return std::nullopt;
      auto pos = mappedNode(expression->getUniqueName());
      if (pos == UINT64_MAX) return std::nullopt;
      keys.push_back(pos);
    }
    std::sort(keys.begin(), keys.end());
    out << "|D";
    for (const auto key : keys) out << key << ',';
    const auto* collection = match->getQueryGraphCollection();
    for (uint32_t gi = 0; gi < collection->getNumQueryGraphs(); ++gi) {
      for (const auto& rel : collection->getQueryGraph(gi)->getQueryRels()) {
        if (!rel->isRecursive()) continue;
        const auto* rec = rel->getRecursiveInfo();
        if (rec == nullptr || rec->bindData == nullptr) return std::nullopt;
        const auto pos = mappedRel(rel->getUniqueName());
        if (pos == UINT64_MAX) return std::nullopt;
        out << "|R" << pos << ':' << static_cast<unsigned>(rel->getRelType())
            << ':' << rec->bindData->lowerBound << ':' << rec->bindData->upperBound
            << ':' << static_cast<unsigned>(rec->bindData->semantic)
            << ':' << canonical(rec->relPredicate, rec->rel->getVariableName(), "@edge")
            << ':' << canonical(rec->nodePredicate, rec->node->getVariableName(), "@node");
      }
    }
  }
  return out.str();
}

inline bool hasDistinctFillShape(const catalog::FillIndexAuxInfo& info) {
  return std::any_of(info.getPredicateTemplates().begin(), info.getPredicateTemplates().end(),
      [](const auto& predicate) { return predicate.starts_with(DISTINCT_FILL_SHAPE_PREFIX); });
}

}  // namespace neug::binder

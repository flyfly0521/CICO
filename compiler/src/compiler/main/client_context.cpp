/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * This file is originally from the Kùzu project
 * (https://github.com/kuzudb/kuzu) Licensed under the MIT License. Modified by
 * Zhou Xiaoli in 2025 to support Neug-specific features.
 */

#include "neug/compiler/main/client_context.h"

#include <atomic>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cstdlib>
#include <deque>
#include <string_view>
#include <set>
#include <map>
#include <functional>

#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/rewriter/bound_rewrite_trace.h"
#include "neug/compiler/binder/rewriter/computation_prefix_rewriter.h"
#include "neug/compiler/catalog/catalog_entry/index_catalog_entry.h"
#include "neug/compiler/common/random_engine.h"
#include "neug/compiler/common/string_utils.h"
#include "neug/compiler/common/task_system/progress_bar.h"
#include "neug/compiler/common/task_system/task.h"
#include "neug/compiler/extension/extension.h"
#include "neug/compiler/extension/extension_manager.h"
#include "neug/compiler/gopt/g_constants.h"
#include "neug/compiler/graph/graph_entry.h"
#include "neug/compiler/main/metadata_manager.h"
#include "neug/compiler/main/option_config.h"
#include "neug/compiler/main/plan_printer.h"
#include "neug/compiler/optimizer/optimizer.h"
#include "neug/compiler/parser/parser.h"
#include "neug/compiler/parser/visitor/standalone_call_rewriter.h"
#include "neug/compiler/parser/visitor/statement_read_write_analyzer.h"
#include "neug/compiler/planner/planner.h"
#include "neug/compiler/planner/execution_cost.h"
#include "neug/compiler/storage/stats_manager.h"
#include "neug/utils/exception/exception.h"

#if defined(_WIN32)
#include "neug/compiler/common/windows_utils.h"
#endif

using namespace neug::parser;
using namespace neug::binder;
using namespace neug::common;
using namespace neug::catalog;
using namespace neug::planner;
using namespace neug::transaction;

namespace neug {
namespace main {

namespace {

// Opt-in aggregate counters for preparation profiling. No plans, bound graphs,
// expressions or per-branch events are serialized by this observer.
class IndexRewritePrepareProfile {
 public:
  enum Phase { BIND, CANONICAL_KEY, GET_BEST_PLAN, OPTIMIZE, EXECUTION_COST, COUNT };
  IndexRewritePrepareProfile() noexcept {
    const auto* setting = std::getenv("NEUG_INDEX_REWRITE_PREPARE_PROFILE");
    enabled = setting && std::string_view(setting) == "1";
    if (enabled) {
      started = Clock::now();
      static std::atomic<uint64_t> next{0};
      id = next.fetch_add(1, std::memory_order_relaxed);
    }
  }
  template<class F> auto measure(Phase phase, F&& operation)
      -> decltype(operation()) {
    if (!enabled) return operation();
    PhaseTimer timer{*this, phase};
    return operation();
  }
  void attemptedBinding() noexcept { if (enabled) ++attemptedBindings; }
  void deduplicatedState() noexcept { if (enabled) ++deduplicated; }
  void emit(bool success, bool compared, uint64_t planned, uint64_t failures,
            bool complete) noexcept {
    if (!enabled) return;
    const auto total = elapsed(started);
    try {
      rapidjson::Document json;
      json.SetObject();
      auto& alloc = json.GetAllocator();
      json.AddMember("version", 1, alloc);
      json.AddMember("prepare_id", id, alloc);
      json.AddMember("scope", "prepare_no_lock_excluding_parse", alloc);
      json.AddMember("success", success, alloc);
      json.AddMember("total_ns", total, alloc);
      const auto* fullTrace = std::getenv("NEUG_INDEX_REWRITE_SEARCH_TRACE");
      json.AddMember("full_trace_enabled",
                     fullTrace && std::string_view(fullTrace) == "1", alloc);
      json.AddMember("search_compared", compared, alloc);
      json.AddMember("search_complete", complete, alloc);
      json.AddMember("search_attempted_bindings", attemptedBindings, alloc);
      json.AddMember("search_planned_alternatives", planned, alloc);
      json.AddMember("search_deduplicated_states", deduplicated, alloc);
      json.AddMember("search_failures", failures, alloc);
      rapidjson::Value phases(rapidjson::kObjectType);
      static constexpr const char* names[] = {
          "bind", "canonical_key", "get_best_plan", "optimize", "execution_cost"};
      uint64_t measured = 0;
      for (size_t i = 0; i < COUNT; ++i) {
        rapidjson::Value phase(rapidjson::kObjectType);
        phase.AddMember("calls", calls[i], alloc);
        phase.AddMember("elapsed_ns", nanos[i], alloc);
        phases.AddMember(rapidjson::Value(names[i], alloc), phase, alloc);
        measured += nanos[i];
      }
      json.AddMember("phases", phases, alloc);
      json.AddMember("other_ns", total >= measured ? total - measured : 0, alloc);
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      json.Accept(writer);
      LOG(INFO) << "INDEX_REWRITE_PREPARE_PROFILE " << buffer.GetString();
    } catch (...) {
      // Profiling must not change successful preparation or error handling.
      try { LOG(INFO) << "INDEX_REWRITE_PREPARE_PROFILE_CAPTURE_ERROR"; }
      catch (...) {}
    }
  }
 private:
  using Clock = std::chrono::steady_clock;
  static uint64_t elapsed(Clock::time_point from) noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - from).count());
  }
  struct PhaseTimer {
    IndexRewritePrepareProfile& profile;
    Phase phase;
    Clock::time_point start;
    PhaseTimer(IndexRewritePrepareProfile& profile, Phase phase) noexcept
        : profile{profile}, phase{phase}, start{Clock::now()} {
      ++profile.calls[phase];
    }
    ~PhaseTimer() noexcept { profile.nanos[phase] += elapsed(start); }
  };
  bool enabled = false;
  uint64_t id = 0, attemptedBindings = 0, deduplicated = 0;
  Clock::time_point started{};
  std::array<uint64_t, COUNT> calls{}, nanos{};
};

// Development-only observer. The exact switch value "1" enables JSON lines in
// the existing INFO log. No extra binds, planner calls or cost estimates run.
// All serialization is lazy and isolated from the selection decision. This
// records prepared logical/operator plans, not execution of physical operators.
class IndexRewriteSearchTrace {
  struct Record {
    rapidjson::Document json;
    Record() { json.SetObject(); }
    rapidjson::Value string(const std::string& value) {
      return rapidjson::Value(value.data(), value.size(), json.GetAllocator());
    }
    void text(const char* key, const std::string& value) {
      json.AddMember(string(key), string(value), json.GetAllocator());
    }
    void number(const char* key, uint64_t value) {
      json.AddMember(string(key), rapidjson::Value(value), json.GetAllocator());
    }
    void flag(const char* key, bool value) {
      json.AddMember(string(key), rapidjson::Value(value), json.GetAllocator());
    }
    void strings(const char* key, const std::vector<std::string>& values) {
      rapidjson::Value array(rapidjson::kArrayType);
      for (const auto& value : values) array.PushBack(string(value), json.GetAllocator());
      json.AddMember(string(key), array, json.GetAllocator());
    }
    void raw(const char* key, const std::string& value) {
      rapidjson::Document parsed;
      parsed.Parse(value.data(), value.size());
      if (parsed.HasParseError()) throw std::runtime_error("Invalid diagnostic JSON");
      rapidjson::Value copy;
      copy.CopyFrom(parsed, json.GetAllocator());
      json.AddMember(string(key), copy, json.GetAllocator());
    }
    void plan(const LogicalPlan& value, ClientContext& context) {
      number("planner_cost", value.getCost());
      flag("empty_plan", value.isEmpty());
      if (!value.isEmpty()) {
        number("cardinality", value.getCardinality());
        text("plan_text", value.toString());
        raw("structured_plan", structuredLogicalPlanJson(*value.getLastOperator(), context));
      }
    }
    std::string dump() const {
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      json.Accept(writer);
      return {buffer.GetString(), buffer.GetSize()};
    }
  };
 public:
  IndexRewriteSearchTrace() noexcept {
    const auto* setting = std::getenv("NEUG_INDEX_REWRITE_SEARCH_TRACE");
    enabled = setting && std::string_view(setting) == "1";
    if (enabled) {
      static std::atomic<uint64_t> next{0};
      id = next.fetch_add(1, std::memory_order_relaxed);
    }
  }
  template<class F> void event(const char* name, F&& populate) noexcept {
    if (!enabled) return;
    try {
      Record record;
      record.number("version", 1);
      record.number("prepare_id", id);
      const auto eventNumber = sequence++;
      record.number("event_number", eventNumber);
      record.text("event", name);
      populate(record);
      // glog bounds each message. Frame small hex chunks so large graphs and
      // UTF-8 strings cannot be silently truncated or split into invalid JSON.
      const auto serialized = record.dump();
      constexpr size_t chunkBytes = 1024;
      const auto parts = (serialized.size() + chunkBytes - 1) / chunkBytes;
      constexpr char hex[] = "0123456789abcdef";
      for (size_t part = 0; part < parts; ++part) {
        const auto offset = part * chunkBytes;
        const auto length = std::min(chunkBytes, serialized.size() - offset);
        std::string encoded;
        encoded.reserve(2 * length);
        for (size_t i = 0; i < length; ++i) {
          const auto byte = static_cast<unsigned char>(serialized[offset + i]);
          encoded.push_back(hex[byte >> 4]);
          encoded.push_back(hex[byte & 15]);
        }
        Record frame;
        frame.number("prepare_id", id);
        frame.number("event_number", eventNumber);
        frame.number("part", part);
        frame.number("parts", parts);
        frame.number("total_bytes", serialized.size());
        frame.text("payload_hex", encoded);
        LOG(INFO) << "INDEX_REWRITE_SEARCH_TRACE_PART " << frame.dump();
      }
    } catch (...) {
      // Failed tracing must not turn an otherwise valid plan into a failure.
      // The run is diagnostically incomplete if this marker is present.
      try {
        LOG(INFO) << "INDEX_REWRITE_SEARCH_TRACE_CAPTURE_ERROR prepare_id=" << id;
      } catch (...) {}
    }
  }
  void stateKey(const std::vector<std::string>& prefix,
                const std::optional<std::string>& key, bool inserted) noexcept {
    event("state_key", [&](auto& record) {
      record.strings("prefix", prefix);
      record.flag("canonical_key_available", key.has_value());
      record.flag("deduplicated", key && !inserted);
      if (!key) {
        record.text("reason", "outside_canonical_count_grammar");
        return;
      }
      record.text("canonical_key", *key);
      record.text("reason", inserted ? "first_key_occurrence" : "already_planned_key");
      if (inserted) owners.emplace(*key, prefix);
      const auto first = owners.find(*key);
      record.flag("owner_recorded", first != owners.end());
      if (first != owners.end()) record.strings("first_prefix", first->second);
    });
  }
 private:
  bool enabled = false;
  uint64_t id = 0, sequence = 0;
  std::map<std::string, std::vector<std::string>> owners;
};

// For this closed grammar, a typed multigraph and binding-aware scalar
// expressions describe the entire global COUNT/SUM input.
// Independent edge substitutions can reach the same graph in many orders;
// plan that graph once instead of spending the search budget on permutations.
// Keep a multiset of edges: parallel occurrences still multiply the row bag.
// Only total BOOL/INT64 scalar operations are admitted. INT64 multiplication
// uses the executor's wrapping arithmetic, so its factor multiset is exact;
// floating arithmetic, division, arbitrary functions, OPTIONAL, projected
// identities and pipelines remain outside the key. This only memoizes search
// states: it does not change the expression evaluated by the selected plan.
std::optional<std::string> plainCountRewriteState(const BoundStatement& statement,
    const std::vector<std::string>& nextChoices,
    const std::unordered_map<std::string, std::string>* renamedNodes = nullptr) {
  if (statement.getStatementType() != StatementType::QUERY) return std::nullopt;
  const auto& query = statement.constCast<BoundRegularQuery>();
  if (query.getNumSingleQueries() != 1 || !query.getPreQueryPart().empty() ||
      !query.getPreQueryExprs().empty() || query.getPostSingleQuery()) return std::nullopt;
  const auto* single = query.getSingleQuery(0);
  if (single->getNumQueryParts() != 1) return std::nullopt;
  const auto* part = single->getQueryPart(0);
  if (part->hasUpdatingClause() || part->getNumReadingClause() != 1 ||
      part->getProjectionBodyPredicate() || !part->hasProjectionBody()) return std::nullopt;
  const auto* body = part->getProjectionBody();
  if (body->isDistinct() || !body->getGroupByExpressions().empty() ||
      !body->getOrderByExpressions().empty() || body->getSkipNumber() || body->getLimitNumber() ||
      body->getAggregateExpressions().size() != 1 || body->getProjectionExpressions().size() != 1)
    return std::nullopt;
  const auto aggregate = body->getAggregateExpressions().front();
  if (aggregate->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
      aggregate->getUniqueName() != body->getProjectionExpressions().front()->getUniqueName())
    return std::nullopt;
  const auto& count = aggregate->constCast<AggregateFunctionExpression>();
  const auto plainCount = count.getFunction().name == "COUNT_STAR" && !count.getNumChildren();
  const auto weightedCount = count.getFunction().name == "SUM" && count.getNumChildren() == 1 &&
      count.getDataType().getLogicalTypeID() == LogicalTypeID::INT64;
  if ((!plainCount && !weightedCount) || count.isDistinct())
    return std::nullopt;
  const auto* clause = part->getReadingClause(0);
  if (clause->getClauseType() != ClauseType::MATCH) return std::nullopt;
  const auto& match = clause->constCast<BoundMatchClause>();
  if (match.getMatchClauseType() != MatchClauseType::MATCH ||
      match.getQueryGraphCollection()->getNumQueryGraphs() != 1) return std::nullopt;
  const auto* graph = match.getQueryGraphCollection()->getQueryGraph(0);
  const auto append = [](std::string& out, const std::string& value) {
    out += std::to_string(value.size()) + ":" + value;
  };
  const auto tableKey = [&](const auto& tableIDs) {
    auto ids = tableIDs;
    std::sort(ids.begin(), ids.end());
    std::string out;
    for (auto id : ids) append(out, std::to_string(id));
    return out;
  };
  const auto nodeName = [&](const std::string& name) {
    return renamedNodes && !name.empty() ? renamedNodes->at(name) : name;
  };
  std::vector<std::string> nodes, edges;
  std::unordered_map<std::string, std::string> owners;
  for (const auto& node : graph->getQueryNodes()) {
    std::string key;
    append(key, nodeName(node->getUniqueName()));
    append(key, tableKey(node->getTableIDs()));
    owners.emplace(node->getUniqueName(), "node/" + key);
    nodes.push_back(std::move(key));
  }
  for (const auto& rel : graph->getQueryRels()) {
    if (rel->isRecursive() || rel->getRelType() != QueryRelType::NON_RECURSIVE)
      return std::nullopt;
    std::string key;
    auto src = nodeName(rel->getSrcNodeName()), dst = nodeName(rel->getDstNodeName());
    auto left = nodeName(rel->getLeftNode() ? rel->getLeftNode()->getUniqueName() : "");
    auto right = nodeName(rel->getRightNode() ? rel->getRightNode()->getUniqueName() : "");
    if (renamedNodes && rel->getDirectionType() == RelDirectionType::BOTH) {
      // This grammar never observes relationship direction/identity. Reversing
      // an undirected occurrence is the same multiset of endpoint bindings.
      if (dst < src) std::swap(src, dst);
      if (right < left) std::swap(left, right);
    } else if (renamedNodes && rel->getDirectionType() == RelDirectionType::SINGLE) {
      // (a)-[:R]->(b) and (b)<-[:R]-(a) have identical ordered
      // source/destination bindings. Left/right only records the spelling.
      left = src;
      right = dst;
    }
    append(key, src);
    append(key, dst);
    append(key, left);
    append(key, right);
    append(key, std::to_string(static_cast<unsigned>(rel->getDirectionType())));
    append(key, tableKey(rel->getTableIDs()));
    owners.emplace(rel->getUniqueName(), "rel/" + key);
    edges.push_back(std::move(key));
  }
  std::sort(nodes.begin(), nodes.end());
  std::sort(edges.begin(), edges.end());
  // A property of one of two structurally identical relation occurrences
  // cannot be identified by endpoints alone. Keep those cases unmerged.
  if ((renamedNodes || !plainCount || clause->getPredicate()) &&
      std::adjacent_find(edges.begin(), edges.end()) != edges.end()) return std::nullopt;
  std::function<std::optional<std::string>(const Expression&)> scalarKey;
  scalarKey = [&](const Expression& expression) -> std::optional<std::string> {
    const auto type = expression.getDataType().getLogicalTypeID();
    if (type != LogicalTypeID::INT64 && type != LogicalTypeID::BOOL) return std::nullopt;
    std::string result;
    append(result, expression.getDataType().toString());
    if (expression.expressionType == ExpressionType::PROPERTY) {
      const auto& property = expression.constCast<PropertyExpression>();
      const auto owner = owners.find(property.getVariableName());
      if (owner == owners.end() || !property.isSingleLabel() || expression.getNumChildren())
        return std::nullopt;
      append(result, "property");
      append(result, owner->second);
      append(result, std::to_string(property.getSingleTableID()));
      append(result, property.getPropertyName());
      return result;
    }
    if (expression.expressionType == ExpressionType::LITERAL) {
      const auto& value = expression.constCast<LiteralExpression>().getValue();
      append(result, value.isNull() ? "null" : "literal");
      append(result, value.toString());
      return result;
    }
    if (expression.expressionType == ExpressionType::CASE_ELSE) {
      // Preserve the complete NULL-to-zero wrapper, including its binding.
      // No property is presumed non-NULL from its name or index metadata.
      const auto& conditional = expression.constCast<CaseExpression>();
      if (conditional.getNumCaseAlternatives() != 1 ||
          conditional.getElseExpression()->expressionType != ExpressionType::PROPERTY) return std::nullopt;
      const auto& property = conditional.getElseExpression()->constCast<PropertyExpression>();
      const auto* alternative = conditional.getCaseAlternative(0);
      if (!property.isSingleLabel() ||
          alternative->whenExpression->expressionType != ExpressionType::IS_NULL ||
          alternative->whenExpression->getNumChildren() != 1 ||
          alternative->whenExpression->getChild(0)->getUniqueName() != property.getUniqueName() ||
          alternative->thenExpression->expressionType != ExpressionType::LITERAL ||
          alternative->thenExpression->getDataType().getLogicalTypeID() != LogicalTypeID::INT64)
        return std::nullopt;
      const auto zero = alternative->thenExpression->constCast<LiteralExpression>().getValue();
      if (zero.isNull() || zero.getValue<int64_t>() != 0) return std::nullopt;
      const auto value = scalarKey(property);
      if (!value) return std::nullopt;
      append(result, "zero-if-null");
      append(result, *value);
      return result;
    }
    const auto* function = dynamic_cast<const ScalarFunctionExpression*>(&expression);
    if (!function) return std::nullopt;
    const auto& name = function->getFunction().name;
    const auto multiply = name == "*" && type == LogicalTypeID::INT64;
    const auto conjunction = expression.expressionType == ExpressionType::AND && type == LogicalTypeID::BOOL;
    const auto comparison = expression.expressionType == ExpressionType::EQUALS ||
        expression.expressionType == ExpressionType::NOT_EQUALS ||
        expression.expressionType == ExpressionType::GREATER_THAN ||
        expression.expressionType == ExpressionType::GREATER_THAN_EQUALS ||
        expression.expressionType == ExpressionType::LESS_THAN ||
        expression.expressionType == ExpressionType::LESS_THAN_EQUALS;
    if ((!multiply && !conjunction && !comparison) || expression.getNumChildren() != 2)
      return std::nullopt;
    append(result, std::to_string(static_cast<unsigned>(expression.expressionType)));
    append(result, name);
    std::vector<std::string> children;
    std::function<bool(const Expression&)> collect = [&](const Expression& child) {
      const auto* nested = dynamic_cast<const ScalarFunctionExpression*>(&child);
      if ((multiply || conjunction) && nested && nested->getFunction().name == name &&
          child.expressionType == expression.expressionType && child.getDataType() == expression.getDataType() &&
          child.getNumChildren() == 2) {
        return collect(*child.getChild(0)) && collect(*child.getChild(1));
      }
      if ((multiply && child.getDataType().getLogicalTypeID() != LogicalTypeID::INT64) ||
          (conjunction && child.getDataType().getLogicalTypeID() != LogicalTypeID::BOOL)) return false;
      auto key = scalarKey(child);
      if (!key) return false;
      children.push_back(std::move(*key));
      return true;
    };
    for (const auto& child : expression.getChildren()) if (!collect(*child)) return std::nullopt;
    if (multiply || conjunction) std::sort(children.begin(), children.end());
    for (const auto& child : children) append(result, child);
    return result;
  };
  std::string key;
  append(key, aggregate->getDataType().toString());
  append(key, count.getFunction().name);
  if (weightedCount) {
    const auto value = scalarKey(*count.getChild(0));
    if (!value) return std::nullopt;
    append(key, *value);
  }
  if (clause->getPredicate()) {
    const auto predicate = scalarKey(*clause->getPredicate());
    if (!predicate) return std::nullopt;
    append(key, *predicate);
  } else append(key, "no-predicate");
  for (const auto& name : body->getProjectionNames()) append(key, name);
  key += "/nodes/";
  for (const auto& node : nodes) append(key, node);
  key += "/edges/";
  for (const auto& edge : edges) append(key, edge);
  std::vector<std::string> continuations;
  for (const auto& choice : nextChoices) {
    if (!renamedNodes) {
      continuations.push_back(choice);
      continue;
    }
    // acceptRewriteChoice emits length-prefixed index/name tokens, then '/'
    // and relationship names. Remap every binding in that exact continuation
    // set as well as in the graph and scalar expressions. Unknown or ambiguous
    // bindings decline memoization; they can never silently lose a branch.
    std::string normalized;
    size_t position = 0;
    const auto token = [&]() -> std::optional<std::string> {
      size_t length = 0;
      const auto start = position;
      while (position < choice.size() && choice[position] >= '0' && choice[position] <= '9') {
        const auto digit = size_t(choice[position++] - '0');
        if (length > choice.size() / 10) return std::nullopt;
        length = length * 10 + digit;
      }
      if (position == start || position >= choice.size() || choice[position++] != ':' ||
          length > choice.size() - position) return std::nullopt;
      auto value = choice.substr(position, length);
      position += length;
      return value;
    };
    const auto index = token();
    if (!index) return std::nullopt;
    append(normalized, *index);
    while (position < choice.size() && choice[position] != '/') {
      const auto name = token();
      if (!name || !renamedNodes->contains(*name)) return std::nullopt;
      append(normalized, renamedNodes->at(*name));
    }
    if (position >= choice.size() || choice[position++] != '/') return std::nullopt;
    normalized += '/';
    while (position < choice.size()) {
      const auto name = token();
      if (!name || !owners.contains(*name) || !owners.at(*name).starts_with("rel/"))
        return std::nullopt;
      append(normalized, owners.at(*name));
    }
    continuations.push_back(std::move(normalized));
  }
  std::sort(continuations.begin(), continuations.end());
  key += "/next/";
  for (const auto& choice : continuations) append(key, choice);
  return key;
}

std::optional<std::string> canonicalCountRewriteState(const BoundStatement& statement,
    const std::vector<std::string>& nextChoices) {
  auto original = plainCountRewriteState(statement, nextChoices);
  if (!original) return std::nullopt;
  const auto* clause = statement.constCast<BoundRegularQuery>().getSingleQuery(0)
      ->getQueryPart(0)->getReadingClause(0);
  const auto* graph = clause->constCast<BoundMatchClause>().getQueryGraphCollection()->getQueryGraph(0);
  std::map<std::vector<table_id_t>, std::vector<std::string>> byType;
  for (const auto& node : graph->getQueryNodes()) {
    auto tables = node->getTableIDs();
    std::sort(tables.begin(), tables.end());
    byType[tables].push_back(node->getUniqueName());
  }
  // Exhaust the small label-preserving renaming space. This is a bijection
  // of complete bindings, never a merge of two variables or two edge rows.
  // Larger/asymmetric inputs retain the conservative binding-specific key.
  size_t permutations = 1;
  std::vector<std::vector<std::string>> groups;
  for (auto& [_, names] : byType) {
    for (size_t i = 2; i <= names.size(); ++i) {
      if (permutations > 720 / i) return original;
      permutations *= i;
    }
    std::sort(names.begin(), names.end());
    groups.push_back(std::move(names));
  }
  if (permutations == 1) return original;
  std::unordered_map<std::string, std::string> names;
  std::optional<std::string> smallest;
  bool supported = true;
  std::function<void(size_t)> enumerate = [&](size_t group) {
    if (!supported) return;
    if (group == groups.size()) {
      auto key = plainCountRewriteState(statement, nextChoices, &names);
      if (!key) { supported = false; return; }
      if (!smallest || *key < *smallest) smallest = std::move(key);
      return;
    }
    auto order = groups[group];
    do {
      for (size_t i = 0; i < order.size(); ++i)
        names[order[i]] = "n" + std::to_string(group) + "." + std::to_string(i);
      enumerate(group + 1);
    } while (supported && std::next_permutation(order.begin(), order.end()));
  };
  enumerate(0);
  return supported ? std::optional<std::string>{"alpha/" + *smallest} : original;
}
}  // namespace

ActiveQuery::ActiveQuery() : interrupted{false} {}

void ActiveQuery::reset() {
  interrupted = false;
  timer = Timer();
}

ClientContext::ClientContext(MetadataManager* database)
    : localDatabase{database}, warningContext(&clientConfig) {
  graphEntrySet = std::make_unique<graph::GraphEntrySet>();
#if defined(_WIN32)
  clientConfig.homeDirectory = getEnvVariable("USERPROFILE");
#else
  clientConfig.homeDirectory = getEnvVariable("HOME");
#endif
  clientConfig.fileSearchPath = "";
  clientConfig.enableSemiMask = ClientConfigDefault::ENABLE_SEMI_MASK;
  clientConfig.enableZoneMap = ClientConfigDefault::ENABLE_ZONE_MAP;
  // clientConfig.numThreads = database->dbConfig.maxNumThreads;
  clientConfig.timeoutInMS = ClientConfigDefault::TIMEOUT_IN_MS;
  clientConfig.varLengthMaxDepth = ClientConfigDefault::VAR_LENGTH_MAX_DEPTH;
  clientConfig.enableProgressBar = ClientConfigDefault::ENABLE_PROGRESS_BAR;
  clientConfig.showProgressAfter = ClientConfigDefault::SHOW_PROGRESS_AFTER;
  clientConfig.recursivePatternSemantic =
      ClientConfigDefault::RECURSIVE_PATTERN_SEMANTIC;
  clientConfig.recursivePatternCardinalityScaleFactor =
      ClientConfigDefault::RECURSIVE_PATTERN_FACTOR;
  clientConfig.disableMapKeyCheck = ClientConfigDefault::DISABLE_MAP_KEY_CHECK;
  clientConfig.warningLimit = ClientConfigDefault::WARNING_LIMIT;
}

ClientContext::~ClientContext() = default;

Value ClientContext::getCurrentSetting(const std::string& optionName) const {
  auto lowerCaseOptionName = optionName;
  StringUtils::toLower(lowerCaseOptionName);
  const ConfigurationOption* option = nullptr;
  if (option != nullptr) {
    return option->getSetting(this);
  }
  if (extensionOptionValues.contains(lowerCaseOptionName)) {
    return extensionOptionValues.at(lowerCaseOptionName);
  }
  const auto defaultOption = getExtensionOption(lowerCaseOptionName);
  if (defaultOption != nullptr) {
    return defaultOption->defaultValue;
  }
  THROW_RUNTIME_ERROR("Invalid option name: " + lowerCaseOptionName + ".");
}

Transaction* ClientContext::getTransaction() const {
  return &neug::Constants::DEFAULT_TRANSACTION;
}

std::unique_ptr<function::ScanReplacementData> ClientContext::tryReplace(
    const std::string& objectName) const {
  for (auto& scanReplacement : scanReplacements) {
    auto replaceData = scanReplacement.replaceFunc(objectName);
    if (replaceData == nullptr) {
      continue;
    }
    return replaceData;
  }
  return nullptr;
}

void ClientContext::setExtensionOption(std::string name, Value value) {
  StringUtils::toLower(name);
  extensionOptionValues.insert_or_assign(name, std::move(value));
}

const main::ExtensionOption* ClientContext::getExtensionOption(
    std::string optionName) const {
  return localDatabase->extensionManager->getExtensionOption(optionName);
}

std::shared_ptr<storage::StatsManager> ClientContext::getStatsManager() const {
  return localDatabase->getStatsManager();
}

storage::MemoryManager* ClientContext::getMemoryManager() const {
  return localDatabase->memoryManager.get();
}

extension::ExtensionManager* ClientContext::getExtensionManager() const {
  return localDatabase->extensionManager.get();
}

Catalog* ClientContext::getCatalog() const {
  return localDatabase->catalog.get();
}

std::string ClientContext::getEnvVariable(const std::string& name) {
#if defined(_WIN32)
  auto envValue = WindowsUtils::utf8ToUnicode(name.c_str());
  auto result = _wgetenv(envValue.c_str());
  if (!result) {
    return std::string();
  }
  return WindowsUtils::unicodeToUTF8(result);
#else
  const char* env = getenv(name.c_str());
  if (!env) {
    return std::string();
  }
  return env;
#endif
}

bool ClientContext::hasDefaultDatabase() const { return false; }

graph::GraphEntrySet& ClientContext::getGraphEntrySetUnsafe() {
  return *graphEntrySet;
}

const graph::GraphEntrySet& ClientContext::getGraphEntrySet() const {
  return *graphEntrySet;
}

void ClientContext::cleanUp() {}

std::unique_ptr<PreparedStatement> ClientContext::prepare(
    std::string_view query) {
  return prepare(query, ParameterTypes{});
}

std::unique_ptr<PreparedStatement> ClientContext::prepare(
    std::string_view query, const ParameterTypes& parameterTypes) {
  std::unique_lock lck{mtx};
  auto parsedStatements = std::vector<std::shared_ptr<Statement>>();
  try {
    parsedStatements = parseQuery(query);
  } catch (std::exception& exception) {
    return preparedStatementWithError(exception.what());
  }
  if (parsedStatements.size() > 1) {
    return preparedStatementWithError(
        "Connection Exception: We do not support prepare multiple statements.");
  }
  auto result =
      prepareNoLock(parsedStatements[0], true /*shouldCommitNewTransaction*/,
                    std::nullopt, &parameterTypes);
  useInternalCatalogEntry_ = false;
  return result;
}

std::unique_ptr<PreparedStatement> ClientContext::preparedStatementWithError(
    std::string_view errMsg) {
  auto preparedStatement = std::make_unique<PreparedStatement>();
  preparedStatement->success = false;
  preparedStatement->errMsg = errMsg;
  return preparedStatement;
}

std::vector<std::shared_ptr<Statement>> ClientContext::parseQuery(
    std::string_view query) {
  if (query.empty()) {
    THROW_CONNECTION_EXCEPTION("Query is empty.");
  }
  std::vector<std::shared_ptr<Statement>> statements;
  auto parserTimer = TimeMetric(true /*enable*/);
  parserTimer.start();
  auto parsedStatements = Parser::parseQuery(query);
  parserTimer.stop();
  const auto avgParsingTime =
      parserTimer.getElapsedTimeMS() / parsedStatements.size() / 1.0;
  StandaloneCallRewriter standaloneCallAnalyzer{this,
                                                parsedStatements.size() == 1};
  for (auto i = 0u; i < parsedStatements.size(); i++) {
    auto rewriteQuery =
        standaloneCallAnalyzer.getRewriteQuery(*parsedStatements[i]);
    if (rewriteQuery.empty()) {
      parsedStatements[i]->setParsingTime(avgParsingTime);
      statements.push_back(std::move(parsedStatements[i]));
    } else {
      parserTimer.start();
      auto rewrittenStatements = Parser::parseQuery(rewriteQuery);
      parserTimer.stop();
      const auto avgRewriteParsingTime =
          parserTimer.getElapsedTimeMS() / rewrittenStatements.size() / 1.0;
      NEUG_ASSERT(rewrittenStatements.size() >= 1);
      for (auto j = 0u; j < rewrittenStatements.size() - 1; j++) {
        rewrittenStatements[j]->setParsingTime(avgParsingTime +
                                               avgRewriteParsingTime);
        rewrittenStatements[j]->setToInternal();
        statements.push_back(std::move(rewrittenStatements[j]));
      }
      auto lastRewrittenStatement = rewrittenStatements.back();
      lastRewrittenStatement->setParsingTime(avgParsingTime +
                                             avgRewriteParsingTime);
      statements.push_back(std::move(lastRewrittenStatement));
    }
  }
  return statements;
}

std::unique_ptr<PreparedStatement> ClientContext::prepareNoLock(
    std::shared_ptr<Statement> parsedStatement, bool shouldCommitNewTransaction,
    std::optional<std::unordered_map<std::string, std::shared_ptr<Value>>>
        inputParams, const ParameterTypes* parameterTypes) {
  IndexRewriteSearchTrace searchTrace;
  IndexRewritePrepareProfile profile;
  using ProfilePhase = IndexRewritePrepareProfile;
  auto preparedStatement = std::make_unique<PreparedStatement>();
  auto prepareTimer = TimeMetric(true /* enable */);
  prepareTimer.start();
  try {
    preparedStatement->preparedSummary.statementType =
        parsedStatement->getStatementType();
    auto readWriteAnalyzer = StatementReadWriteAnalyzer(this);

    readWriteAnalyzer.visit(*parsedStatement);

    preparedStatement->readOnly = readWriteAnalyzer.isReadOnly();
    preparedStatement->parsedStatement = std::move(parsedStatement);

    auto binder = Binder(this);
    if (inputParams) {
      binder.setInputParameters(*inputParams);
    } else if (parameterTypes) {
      binder.setParameterTypes(*parameterTypes);
    }
    const auto boundStatement = profile.measure(ProfilePhase::BIND, [&] {
      return binder.bind(*preparedStatement->parsedStatement);
    });
    binder.validateParameterTypes();
    preparedStatement->propertyUpdateReadPrefixProof = binder.getPropertyUpdateReadPrefixProof();
    preparedStatement->independentTerminalPropertySet = !preparedStatement->readOnly &&
        isIndependentTerminalPropertySet(*boundStatement, *this);
    searchTrace.event("initial_bound", [&](auto& record) {
      record.raw("bound_statement", structuredBoundStatementJson(*boundStatement, *this));
      record.flag("read_only", preparedStatement->readOnly);
    });
    preparedStatement->parameterMap = binder.getParameterMap();
    preparedStatement->statementResult = std::make_unique<BoundStatementResult>(
        boundStatement->getStatementResult()->copy());
    auto planner = Planner(this);
    auto bestPlan = profile.measure(ProfilePhase::GET_BEST_PLAN, [&] {
      return planner.getBestPlan(*boundStatement);
    });
    searchTrace.event("initial_get_best_plan", [&](auto& record) { record.plan(*bestPlan, *this); });
    profile.measure(ProfilePhase::OPTIMIZE, [&] {
      optimizer::Optimizer::optimize(bestPlan.get(), this,
                                     planner.getCardinalityEstimator());
    });
    if (preparedStatement->readOnly &&
        (preparedStatement->preparedSummary.statementType == StatementType::QUERY ||
         preparedStatement->preparedSummary.statementType == StatementType::EXPLAIN) &&
        binder.hasVariadicAnchorSplitCandidate()) {
      // Bind independently: sharing/mutating expressions between alternatives
      // would invalidate the connected plan and its output/parameter mapping.
      auto splitBinder = Binder(this);
      splitBinder.enableVariadicAnchorSplit();
      if (inputParams) {
        splitBinder.setInputParameters(*inputParams);
      } else if (parameterTypes) {
        splitBinder.setParameterTypes(*parameterTypes);
      }
      auto splitStatement = profile.measure(ProfilePhase::BIND, [&] {
        return splitBinder.bind(*preparedStatement->parsedStatement);
      });
      splitBinder.validateParameterTypes();
      auto splitPlanner = Planner(this);
      auto splitPlan = profile.measure(ProfilePhase::GET_BEST_PLAN, [&] {
        return splitPlanner.getBestPlan(*splitStatement);
      });
      profile.measure(ProfilePhase::OPTIMIZE, [&] {
        optimizer::Optimizer::optimize(splitPlan.get(), this,
                                       splitPlanner.getCardinalityEstimator());
      });
      const auto connectedCost = bestPlan->getCost();
      const auto splitCost = splitPlan->getCost();
      // The override is for plan diagnostics; normal execution compares both.
      const auto* overrideEnv = std::getenv("NEUG_VARIADIC_ANCHOR_PLAN");
      const auto overrideMode = overrideEnv == nullptr ? std::string_view{} :
                                                       std::string_view{overrideEnv};
      const auto useSplit = overrideMode == "split" ||
          (overrideMode != "connected" && splitCost < connectedCost);
      VLOG(1) << "Variadic anchor alternatives: connected_cost=" << connectedCost
              << " split_cost=" << splitCost
              << " selected=" << (useSplit ? "split" : "connected");
      if (useSplit) {
        preparedStatement->parameterMap = splitBinder.getParameterMap();
        preparedStatement->statementResult = std::make_unique<BoundStatementResult>(
            splitStatement->getStatementResult()->copy());
        bestPlan = std::move(splitPlan);
      }
    }
    const auto* rewriteDisabled = std::getenv("NEUG_DISABLE_INDEX_REWRITE");
    const auto mayCompareIndexRoutes =
        (preparedStatement->readOnly || preparedStatement->independentTerminalPropertySet) &&
        preparedStatement->preparedSummary.statementType == StatementType::QUERY &&
        !(rewriteDisabled && std::string_view(rewriteDisabled) == "1");
    if (mayCompareIndexRoutes) {
      const ExecutionCostOptions costOptions{
          preparedStatement->independentTerminalPropertySet,
          preparedStatement->independentTerminalPropertySet};
      const auto entries = getCatalog()->getExtendedIndexEntries(getTransaction());
      const auto hasReadyIndex = std::any_of(entries.begin(), entries.end(),
          [&](const auto* entry) {
            return !getCatalog()->isExtendedIndexBuilding(entry->getName());
          });
      const auto legacyCost = profile.measure(ProfilePhase::EXECUTION_COST, [&] {
        return estimateExecutionCost(*bestPlan, this, costOptions);
      });
      searchTrace.event("initial_prepared_plan", [&](auto& record) {
        record.plan(*bestPlan, *this);
        record.raw("execution_cost", executionCostJson(legacyCost));
        record.flag("has_ready_index", hasReadyIndex);
        record.flag("search_started", hasReadyIndex && legacyCost.supported);
      });
      if (hasReadyIndex && legacyCost.supported) {
        using Prefix = std::vector<std::string>;
        struct Alternative {
          std::unique_ptr<LogicalPlan> plan;
          std::unique_ptr<BoundStatementResult> result;
          std::unordered_map<std::string, std::shared_ptr<Value>> parameters;
          std::vector<std::string> next;
          std::string propertyUpdateProof;
        };
        std::set<std::string> plannedCountStates;
        const auto bindAlternative = [&](const Prefix& prefix, bool deduplicate = false) {
          const char* stage = "bind";
          const char* role = deduplicate ? "search" : "winner_replay";
          searchTrace.event("branch_begin", [&](auto& record) {
            record.strings("prefix", prefix);
            record.text("role", role);
          });
          try {
            auto alternativeBinder = Binder(this);
            alternativeBinder.setIndexRewriteReplay(prefix);
            if (inputParams) alternativeBinder.setInputParameters(*inputParams);
            else if (parameterTypes) alternativeBinder.setParameterTypes(*parameterTypes);
            auto statement = profile.measure(ProfilePhase::BIND, [&] {
              return alternativeBinder.bind(*preparedStatement->parsedStatement);
            });
            stage = "parameter_validation";
            alternativeBinder.validateParameterTypes();
            stage = "replay_validation";
            if (!alternativeBinder.indexRewriteReplayComplete())
              THROW_BINDER_EXCEPTION("Index rewrite alternative could not replay its verified prefix.");
            Alternative alternative;
            auto next = alternativeBinder.getIndexRewriteNextChoices();
            searchTrace.event("bound_branch", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("role", role);
              record.raw("bound_statement", structuredBoundStatementJson(*statement, *this));
              record.strings("next_raw", next);
              record.text("next_source", "binder_accepted_choices_only");
              record.text("matcher_rejection_reasons", "not_exposed_by_this_observer");
            });
            std::erase_if(next, [&](const auto& choice) {
              return std::find(prefix.begin(), prefix.end(), choice) != prefix.end();
            });
            searchTrace.event("next_after_prefix_filter", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("role", role);
              record.strings("next_available", next);
              record.text("filter_rule", "drop_exact_choice_already_in_prefix");
            });
            stage = "canonical_state";
            if (deduplicate) {
              auto key = profile.measure(ProfilePhase::CANONICAL_KEY, [&] {
                return canonicalCountRewriteState(*statement, next);
              });
              VLOG(3) << "Plain COUNT search state at depth " << prefix.size() << ": "
                      << (key ? *key : "outside closed grammar");
              const bool inserted = !key || plannedCountStates.insert(*key).second;
              searchTrace.stateKey(prefix, key, inserted);
              if (!inserted) {
                profile.deduplicatedState();
                return alternative;
              }
            }
            alternative.parameters = alternativeBinder.getParameterMap();
            alternative.propertyUpdateProof = alternativeBinder.getPropertyUpdateReadPrefixProof();
            alternative.result = std::make_unique<BoundStatementResult>(
                statement->getStatementResult()->copy());
            auto alternativePlanner = Planner(this);
            stage = "get_best_plan";
            alternative.plan = profile.measure(ProfilePhase::GET_BEST_PLAN, [&] {
              return alternativePlanner.getBestPlan(*statement);
            });
            searchTrace.event("branch_get_best_plan", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("role", role);
              record.plan(*alternative.plan, *this);
            });
            stage = "optimize";
            profile.measure(ProfilePhase::OPTIMIZE, [&] {
              optimizer::Optimizer::optimize(alternative.plan.get(), this,
                  alternativePlanner.getCardinalityEstimator());
            });
            searchTrace.event("branch_optimized_plan", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("role", role);
              record.plan(*alternative.plan, *this);
            });
            alternative.next = std::move(next);
            return alternative;
          } catch (const std::exception& error) {
            searchTrace.event("branch_failure", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("role", role);
              record.text("stage", stage);
              record.text("error", error.what());
            });
            throw;
          }
        };
        const auto preservesContract = [&](const Alternative& alternative) {
          if (alternative.result->getColumnNames() !=
                  preparedStatement->statementResult->getColumnNames() ||
              alternative.result->getColumnTypes() !=
                  preparedStatement->statementResult->getColumnTypes() ||
              alternative.parameters.size() != preparedStatement->parameterMap.size()) return false;
          for (const auto& [name, value] : alternative.parameters) {
            const auto original = preparedStatement->parameterMap.find(name);
            if (original == preparedStatement->parameterMap.end() ||
                value->getDataType() != original->second->getDataType()) return false;
          }
          return true;
        };
        // Search executable prefixes, including the original query. Every
        // branch is rebound from the immutable parsed statement. Choosing a
        // second index thus optimizes a complete first-step query and cannot
        // mutate the bound expressions of another alternative.
        constexpr size_t MAX_REWRITE_ALTERNATIVES = 1024;
        constexpr size_t MAX_REWRITE_DEPTH = 64;
        constexpr size_t MAX_REWRITE_BINDINGS = 32 * MAX_REWRITE_ALTERNATIVES;
        size_t attemptedBindings = 0;
        std::deque<Prefix> pending{Prefix{}};
        std::optional<Prefix> selectedPrefix;
        auto selectedWork = legacyCost.work;
        auto originalWork = uint64_t{0};
        bool originalSupported = false;
        while (!pending.empty() &&
               preparedStatement->indexRewriteAlternatives < MAX_REWRITE_ALTERNATIVES &&
               attemptedBindings < MAX_REWRITE_BINDINGS) {
          auto prefix = std::move(pending.front());
          pending.pop_front();
          ++attemptedBindings;
          profile.attemptedBinding();
          ++preparedStatement->indexRewriteAlternatives;
          try {
            BoundRewriteTracePause pause;
            auto alternative = bindAlternative(prefix, true);
            if (!alternative.plan) {
              --preparedStatement->indexRewriteAlternatives;
              continue;
            }
            const auto cost = profile.measure(ProfilePhase::EXECUTION_COST, [&] {
              return estimateExecutionCost(*alternative.plan, this, costOptions);
            });
            const bool contractPreserved = preservesContract(alternative);
            searchTrace.event("branch_cost", [&](auto& record) {
              record.strings("prefix", prefix);
              record.raw("execution_cost", executionCostJson(cost));
              record.flag("contract_preserved", contractPreserved);
              record.strings("column_names", alternative.result->getColumnNames());
              std::vector<std::string> types, parameters;
              for (const auto& type : alternative.result->getColumnTypes()) types.push_back(type.toString());
              for (const auto& [name, value] : alternative.parameters)
                parameters.push_back(name + ":" + value->getDataType().toString());
              std::sort(parameters.begin(), parameters.end());
              record.strings("column_types", types);
              record.strings("parameter_types", parameters);
              record.strings("expected_column_names", preparedStatement->statementResult->getColumnNames());
              types.clear();
              parameters.clear();
              for (const auto& type : preparedStatement->statementResult->getColumnTypes())
                types.push_back(type.toString());
              for (const auto& [name, value] : preparedStatement->parameterMap)
                parameters.push_back(name + ":" + value->getDataType().toString());
              std::sort(parameters.begin(), parameters.end());
              record.strings("expected_column_types", types);
              record.strings("expected_parameter_types", parameters);
              record.number("incumbent_work", selectedWork);
              record.flag("incumbent_is_initial_route", !selectedPrefix);
              if (selectedPrefix) record.strings("incumbent_prefix", *selectedPrefix);
            });
            if (!contractPreserved) {
              searchTrace.event("branch_pruned", [&](auto& record) {
                record.strings("prefix", prefix);
                record.text("reason", "output_or_parameter_contract_changed");
                record.strings("not_enqueued_choices", alternative.next);
                record.flag("stops_search", prefix.empty());
              });
              VLOG(1) << "Index rewrite alternative changed output/parameter contract at depth "
                      << prefix.size() << ": " << alternative.plan->toString();
              ++preparedStatement->indexRewriteSearchFailures;
              preparedStatement->indexRewriteSearchComplete = false;
              if (prefix.empty()) break;
              continue;
            }
            if (!cost.supported) {
              searchTrace.event("unsupported_cost", [&](auto& record) {
                record.strings("prefix", prefix);
                record.text("reason", cost.reason);
                record.flag("stops_search", prefix.empty());
                record.flag("successors_still_eligible", !prefix.empty());
              });
              VLOG(1) << "Index rewrite alternative cost unavailable at depth "
                      << prefix.size() << ": " << cost.reason << " plan="
                      << alternative.plan->toString();
              ++preparedStatement->indexRewriteSearchFailures;
              preparedStatement->indexRewriteSearchComplete = false;
              if (prefix.empty()) break;
              // A later independent substitution can remove the operator
              // whose cost is unsupported. Keep exploring this legal query.
            }
            if (prefix.empty()) {
              originalSupported = true;
              originalWork = cost.work;
            }
            if (cost.supported && (cost.work < selectedWork ||
                (cost.work == selectedWork &&
                 (!selectedPrefix || prefix.size() < selectedPrefix->size())))) {
              selectedWork = cost.work;
              selectedPrefix = prefix;
              searchTrace.event("incumbent_changed", [&](auto& record) {
                record.strings("prefix", prefix);
                record.number("work", selectedWork);
                record.text("rule", "lower_work_or_equal_work_shorter_prefix");
              });
            }
            if (prefix.size() >= MAX_REWRITE_DEPTH) {
              searchTrace.event("depth_limit", [&](auto& record) {
                record.strings("prefix", prefix);
                record.strings("not_enqueued_choices", alternative.next);
                record.number("limit", MAX_REWRITE_DEPTH);
              });
              if (!alternative.next.empty())
                preparedStatement->indexRewriteSearchComplete = false;
              continue;
            }
            for (auto& choice : alternative.next) {
              // Reapplying the same physical index to precisely the same
              // surviving bindings cannot consume a new independent region.
              if (std::find(prefix.begin(), prefix.end(), choice) != prefix.end()) continue;
              auto successor = prefix;
              successor.push_back(std::move(choice));
              searchTrace.event("enqueued", [&](auto& record) {
                record.strings("parent_prefix", prefix);
                record.strings("prefix", successor);
              });
              pending.push_back(std::move(successor));
            }
          } catch (const std::exception& error) {
            searchTrace.event("search_failure", [&](auto& record) {
              record.strings("prefix", prefix);
              record.text("error", error.what());
              record.flag("stops_search", prefix.empty());
            });
            ++preparedStatement->indexRewriteSearchFailures;
            preparedStatement->indexRewriteSearchComplete = false;
            VLOG(1) << "Index rewrite alternative declined: " << error.what();
            if (prefix.empty()) break;
          }
        }
        if (!pending.empty()) preparedStatement->indexRewriteSearchComplete = false;
        searchTrace.event("search_end", [&](auto& record) {
          record.number("attempted_bindings", attemptedBindings);
          record.number("planned_alternatives", preparedStatement->indexRewriteAlternatives);
          record.number("pending_prefixes", pending.size());
          record.number("failures", preparedStatement->indexRewriteSearchFailures);
          record.flag("complete", preparedStatement->indexRewriteSearchComplete);
          record.flag("alternative_limit_reached", preparedStatement->indexRewriteAlternatives >= MAX_REWRITE_ALTERNATIVES);
          record.flag("binding_limit_reached", attemptedBindings >= MAX_REWRITE_BINDINGS);
          record.flag("original_supported", originalSupported);
          record.number("original_work", originalWork);
          record.number("selected_work", selectedWork);
          record.flag("selected_is_initial_route", !selectedPrefix);
          if (selectedPrefix) record.strings("selected_prefix", *selectedPrefix);
          record.text("scope", "accepted_choices_after_current_proof_gates");
        });
        if (originalSupported) {
          preparedStatement->indexRewriteCostCompared = true;
          preparedStatement->indexRewriteOriginalWork = originalWork;
          preparedStatement->indexRewriteSelectedWork = selectedWork;
          if (selectedPrefix) {
            // Rebind the selected route outside the trace pause so semantic
            // evidence describes exactly the plan that will be executed.
            auto selected = bindAlternative(*selectedPrefix);
            const auto finalCost = profile.measure(ProfilePhase::EXECUTION_COST, [&] {
              return estimateExecutionCost(*selected.plan, this, costOptions);
            });
            searchTrace.event("winner_replay_cost", [&](auto& record) {
              record.strings("prefix", *selectedPrefix);
              record.raw("execution_cost", executionCostJson(finalCost));
              record.number("expected_work", selectedWork);
              record.flag("contract_preserved", preservesContract(selected));
            });
            if (!preservesContract(selected) || !finalCost.supported ||
                finalCost.work != selectedWork)
              THROW_BINDER_EXCEPTION("Index rewrite selected plan changed during final preparation.");
            preparedStatement->parameterMap = std::move(selected.parameters);
            preparedStatement->statementResult = std::move(selected.result);
            preparedStatement->propertyUpdateReadPrefixProof = std::move(selected.propertyUpdateProof);
            bestPlan = std::move(selected.plan);
          }
        }
        VLOG(1) << "Index rewrite alternatives: explored="
                << preparedStatement->indexRewriteAlternatives
                << " complete=" << preparedStatement->indexRewriteSearchComplete
                << " original_work=" << originalWork
                << " selected_work=" << selectedWork;
      }
    }
    searchTrace.event("prepared_winner", [&](auto& record) {
      record.plan(*bestPlan, *this);
      record.flag("search_compared", preparedStatement->indexRewriteCostCompared);
      record.number("selected_work", preparedStatement->indexRewriteSelectedWork);
    });
    preparedStatement->logicalPlan = std::move(bestPlan);

  } catch (std::exception& exception) {
    searchTrace.event("prepare_failure", [&](auto& record) { record.text("error", exception.what()); });
    preparedStatement->success = false;
    preparedStatement->errMsg = exception.what();
  }
  preparedStatement->useInternalCatalogEntry = useInternalCatalogEntry_;
  prepareTimer.stop();
  preparedStatement->preparedSummary.compilingTime =
      preparedStatement->parsedStatement->getParsingTime() +
      prepareTimer.getElapsedTimeMS();
  profile.emit(preparedStatement->success,
               preparedStatement->indexRewriteCostCompared,
               preparedStatement->indexRewriteAlternatives,
               preparedStatement->indexRewriteSearchFailures,
               preparedStatement->indexRewriteSearchComplete);
  return preparedStatement;
}

}  // namespace main
}  // namespace neug

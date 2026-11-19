/**
 * Copyright 2026 Alibaba Group Holding Limited.
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
#include "neug/compiler/index/index_candidate_enumerator.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace neug::index {
namespace {
using Names = std::set<std::string>;
std::string upper(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string token(const std::string& s) {
  return std::to_string(s.size()) + ":" + s;
}
std::string identifier(const std::string& s) {
  std::string out = "`";
  for (char c : s) {
    out += c;
    if (c == '`')
      out += '`';
  }
  return out + '`';
}
std::string quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\\' || c == '\'')
      out += '\\';
    if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out += c;
  }
  return out + '\'';
}
template <class T>
bool contains(const std::vector<T>& values, const T& v) {
  return std::find(values.begin(), values.end(), v) != values.end();
}
std::vector<std::string> sorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}
Carrier normalizedCarrier(Carrier value) {
  value.sourceLabels = sorted(std::move(value.sourceLabels));
  value.destinationLabels = sorted(std::move(value.destinationLabels));
  return value;
}
std::string carrierCapabilitiesKey(const Carrier& value) {
  std::string key = token(std::to_string(value.coversAllKeys)) +
                    token(std::to_string(value.uniqueKey)) +
                    token(std::to_string(value.preservesNull)) +
                    token(std::to_string(value.preservesGroupPresence)) +
                    token(std::to_string(value.resolvesDestinationId));
  for (const auto& type : value.scalarTypes)
    key += token(type);
  return key;
}
bool intersects(const std::vector<std::string>& a,
                const std::vector<std::string>& b) {
  if (a.empty() || b.empty())
    return true;
  for (const auto& v : a)
    if (contains(b, v))
      return true;
  return false;
}
bool includes(const std::vector<std::string>& declared,
              const std::vector<std::string>& actual) {
  return !actual.empty() &&
         std::all_of(actual.begin(), actual.end(),
                     [&](const auto& v) { return contains(declared, v); });
}
bool integerType(const std::string& type) {
  static const Names types = {"INT8",  "INT16",  "INT32",  "INT64",  "INT128",
                              "UINT8", "UINT16", "UINT32", "UINT64", "SERIAL"};
  return types.contains(upper(type));
}
bool decimalType(const std::string& type) {
  const auto t = upper(type);
  if (t == "DECIMAL")
    return true;
  if (!t.starts_with("DECIMAL(") || !t.ends_with(")"))
    return false;
  return std::all_of(t.begin() + 8, t.end() - 1, [](unsigned char c) {
    return std::isdigit(c) || c == ',' || c == ' ';
  });
}
bool scalar(const std::string& type) {
  static const Names types = {"BOOL",         "BOOLEAN",      "STRING",
                              "DATE",         "DATE32",       "INTERVAL",
                              "FLOAT",        "DOUBLE",       "UUID",
                              "BLOB",         "TIMESTAMP",    "TIMESTAMP64",
                              "TIMESTAMP_NS", "TIMESTAMP_MS", "TIMESTAMP_SEC",
                              "TIMESTAMP_S",  "TIMESTAMP_TZ"};
  return types.contains(upper(type)) || integerType(type) || decimalType(type);
}
bool exactNumeric(const std::string& type) {
  return integerType(type) || decimalType(type);
}
// A TRUE comparison of these values is reflexive/ordered. Floating values are
// deliberately excluded: NaN can make x <> x true, invalidating an identity
// proof. Nullable operands are safe here because we reason only about rows on
// which the complete MATCH predicate is TRUE, never FALSE or UNKNOWN.
bool identityComparable(const std::string& type) {
  static const Names types = {"BOOL",          "BOOLEAN",      "STRING",
                              "DATE",          "DATE32",       "TIMESTAMP",
                              "TIMESTAMP64",   "TIMESTAMP_NS", "TIMESTAMP_MS",
                              "TIMESTAMP_SEC", "TIMESTAMP_S",  "TIMESTAMP_TZ"};
  return exactNumeric(type) || types.contains(upper(type));
}

// Prove a predicate impossible after identifying the endpoints required for
// two relationship atoms to denote the same data edge. This is a small,
// conservative order solver, not a general constraint solver. AND facts may
// combine transitively; every OR branch must independently be impossible.
// Functions, casts, arithmetic and mixed types remain unknown.
class EdgeIdentityProof {
 public:
  explicit EdgeIdentityProof(const QueryPart& query) : query(query) {
    for (const auto& node : query.nodes)
      representatives[node.binding] = node.binding;
  }
  void identify(size_t left, size_t right) {
    const auto a = representative(query.nodes[left].binding);
    const auto b = representative(query.nodes[right].binding);
    if (a != b)
      representatives[b] = a;
  }
  bool impossible(const std::vector<Expression>& predicates) const {
    std::vector<const Expression*> conjuncts;
    std::function<void(const Expression&)> flatten = [&](const auto& e) {
      if (e.kind == ExpressionKind::Call && upper(e.name) == "AND") {
        for (const auto& child : e.children)
          flatten(child);
      } else {
        conjuncts.push_back(&e);
      }
    };
    for (const auto& predicate : predicates)
      flatten(predicate);
    std::map<std::string, size_t> terms;
    struct Relation {
      size_t left, right;
      std::string op;
    };
    std::vector<Relation> relations;
    const auto position = [&](const std::string& value) {
      return terms.try_emplace(value, terms.size()).first->second;
    };
    for (const auto* original : conjuncts) {
      if (original->kind != ExpressionKind::Call)
        continue;
      const auto name = upper(original->name);
      if (name == "OR") {
        if (!original->children.empty() &&
            std::all_of(original->children.begin(), original->children.end(),
                        [&](const auto& child) { return impossible({child}); }))
          return true;
        continue;
      }
      const auto* e = original;
      std::string op = name;
      if (name == "NOT" && original->children.size() == 1) {
        e = &original->children[0];
        static const std::map<std::string, std::string> inverse = {
            {"=", "<>"}, {"<>", "="}, {"!=", "="}, {"<", ">="},
            {">", "<="}, {"<=", ">"}, {">=", "<"}};
        auto it = inverse.find(upper(e->name));
        if (e->kind != ExpressionKind::Call || it == inverse.end())
          continue;
        op = it->second;
      }
      static const Names comparisons = {"=", "<>", "!=", "<", ">", "<=", ">="};
      if (!comparisons.contains(op) || e->children.size() != 2)
        continue;
      const auto& left = e->children[0];
      const auto& right = e->children[1];
      if (upper(left.type) != upper(right.type))
        continue;
      const bool equality = op == "=" || op == "<>" || op == "!=";
      auto a = term(left, equality), b = term(right, equality);
      if (!a || !b)
        continue;
      relations.push_back({position(*a), position(*b), op});
    }
    // 0: unknown, 1: <=, 2: <. Equality contributes both non-strict arcs.
    std::vector<std::vector<uint8_t>> order(terms.size(),
                                            std::vector<uint8_t>(terms.size()));
    for (size_t i = 0; i < terms.size(); ++i)
      order[i][i] = 1;
    std::vector<std::pair<size_t, size_t>> unequal;
    for (const auto& r : relations) {
      if (r.op == "<>" || r.op == "!=") {
        unequal.emplace_back(r.left, r.right);
        continue;
      }
      if (r.op == "<" || r.op == "<=" || r.op == "=")
        order[r.left][r.right] =
            std::max<uint8_t>(order[r.left][r.right], r.op == "<" ? 2 : 1);
      if (r.op == ">" || r.op == ">=" || r.op == "=")
        order[r.right][r.left] =
            std::max<uint8_t>(order[r.right][r.left], r.op == ">" ? 2 : 1);
    }
    for (size_t k = 0; k < terms.size(); ++k)
      for (size_t i = 0; i < terms.size(); ++i)
        for (size_t j = 0; j < terms.size(); ++j)
          if (order[i][k] && order[k][j])
            order[i][j] =
                std::max(order[i][j], std::max(order[i][k], order[k][j]));
    for (size_t i = 0; i < terms.size(); ++i)
      if (order[i][i] == 2)
        return true;
    return std::any_of(unequal.begin(), unequal.end(), [&](const auto& pair) {
      return order[pair.first][pair.second] && order[pair.second][pair.first];
    });
  }

 private:
  std::string representative(std::string binding) const {
    auto it = representatives.find(binding);
    while (it != representatives.end() && it->second != binding) {
      binding = it->second;
      it = representatives.find(binding);
    }
    return binding;
  }
  std::optional<std::string> term(const Expression& e, bool equality) const {
    if (!representatives.contains(e.binding) || !e.children.empty())
      return std::nullopt;
    if (e.kind == ExpressionKind::Property &&
        (identityComparable(e.type) ||
         (equality && upper(e.type) == "INTERNAL_ID")))
      return "P" + token(representative(e.binding)) + token(e.name) +
             token(upper(e.type));
    if (e.kind == ExpressionKind::Variable && equality &&
        upper(e.type) == "NODE")
      return "N" + token(representative(e.binding));
    return std::nullopt;
  }
  const QueryPart& query;
  std::map<std::string, std::string> representatives;
};

std::string sumResultType(const Expression& e) {
  return integerType(e.type) ? "INT64" : e.type;
}
Names dependencies(const Expression& e) {
  Names names;
  if (e.kind == ExpressionKind::Variable || e.kind == ExpressionKind::Property)
    names.insert(e.binding);
  for (const auto& child : e.children) {
    auto sub = dependencies(child);
    names.insert(sub.begin(), sub.end());
  }
  return names;
}
bool subset(const Names& a, const Names& b) {
  return std::includes(b.begin(), b.end(), a.begin(), a.end());
}
bool safeInput(const Expression& e) {
  return e.deterministic &&
         (e.safeToPrecompute || e.kind == ExpressionKind::ResidualPredicate) &&
         std::all_of(e.children.begin(), e.children.end(), safeInput);
}
bool deterministicInput(const Expression& e) {
  return e.deterministic &&
         std::all_of(e.children.begin(), e.children.end(), deterministicInput);
}
bool stableDistinctOperand(const Expression& e);
bool orderedExtremum(const Expression& e, const Constraints& constraints);
bool duplicateInsensitive(const QueryPart& q, const Constraints& constraints) {
  if (q.aggregates.empty())
    return q.distinct;
  return std::all_of(q.aggregates.begin(), q.aggregates.end(), [&](const auto& g) {
    return (g.distinct && g.children.size() == 1 &&
            stableDistinctOperand(g.children[0])) ||
           ((upper(g.name) == "MIN" || upper(g.name) == "MAX") &&
            orderedExtremum(g, constraints));
  });
}
bool rowExpression(const Expression& e) {
  // These are storage-only type names, not legal Cypher CAST targets.
  if (e.kind == ExpressionKind::Literal &&
      (upper(e.type) == "DATE32" || upper(e.type) == "TIMESTAMP64"))
    return false;
  return e.kind != ExpressionKind::Aggregate &&
         e.kind != ExpressionKind::ResidualPredicate && e.safeToPrecompute &&
         e.deterministic &&
         std::all_of(e.children.begin(), e.children.end(), rowExpression);
}
bool hasParameter(const Expression& e) {
  if (e.kind == ExpressionKind::Parameter)
    return true;
  return std::any_of(e.children.begin(), e.children.end(), hasParameter);
}
bool local(const Expression& e, const Names& vars, bool requireScalar = true) {
  return (!requireScalar || scalar(e.type)) && rowExpression(e) &&
         !hasParameter(e) && subset(dependencies(e), vars);
}
// These grouping expressions identify a complete typed node. A property is
// sufficient only for the declared primary key of one concrete node label.
std::optional<std::string> groupingNode(const QueryPart& q,
                                        const Expression& e) {
  for (const auto& n : q.nodes) {
    if (e.binding != n.binding)
      continue;
    if (e.kind == ExpressionKind::Variable ||
        (e.kind == ExpressionKind::Property && n.labels.size() == 1 &&
         !n.primaryKey.empty() && e.name == n.primaryKey &&
         e.type == n.primaryKeyType))
      return n.binding;
  }
  return std::nullopt;
}
Names groupingNodes(const QueryPart& q) {
  Names known;
  for (const auto& e : q.groupBy)
    if (auto node = groupingNode(q, e))
      known.insert(*node);
  return known;
}
Names queryFixedNodes(const QueryPart& q, const Constraints& constraints) {
  Names fixed;
  // A runtime equality on a unique identity chooses at most one graph node
  // for this invocation. The parameter remains in the residual predicate;
  // neither its value nor its spelling enters the materialized definition.
  for (const auto& predicate : q.predicates) {
    if (predicate.kind != ExpressionKind::Call || predicate.name != "=" ||
        predicate.children.size() != 2)
      continue;
    for (size_t side = 0; side < 2; ++side) {
      const auto& property = predicate.children[side];
      const auto& value = predicate.children[1 - side];
      if (property.kind != ExpressionKind::Property ||
          (value.kind != ExpressionKind::Parameter &&
           value.kind != ExpressionKind::Literal) ||
          (value.kind == ExpressionKind::Literal && value.nullable) ||
          property.type != value.type)
        continue;  // Do not infer uniqueness through lossy type coercion.
      for (const auto& node : q.nodes) {
        if (node.binding != property.binding || node.labels.size() != 1)
          continue;
        const auto unique = constraints.uniqueProperties.find(node.labels[0]);
        const auto nonnull = constraints.nonNullProperties.find(node.labels[0]);
        const bool declared = unique != constraints.uniqueProperties.end() &&
            nonnull != constraints.nonNullProperties.end() &&
            unique->second.contains(property.name) &&
            nonnull->second.contains(property.name);
        if ((!node.primaryKey.empty() && property.name == node.primaryKey &&
             property.type == node.primaryKeyType) || declared)
          fixed.insert(node.binding);
      }
    }
  }
  return fixed;
}
bool orderedExtremum(const Expression& e, const Constraints& constraints) {
  const auto type = upper(e.type);
  return scalar(e.type) &&
         (constraints.orderedExtrema || (type != "FLOAT" && type != "DOUBLE"));
}
bool stableDistinctOperand(const Expression& e) {
  const auto type = upper(e.type);
  // The execution engine uses hash-set equality. NaN and composites containing
  // it need not deduplicate a repeated input value. Entity identity is stable.
  return (scalar(type) && type != "FLOAT" && type != "DOUBLE") ||
         (e.kind == ExpressionKind::Variable &&
          (type == "NODE" || type == "REL" || type.starts_with("NODE(") ||
           type.starts_with("REL(")));
}
void splitAnd(const Expression& e, std::vector<Expression>& result) {
  if (e.kind == ExpressionKind::Call && upper(e.name) == "AND") {
    for (const auto& child : e.children)
      splitAnd(child, result);
  } else
    result.push_back(e);
}
// Returns false when the visitor requests an early stop. No bitmask overflow.
bool subsets(size_t n, size_t cap, size_t first,
             const std::function<bool(const std::vector<size_t>&)>& visit) {
  std::vector<size_t> selected;
  std::function<bool(size_t, size_t)> rec = [&](size_t begin, size_t left) {
    if (!left)
      return visit(selected);
    if (left > n - begin)
      return true;
    for (size_t i = begin; i <= n - left; ++i) {
      selected.push_back(i);
      if (!rec(i + 1, left - 1))
        return false;
      selected.pop_back();
    }
    return true;
  };
  for (size_t k = first; k <= std::min(n, cap); ++k)
    if (!rec(0, k))
      return false;
  return true;
}
Expression rename(Expression e,
                  const std::map<std::string, std::string>& names) {
  if (!e.binding.empty())
    e.binding = names.at(e.binding);
  for (auto& child : e.children)
    child = rename(std::move(child), names);
  return e;
}
std::string join(const std::vector<std::string>& values,
                 const std::string& sep) {
  std::string out;
  for (const auto& v : values) {
    if (!out.empty())
      out += sep;
    out += v;
  }
  return out;
}
std::string columnQuery(const Column& c) {
  const auto arg = (c.distinct ? "DISTINCT " : "") +
                   (c.expression ? c.expression->toCypher() : "*");
  switch (c.kind) {
  case ColumnKind::Projection:
  case ColumnKind::DestinationId:
    return arg;
  case ColumnKind::CountRows:
    return "COUNT(*)";
  case ColumnKind::CountNonNull:
    return "COUNT(" + arg + ")";
  case ColumnKind::Sum:
    return "SUM(" + arg + ")";
  case ColumnKind::Average:
    return "AVG(" + arg + ")";
  case ColumnKind::Minimum:
    return "MIN(" + arg + ")";
  case ColumnKind::Maximum:
    return "MAX(" + arg + ")";
  case ColumnKind::CountDistinctNonNull:
    return "COUNT(DISTINCT " + arg + ")";
  }
  throw std::logic_error("Invalid index column");
}
Candidate canonicalize(const Candidate& source, size_t permutationLimit,
                       bool& complete,
                       std::map<std::string, std::string>* mapping = nullptr) {
  auto orientation = [&](const std::vector<size_t>& nodes,
                         const std::vector<size_t>& relationships) {
    Candidate out = source;
    out.nodes.clear();
    out.relationships.clear();
    std::map<std::string, std::string> names;
    std::vector<size_t> remap(source.nodes.size());
    for (size_t i = 0; i < source.nodes.size(); ++i) {
      const auto old = nodes[i];
      auto node = source.nodes[old];
      node.binding = "n" + std::to_string(i);
      node.labels = sorted(node.labels);
      names[source.nodes[old].binding] = node.binding;
      remap[old] = i;
      out.nodes.push_back(std::move(node));
    }
    std::map<size_t, size_t> groups;
    for (size_t i = 0; i < source.relationships.size(); ++i) {
      auto rel = source.relationships[relationships[i]];
      auto oldBinding = rel.binding;
      rel.binding = "e" + std::to_string(i);
      names[oldBinding] = rel.binding;
      rel.source = remap.at(rel.source);
      rel.destination = remap.at(rel.destination);
      if (!rel.directed && rel.source > rel.destination)
        std::swap(rel.source, rel.destination);
      if (!groups.contains(rel.matchGroup))
        groups[rel.matchGroup] = groups.size();
      rel.matchGroup = groups.at(rel.matchGroup);
      rel.types = sorted(rel.types);
      out.relationships.push_back(std::move(rel));
    }
    for (auto& key : out.keys)
      key = names.at(key);
    for (auto& e : out.predicates)
      e = rename(std::move(e), names);
    std::sort(out.predicates.begin(), out.predicates.end(),
              [](const auto& a, const auto& b) {
                return a.structuralKey() < b.structuralKey();
              });
    for (auto& c : out.columns)
      if (c.expression)
        c.expression = rename(*c.expression, names);
    std::sort(out.columns.begin(), out.columns.end(),
              [](const auto& a, const auto& b) {
                return a.structuralKey() < b.structuralKey();
              });
    std::string key = token(std::to_string(static_cast<int>(out.kind)));
    for (const auto& n : out.nodes) {
      key += "N" + token(n.binding);
      for (const auto& label : n.labels)
        key += token(label);
      key += token(n.primaryKey) + token(n.primaryKeyType) + ";";
    }
    for (const auto& r : out.relationships) {
      key += "R" + token(std::to_string(r.source)) +
             token(std::to_string(r.destination));
      key += token(std::to_string(r.directed)) +
             token(std::to_string(r.recursive));
      key += token(std::to_string(r.lowerBound)) +
             token(std::to_string(r.upperBound));
      key += token(std::to_string(r.matchGroup)) + token(r.pathSemantics);
      for (const auto& t : r.types)
        key += token(t);
      key += ";";
    }
    for (const auto& k : out.keys)
      key += "K" + token(k);
    for (const auto& p : out.predicates)
      key += "F" + token(p.structuralKey());
    for (const auto& c : out.columns)
      key += "C" + token(c.structuralKey());
    out.logicalDefinitionKey = key;
    if (out.storage)
      key += "T" + token(out.storage->structuralKey());
    out.definitionKey = std::move(key);
    return std::make_pair(std::move(out), std::move(names));
  };
  // A source region is a graph, not necessarily the vertex/edge order in the
  // original MATCH text. Merely comparing that order with its reversal splits
  // isomorphic regions into different physical indexes, losing reuse of one
  // materialization at multiple disjoint query locations.
  //
  // Partition nodes by invariants (including ordered interface-key roles),
  // then visit permutations only within indistinguishable classes. Once node
  // positions are known, do the same for structurally identical edge atoms.
  // Edge occurrences are never collapsed: their bindings, predicates, columns,
  // direction, recursive bounds, and MATCH-group partition all enter the key.
  std::map<std::string, std::vector<size_t>> nodeClasses;
  for (size_t i = 0; i < source.nodes.size(); ++i) {
    const auto& node = source.nodes[i];
    std::string invariant;
    for (const auto& label : sorted(node.labels)) invariant += token(label);
    invariant += ";" + token(node.primaryKey) + token(node.primaryKeyType);
    for (size_t k = 0; k < source.keys.size(); ++k)
      if (source.keys[k] == node.binding) invariant += "K" + token(std::to_string(k));
    nodeClasses[invariant].push_back(i);
  }
  const auto classes = [](const auto& grouped) {
    std::vector<std::vector<size_t>> result;
    for (const auto& [key, values] : grouped) result.push_back(values);
    return result;
  };
  const auto permutations = [](std::vector<std::vector<size_t>> groups,
                               const auto& callback) {
    std::vector<size_t> order;
    std::function<bool(size_t)> visit = [&](size_t group) {
      if (group == groups.size()) return callback(order);
      auto& values = groups[group];
      do {
        order.insert(order.end(), values.begin(), values.end());
        const bool more = visit(group + 1);
        order.resize(order.size() - values.size());
        if (!more) return false;
      } while (std::next_permutation(values.begin(), values.end()));
      return true;
    };
    return visit(0);
  };
  std::optional<std::pair<Candidate, std::map<std::string, std::string>>> chosen;
  size_t examined = 0;
  complete = permutations(classes(nodeClasses), [&](const auto& nodes) {
    std::vector<size_t> remap(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) remap[nodes[i]] = i;
    std::map<std::string, std::vector<size_t>> edgeClasses;
    for (size_t i = 0; i < source.relationships.size(); ++i) {
      const auto& edge = source.relationships[i];
      auto first = remap[edge.source], second = remap[edge.destination];
      if (!edge.directed && first > second) std::swap(first, second);
      std::string invariant = token(std::to_string(first)) + token(std::to_string(second)) +
          token(std::to_string(edge.directed)) + token(std::to_string(edge.recursive)) +
          token(std::to_string(edge.lowerBound)) + token(std::to_string(edge.upperBound)) +
          token(edge.pathSemantics);
      for (const auto& type : sorted(edge.types)) invariant += token(type);
      edgeClasses[invariant].push_back(i);
    }
    return permutations(classes(edgeClasses), [&](const auto& edges) {
      if (permutationLimit && examined >= permutationLimit) return false;
      ++examined;
      auto option = orientation(nodes, edges);
      if (!chosen || option.first.definitionKey < chosen->first.definitionKey)
        chosen = std::move(option);
      return true;
    });
  });
  if (!complete) {
    // An incomplete minimum would depend on incidental source positions. Keep
    // the previous safe identity and explicitly report incomplete sharing.
    std::vector<size_t> nodes(source.nodes.size()), edges(source.relationships.size());
    std::iota(nodes.begin(), nodes.end(), 0);
    std::iota(edges.begin(), edges.end(), 0);
    auto forward = orientation(nodes, edges);
    std::reverse(nodes.begin(), nodes.end());
    std::reverse(edges.begin(), edges.end());
    auto reverse = orientation(nodes, edges);
    chosen = forward.first.definitionKey < reverse.first.definitionKey
        ? std::move(forward) : std::move(reverse);
  }
  if (mapping)
    *mapping = chosen->second;
  return std::move(chosen->first);
}

struct Path {
  std::vector<size_t> nodes, edges;
  std::optional<size_t> optionalClause;
};
// V_I contains every vertex of the computation, not just its interface keys.
// Preserve every query relationship occurrence internal to V_I, including
// parallel atoms and self loops. OPTIONAL boundaries are checked separately.
Path inducedRegion(const QueryPart& q, std::vector<size_t> nodes) {
  Path region;
  std::sort(nodes.begin(), nodes.end());
  region.nodes = std::move(nodes);
  for (size_t i = 0; i < q.relationships.size(); ++i) {
    const auto& edge = q.relationships[i];
    if (contains(region.nodes, edge.source) && contains(region.nodes, edge.destination))
      region.edges.push_back(i);
  }
  return region;
}
struct Item {
  Candidate definition;
  Path path;
  std::vector<size_t> predicates;
  std::vector<size_t> optionalPredicates;
  std::vector<Recovery> recoveries;
  Names exposed;
  bool existenceOnly = false;
};
struct PhysicalItem {
  Candidate definition;
  IndexRead read;
};
struct Recipe {
  Recovery recovery;
  std::vector<Column> columns;
};
struct Search {
  const Constraints& constraints;
  const std::vector<Carrier>& carriers;
  const Limits& limits;
  bool includeUses;
  EnumerationResult result;
  std::map<std::string, Candidate> output;
  bool stopGeneration = false, stopCombinations = false;
  std::vector<std::vector<bool>> reusableEdges;

  bool generationStep() {
    if (limits.maxGenerated && result.generated >= limits.maxGenerated) {
      result.truncated = stopGeneration = true;
      return false;
    }
    ++result.generated;
    return true;
  }
  bool combinationStep() {
    if (limits.maxCombinations &&
        result.combinations >= limits.maxCombinations) {
      result.truncated = stopCombinations = true;
      return false;
    }
    ++result.combinations;
    return true;
  }
  RelationshipConstraint relationConstraint(const Relationship& rel) const {
    RelationshipConstraint result;
    result.types = rel.types;
    const auto types = sorted(rel.types);
    if (types.empty())
      return result;
    for (const auto& c : constraints.relationships) {
      const auto declared = sorted(c.types);
      // Removing impossible relationship tables preserves an upper bound on
      // matches. A contract on only one member cannot constrain their union.
      if (!std::includes(declared.begin(), declared.end(), types.begin(),
                         types.end()))
        continue;
      result.uniqueEndpoints |= c.uniqueEndpoints;
      result.atMostOneOutgoing |= c.atMostOneOutgoing;
      result.atMostOneIncoming |= c.atMostOneIncoming;
    }
    return result;
  }
  RecursiveConstraint recursiveConstraint(const QueryPart& q,
                                          const Relationship& r) const {
    for (const auto& c : constraints.recursivePaths)
      if (sorted(c.sourceLabels) == sorted(q.nodes[r.source].labels) &&
          sorted(c.destinationLabels) ==
              sorted(q.nodes[r.destination].labels) &&
          sorted(c.relationshipTypes) == sorted(r.types) &&
          c.lowerBound == r.lowerBound && c.upperBound == r.upperBound &&
          c.pathSemantics == r.pathSemantics)
        return c;
    return {};
  }
  bool uniqueMatch(const QueryPart& q, const Path& p, const Names& keys) const {
    Names known = keys;
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto i : p.edges) {
        const auto& r = q.relationships[i];
        if (!r.directed)
          return false;
        const auto& a = q.nodes[r.source].binding;
        const auto& b = q.nodes[r.destination].binding;
        if (r.recursive) {
          const auto c = recursiveConstraint(q, r);
          if (!c.finiteMatches || !c.uniquePath)
            return false;
          if (known.contains(a) && c.uniqueDestination)
            changed |= known.insert(b).second;
          if (known.contains(b) && c.uniqueSource)
            changed |= known.insert(a).second;
        } else {
          auto c = relationConstraint(r);
          if (known.contains(a) && c.atMostOneOutgoing)
            changed |= known.insert(b).second;
          if (known.contains(b) && c.atMostOneIncoming)
            changed |= known.insert(a).second;
        }
      }
    }
    for (auto n : p.nodes)
      if (!known.contains(q.nodes[n].binding))
        return false;
    for (auto i : p.edges) {
      if (q.relationships[i].recursive)
        continue;  // Finite matches and path uniqueness were proved above.
      auto c = relationConstraint(q.relationships[i]);
      if (!c.uniqueEndpoints && !c.atMostOneOutgoing && !c.atMostOneIncoming)
        return false;
    }
    return true;
  }
  std::vector<size_t> storageOptions(const QueryPart& q,
                                     const Item& item) const {
    std::vector<size_t> options;
    const auto& def = item.definition;
    auto findNode = [&](const std::string& name) -> const Node& {
      for (const auto& n : q.nodes)
        if (n.binding == name)
          return n;
      throw std::invalid_argument("Unknown storage key");
    };
    for (size_t carrier = 0; carrier < carriers.size(); ++carrier) {
      const auto& c = carriers[carrier];
      if (!c.uniqueKey || !c.preservesGroupPresence)
        continue;
      if (def.keys.size() == 1) {
        if (c.kind != CarrierKind::VertexProperties || !c.coversAllKeys ||
            !includes(c.sourceLabels, findNode(def.keys[0]).labels))
          continue;
      } else {
        if (c.kind == CarrierKind::VertexProperties ||
            !includes(c.sourceLabels, findNode(def.keys[0]).labels) ||
            !includes(c.destinationLabels, findNode(def.keys[1]).labels))
          continue;
        bool covered = c.coversAllKeys || c.kind == CarrierKind::DerivedEdge;
        // A carrier which is this very selected edge covers its emitted keys.
        if (!covered)
          for (auto edge : item.path.edges) {
            const auto& r = q.relationships[edge];
            covered |= !r.recursive && r.directed && r.types.size() == 1 &&
                       r.types[0] == c.relationshipType &&
                       q.nodes[r.source].binding == def.keys[0] &&
                       q.nodes[r.destination].binding == def.keys[1];
          }
        // Storage need only cover keys observed by this usage. A mandatory
        // outside edge, or a preserved upstream fact, proves those keys exist
        // without claiming that every pair in the materialization has an edge.
        const auto matchesDomain =
            [&](const std::string& source, const std::string& destination,
                const std::vector<std::string>& types, bool directed) {
              return directed && types.size() == 1 &&
                     types[0] == c.relationshipType && source == def.keys[0] &&
                     destination == def.keys[1];
            };
        if (!covered && c.kind == CarrierKind::ExistingEdgeProperties) {
          for (const auto& domain : q.carrierDomains)
            covered |= q.inputBindings.contains(domain.source) &&
                       q.inputBindings.contains(domain.destination) &&
                       matchesDomain(domain.source, domain.destination,
                                     domain.types, domain.directed);
          for (size_t edge = 0; edge < q.relationships.size(); ++edge) {
            if (contains(item.path.edges, edge))
              continue;
            const auto& r = q.relationships[edge];
            bool mandatory = true;
            for (const auto& clause : q.clauses)
              if (clause.optional && contains(clause.relationships, edge))
                mandatory = false;
            if (mandatory && !r.recursive)
              covered |= matchesDomain(q.nodes[r.source].binding,
                                       q.nodes[r.destination].binding, r.types,
                                       r.directed);
          }
        }
        if (!covered)
          continue;
      }
      if (def.kind == CandidateKind::EndpointMapping && def.keys.size() == 1 &&
          !c.resolvesDestinationId)
        continue;
      bool ok = true;
      for (const auto& col : def.columns) {
        if (!scalar(col.type) ||
            (!c.scalarTypes.empty() && !c.scalarTypes.contains(col.type)))
          ok = false;
        if (col.kind != ColumnKind::CountRows &&
            col.kind != ColumnKind::CountNonNull &&
            col.kind != ColumnKind::CountDistinctNonNull && !c.preservesNull)
          ok = false;
      }
      if (ok)
        options.push_back(carrier);
    }
    return options;
  }
  bool available(const Expression& e, const Names& vars, const Names& stored,
                 const Names& aggregates = {}) const {
    if (stored.contains(e.structuralKey()) ||
        aggregates.contains(e.structuralKey()))
      return true;
    if (e.kind == ExpressionKind::Aggregate)
      return false;
    if ((e.kind == ExpressionKind::Variable ||
         e.kind == ExpressionKind::Property) &&
        !vars.contains(e.binding))
      return false;
    return std::all_of(
        e.children.begin(), e.children.end(),
        [&](const auto& c) { return available(c, vars, stored, aggregates); });
  }
  bool couldReuse(const QueryPart& q, const Relationship& a,
                  const Relationship& b) const {
    if (a.matchGroup != b.matchGroup || !intersects(a.types, b.types))
      return false;
    if (a.recursive || b.recursive)
      return true;
    const auto& as = q.nodes[a.source].labels;
    const auto& ad = q.nodes[a.destination].labels;
    // An OPTIONAL ON predicate is a fact only for its own positive matches.
    // Do not borrow predicates from later or unrelated clauses: an illegal
    // optional match can suppress a NULL-extended row before those predicates
    // run. The formal clause-free API treats q.predicates as this MATCH's
    // WHERE.
    const std::vector<Expression>* predicates = &q.predicates;
    if (!q.clauses.empty()) {
      predicates = nullptr;
      for (const auto& clause : q.clauses)
        if (clause.position == a.matchGroup) {
          predicates = &clause.predicates;
          break;
        }
    }
    const auto orientationPossible = [&](size_t source, size_t destination) {
      if (!intersects(as, q.nodes[source].labels) ||
          !intersects(ad, q.nodes[destination].labels))
        return false;
      if (!predicates)
        return true;
      EdgeIdentityProof proof(q);
      proof.identify(a.source, source);
      proof.identify(a.destination, destination);
      return !proof.impossible(*predicates);
    };
    return orientationPossible(b.source, b.destination) ||
           ((!a.directed || !b.directed) &&
            orientationPossible(b.destination, b.source));
  }
  std::optional<Usage> validate(const QueryPart& q,
                                const std::vector<Item>& pool,
                                const std::vector<size_t>& selected) {
    auto fail = [&](const std::string& reason) -> std::optional<Usage> {
      ++result.rejections[reason];
      return std::nullopt;
    };
    std::vector<int> owner(q.relationships.size(), -1);
    Names coveredVars, vars = q.inputBindings, stored;
    std::set<size_t> coveredPredicates;
    std::vector<const Item*> aggregateItems;
    std::vector<std::pair<size_t, Expression>> edgeIdentityPredicates;
    bool exists = false, complete = false;
    const bool partialFilter = std::any_of(
        q.predicates.begin(), q.predicates.end(),
        [](const auto& e) { return !safeInput(e); });
    for (size_t pos = 0; pos < selected.size(); ++pos) {
      const auto& item = pool[selected[pos]];
      // A partial filter may remain at its original online MATCH boundary
      // only for an exact bag-preserving lookup. Do not regroup/deduplicate
      // its input, or push another filter into the materialization: either
      // could hide an error on a row on which the original filter runs.
      // available() below separately checks that its bindings survive.
      if (partialFilter &&
          ((item.definition.kind != CandidateKind::Projection &&
            item.definition.kind != CandidateKind::EndpointMapping) ||
           item.existenceOnly || !item.predicates.empty() ||
           !item.optionalPredicates.empty()))
        return fail("partial_filter_requires_unfiltered_bag_preserving_lookup");
      for (auto edge : item.path.edges) {
        if (owner[edge] >= 0)
          return fail("overlapping_edges");
        owner[edge] = static_cast<int>(pos);
        coveredVars.insert(q.relationships[edge].binding);
      }
      for (auto n : item.path.nodes)
        coveredVars.insert(q.nodes[n].binding);
      vars.insert(item.exposed.begin(), item.exposed.end());
      coveredPredicates.insert(item.predicates.begin(), item.predicates.end());
      if (item.definition.kind == CandidateKind::Aggregate)
        aggregateItems.push_back(&item);
      complete |= item.definition.kind == CandidateKind::CompleteAggregate;
      exists |= item.existenceOnly;
      for (const auto& col : item.definition.columns)
        if (col.kind == ColumnKind::Projection)
          stored.insert(col.expression->structuralKey());
    }
    Names remaining;
    for (size_t i = 0; i < owner.size(); ++i)
      if (owner[i] < 0) {
        const auto& rel = q.relationships[i];
        remaining.insert(rel.binding);
        remaining.insert(q.nodes[rel.source].binding);
        remaining.insert(q.nodes[rel.destination].binding);
      }
    for (const auto& n : q.nodes)
      if (!coveredVars.contains(n.binding))
        remaining.insert(n.binding);
    vars.insert(remaining.begin(), remaining.end());
    // Every region must expose each binding shared with any other input.
    for (size_t pos = 0; pos < selected.size(); ++pos) {
      const auto& item = pool[selected[pos]];
      Names outside = q.inputBindings;
      outside.insert(remaining.begin(), remaining.end());
      for (size_t other = 0; other < selected.size(); ++other)
        if (other != pos) {
          for (auto n : pool[selected[other]].path.nodes)
            outside.insert(q.nodes[n].binding);
        }
      for (auto n : item.path.nodes) {
        const auto& name = q.nodes[n].binding;
        if (outside.contains(name) && !item.exposed.contains(name))
          return fail("shared_binding_hidden");
      }
      for (auto edge : item.path.edges)
        if (q.inputBindings.contains(q.relationships[edge].binding))
          return fail("bound_edge_hidden");
    }
    if (q.forbidEdgeReuse)
      for (size_t a = 0; a < owner.size(); ++a)
        for (size_t b = a + 1; b < owner.size(); ++b)
          if (owner[a] != owner[b] && (owner[a] >= 0 || owner[b] >= 0) &&
              reusableEdges[a][b]) {
            const auto& first = q.relationships[a];
            const auto& second = q.relationships[b];
            const auto proof = relationConstraint(first);
            if (first.recursive || second.recursive || first.types.size() != 1 ||
                first.types != second.types ||
                !(proof.uniqueEndpoints || proof.atMostOneOutgoing || proof.atMostOneIncoming))
              return fail("edge_reuse_not_excluded");
            for (auto node : {first.source, first.destination, second.source, second.destination})
              if (!vars.contains(q.nodes[node].binding))
                return fail("edge_reuse_not_excluded");
            const auto equal = [&](size_t x, size_t y) {
              return Expression::call("=", {Expression::property(q.nodes[x].binding, "_ID", "INTERNAL_ID", false),
                  Expression::property(q.nodes[y].binding, "_ID", "INTERNAL_ID", false)}, "BOOL");
            };
            auto same = Expression::call("AND", {equal(first.source, second.source),
                equal(first.destination, second.destination)}, "BOOL");
            if (!first.directed || !second.directed)
              same = Expression::call("OR", {same, Expression::call("AND", {
                  equal(first.source, second.destination), equal(first.destination, second.source)}, "BOOL")}, "BOOL");
            edgeIdentityPredicates.emplace_back(first.matchGroup,
                Expression::call("NOT", {same}, "BOOL"));
          }
    for (size_t i = 0; i < q.predicates.size(); ++i)
      if (!coveredPredicates.contains(i) &&
          !available(q.predicates[i], vars, stored))
        return fail("unreadable_filter");
    for (size_t block = 0; block < q.clauses.size(); ++block) {
      const auto& clause = q.clauses[block];
      if (!clause.optional)
        continue;
      const Item* replacement = nullptr;
      for (auto pos : selected)
        if (pool[pos].path.optionalClause == block)
          replacement = &pool[pos];
      for (size_t pred = 0; pred < clause.predicates.size(); ++pred) {
        if (replacement && contains(replacement->optionalPredicates, pred))
          continue;
        if (!available(clause.predicates[pred], vars, stored))
          return fail("unreadable_optional_filter");
        // A remaining ON predicate must be evaluable before this optional read.
        if (replacement &&
            !subset(dependencies(clause.predicates[pred]), clause.boundBefore))
          return fail("optional_filter_not_uniform");
      }
    }
    for (const auto& e : q.groupBy)
      if (!available(e, vars, stored))
        return fail("unreadable_grouping");
    if (exists && (!duplicateInsensitive(q, constraints) || !aggregateItems.empty() || complete))
      return std::nullopt;
    Usage use;
    use.source = q.source;
    use.edgeIdentityPredicates = std::move(edgeIdentityPredicates);
    for (size_t i = 0; i < q.predicates.size(); ++i)
      if (!coveredPredicates.contains(i))
        use.remainingFilters.push_back(q.predicates[i]);
    for (size_t i = 0; i < owner.size(); ++i)
      if (owner[i] < 0)
        use.remainingRelationships.push_back(i);
    Names restored;
    if (complete) {
      if (selected.size() != 1 || !use.remainingRelationships.empty())
        return std::nullopt;
      for (const auto& g : q.aggregates)
        restored.insert(g.structuralKey());
    } else if (aggregateItems.size() > 1) {
      if (q.aggregates.empty())
        return std::nullopt;
      for (size_t i = 0; i < q.aggregates.size(); ++i) {
        const auto& g = q.aggregates[i];
        if (upper(g.name) != "COUNT" || !g.children.empty() || g.distinct)
          return std::nullopt;
        Recovery rec;
        rec.aggregate = i;
        rec.rule = "R7";
        for (const auto* item : aggregateItems) {
          for (size_t pos = 0; pos < selected.size(); ++pos)
            if (&pool[selected[pos]] == item)
              rec.reads.push_back(pos);
          auto col = std::find_if(
              item->definition.columns.begin(), item->definition.columns.end(),
              [](const auto& c) { return c.kind == ColumnKind::CountRows; });
          if (col == item->definition.columns.end())
            return std::nullopt;
          rec.requiredColumns.push_back(col->structuralKey());
        }
        use.recoveries.push_back(std::move(rec));
        restored.insert(g.structuralKey());
      }
    } else if (aggregateItems.size() == 1) {
      for (size_t i = 0; i < q.aggregates.size(); ++i) {
        bool found = false;
        for (const auto& rec : aggregateItems[0]->recoveries) {
          if (rec.aggregate != i ||
              (rec.outsideExpression &&
               !available(*rec.outsideExpression, vars, stored)))
            continue;
          auto recovery = rec;
          // The DISTINCT normalization changes upstream multiplicities. Its
          // first implementation composes only one local read in this stage;
          // other stages may still use independently proved views.
          if (rec.rule == "R2partition" && selected.size() != 1)
            return fail("partition_distinct_requires_single_region");
          for (size_t pos = 0; pos < selected.size(); ++pos)
            if (&pool[selected[pos]] == aggregateItems[0])
              recovery.reads = {pos};
          use.recoveries.push_back(std::move(recovery));
          restored.insert(q.aggregates[i].structuralKey());
          found = true;
          break;
        }
        if (!found)
          return fail("missing_aggregate_recovery");
      }
      if (q.aggregates.empty())
        return std::nullopt;
    } else {
      for (const auto& g : q.aggregates) {
        for (const auto& child : g.children)
          if (!available(child, vars, stored))
            return std::nullopt;
        restored.insert(g.structuralKey());
      }
    }
    for (const auto& e : q.outputs)
      if (!available(e, vars, stored, restored))
        return fail("unreadable_output");
    for (const auto& e : q.postExpressions)
      if (!available(e, vars, stored, restored))
        return fail("unreadable_post_expression");
    return use;
  }
};

Names pathVariables(const QueryPart& q, const Path& p) {
  Names out;
  for (auto n : p.nodes)
    out.insert(q.nodes[n].binding);
  for (auto r : p.edges)
    out.insert(q.relationships[r].binding);
  return out;
}
Expression nullExtend(Expression e, const Names& introduced) {
  if ((e.kind == ExpressionKind::Variable ||
       e.kind == ExpressionKind::Property) &&
      introduced.contains(e.binding)) {
    auto result = Expression::literal("NULL", e.type);
    result.nullable = true;
    return result;
  }
  for (auto& child : e.children)
    child = nullExtend(std::move(child), introduced);
  return e;
}
bool notTrueAfterNullExtension(const Expression& e, const Names& introduced) {
  if (e.kind != ExpressionKind::Call)
    return false;
  if (e.name == "AND")
    return std::any_of(e.children.begin(), e.children.end(), [&](const auto& c) {
      return notTrueAfterNullExtension(c, introduced);
    });
  auto padded = [&](const Expression& child) {
    return (child.kind == ExpressionKind::Variable ||
            child.kind == ExpressionKind::Property) &&
           introduced.contains(child.binding);
  };
  if (e.name == "IS NOT NULL" && e.children.size() == 1)
    return padded(e.children[0]);
  static const Names comparisons = {"=", "<>", "!=", "<", ">", "<=", ">="};
  return comparisons.contains(e.name) && e.children.size() == 2 &&
         (padded(e.children[0]) || padded(e.children[1]));
}
Expression simplifyMatchedNonNull(Expression e, const Names& matched) {
  if (e.kind != ExpressionKind::Call)
    return e;
  if (e.name == "IS NOT NULL" && e.children.size() == 1 &&
      e.children[0].kind == ExpressionKind::Variable &&
      matched.contains(e.children[0].binding))
    return Expression::literal("true", "BOOL");
  for (auto& child : e.children)
    child = simplifyMatchedNonNull(std::move(child), matched);
  if (e.name == "AND" && e.children.size() == 2) {
    for (size_t side = 0; side < 2; ++side)
      if (e.children[side].kind == ExpressionKind::Literal &&
          !e.children[side].nullable && e.children[side].type == "BOOL" &&
          upper(e.children[side].name) == "TRUE")
        return e.children[1 - side];
  }
  return e;
}
const std::vector<Expression>& sourcePredicates(const QueryPart& q,
                                                const Path& p) {
  return p.optionalClause ? q.clauses[*p.optionalClause].predicates
                          : q.predicates;
}
Candidate baseDefinition(const QueryPart& q, const Path& p,
                         const std::vector<std::string>& keys,
                         const std::vector<size_t>& predicates) {
  Candidate out;
  out.keys = keys;
  std::map<size_t, size_t> remap;
  for (auto n : p.nodes) {
    remap[n] = out.nodes.size();
    out.nodes.push_back(q.nodes[n]);
  }
  for (auto r : p.edges) {
    auto rel = q.relationships[r];
    rel.source = remap.at(rel.source);
    rel.destination = remap.at(rel.destination);
    out.relationships.push_back(std::move(rel));
  }
  for (auto pred : predicates)
    out.predicates.push_back(sourcePredicates(q, p)[pred]);
  return out;
}
Column countRows() { return {ColumnKind::CountRows, std::nullopt, "INT64"}; }

std::optional<Names> distinctPartitionBindings(
    const QueryPart& q, const Path& p, const Names& keys,
    const Expression& operand, const Search& search) {
  // A single DISTINCT count tolerates deduplicating upstream witnesses. Other
  // aggregates generally do not. Final groups must identify the graph nodes
  // retained through this normalization, not merely a nonunique scalar value.
  const auto groups = groupingNodes(q);
  if (q.aggregates.size() != 1 || groups.empty() ||
      std::any_of(q.groupBy.begin(), q.groupBy.end(), [&](const auto& e) {
        return !subset(dependencies(e), groups) || !stableDistinctOperand(e);
      }))
    return std::nullopt;
  Names known = groups;
  const auto fixed = queryFixedNodes(q, search.constraints);
  known.insert(fixed.begin(), fixed.end());
  bool entity = false;
  if (auto node = groupingNode(q, operand)) {
    known.insert(*node);
    entity = true;
  } else if (operand.kind == ExpressionKind::Variable) {
    for (auto edge : p.edges) {
      const auto& r = q.relationships[edge];
      if (r.binding != operand.binding || r.recursive || !r.directed)
        continue;
      // A directed relationship identity fixes both endpoint identities even
      // when parallel edges exist. An undirected occurrence may reverse them.
      known.insert(q.nodes[r.source].binding);
      known.insert(q.nodes[r.destination].binding);
      entity = true;
    }
  }
  if (!entity)
    return std::nullopt;  // Arbitrary equal scalar values can cross partitions.
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto edge : p.edges) {
      const auto& r = q.relationships[edge];
      if (!r.directed)
        continue;
      const auto& source = q.nodes[r.source].binding;
      const auto& destination = q.nodes[r.destination].binding;
      bool forward = false, backward = false;
      if (r.recursive) {
        const auto proof = search.recursiveConstraint(q, r);
        forward = proof.finiteMatches && proof.uniqueDestination;
        backward = proof.finiteMatches && proof.uniqueSource;
      } else {
        const auto proof = search.relationConstraint(r);
        forward = proof.atMostOneOutgoing;
        backward = proof.atMostOneIncoming;
      }
      if (forward && known.contains(source))
        changed |= known.insert(destination).second;
      if (backward && known.contains(destination))
        changed |= known.insert(source).second;
    }
  }
  if (!subset(keys, known))
    return std::nullopt;
  // At a fixed final group, the same non-NULL counted entity cannot occur in
  // two different key tuples. Their sets are disjoint. DISTINCT on these live
  // graph identities also removes repeated copies of the same local set.
  Names retained = groups;
  retained.insert(keys.begin(), keys.end());
  return retained;
}

std::vector<Recipe> aggregateRecipes(const QueryPart& q, const Path& p,
                                     const Names& keys,
                                     const Search& search) {
  const auto& constraints = search.constraints;
  auto fixedPerGroup = groupingNodes(q);
  const auto fixedPerQuery = queryFixedNodes(q, constraints);
  fixedPerGroup.insert(fixedPerQuery.begin(), fixedPerQuery.end());
  auto localVars = pathVariables(q, p);
  Names outside = q.inputBindings;
  for (const auto& n : q.nodes)
    if (!localVars.contains(n.binding))
      outside.insert(n.binding);
  for (const auto& r : q.relationships)
    if (!localVars.contains(r.binding)) {
      outside.insert(r.binding);
      outside.insert(q.nodes[r.source].binding);
      outside.insert(q.nodes[r.destination].binding);
    }
  outside.insert(keys.begin(), keys.end());
  auto isOuter = [&](const Expression& e) {
    return rowExpression(e) && subset(dependencies(e), outside);
  };
  std::vector<Recipe> recipes;
  auto add = [&](size_t pos, std::string rule, std::vector<Column> cols,
                 std::optional<Expression> external = std::nullopt) {
    Recovery r;
    r.aggregate = pos;
    r.rule = std::move(rule);
    r.outsideExpression = std::move(external);
    if (p.optionalClause) {
      const auto& g = q.aggregates[pos];
      r.noMatchOperand =
          g.children.empty()
              ? Expression::literal("1")
              : nullExtend(g.children[0],
                           q.clauses[*p.optionalClause].introduced);
    }
    for (const auto& c : cols)
      r.requiredColumns.push_back(c.structuralKey());
    recipes.push_back({std::move(r), std::move(cols)});
  };
  for (size_t i = 0; i < q.aggregates.size(); ++i) {
    const auto& g = q.aggregates[i];
    const auto name = upper(g.name);
    if (g.distinct && name != "MIN" && name != "MAX") {
      // A final group must identify one complete local set. Repeated upstream
      // rows may repeat that set. Different sets may be added only with the
      // separate disjoint-partition proof below.
      if (name == "COUNT" && g.children.size() == 1 &&
          stableDistinctOperand(g.children[0]) &&
          local(g.children[0], localVars, false)) {
        const auto& operand = g.children[0];
        bool emptyNullExtension = true;
        if (p.optionalClause) {
          const auto padded =
              nullExtend(operand, q.clauses[*p.optionalClause].introduced);
          emptyNullExtension =
              padded.kind == ExpressionKind::Literal && padded.nullable;
        }
        if (emptyNullExtension) {
          if (subset(keys, fixedPerGroup)) {
            add(i, "R2distinct",
                {{ColumnKind::CountDistinctNonNull, operand, "INT64"}});
          } else if (auto retained =
                         distinctPartitionBindings(q, p, keys, operand, search)) {
            add(i, "R2partition",
                {{ColumnKind::CountDistinctNonNull, operand, "INT64"}});
            recipes.back().recovery.deduplicateBindings.assign(
                retained->begin(), retained->end());
          }
        }
      }
      if ((name == "SUM" || name == "AVG") && !p.optionalClause &&
          g.children.size() == 1 && subset(keys, fixedPerGroup) &&
          stableDistinctOperand(g.children[0]) &&
          local(g.children[0], localVars)) {
        add(i, "DISTINCT_SAME_KEY",
            {{name == "SUM" ? ColumnKind::Sum : ColumnKind::Average,
              g.children[0], g.type, true}});
      }
      continue;
    }
    if (name == "COUNT" && g.children.empty()) {
      add(i, "R2a", {countRows()});
      continue;
    }
    if (g.children.size() != 1)
      continue;
    const auto& e = g.children[0];
    if (name == "COUNT") {
      if (local(e, localVars, false)) {
        // A newly matched node/edge cannot be NULL on the actual-match branch.
        const bool nonNullBinding =
            p.optionalClause &&
            (e.kind == ExpressionKind::Variable ||
             (e.kind == ExpressionKind::Property && !e.nullable)) &&
            q.clauses[*p.optionalClause].introduced.contains(e.binding);
        add(i, "R2b",
            nonNullBinding
                ? std::vector<Column>{countRows()}
                : std::vector<Column>{{ColumnKind::CountNonNull, e, "INT64"}});
      }
      if (isOuter(e))
        add(i, "R2c", {countRows()}, e);
    } else if ((name == "MIN" || name == "MAX") &&
               orderedExtremum(e, constraints) && local(e, localVars)) {
      add(i, name == "MIN" ? "R9min" : "R9max",
          {{name == "MIN" ? ColumnKind::Minimum : ColumnKind::Maximum, e,
            g.type}});
    } else if (name == "SUM" && constraints.exactArithmetic &&
               exactNumeric(e.type)) {
      if (local(e, localVars))
        add(i, "R3a", {{ColumnKind::Sum, e, g.type}});
      if (isOuter(e))
        add(i, "R3b", {countRows()}, e);
      if (e.kind == ExpressionKind::Call && e.name == "*" &&
          e.children.size() == 2) {
        for (size_t side = 0; side < 2; ++side) {
          const auto& a = e.children[side];
          const auto& b = e.children[1 - side];
          if (local(a, localVars) && exactNumeric(a.type) && isOuter(b))
            add(i, "R3c", {{ColumnKind::Sum, a, g.type}}, b);
        }
      }
    } else if (name == "AVG" && constraints.exactArithmetic &&
               exactNumeric(e.type) && local(e, localVars)) {
      add(i, "R4a",
          {{ColumnKind::Sum, e, sumResultType(e)},
           {ColumnKind::CountNonNull, e, "INT64"}});
    }
  }
  return recipes;
}
void collectLocal(const Expression& e, const Names& vars,
                  std::map<std::string, Expression>& out) {
  // A constant has no hidden binding to recover. Keeping it online is exact
  // and avoids enumerating every stored subset of literal expression leaves.
  if (local(e, vars) && !dependencies(e).empty())
    out.emplace(e.structuralKey(), e);
  for (const auto& child : e.children)
    collectLocal(child, vars, out);
}
void generateContext(Search& search, const QueryPart& q, const Path& path,
                     const std::vector<std::string>& keys,
                     const std::vector<size_t>& predicates,
                     std::vector<Item>& pool, bool fullRegion = false) {
  if (!search.generationStep())
    return;
  Names keySet(keys.begin(), keys.end());
  auto vars = pathVariables(q, path);
  auto base = baseDefinition(q, path, keys, predicates);
  std::map<std::string, Item> items;
  bool capped = false;
  auto emit = [&](CandidateKind kind, std::vector<Column> columns,
                  std::vector<Recovery> recoveries, bool existence = false,
                  Names exposed = Names{},
                  std::vector<Expression> additionalPredicates = {}) {
    if (!search.generationStep())
      return false;
    Item item;
    item.definition = base;
    item.definition.predicates.insert(item.definition.predicates.end(),
                                     additionalPredicates.begin(),
                                     additionalPredicates.end());
    item.definition.kind = kind;
    item.definition.columns = std::move(columns);
    item.path = path;
    if (path.optionalClause)
      item.optionalPredicates = predicates;
    else
      item.predicates = predicates;
    item.existenceOnly = existence;
    item.exposed = exposed.empty() ? keySet : std::move(exposed);
    item.recoveries = std::move(recoveries);
    if (search.storageOptions(q, item).empty()) {
      ++search.result.rejections["storage_unavailable"];
      return true;
    }
    std::string localKey = std::to_string(static_cast<int>(kind));
    for (const auto& c : item.definition.columns)
      localKey += token(c.structuralKey());
    for (const auto& predicate : additionalPredicates)
      localKey += token(predicate.structuralKey());
    if (items.contains(localKey))
      return true;
    if (search.limits.local && items.size() >= search.limits.local) {
      search.result.truncated = true;
      capped = true;
      return false;
    }
    items.try_emplace(std::move(localKey), std::move(item));
    return true;
  };
  std::map<std::string, Expression> expressions;
  for (const auto* list : {&q.predicates, &q.groupBy, &q.aggregates, &q.outputs,
                           &q.postExpressions})
    for (const auto& e : *list)
      collectLocal(e, vars, expressions);
  if (path.optionalClause)
    for (const auto& e : q.clauses[*path.optionalClause].predicates)
      collectLocal(e, vars, expressions);
  const bool recursive =
      std::any_of(path.edges.begin(), path.edges.end(),
                  [&](auto e) { return q.relationships[e].recursive; });
  // A source aggregate must itself terminate on every admissible input. Finite
  // recursive matches suffice for exact source statistics; endpoint projection
  // additionally needs the functional/path-uniqueness proofs below.
  if (recursive)
    for (auto edge : path.edges) {
      const auto& r = q.relationships[edge];
      if (r.recursive && (!r.directed || r.pathSemantics != "WALK" ||
                          !search.recursiveConstraint(q, r).finiteMatches))
        return;
    }
  // A nonnegative integer indicator sum over an OPTIONAL source can be stored
  // as a filtered COUNT. The false/NULL branch contributes zero, including the
  // padded row when no witness matches. No floating-point reassociation or
  // unchecked signed-sum assumption is needed: every partial total is bounded
  // by the original nonnegative sum. Keep other aggregates out of this rule,
  // since removing their false rows could change their multiplicity.
  if (!fullRegion && path.optionalClause && q.aggregates.size() == 1) {
    const auto& aggregate = q.aggregates[0];
    if (upper(aggregate.name) == "SUM" && !aggregate.distinct &&
        aggregate.type == "INT64" && aggregate.children.size() == 1) {
      const auto& value = aggregate.children[0];
      const auto literal = [&](const Expression& e, const std::string& n) {
        return e.kind == ExpressionKind::Literal && !e.nullable &&
               integerType(e.type) && e.name == n;
      };
      if (value.kind == ExpressionKind::Call && value.name == "CASE" &&
          value.children.size() == 3 && literal(value.children[1], "1") &&
          literal(value.children[2], "0") && local(value.children[0], vars) &&
          notTrueAfterNullExtension(value.children[0],
                                   q.clauses[*path.optionalClause].introduced)) {
        const auto column = countRows();
        Recovery recovery;
        recovery.aggregate = 0;
        recovery.rule = "R3a";
        recovery.requiredColumns = {column.structuralKey()};
        recovery.noMatchOperand = Expression::literal("0", "INT64");
        emit(CandidateKind::Aggregate, {column}, {std::move(recovery)}, false,
             {}, {simplifyMatchedNonNull(value.children[0], vars)});
      }
    }
  }
  if (!fullRegion && recursive && path.edges.size() == 1) {
    const auto& r = q.relationships[path.edges[0]];
    const auto& src = q.nodes[r.source];
    const auto& dst = q.nodes[r.destination];
    const auto proof = search.recursiveConstraint(q, r);
    const bool sourceKey = keys == std::vector<std::string>{src.binding};
    const bool pairKey =
        keys == std::vector<std::string>{src.binding, dst.binding};
    // A many-to-one mapping still preserves every source row. Inverse
    // uniqueness is irrelevant when both endpoint identities survive.
    if ((sourceKey || pairKey) && proof.uniqueDestination &&
        (proof.uniquePath || (q.distinct && q.aggregates.empty())) &&
        (!sourceKey ||
         (!dst.primaryKey.empty() && scalar(dst.primaryKeyType)))) {
      std::vector<Column> required;
      if (sourceKey)
        required.push_back({ColumnKind::DestinationId,
                            Expression::property(dst.binding, dst.primaryKey,
                                                 dst.primaryKeyType, false),
                            dst.primaryKeyType});
      if (required.size() <= search.limits.columns) {
        Names endpoints{src.binding, dst.binding};
        // Both graph identities survive the read. Every admissible R8 extra
        // depended only on these endpoints and can be evaluated online. The
        // destination-ID column above is different: it resolves that identity
        // when the source alone is the physical key and must be retained.
        emit(CandidateKind::EndpointMapping, std::move(required), {},
             !proof.uniquePath, endpoints);
      }
    }
  }
  if (!fullRegion && (!recursive || path.edges.size() > 1) &&
      search.uniqueMatch(q, path, keySet)) {
    // A functional nonrecursive region can return graph identities as several
    // typed scalar values. This keeps join boundaries and edge-identity tests
    // readable while using one physical vertex key.
    if (!recursive && !path.optionalClause && keys.size() == 1) {
      std::vector<size_t> targets;
      for (auto node : path.nodes)
        if (!keySet.contains(q.nodes[node].binding) && q.nodes[node].labels.size() == 1 &&
            !q.nodes[node].primaryKey.empty() && scalar(q.nodes[node].primaryKeyType))
          targets.push_back(node);
      subsets(targets.size(), search.limits.columns, 1, [&](const auto& indices) {
        std::vector<Column> ids;
        Names exposed = keySet;
        for (auto i : indices) {
          const auto& node = q.nodes[targets[i]];
          ids.push_back({ColumnKind::DestinationId,
              Expression::property(node.binding, node.primaryKey, node.primaryKeyType, false), node.primaryKeyType});
          exposed.insert(node.binding);
        }
        return emit(CandidateKind::EndpointMapping, std::move(ids), {}, false, std::move(exposed));
      });
    }
    std::vector<Column> cols;
    for (const auto& [key, e] : expressions)
      // R1 retains the key graph objects; their properties and pure expressions
      // remain available under exactly the same row and NULL-extension context.
      if (!subset(dependencies(e), keySet))
        cols.push_back({ColumnKind::Projection, e, e.type});
    subsets(cols.size(), search.limits.columns, 0, [&](const auto& indices) {
      std::vector<Column> chosen;
      for (auto i : indices)
        chosen.push_back(cols[i]);
      return emit(CandidateKind::Projection, std::move(chosen), {});
    });
  }
  if (!fullRegion && !capped && !search.stopGeneration) {
    const auto recipes = aggregateRecipes(q, path, keySet, search);
    std::map<std::string, Column> uniqueCols;
    for (const auto& recipe : recipes)
      for (const auto& c : recipe.columns)
        uniqueCols.emplace(c.structuralKey(), c);
    std::vector<Column> cols;
    for (const auto& [key, c] : uniqueCols)
      cols.push_back(c);
    subsets(cols.size(), search.limits.columns, 1, [&](const auto& indices) {
      if (!search.generationStep())
        return false;
      std::vector<Column> chosen;
      Names have;
      for (auto i : indices) {
        chosen.push_back(cols[i]);
        have.insert(cols[i].structuralKey());
      }
      std::vector<Recovery> uses;
      for (const auto& recipe : recipes)
        if (std::all_of(recipe.recovery.requiredColumns.begin(),
                        recipe.recovery.requiredColumns.end(),
                        [&](const auto& c) { return have.contains(c); }))
          uses.push_back(recipe.recovery);
      if (uses.empty())
        return true;
      return emit(CandidateKind::Aggregate, std::move(chosen), std::move(uses));
    });
  }
  if (search.limits.complete && !path.optionalClause && !capped &&
      !search.stopGeneration && q.singletonEmptyInput &&
      q.inputBindings.empty() && path.edges.size() == q.relationships.size() &&
      path.nodes.size() == q.nodes.size() &&
      predicates.size() == q.predicates.size() && !q.aggregates.empty()) {
    Names grouping;
    bool exact = true, needsComplete = fullRegion;
    for (const auto& e : q.groupBy) {
      if (auto node = groupingNode(q, e))
        grouping.insert(*node);
      else
        exact = false;
    }
    std::map<std::string, Column> columns;
    for (const auto& g : q.aggregates) {
      const auto name = upper(g.name);
      if (g.distinct && name != "COUNT" && name != "SUM" && name != "AVG" &&
          name != "MIN" && name != "MAX") {
        exact = false;
        break;
      }
      Column col;
      if (name == "COUNT" && g.children.empty() && !g.distinct)
        col = countRows();
      else if (g.children.size() == 1 &&
               local(g.children[0], vars, name != "COUNT")) {
        ColumnKind kind;
        if (name == "COUNT")
          kind = g.distinct ? ColumnKind::CountDistinctNonNull
                            : ColumnKind::CountNonNull;
        else if (name == "SUM")
          kind = ColumnKind::Sum;
        else if (name == "AVG")
          kind = ColumnKind::Average;
        else if (name == "MIN")
          kind = ColumnKind::Minimum;
        else if (name == "MAX")
          kind = ColumnKind::Maximum;
        else {
          exact = false;
          break;
        }
        col = {kind, g.children[0], g.type,
               g.distinct && (name == "SUM" || name == "AVG")};
        needsComplete |=
            name == "AVG" ||
            (name == "COUNT" && g.distinct &&
             !stableDistinctOperand(g.children[0])) ||
            (name == "SUM" && (!search.constraints.exactArithmetic ||
                               !exactNumeric(g.children[0].type))) ||
            ((name == "MIN" || name == "MAX") &&
             !orderedExtremum(g.children[0], search.constraints));
      } else {
        exact = false;
        break;
      }
      columns.emplace(col.structuralKey(), col);
    }
    if (exact && needsComplete && grouping == keySet &&
        columns.size() <= search.limits.columns) {
      std::vector<Column> out;
      for (const auto& [key, c] : columns)
        out.push_back(c);
      emit(CandidateKind::CompleteAggregate, std::move(out), {});
    }
  }
  if (!fullRegion && !path.optionalClause && !capped &&
      !search.stopGeneration && duplicateInsensitive(q, search.constraints))
    emit(CandidateKind::Existence, {}, {}, true);
  for (auto& [key, item] : items)
    pool.push_back(std::move(item));
}

void generatePath(Search& search, const QueryPart& q, const Path& inputPath,
                  std::vector<Item>& pool) {
  for (auto edge : inputPath.edges) {
    if (search.limits.runtimeBoundRelationships.contains(q.relationships[edge].binding)) {
      ++search.result.pruned["runtime_parameter_path_bound"];
      return;
    }
  }
  auto path = inputPath;
  const bool hasOptional =
      std::any_of(q.clauses.begin(), q.clauses.end(),
                  [](const auto& c) { return c.optional; });
  if (hasOptional) {
    for (size_t i = 0; i < q.clauses.size(); ++i) {
      const auto& clause = q.clauses[i];
      if (!clause.optional)
        continue;
      auto actual = path.edges, required = clause.relationships;
      std::sort(actual.begin(), actual.end());
      std::sort(required.begin(), required.end());
      auto actualNodes = path.nodes, requiredNodes = clause.nodes;
      std::sort(actualNodes.begin(), actualNodes.end());
      std::sort(requiredNodes.begin(), requiredNodes.end());
      if (actual == required && actualNodes == requiredNodes)
        path.optionalClause = i;
    }
    // Ordinary prefixes remain in place in stages containing OPTIONAL MATCH.
    if (!path.optionalClause)
      return;
  }
  if (!path.optionalClause && inducedRegion(q, path.nodes).edges != path.edges)
    throw std::logic_error("Candidate region is not induced by all its vertices");
  const auto vars = pathVariables(q, path);
  // Every node shared with an upstream record or an outside edge must remain
  // exposed in every eventual combination. An outside edge may belong to the
  // residual pattern or another index; either case needs the same binding.
  Names boundary;
  for (auto n : path.nodes)
    if (q.inputBindings.contains(q.nodes[n].binding))
      boundary.insert(q.nodes[n].binding);
  for (size_t edge = 0; edge < q.relationships.size(); ++edge) {
    const auto& rel = q.relationships[edge];
    if (contains(path.edges, edge)) {
      if (q.inputBindings.contains(rel.binding)) {
        ++search.result.pruned["bound_edge_hidden"];
        return;
      }
      continue;
    }
    for (auto endpoint : {rel.source, rel.destination})
      if (contains(path.nodes, endpoint))
        boundary.insert(q.nodes[endpoint].binding);
  }
  const bool endpointMapping =
      path.edges.size() == 1 && q.relationships[path.edges[0]].recursive;
  std::vector<std::vector<std::string>> keyOptions;
  subsets(path.nodes.size(), 2, 1, [&](const auto& indices) {
    std::vector<std::string> keys;
    for (auto n : indices)
      keys.push_back(q.nodes[path.nodes[n]].binding);
    const Names keySet(keys.begin(), keys.end());
    if (path.optionalClause &&
        !subset(keySet, q.clauses[*path.optionalClause].boundBefore)) {
      ++search.result.pruned["optional_key_not_bound"];
      return true;
    }
    // R8 exposes both endpoints even when only its source is the storage key.
    auto exposed = keySet;
    if (endpointMapping) {
      const auto& rel = q.relationships[path.edges[0]];
      exposed.insert(q.nodes[rel.source].binding);
      exposed.insert(q.nodes[rel.destination].binding);
    }
    if (keys.size() == 1 && !path.optionalClause &&
        std::none_of(path.edges.begin(), path.edges.end(), [&](auto edge) { return q.relationships[edge].recursive; }) &&
        search.uniqueMatch(q, path, keySet))
      for (auto node : path.nodes)
        if (q.nodes[node].labels.size() == 1 && !q.nodes[node].primaryKey.empty() &&
            scalar(q.nodes[node].primaryKeyType))
          exposed.insert(q.nodes[node].binding);
    if (!subset(boundary, exposed)) {
      ++search.result.pruned["shared_binding_hidden"];
      return true;
    }
    keyOptions.push_back(keys);
    if (keys.size() == 2) {
      std::reverse(keys.begin(), keys.end());
      keyOptions.push_back(std::move(keys));
    }
    return true;
  });
  if (keyOptions.empty())
    return;
  std::vector<size_t> filters;
  const auto& eligible = sourcePredicates(q, path);
  for (size_t i = 0; i < eligible.size(); ++i)
    if (local(eligible[i], vars))
      filters.push_back(i);
  size_t visitedSubsets = 0;
  auto visit = [&](const auto& chosen) {
    if (search.limits.predicateSubsets &&
        visitedSubsets >= search.limits.predicateSubsets) {
      search.result.truncated = true;
      ++search.result.pruned["predicate_subset_limit"];
      return false;
    }
    ++visitedSubsets;
    std::vector<size_t> predicates;
    for (auto i : chosen)
      predicates.push_back(filters[i]);
    for (const auto& keys : keyOptions) {
      generateContext(search, q, path, keys, predicates, pool);
      if (search.stopGeneration)
        return false;
    }
    return true;
  };
  // The full safe conjunction is often necessary to hide a filtered binding.
  // Visiting it last can exhaust a whole-call budget on unusable subsets and
  // lose even the original aggregate. Runtime parameters never enter filters.
  std::vector<size_t> all(filters.size());
  std::iota(all.begin(), all.end(), 0);
  if (!visit(all) || filters.empty())
    return;
  subsets(filters.size(), filters.size() - 1, 0, visit);
}
void runPart(Search& search, QueryPart q) {
  if (!q.unsupportedReason.empty()) {
    search.result.diagnostics.push_back({q.source, q.unsupportedReason});
    return;
  }
  // Projection-only and vertex-scan stages have no path to replace. Their
  // online operators do not need to satisfy materialization restrictions.
  if (q.relationships.empty())
    return;
  Names bindings;
  for (const auto& n : q.nodes) {
    if (n.binding.empty() || !bindings.insert(n.binding).second)
      throw std::invalid_argument(
          "Node bindings must be distinct and nonempty");
  }
  for (const auto& r : q.relationships) {
    if (r.source >= q.nodes.size() || r.destination >= q.nodes.size() ||
        r.binding.empty() || !bindings.insert(r.binding).second ||
        r.lowerBound > r.upperBound)
      throw std::invalid_argument(
          "Invalid relationship in index candidate input");
  }
  for (const auto& g : q.aggregates) {
    auto name = upper(g.name);
    const Names known = {"COUNT", "SUM", "AVG", "MIN", "MAX", "COLLECT"};
    if (g.kind != ExpressionKind::Aggregate || !known.contains(name) ||
        (name == "COUNT" && g.distinct && g.children.empty())) {
      search.result.diagnostics.push_back(
          {q.source, "Unsupported or malformed aggregate expression"});
      return;
    }
  }
  for (const auto* list : {&q.predicates, &q.groupBy, &q.aggregates, &q.outputs,
                           &q.postExpressions})
    for (const auto& e : *list) {
      // Aggregate outputs and post-aggregate operators stay at their original
      // stage. A deterministic partial finalizer may remain online; neither its
      // root nor a partial child can pass local() for precomputation.
      const bool onlineFinalizer =
          !q.aggregates.empty() &&
          (list == &q.outputs || list == &q.postExpressions);
      const bool onlineFilter = list == &q.predicates;
      if (!((onlineFinalizer || onlineFilter) ? deterministicInput(e)
                                             : safeInput(e))) {
        search.result.diagnostics.push_back(
            {q.source,
             "Unsupported, non-deterministic or potentially partial "
             "expression"});
        return;
      }
    }
  for (auto& clause : q.clauses) {
    std::vector<Expression> split;
    for (const auto& e : clause.predicates) {
      if (!(clause.optional ? safeInput(e) : deterministicInput(e))) {
        search.result.diagnostics.push_back(
            {q.source, "Unsafe match predicate"});
        return;
      }
      splitAnd(e, split);
    }
    clause.predicates = std::move(split);
  }
  std::vector<Expression> predicates;
  for (const auto& e : q.predicates)
    splitAnd(e, predicates);
  q.predicates = std::move(predicates);
  // A proof depends only on the bound QueryPart, not the selected subset.
  // Compute it once instead of repeating symbolic work per candidate bundle.
  search.reusableEdges.assign(q.relationships.size(),
                              std::vector<bool>(q.relationships.size()));
  for (size_t a = 0; a < q.relationships.size(); ++a)
    for (size_t b = a + 1; b < q.relationships.size(); ++b)
      search.reusableEdges[a][b] =
          search.couldReuse(q, q.relationships[a], q.relationships[b]);
  std::vector<std::vector<std::pair<size_t, size_t>>> adjacent(q.nodes.size());
  for (size_t i = 0; i < q.relationships.size(); ++i) {
    const auto& e = q.relationships[i];
    adjacent[e.source].push_back({e.destination, i});
    adjacent[e.destination].push_back({e.source, i});
  }
  std::vector<Item> pool;
  // OPTIONAL MATCH is its own relational input. Edges from an earlier MATCH
  // between the same boundary vertices remain online; closing this region over
  // every edge in the QueryPart would incorrectly absorb that earlier input.
  // Keep every atom of the OPTIONAL clause together, including parallel atoms,
  // and let generatePath/validate check the shared keys and null extension.
  for (const auto& clause : q.clauses) {
    if (!clause.optional || clause.relationships.empty() ||
        clause.relationships.size() > search.limits.path)
      continue;
    auto region = inducedRegion(q, clause.nodes);
    auto edges = clause.relationships;
    std::sort(edges.begin(), edges.end());
    if (region.edges == edges)
      continue;  // The ordinary induced-region traversal handles this case.
    region.edges = std::move(edges);
    generatePath(search, q, region, pool);
    if (search.stopGeneration)
      break;
  }
  std::set<std::vector<size_t>> seenRegions;
  std::function<void(std::vector<size_t>)> grow = [&](std::vector<size_t> nodes) {
    if (search.stopGeneration)
      return;
    auto region = inducedRegion(q, std::move(nodes));
    if (!seenRegions.insert(region.nodes).second)
      return;
    // The structural atom limit applies after closure. Dropping an internal
    // edge to fit the limit would discard a query constraint.
    if (region.edges.size() > search.limits.path) {
      ++search.result.pruned["induced_region_relationship_limit"];
      return;
    }
    if (!region.edges.empty())
      generatePath(search, q, region, pool);
    std::set<size_t> nextNodes;
    for (auto n : region.nodes)
      for (auto [next, edge] : adjacent[n])
        if (!contains(region.nodes, next))
          nextNodes.insert(next);
    for (auto next : nextNodes) {
      auto expanded = region.nodes;
      expanded.push_back(next);
      grow(std::move(expanded));
      if (search.stopGeneration)
        return;
    }
  };
  for (size_t i = 0; i < q.nodes.size() && !search.stopGeneration; ++i) {
    grow({i});
  }
  // A complete connected group need not be a simple path. Preserve its whole
  // source and aggregate it once; no partial-set or numeric regrouping occurs.
  if (search.limits.complete && q.singletonEmptyInput &&
      q.inputBindings.empty() && !q.aggregates.empty() &&
      !q.relationships.empty() &&
      std::none_of(q.relationships.begin(), q.relationships.end(),
                   [&](const auto& rel) { return search.limits.runtimeBoundRelationships.contains(rel.binding); }) &&
      q.relationships.size() <= search.limits.path &&
      std::none_of(q.clauses.begin(), q.clauses.end(),
                   [](const auto& c) { return c.optional; })) {
    Path whole;
    whole.nodes.resize(q.nodes.size());
    whole.edges.resize(q.relationships.size());
    std::iota(whole.nodes.begin(), whole.nodes.end(), 0);
    std::iota(whole.edges.begin(), whole.edges.end(), 0);
    Names grouping = groupingNodes(q);
    std::vector<size_t> pending{0};
    std::set<size_t> connected{0};
    while (!pending.empty()) {
      const auto n = pending.back();
      pending.pop_back();
      for (auto [next, edge] : adjacent[n])
        if (connected.insert(next).second)
          pending.push_back(next);
    }
    const bool simplePath = q.relationships.size() + 1 == q.nodes.size() &&
        std::all_of(adjacent.begin(), adjacent.end(), [](const auto& edges) { return edges.size() <= 2; });
    if (!simplePath && connected.size() == q.nodes.size() && !grouping.empty() &&
        grouping.size() <= 2 &&
        std::all_of(
            q.groupBy.begin(), q.groupBy.end(),
            [&](const auto& e) { return groupingNode(q, e).has_value(); }) &&
        std::all_of(
            q.predicates.begin(), q.predicates.end(),
            [&](const auto& e) { return local(e, pathVariables(q, whole)); })) {
      std::vector<size_t> filters(q.predicates.size());
      std::iota(filters.begin(), filters.end(), 0);
      std::vector<std::string> keys(grouping.begin(), grouping.end());
      generateContext(search, q, whole, keys, filters, pool, true);
      if (keys.size() == 2) {
        std::reverse(keys.begin(), keys.end());
        generateContext(search, q, whole, keys, filters, pool, true);
      }
    }
  }
  // Logical alternatives are validated once. Their admissible carrier choices
  // are expanded only after a complete replacement has been proved.
  std::vector<std::optional<std::vector<PhysicalItem>>> physical(pool.size());
  auto physicalOptions = [&](size_t pos) -> const std::vector<PhysicalItem>& {
    if (physical[pos])
      return *physical[pos];
    const auto& item = pool[pos];
    IndexRead read;
    bool canonicalComplete;
    auto logical = canonicalize(item.definition, search.limits.canonicalPermutations,
                                canonicalComplete, &read.bindings);
    if (!canonicalComplete) {
      search.result.truncated = true;
      ++search.result.pruned["definition_canonicalization_symmetry_limit"];
    }
    read.relationships = item.path.edges;
    if (item.path.optionalClause) {
      const auto& block = q.clauses[*item.path.optionalClause];
      read.optional = true;
      read.clause = block.position;
      read.nullBindings = block.introduced;
      for (size_t j = 0; j < block.predicates.size(); ++j)
        if (!contains(item.optionalPredicates, j))
          read.matchPredicates.push_back(block.predicates[j]);
    }
    for (const auto& col : item.definition.columns) {
      auto renamed = col;
      if (renamed.expression)
        renamed.expression = rename(*renamed.expression, read.bindings);
      for (size_t j = 0; j < logical.columns.size(); ++j)
        if (renamed.structuralKey() == logical.columns[j].structuralKey()) {
          read.columns[col.structuralKey()] = j;
          if (read.optional && col.kind == ColumnKind::Projection)
            read.noMatchProjections.emplace(
                j, nullExtend(*col.expression, read.nullBindings));
        }
    }
    std::map<std::string, PhysicalItem> options;
    for (auto carrier : search.storageOptions(q, item)) {
      auto storage = normalizedCarrier(search.carriers[carrier]);
      const auto key = storage.structuralKey();
      auto [entry, inserted] = options.try_emplace(key);
      if (inserted) {
        entry->second.definition = logical;
        entry->second.definition.storage = std::move(storage);
        entry->second.definition.definitionKey =
            logical.logicalDefinitionKey + "T" + token(key);
        entry->second.read = read;
      } else if (carrierCapabilitiesKey(storage) <
                 carrierCapabilitiesKey(*entry->second.definition.storage)) {
        // Same target, different legal declarations: keep one deterministic
        // original contract. Combining flags could invent an unproved contract.
        entry->second.definition.storage = std::move(storage);
      }
      entry->second.read.carriers.push_back(carrier);
    }
    auto& cached = physical[pos].emplace();
    for (auto& [key, option] : options)
      cached.push_back(std::move(option));
    return cached;
  };
  auto validateSelection = [&](const std::vector<size_t>& selected) {
    if (!search.combinationStep())
      return false;
    auto logicalUse = search.validate(q, pool, selected);
    if (!logicalUse)
      return true;
    std::vector<const PhysicalItem*> chosen;
    bool firstPhysical = true;
    std::function<bool(size_t)> expand = [&](size_t pos) {
      if (pos == selected.size()) {
        // The first physical realization reuses its logical validation budget
        // unit. Every additional realization is separately bounded, even when
        // usage witnesses were not requested.
        if (!firstPhysical && !search.combinationStep())
          return false;
        firstPhysical = false;
        Usage use = *logicalUse;
        for (const auto* option : chosen) {
          const auto& definition = option->definition;
          search.output.try_emplace(definition.definitionKey, definition);
          if (search.includeUses) {
            use.definitions.push_back(definition.definitionKey);
            use.reads.push_back(option->read);
          }
        }
        if (search.includeUses)
          search.result.uses.push_back(std::move(use));
        return true;
      }
      for (const auto& option : physicalOptions(selected[pos])) {
        chosen.push_back(&option);
        if (!expand(pos + 1))
          return false;
        chosen.pop_back();
      }
      return true;
    };
    return expand(0);
  };
  // Enumerate subsets in the same cardinality/position order as before, but
  // never descend below an overlapping prefix. A failed complete validation
  // does not prune its supersets: another R1 item may expose required values.
  std::vector<size_t> selected;
  std::vector<bool> occupied(q.relationships.size());
  std::function<bool(size_t, size_t)> combinations = [&](size_t begin,
                                                         size_t left) {
    if (!left)
      return validateSelection(selected);
    if (left > pool.size() - begin)
      return true;
    for (size_t i = begin; i <= pool.size() - left; ++i) {
      const auto& edges = pool[i].path.edges;
      if (std::any_of(edges.begin(), edges.end(),
                      [&](auto e) { return occupied[e]; })) {
        ++search.result.pruned["overlapping_edges"];
        continue;
      }
      selected.push_back(i);
      for (auto e : edges)
        occupied[e] = true;
      const bool keepGoing = combinations(i + 1, left - 1);
      for (auto e : edges)
        occupied[e] = false;
      selected.pop_back();
      if (!keepGoing)
        return false;
    }
    return true;
  };
  for (size_t count = 1; count <= std::min(pool.size(), search.limits.views);
       ++count)
    if (!combinations(0, count))
      break;
}
}  // namespace

Expression Expression::literal(std::string value, std::string type) {
  Expression out;
  out.kind = ExpressionKind::Literal;
  out.name = std::move(value);
  out.type = std::move(type);
  out.nullable = upper(out.name) == "NULL" && upper(out.type) != "STRING";
  return out;
}
Expression Expression::variable(std::string binding, std::string type) {
  Expression out;
  out.kind = ExpressionKind::Variable;
  out.binding = std::move(binding);
  out.type = std::move(type);
  return out;
}
Expression Expression::property(std::string binding, std::string name,
                                std::string type, bool nullable) {
  Expression out;
  out.kind = ExpressionKind::Property;
  out.binding = std::move(binding);
  out.name = std::move(name);
  out.type = std::move(type);
  out.nullable = nullable;
  return out;
}
Expression Expression::parameter(std::string name, std::string type) {
  Expression out;
  out.kind = ExpressionKind::Parameter;
  out.name = std::move(name);
  out.type = std::move(type);
  return out;
}
Expression Expression::call(std::string name, std::vector<Expression> children,
                            std::string type, bool safe) {
  Expression out;
  out.kind = ExpressionKind::Call;
  out.name = upper(std::move(name));
  out.children = std::move(children);
  out.type = std::move(type);
  out.safeToPrecompute = safe;
  return out;
}
Expression Expression::aggregate(std::string name,
                                 std::vector<Expression> children,
                                 std::string type, bool distinct) {
  Expression out;
  out.kind = ExpressionKind::Aggregate;
  out.name = upper(std::move(name));
  out.children = std::move(children);
  out.type = std::move(type);
  out.distinct = distinct;
  return out;
}
std::string Expression::structuralKey() const {
  std::string out =
      token(std::to_string(static_cast<int>(kind))) + token(type) +
      token(name) + token(binding) + token(std::to_string(distinct)) +
      token(std::to_string(kind == ExpressionKind::Literal && nullable));
  for (const auto& c : children)
    out += token(c.structuralKey());
  return out;
}
std::string Expression::toCypher() const {
  if (kind == ExpressionKind::Call && name == "PATTERN_NODE") {
    std::string out = "(" + children.at(0).toCypher();
    for (size_t i = 1; i < children.size(); ++i)
      out += ":" + identifier(children[i].name);
    return out + ")";
  }
  if (kind == ExpressionKind::PatternExists) {
    std::vector<std::string> types;
    for (size_t i = 2; i < children.size(); ++i)
      types.push_back(identifier(children[i].name));
    if (types.empty() || (name != "DIRECTED" && name != "UNDIRECTED"))
      throw std::invalid_argument("Pattern EXISTS requires explicit types and direction");
    return "EXISTS { MATCH " + children.at(0).toCypher() + "-[:" + join(types, "|") +
           (name == "DIRECTED" ? "]->" : "]-") + children.at(1).toCypher() + " }";
  }
  if (kind == ExpressionKind::ResidualPredicate)
    return name;
  if (kind == ExpressionKind::Variable)
    return identifier(binding);
  if (kind == ExpressionKind::Property) {
    if (upper(name) == "_ID")
      return "id(" + identifier(binding) + ")";
    return identifier(binding) + "." + identifier(name);
  }
  if (kind == ExpressionKind::Parameter)
    return "$" + identifier(name);
  if (kind == ExpressionKind::Literal) {
    const auto valueType = upper(type);
    if (valueType == "DATE32" || valueType == "TIMESTAMP64")
      throw std::invalid_argument(
          "Cannot render a storage-only temporal literal as Cypher");
    if (nullable) {
      // Bound graph types include catalog label metadata that is not a Cypher
      // CAST target. CASE/COUNT context rebinds a bare NULL to the graph type;
      // the original type remains in structuralKey() for definition identity.
      const bool graphType =
          valueType == "NODE" || valueType == "REL" ||
          valueType == "RECURSIVE_REL" ||
          ((valueType.starts_with("NODE(") || valueType.starts_with("REL(") ||
            valueType.starts_with("RECURSIVE_REL(")) &&
           valueType.ends_with(")"));
      return valueType == "ANY" || graphType
                 ? "NULL"
                 : "CAST(NULL, " + quote(type) + ")";
    }
    if (valueType == "STRING")
      return quote(name);
    if (valueType == "BOOL" || valueType == "BOOLEAN")
      return name;
    if (scalar(type))
      return "CAST(" + quote(name) + ", " + quote(type) + ")";
    throw std::invalid_argument(
        "Cannot render a non-scalar literal as an index column");
  }
  std::vector<std::string> args;
  for (const auto& c : children)
    args.push_back(c.toCypher());
  if (kind == ExpressionKind::Call && name == "LIST_CREATION")
    return "[" + join(args, ", ") + "]";
  if (kind == ExpressionKind::Aggregate)
    return name + "(" + (distinct ? "DISTINCT " : "") +
           (args.empty() ? "*" : join(args, ", ")) + ")";
  if (name == "CASE") {
    if (args.empty() || args.size() % 2 == 0)
      throw std::invalid_argument("Malformed CASE expression");
    std::string out = "CASE";
    for (size_t i = 0; i + 1 < args.size(); i += 2)
      out += " WHEN " + args[i] + " THEN " + args[i + 1];
    return out + " ELSE " + args.back() + " END";
  }
  if ((name == "IS NULL" || name == "IS NOT NULL") && args.size() == 1)
    return "(" + args[0] + " " + name + ")";
  if (name == "NOT" && args.size() == 1)
    return "(NOT " + args[0] + ")";
  static const Names binary = {"+", "-", "*",  "/",  "%",   "=",  "<>", "!=",
                               "<", ">", "<=", ">=", "AND", "OR", "XOR"};
  if (binary.contains(name) && args.size() == 2)
    return "(" + args[0] + " " + name + " " + args[1] + ")";
  return name + "(" + join(args, ", ") + ")";
}
std::string Carrier::structuralKey() const {
  std::string key = "T" + token(std::to_string(static_cast<int>(kind)));
  for (const auto& label : sorted(sourceLabels))
    key += "S" + token(label);
  for (const auto& label : sorted(destinationLabels))
    key += "D" + token(label);
  key += "R" + token(relationshipType);
  return key;
}
std::string Column::structuralKey() const {
  return token(std::to_string(static_cast<int>(kind))) + token(type) +
         token(expression ? expression->structuralKey() : "") +
         (distinct ? "DISTINCT" : "");
}
std::string Candidate::toCypher() const {
  if (!sourceQuery.empty())
    return sourceQuery;
  auto node = [&](size_t i) {
    const auto& n = nodes.at(i);
    std::string out = "(" + identifier(n.binding);
    for (const auto& type : n.labels)
      out += ":" + identifier(type);
    return out + ")";
  };
  std::map<size_t, std::vector<std::string>> clauses;
  for (const auto& rel : relationships) {
    std::vector<std::string> types;
    for (const auto& t : rel.types)
      types.push_back(identifier(t));
    std::string edge = "[" + identifier(rel.binding);
    if (!types.empty())
      edge += ":" + join(types, "|");
    if (rel.recursive) {
      if (rel.pathSemantics != "WALK")
        throw std::invalid_argument(
            "Cypher rendering supports WALK recursion only");
      edge += "*" + std::to_string(rel.lowerBound) + "..";
      if (rel.upperBound != std::numeric_limits<uint64_t>::max())
        edge += std::to_string(rel.upperBound);
    }
    edge += "]";
    clauses[rel.matchGroup].push_back(node(rel.source) + "-" + edge +
                                      (rel.directed ? "->" : "-") +
                                      node(rel.destination));
  }
  std::string out;
  for (const auto& [group, patterns] : clauses)
    out += "MATCH " + join(patterns, ", ") + "\n";
  if (storage && storage->kind == CarrierKind::ExistingEdgeProperties &&
      keys.size() == 2) {
    // The unique-key storage contract makes this a semijoin: one existing
    // carrier per pair. Keeping it in a separate MATCH avoids introducing a
    // relationship-uniqueness predicate against the computation's own edges.
    out += "MATCH (" + identifier(keys[0]) +
           ")-[:" + identifier(storage->relationshipType) + "]->(" +
           identifier(keys[1]) + ")\n";
  }
  if (!predicates.empty()) {
    std::vector<std::string> filters;
    for (const auto& p : predicates)
      filters.push_back("(" + p.toCypher() + ")");
    out += "WHERE " + join(filters, " AND ") + "\n";
  }
  out += "RETURN ";
  if (kind == CandidateKind::Existence ||
      kind == CandidateKind::EndpointMapping)
    out += "DISTINCT ";
  std::vector<std::string> outputs;
  for (size_t i = 0; i < keys.size(); ++i)
    outputs.push_back(identifier(keys[i]) + " AS key_" + std::to_string(i));
  for (size_t i = 0; i < columns.size(); ++i)
    outputs.push_back(columnQuery(columns[i]) + " AS value_" +
                      std::to_string(i));
  return out + join(outputs, ", ");
}
EnumerationResult enumerateCandidates(const std::vector<QueryPart>& parts,
                                      const Constraints& constraints,
                                      const std::vector<Carrier>& carriers,
                                      const Limits& limits, bool includeUses) {
  if (!limits.path || !limits.views)
    throw std::invalid_argument("path and views limits must be positive");
  Search search{constraints, carriers, limits, includeUses, {},
                {},          false,    false,  {}};
  for (const auto& part : parts) {
    if (search.stopGeneration || search.stopCombinations) {
      search.result.truncated = true;
      break;
    }
    runPart(search, part);
  }
  for (auto& [key, definition] : search.output)
    search.result.candidates.push_back(std::move(definition));
  return std::move(search.result);
}
}  // namespace neug::index

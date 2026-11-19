#include "neug/compiler/index/computation_prefix_enumerator.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <limits>
#include <stdexcept>

#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/parameter_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/parser/parser.h"
#include "neug/compiler/parser/query/regular_query.h"

namespace neug::index {
namespace {
using E = std::shared_ptr<binder::Expression>;
using Names = std::set<std::string>;
using ET = common::ExpressionType;
struct Token {
  std::string text;
  size_t begin, end;
  int depth;
};
std::string upper(std::string value) {
  for (auto& c : value)
    c = std::toupper(static_cast<unsigned char>(c));
  return value;
}
// Lexical boundaries are checked against the real parser's projection count.
// Quoted identifiers, strings, comments, parameters and nested subqueries cannot
// masquerade as top-level WITH/RETURN clauses.
std::vector<Token> tokens(const std::string& input) {
  std::vector<Token> result;
  int depth = 0;
  for (size_t i = 0; i < input.size();) {
    const auto start = i;
    const auto c = input[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (input.compare(i, 2, "//") == 0) {
      i = input.find('\n', i);
      if (i == std::string::npos)
        break;
      continue;
    }
    if (input.compare(i, 2, "/*") == 0) {
      auto end = input.find("*/", i + 2);
      if (end == std::string::npos)
        throw std::invalid_argument("Unclosed comment");
      i = end + 2;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      ++i;
      while (i < input.size()) {
        if (input[i] == '\\') {
          i += 2;
          continue;
        }
        if (input[i++] == c) {
          if (i < input.size() && input[i] == c) {
            ++i;
            continue;
          }
          break;
        }
      }
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      while (i < input.size() &&
             (std::isalnum(static_cast<unsigned char>(input[i])) ||
              input[i] == '_' || input[i] == '$'))
        ++i;
    } else {
      ++i;
    }
    if (c == ')' || c == ']' || c == '}')
      --depth;
    result.push_back({input.substr(start, i - start), start, i, depth});
    if (c == '(' || c == '[' || c == '{')
      ++depth;
  }
  return result;
}
std::string fingerprint(const std::string& source) {
  std::string key;
  for (const auto& t : tokens(source))
    key += std::to_string(t.text.size()) + ":" + t.text;
  return key;
}
const Names clauseBoundaries = {"WHERE", "ORDER", "SKIP", "LIMIT", "MATCH", "OPTIONAL", "WITH", "RETURN", "UNWIND", "CALL"};
struct ProjectionText {
  std::vector<std::string> expressions;
  size_t tail;
};
ProjectionText projectionText(const std::string& source,
                              const std::vector<Token>& ts, size_t projection) {
  size_t begin = projection + 1;
  if (begin < ts.size() && upper(ts[begin].text) == "DISTINCT") ++begin;
  ProjectionText result{{}, source.size()};
  size_t item = begin;
  size_t alias = std::string::npos;
  int cases = 0;
  auto append = [&](size_t end) {
    if (item < ts.size() && ts[item].begin < end)
      result.expressions.push_back(source.substr(ts[item].begin,
          (alias == std::string::npos ? end : alias) - ts[item].begin));
  };
  for (size_t i = begin; i < ts.size(); ++i) {
    if (ts[i].depth) continue;
    const auto word = upper(ts[i].text);
    if (word == "CASE") ++cases;
    if (word == "END") --cases;
    if (cases) continue;
    if (word == "AS" && (i == 0 || ts[i - 1].text != ".")) alias = ts[i].begin;
    if (word == ",") {
      append(ts[i].begin);
      item = i + 1;
      alias = std::string::npos;
      continue;
    }
    if (word == ";" || (clauseBoundaries.contains(word) &&
        (i == 0 || (upper(ts[i - 1].text) != "AS" && ts[i - 1].text != "." &&
                    !(word == "WITH" && (upper(ts[i - 1].text) == "STARTS" || upper(ts[i - 1].text) == "ENDS")))))) {
      append(ts[i].begin);
      result.tail = ts[i].begin;
      return result;
    }
  }
  append(source.size());
  return result;
}
std::string identifier(const std::string& value) {
  return Expression::variable(value).toCypher();
}
std::string join(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& v : values)
    result += (result.empty() ? "" : ", ") + v;
  return result;
}
bool subset(const Names& a, const Names& b) {
  return std::includes(b.begin(), b.end(), a.begin(), a.end());
}
bool includes(std::vector<std::string> a, std::vector<std::string> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return !a.empty() && std::includes(a.begin(), a.end(), b.begin(), b.end());
}
std::vector<E> children(const E& e) {
  if (e->expressionType == ET::CASE_ELSE) {
    auto* c = e->constPtrCast<binder::CaseExpression>();
    std::vector<E> out;
    for (size_t i = 0; i < c->getNumCaseAlternatives(); ++i) {
      out.push_back(c->getCaseAlternative(i)->whenExpression);
      out.push_back(c->getCaseAlternative(i)->thenExpression);
    }
    out.push_back(c->getElseExpression());
    return out;
  }
  return e->getChildren();
}
bool deterministic(const E& e, const Constraints& constraints);
bool deterministicGraph(const binder::QueryGraphCollection& graphs, const Constraints& constraints,
                        const Names& runtimeBounds = {}) {
  for (const auto& rel : graphs.getQueryRels()) {
    if (runtimeBounds.contains(rel->getUniqueName()))
      return false;
    if (!rel->isRecursive())
      continue;
    const auto* info = rel->getRecursiveInfo();
    if (!info || !info->bindData)
      return false;
    if (rel->getRelType() == common::QueryRelType::VARIABLE_LENGTH_WALK &&
        info->bindData->upperBound == std::numeric_limits<uint32_t>::max()) {
      // RecursiveConstraint describes directed source-to-destination paths,
      // as in R8. A directed forest does not prove undirected WALK termination.
      if (rel->getDirectionType() != binder::RelDirectionType::SINGLE)
        return false;
      std::vector<std::string> src, dst, types;
      for (auto* entry : rel->getSrcNode()->getEntries()) src.push_back(entry->getName());
      for (auto* entry : rel->getDstNode()->getEntries()) dst.push_back(entry->getName());
      for (auto* entry : rel->getEntries()) types.push_back(entry->getName());
      bool finite = false;
      for (const auto& proof : constraints.recursivePaths)
        finite |= proof.finiteMatches && proof.pathSemantics == "WALK" &&
                  proof.lowerBound == info->bindData->lowerBound &&
                  proof.upperBound == std::numeric_limits<uint64_t>::max() &&
                  includes(proof.sourceLabels, src) && includes(src, proof.sourceLabels) &&
                  includes(proof.destinationLabels, dst) && includes(dst, proof.destinationLabels) &&
                  includes(proof.relationshipTypes, types) && includes(types, proof.relationshipTypes);
      if (!finite)
        return false;
    }
    if ((info->nodePredicate && !deterministic(info->nodePredicate, constraints)) ||
        (info->relPredicate && !deterministic(info->relPredicate, constraints)))
      return false;
    for (const auto* list : {&info->nodeProjectionList, &info->relProjectionList})
      for (const auto& expression : *list)
        if (!deterministic(expression, constraints))
          return false;
  }
  return true;
}
bool deterministic(const E& e, const Constraints& constraints) {
  if (!e || e->expressionType == ET::PARAMETER)
    return false;
  if (e->expressionType == ET::FUNCTION) {
    const auto* f = e->constPtrCast<binder::ScalarFunctionExpression>();
    static const Names allowed = {
        "+", "-", "*", "/", "%", "NEGATE", "ABS", "CAST", "LABEL",
        "LOWER", "UPPER", "LENGTH", "CHAR_LENGTH", "COALESCE", "NULLIF",
        "STARTS_WITH", "ENDS_WITH", "CONTAINS", "DATE_PART", "LIST_CONTAINS",
        "LIST_EXTRACT", "LIST_CREATION", "ROUND", "SQRT", "FLOOR", "CEIL",
        "TO_DATE", "DATE", "TIMESTAMP", "TO_INTERVAL", "INTERVAL", "DURATION"};
    if (!f->getFunction().isReadOnly ||
        !allowed.contains(upper(f->getFunction().name)))
      return false;
  }
  if (e->expressionType == ET::AGGREGATE_FUNCTION) {
    static const Names allowed = {"COUNT", "COUNT_STAR", "SUM", "AVG", "MIN", "MAX"};
    if (!allowed.contains(upper(e->constPtrCast<binder::AggregateFunctionExpression>()->getFunction().name)))
      return false;
  }
  if (e->expressionType == ET::SUBQUERY) {
    const auto* s = e->constPtrCast<binder::SubqueryExpression>();
    if (s->hasWhereExpression() && !deterministic(s->getWhereExpression(), constraints))
      return false;
    if (!deterministicGraph(*s->getQueryGraphCollection(), constraints) ||
        (s->getProjectionExpr() && !deterministic(s->getProjectionExpr(), constraints)) ||
        (s->getCountStarExpr() && !deterministic(s->getCountStarExpr(), constraints)))
      return false;
  }
  const auto cs = children(e);
  return std::all_of(cs.begin(), cs.end(), [&](const auto& child) { return deterministic(child, constraints); });
}
bool scalar(const E& e) {
  const auto type = e->getDataType().getLogicalTypeID();
  return type != common::LogicalTypeID::NODE && type != common::LogicalTypeID::REL &&
         type != common::LogicalTypeID::RECURSIVE_REL && type != common::LogicalTypeID::LIST &&
         type != common::LogicalTypeID::STRUCT && type != common::LogicalTypeID::MAP &&
         type != common::LogicalTypeID::ARRAY && type != common::LogicalTypeID::UNION &&
         type != common::LogicalTypeID::POINTER && type != common::LogicalTypeID::ANY &&
         type != common::LogicalTypeID::INTERNAL_ID;
}

// Generalizing a parameter-selected prefix evaluates additional groups. Keep
// this extension to total expressions: a partial calculation in an unselected
// group must not make an otherwise valid online query fail during construction.
bool totalForGeneralization(const E& e) {
  if (!e || e->expressionType == ET::SUBQUERY ||
      e->expressionType == ET::PARAMETER)
    return false;
  if (e->expressionType == ET::AGGREGATE_FUNCTION) {
    const auto name = upper(e->constPtrCast<binder::AggregateFunctionExpression>()->getFunction().name);
    if (name != "COUNT" && name != "COUNT_STAR")
      return false;
  }
  if (e->expressionType == ET::FUNCTION) {
    static const Names total = {"COALESCE", "NULLIF", "LOWER", "UPPER", "LENGTH",
                                "CHAR_LENGTH", "STARTS_WITH", "ENDS_WITH", "CONTAINS"};
    if (!total.contains(upper(e->constPtrCast<binder::ScalarFunctionExpression>()->getFunction().name)))
      return false;
  }
  const auto cs = children(e);
  return std::all_of(cs.begin(), cs.end(), totalForGeneralization);
}

struct DeferredPredicate {
  std::string binding, alias, property, parameter, comparison;
};
std::string reverseComparison(const std::string& value) {
  if (value == ">") return "<";
  if (value == "<") return ">";
  if (value == ">=") return "<=";
  if (value == "<=") return ">=";
  return value;
}
std::string unquoteIdentifier(const std::string& text) {
  if (text.size() < 2 || text.front() != '`' || text.back() != '`')
    return text;
  std::string out;
  for (size_t i = 1; i + 1 < text.size(); ++i) {
    out += text[i];
    if (text[i] == '`' && i + 2 < text.size() && text[i + 1] == '`') ++i;
  }
  return out;
}
std::string parsedIdentifierName(const std::string& name) {
  // The parser removes the enclosing quotes but retains doubled backticks.
  // Convert that representation to the name accepted by identifier(), without
  // treating backticks that are part of the name as another enclosing layer.
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    out += name[i];
    if (name[i] == '`' && i + 1 < name.size() && name[i + 1] == '`') ++i;
  }
  return out;
}
std::string projectionName(const std::unique_ptr<parser::ParsedExpression>& expression) {
  if (expression->hasAlias()) return parsedIdentifierName(expression->getAlias());
  const auto& raw = expression->getRawName();
  const auto lexical = tokens(raw);
  // Raw names of bare quoted variables retain their lexical quotes. Other
  // expression names (including quoted property access) are output names,
  // while explicit aliases have already lost only their enclosing quotes.
  if (lexical.size() == 1 && lexical[0].text.size() >= 2 &&
      lexical[0].text.front() == '`' && lexical[0].text.back() == '`')
    return unquoteIdentifier(lexical[0].text);
  return raw;
}

// Bound predicates prove the algebraic preconditions; this lexer only edits
// the corresponding simple spellings. Unsupported spellings fail closed.
// Literal strings, comments and nested expressions cannot be mistaken for a
// runtime parameter or a node property map.
std::optional<std::string> removeDeferredPredicates(
    const std::string& source, const std::vector<DeferredPredicate>& deferred) {
  const auto ts = tokens(source);
  std::string marker = "__iv_deferred_predicate__";
  while (source.find(marker) != std::string::npos) marker += "_";
  struct Edit { size_t begin, end; std::string replacement; };
  std::vector<Edit> edits;
  std::vector<bool> seen(deferred.size(), false);
  auto matches = [&](const std::string& alias, const std::string& property,
                     const std::string& parameter, std::string comparison) {
    if (comparison == "!=") comparison = "<>";
    bool found = false;
    for (size_t i = 0; i < deferred.size(); ++i)
      if (unquoteIdentifier(alias) == deferred[i].alias &&
          unquoteIdentifier(property) == deferred[i].property &&
          parameter == "$" + deferred[i].parameter && comparison == deferred[i].comparison) {
        seen[i] = true;
        found = true;
      }
    return found;
  };
  for (size_t i = 0; i < ts.size(); ++i) {
    if (ts[i].text.empty() || ts[i].text.front() != '$') continue;
    bool replaced = false;
    for (size_t width : {1u, 2u}) {
      if (i >= width + 3 && ts[i - width - 2].text == ".") {
        std::string comparison;
        for (size_t j = i - width; j < i; ++j) comparison += ts[j].text;
        if (matches(ts[i - width - 3].text, ts[i - width - 1].text, ts[i].text, comparison)) {
          edits.push_back({ts[i - width - 3].begin, ts[i].end, marker});
          replaced = true;
          break;
        }
      }
      if (i + width + 3 < ts.size() && ts[i + width + 2].text == ".") {
        std::string comparison;
        for (size_t j = i + 1; j <= i + width; ++j) comparison += ts[j].text;
        if (matches(ts[i + width + 1].text, ts[i + width + 3].text, ts[i].text,
                    reverseComparison(comparison))) {
          edits.push_back({ts[i].begin, ts[i + width + 3].end, marker});
          i += width + 3;
          replaced = true;
          break;
        }
      }
    }
    if (replaced) continue;
    if (i < 2 || ts[i - 1].text != ":") return std::nullopt;
    size_t open = i;
    while (open > 0 && !(ts[open].text == "{" && ts[open].depth == ts[i].depth - 1)) --open;
    if (ts[open].text != "{") return std::nullopt;
    size_t node = open;
    while (node > 0 && ts[node].depth >= ts[open].depth) --node;
    if (ts[node].text != "(" || node + 1 >= open) return std::nullopt;
    size_t close = i;
    while (close < ts.size() && !(ts[close].text == "}" && ts[close].depth == ts[open].depth)) ++close;
    if (close == ts.size()) return std::nullopt;
    std::vector<std::string> fields;
    size_t begin = open + 1;
    for (size_t end = begin; end <= close; ++end) {
      if (end != close && !(ts[end].text == "," && ts[end].depth == ts[open].depth + 1)) continue;
      bool runtime = false;
      for (size_t j = begin; j < end; ++j)
        runtime |= !ts[j].text.empty() && ts[j].text.front() == '$';
      if (runtime) {
        if (end != begin + 3 || ts[begin + 1].text != ":" ||
            !matches(ts[node + 1].text, ts[begin].text, ts[begin + 2].text, "="))
          return std::nullopt;
      } else if (begin < end) {
        fields.push_back(source.substr(ts[begin].begin, ts[end - 1].end - ts[begin].begin));
      }
      begin = end + 1;
    }
    edits.push_back({ts[open].begin, ts[close].end, fields.empty() ? "" : "{" + join(fields) + "}"});
    i = close;
  }
  if (std::any_of(seen.begin(), seen.end(), [](bool value) { return !value; }))
    return std::nullopt;
  std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.begin > b.begin; });
  auto result = source;
  size_t next = source.size();
  for (const auto& edit : edits) {
    if (edit.end > next) return std::nullopt;
    result.replace(edit.begin, edit.end - edit.begin, edit.replacement);
    next = edit.begin;
  }
  // Remove marked conjuncts and their WHERE when nothing remains. Emitting a
  // constant-true filter (or a synthetic node NULL test) here triggers existing
  // execution-backend bugs before OPTIONAL MATCH. Syntactic removal also gives
  // the optimizer the actual parameter-independent pattern without tautologies.
  const auto cleanedTokens = tokens(result);
  std::function<std::string(size_t, size_t)> simplify = [&](size_t begin, size_t end) -> std::string {
    while (begin < end && cleanedTokens[begin].text == "(" && cleanedTokens[end - 1].text == ")") {
      size_t closing = begin + 1;
      while (closing < end && !(cleanedTokens[closing].text == ")" &&
             cleanedTokens[closing].depth == cleanedTokens[begin].depth)) ++closing;
      if (closing != end - 1) break;
      ++begin;
      --end;
    }
    if (begin == end || (begin + 1 == end && cleanedTokens[begin].text == marker)) return {};
    std::vector<std::string> retained;
    size_t item = begin;
    int cases = 0;
    for (size_t i = begin; i < end; ++i) {
      if (cleanedTokens[i].depth != cleanedTokens[begin].depth) continue;
      const auto word = upper(cleanedTokens[i].text);
      if (word == "CASE") ++cases;
      if (word == "END") --cases;
      if (word == "AND" && !cases && (i == begin || cleanedTokens[i - 1].text != ".")) {
        auto part = simplify(item, i);
        if (!part.empty()) retained.push_back(std::move(part));
        item = i + 1;
      }
    }
    if (item == begin)
      return result.substr(cleanedTokens[begin].begin, cleanedTokens[end - 1].end - cleanedTokens[begin].begin);
    auto part = simplify(item, end);
    if (!part.empty()) retained.push_back(std::move(part));
    std::string expression;
    for (const auto& value : retained)
      expression += (expression.empty() ? "" : " AND ") + ("(" + value + ")");
    return expression;
  };
  edits.clear();
  for (size_t i = 0; i < cleanedTokens.size(); ++i) {
    if (cleanedTokens[i].depth || upper(cleanedTokens[i].text) != "WHERE" ||
        (i && cleanedTokens[i - 1].text == ".")) continue;
    size_t end = i + 1;
    int cases = 0;
    for (; end < cleanedTokens.size(); ++end) {
      if (cleanedTokens[end].depth) continue;
      const auto word = upper(cleanedTokens[end].text);
      if (word == "CASE") ++cases;
      if (word == "END") --cases;
      if (!cases && clauseBoundaries.contains(word) && cleanedTokens[end - 1].text != "." &&
          !(word == "WITH" && (upper(cleanedTokens[end - 1].text) == "STARTS" ||
                               upper(cleanedTokens[end - 1].text) == "ENDS"))) break;
    }
    const auto finish = end < cleanedTokens.size() ? cleanedTokens[end].begin : result.size();
    if (result.substr(cleanedTokens[i].begin, finish - cleanedTokens[i].begin).find(marker) != std::string::npos) {
      const auto expression = simplify(i + 1, end);
      edits.push_back({cleanedTokens[i].begin, finish, expression.empty() ? "\n" : "WHERE " + expression + "\n"});
    }
    i = end ? end - 1 : end;
  }
  for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
    result.replace(edit->begin, edit->end - edit->begin, edit->replacement);
  if (result.find(marker) != std::string::npos) return std::nullopt;
  return result;
}
}  // namespace

EnumerationResult enumerateComputationPrefixes(
    const std::string& cypher, const binder::BoundRegularQuery& bound,
    const std::vector<Carrier>& carriers, const Limits& limits,
    const UniqueProperties& uniqueProperties, const std::string& source,
    const Constraints& constraints) {
  EnumerationResult result;
  if (!limits.prefixes)
    return result;
  auto parsed = parser::Parser::parseQuery(cypher);
  if (parsed.size() != 1 || bound.getNumSingleQueries() != 1 ||
      !bound.getPreQueryPart().empty() || bound.getPostSingleQuery())
    return result;
  const auto* query = parsed[0]->constPtrCast<parser::RegularQuery>()->getSingleQuery(0);
  const auto* single = bound.getSingleQuery(0);
  const auto ts = tokens(cypher);
  std::string temporary = "__iv_";
  while (cypher.find(temporary) != std::string::npos)
    temporary += "_";
  std::vector<size_t> projections;
  for (size_t i = 0; i < ts.size(); ++i) {
    if (ts[i].depth)
      continue;
    const auto keyword = upper(ts[i].text);
    if (keyword == "RETURN" ||
        (keyword == "WITH" && (i == 0 || (upper(ts[i - 1].text) != "STARTS" && upper(ts[i - 1].text) != "ENDS"))))
      projections.push_back(i);
  }
  if (projections.size() != single->getNumQueryParts() ||
      projections.size() != query->getNumQueryParts() + (query->hasReturnClause() ? 1 : 0)) {
    result.diagnostics.push_back({source, "Cannot align exact prefix projections with bound QueryParts"});
    return result;
  }
  std::map<std::string, const binder::NodeExpression*> nodes;
  struct EdgeDomain { std::string source, destination; std::vector<std::string> types; };
  std::vector<EdgeDomain> edgeDomains;
  std::map<std::string, std::string> liveNames;
  std::map<std::string, Names> dependencies;
  Names liveNodes, nonNullNodes, fixedNodes, previousKey;
  // Only values produced by an earlier COUNT are nonnegative integer measures.
  // Keep their bound identity across aliases; arbitrary SUM inputs do not have
  // the monotonicity needed to partition a checked fixed-width sum safely.
  Names countMeasures;
  bool validPrefix = true, uniqueInput = false;
  std::map<std::string, Candidate> output;
  for (size_t pos = 0; pos < projections.size(); ++pos) {
    const auto* part = single->getQueryPart(pos);
    if (part->hasUpdatingClause() || !part->hasProjectionBody())
      break;
    const auto* body = part->getProjectionBody();
    std::vector<DeferredPredicate> deferred;
    Names deferredNodes;
    bool totalGeneralization = true;
    std::function<Names(const E&)> deps = [&](const E& e) -> Names {
      auto name = e->getUniqueName();
      if (auto it = dependencies.find(name); it != dependencies.end())
        return it->second;
      if (e->expressionType == ET::PROPERTY)
        return {e->constPtrCast<binder::PropertyExpression>()->getVariableName()};
      if (e->expressionType == ET::SUBQUERY)
        return {"__unproved_subquery_dependency:" + name};
      if (e->expressionType == ET::PATTERN || e->expressionType == ET::VARIABLE)
        return {name};
      Names out;
      for (const auto& c : children(e)) {
        auto values = deps(c);
        out.insert(values.begin(), values.end());
      }
      return out;
    };
    bool expands = part->hasReadingClause();
    for (size_t i = 0; i < part->getNumReadingClause(); ++i) {
      const auto* clause = part->getReadingClause(i);
      if (clause->getClauseType() != common::ClauseType::MATCH) {
        validPrefix = false;
        continue;
      }
      const auto* match = clause->constPtrCast<binder::BoundMatchClause>();
      validPrefix &= deterministicGraph(*match->getQueryGraphCollection(), constraints,
                                        limits.runtimeBoundRelationships);
      for (const auto& rel : match->getQueryGraphCollection()->getQueryRels())
        totalGeneralization &= !rel->isRecursive();
      if (match->getMatchClauseType() == common::MatchClauseType::MATCH)
        for (const auto& rel : match->getQueryGraphCollection()->getQueryRels())
          if (!rel->isRecursive() && rel->getDirectionType() == binder::RelDirectionType::SINGLE) {
            EdgeDomain domain{rel->getSrcNodeName(), rel->getDstNodeName(), {}};
            for (auto* entry : rel->getEntries()) domain.types.push_back(entry->getName());
            edgeDomains.push_back(std::move(domain));
          }
      for (const auto& n : match->getQueryGraphCollection()->getQueryNodes()) {
        nodes[n->getUniqueName()] = n.get();
        if (!liveNames.contains(n->getUniqueName()))
          liveNames[n->getUniqueName()] = parsedIdentifierName(n->getVariableName());
        liveNodes.insert(n->getUniqueName());
        if (match->getMatchClauseType() == common::MatchClauseType::MATCH)
          nonNullNodes.insert(n->getUniqueName());
      }
      if (clause->hasPredicate()) {
        for (const auto& predicate : clause->getPredicate()->splitOnAND()) {
          if (deterministic(predicate, constraints)) {
            totalGeneralization &= totalForGeneralization(predicate);
            continue;
          }
          bool accepted = false;
          // A predicate on an existing grouping vertex is constant per group.
          // A unique-property equality additionally allows retaining a new
          // vertex as a grouping dimension: it selects at most one vertex, so
          // the original groups survive after online filtering. Limit the
          // extension to the first boundary: subsequent regrouping or LIMIT
          // must not accidentally mix the additional parameter dimension.
          if (pos == 0 && body->hasAggregateExpressions() &&
              match->getMatchClauseType() == common::MatchClauseType::MATCH &&
              common::ExpressionTypeUtil::isComparison(predicate->expressionType) &&
              predicate->getNumChildren() == 2) {
            for (size_t side = 0; side < 2; ++side) {
              const auto value = predicate->getChild(side), parameter = predicate->getChild(1 - side);
              if (value->expressionType != ET::PROPERTY || parameter->expressionType != ET::PARAMETER)
                continue;
              const auto* property = value->constPtrCast<binder::PropertyExpression>();
              const auto it = nodes.find(property->getVariableName());
              if (it == nodes.end() || it->second->getNumEntries() != 1) continue;
              const auto label = it->second->getEntries()[0]->getName();
              const auto name = property->getPropertyName();
              const bool unique = property->isPrimaryKey() ||
                  (uniqueProperties.contains(label) && uniqueProperties.at(label).contains(name)) ||
                  (constraints.uniqueProperties.contains(label) && constraints.uniqueProperties.at(label).contains(name));
              const bool nonNull = property->isPrimaryKey() ||
                  (constraints.nonNullProperties.contains(label) && constraints.nonNullProperties.at(label).contains(name));
              bool alreadyGrouped = false;
              for (const auto& group : body->getGroupByExpressions()) {
                alreadyGrouped |= group->expressionType == ET::PATTERN && group->getUniqueName() == it->first;
                if (group->expressionType == ET::PROPERTY) {
                  const auto* groupedProperty = group->constPtrCast<binder::PropertyExpression>();
                  if (groupedProperty->getVariableName() != it->first) continue;
                  const auto groupedName = groupedProperty->getPropertyName();
                  const bool groupedUnique = groupedProperty->isPrimaryKey() ||
                      (uniqueProperties.contains(label) && uniqueProperties.at(label).contains(groupedName)) ||
                      (constraints.uniqueProperties.contains(label) && constraints.uniqueProperties.at(label).contains(groupedName));
                  const bool groupedNonNull = groupedProperty->isPrimaryKey() ||
                      (constraints.nonNullProperties.contains(label) && constraints.nonNullProperties.at(label).contains(groupedName));
                  alreadyGrouped |= groupedUnique && groupedNonNull;
                }
              }
              if (!alreadyGrouped && !(predicate->expressionType == ET::EQUALS && unique && nonNull)) continue;
              auto comparison = common::ExpressionTypeUtil::toParsableString(predicate->expressionType);
              if (side == 1) comparison = reverseComparison(comparison);
              deferred.push_back({it->first, it->second->getVariableName(), name,
                                 parameter->constPtrCast<binder::ParameterExpression>()->getName(), comparison});
              deferredNodes.insert(it->first);
              accepted = true;
            }
          }
          validPrefix &= accepted;
        }
        if (match->getMatchClauseType() == common::MatchClauseType::MATCH) {
          for (const auto& p : clause->getPredicate()->splitOnAND()) {
            if (p->expressionType != ET::EQUALS || p->getNumChildren() != 2)
              continue;
            for (size_t side = 0; side < 2; ++side) {
              const auto value = p->getChild(side), constant = p->getChild(1 - side);
              if (value->expressionType != ET::PROPERTY || !deps(constant).empty() || !deterministic(constant, constraints))
                continue;
              auto* property = value->constPtrCast<binder::PropertyExpression>();
              auto it = nodes.find(property->getVariableName());
              if (it == nodes.end() || it->second->getNumEntries() != 1)
                continue;
              const auto label = it->second->getEntries()[0]->getName();
              const bool unique = property->isPrimaryKey() ||
                  (uniqueProperties.contains(label) && uniqueProperties.at(label).contains(property->getPropertyName())) ||
                  (constraints.uniqueProperties.contains(label) && constraints.uniqueProperties.at(label).contains(property->getPropertyName()));
              if (unique)
                fixedNodes.insert(it->first);
            }
          }
        }
      }
    }
    const auto outputs = body->getProjectionExpressions();
    for (const auto& e : outputs) {
      validPrefix &= deterministic(e, constraints);
      totalGeneralization &= totalForGeneralization(e);
    }
    if (!deferred.empty() && !totalGeneralization)
      validPrefix = false;
    if (!validPrefix)
      break;
    Names groupDependencies;
    for (const auto& g : body->getGroupByExpressions()) {
      auto values = deps(g);
      groupDependencies.insert(values.begin(), values.end());
    }
    for (const auto& f : fixedNodes)
      groupDependencies.erase(f);
    // A global aggregate returns one row even for a missing parameter value.
    // The grouped materialization would have no row, so it needs a separate
    // empty-group finalizer and is deliberately outside this extension.
    if (!deferred.empty() && body->getGroupByExpressions().empty())
      break;
    // A node (or its single-label primary key) must explicitly be a grouping
    // key. Merely depending on a node does not prove injectivity of an expression.
    Names explicitGroupNodes;
    for (const auto& g : body->getGroupByExpressions()) {
      if (g->expressionType == ET::PATTERN && nodes.contains(g->getUniqueName()))
        explicitGroupNodes.insert(g->getUniqueName());
      if (g->expressionType == ET::PROPERTY) {
        auto* p = g->constPtrCast<binder::PropertyExpression>();
        if (nodes.contains(p->getVariableName()) && nodes.at(p->getVariableName())->getNumEntries() == 1) {
          const auto label = nodes.at(p->getVariableName())->getEntries()[0]->getName();
          const auto property = p->getPropertyName();
          const bool unique = (uniqueProperties.contains(label) && uniqueProperties.at(label).contains(property)) ||
                              (constraints.uniqueProperties.contains(label) && constraints.uniqueProperties.at(label).contains(property));
          const bool nonNull = constraints.nonNullProperties.contains(label) && constraints.nonNullProperties.at(label).contains(property);
          if (p->isPrimaryKey() || (unique && nonNull))
            explicitGroupNodes.insert(p->getVariableName());
        }
      }
    }
    Names key;
    if (body->hasAggregateExpressions()) {
      key = groupDependencies;
      key.insert(deferredNodes.begin(), deferredNodes.end());
      explicitGroupNodes.insert(deferredNodes.begin(), deferredNodes.end());
      // A fixed vertex is unnecessary for uniqueness, but an explicitly
      // returned graph object must remain recoverable through a typed key.
      for (const auto& e : outputs)
        if (e->expressionType == ET::PATTERN && fixedNodes.contains(e->getUniqueName()))
          key.insert(e->getUniqueName());
      uniqueInput = !key.empty() && key.size() <= 2 && subset(key, explicitGroupNodes) &&
                    subset(key, liveNodes) && subset(key, nonNullNodes);
    } else if (!expands && uniqueInput) {
      key = previousKey;
      uniqueInput = subset(key, liveNodes);
    } else {
      uniqueInput = false;
    }
    if (body->isDistinct() && !body->hasAggregateExpressions()) {
      Names retainedKeys;
      for (const auto& e : outputs) {
        if (e->expressionType == ET::PATTERN)
          retainedKeys.insert(e->getUniqueName());
        if (e->expressionType == ET::PROPERTY) {
          const auto* property = e->constPtrCast<binder::PropertyExpression>();
          if (property->isPrimaryKey() && nodes.contains(property->getVariableName()) &&
              nodes.at(property->getVariableName())->getNumEntries() == 1)
            retainedKeys.insert(property->getVariableName());
        }
      }
      // Appending a dropped key to DISTINCT would split equivalence classes.
      uniqueInput &= subset(key, retainedKeys);
    }
    const auto* parsedBody = pos < query->getNumQueryParts()
        ? query->getQueryPart(pos)->getWithClause()->getProjectionBody()
        : query->getReturnClause()->getProjectionBody();
    const auto& original = parsedBody->getProjectionExpressions();
    const auto originalText = projectionText(cypher, ts, projections[pos]);
    // A grouped Top-K followed by DISTINCT on one graph key may use MAX of
    // its COUNT to prune keys safely. This is only a candidate-key prefilter:
    // replay the original groups and their original Top-K afterwards. Replacing
    // the original ranking by MAX directly would change which keys survive
    // when several groups of the same key occupy positions in the Top-K.
    if (pos == 0 && projections.size() > 1 && uniqueInput && key.size() == 2 &&
        !body->isDistinct() && !body->hasSkip() && body->hasLimit() &&
        body->getLimitNumber()->expressionType == ET::LITERAL &&
        body->getLimitNumber()->getDataType().getLogicalTypeID() == common::LogicalTypeID::INT64 &&
        body->getAggregateExpressions().size() == 1 &&
        body->getOrderByExpressions().size() >= 2 &&
        !body->getSortingOrders()[0] && !part->hasProjectionBodyPredicate() &&
        part->getNumReadingClause() == 1 &&
        originalText.expressions.size() == outputs.size()) {
      const auto* next = single->getQueryPart(1);
      const auto* nextBody = next->hasProjectionBody() ? next->getProjectionBody() : nullptr;
      const auto aggregate = body->getAggregateExpressions()[0];
      const auto* function = aggregate->constPtrCast<binder::AggregateFunctionExpression>();
      const auto limit = body->getLimitNumber()->constPtrCast<binder::LiteralExpression>()->getValue().getValue<int64_t>();
      const auto* match = part->getReadingClause(0)->getClauseType() == common::ClauseType::MATCH
          ? part->getReadingClause(0)->constPtrCast<binder::BoundMatchClause>() : nullptr;
      const auto orderKey = body->getOrderByExpressions()[1];
      if (limit > 0 && match && match->getMatchClauseType() == common::MatchClauseType::MATCH &&
          nextBody && nextBody->isDistinct() && !next->hasReadingClause() &&
          !next->hasUpdatingClause() && !next->hasProjectionBodyPredicate() &&
          !nextBody->hasAggregateExpressions() && !nextBody->hasSkipOrLimit() &&
          !nextBody->hasOrderByExpressions() && nextBody->getProjectionExpressions().size() == 1 &&
          nextBody->getProjectionExpressions()[0]->expressionType == ET::PATTERN &&
          (upper(function->getFunction().name) == "COUNT" || upper(function->getFunction().name) == "COUNT_STAR") &&
          !function->isDistinct() && aggregate->getDataType().getLogicalTypeID() == common::LogicalTypeID::INT64 &&
          body->getOrderByExpressions()[0]->getUniqueName() == aggregate->getUniqueName() &&
          orderKey->expressionType == ET::PROPERTY) {
        const auto retained = nextBody->getProjectionExpressions()[0]->getUniqueName();
        const auto* property = orderKey->constPtrCast<binder::PropertyExpression>();
        bool safe = key.contains(retained) && nodes.contains(retained) &&
                    nodes.at(retained)->getNumEntries() == 1 && property->isPrimaryKey() &&
                    property->getVariableName() == retained;
        if (match->hasPredicate()) {
          // The summary removes literal filters as well as parameter filters.
          // Evaluating a partial COUNT argument on an excluded key can fail
          // even though the original filtered query succeeds.
          safe &= totalGeneralization;
          for (const auto& predicate : match->getPredicate()->splitOnAND())
            safe &= subset(deps(predicate), Names{retained}) &&
                    (totalForGeneralization(predicate) ||
                     (!deterministic(predicate, constraints) && !deferred.empty()));
        }
        for (const auto& predicate : deferred)
          safe &= predicate.binding == retained;
        auto sourcePrefix = cypher.substr(0, ts[projections[pos]].begin);
        std::string filter;
        if (match->hasPredicate()) {
          const auto prefixTokens = tokens(sourcePrefix);
          const auto where = std::find_if(prefixTokens.begin(), prefixTokens.end(), [](const auto& token) {
            return token.depth == 0 && upper(token.text) == "WHERE";
          });
          if (where == prefixTokens.end()) {
            safe = false;  // Do not rewrite property-map spellings implicitly.
          } else {
            filter = sourcePrefix.substr(where->begin);
            sourcePrefix.resize(where->begin);
          }
        }
        const auto aggregateOutput = std::find_if(outputs.begin(), outputs.end(), [&](const auto& value) {
          return value->getUniqueName() == aggregate->getUniqueName();
        });
        safe &= aggregateOutput != outputs.end();
        if (safe && (!limits.maxGenerated || result.generated < limits.maxGenerated)) {
          ++result.generated;
          const auto retainedAlias = identifier(liveNames.at(retained));
          const auto maximumAlias = identifier(temporary + "rank_max");
          const auto storedKey = identifier(temporary + "rank_key");
          std::vector<std::string> projection;
          for (size_t i = 0; i < outputs.size(); ++i)
            projection.push_back(originalText.expressions[i] + "\n AS " +
                                 identifier(temporary + "rank_out_" + std::to_string(i)));
          projection.push_back(retainedAlias + " AS " + storedKey);
          Candidate candidate;
          candidate.kind = CandidateKind::Computation;
          candidate.keys = {"key_0"};
          candidate.nodes = {{"key_0", {nodes.at(retained)->getEntries()[0]->getName()}}};
          candidate.columns = {{ColumnKind::Projection, Expression::variable(temporary + "rank_max", "INT64"), "INT64"}};
          candidate.sourceQuery = sourcePrefix + "WITH " + join(projection) +
              "\nRETURN " + storedKey + " AS key_0, MAX(" +
              identifier(temporary + "rank_out_" + std::to_string(std::distance(outputs.begin(), aggregateOutput))) + ") AS value_0";
          candidate.logicalDefinitionKey = "GROUP_TOPK_BOUND:" + fingerprint(candidate.sourceQuery) + ":INT64";
          std::set<std::string> seenTargets;
          for (size_t store = 0; store < carriers.size(); ++store) {
            const auto& carrier = carriers[store];
            if (carrier.kind != CarrierKind::VertexProperties || !carrier.coversAllKeys ||
                !carrier.uniqueKey || !carrier.preservesNull || !carrier.preservesGroupPresence ||
                !includes(carrier.sourceLabels, candidate.nodes[0].labels) ||
                (!carrier.scalarTypes.empty() && !carrier.scalarTypes.contains("INT64")) ||
                !seenTargets.insert(carrier.structuralKey()).second)
              continue;
            if (limits.maxCombinations && result.combinations >= limits.maxCombinations) {
              result.truncated = true;
              break;
            }
            ++result.combinations;
            auto physical = candidate;
            physical.storage = carrier;
            physical.definitionKey = physical.logicalDefinitionKey + ":TARGET:" + carrier.structuralKey();
            Usage use;
            use.source = source + "/0/0";
            use.scope = "prefix";
            use.replacedThroughPart = 0;
            use.definitions = {physical.definitionKey};
            IndexRead read;
            read.carriers = {store};
            read.bindings[retained] = "key_0";
            read.columns[temporary + "rank_max"] = 0;
            use.reads.push_back(std::move(read));
            use.residualQuery = "WITH key_0 AS " + retainedAlias + ", value_0 AS " + maximumAlias +
                "\n" + filter + "\nWITH " + retainedAlias + ", " + maximumAlias +
                "\nORDER BY " + maximumAlias + " DESC, " + retainedAlias + "." +
                identifier(property->getPropertyName()) + (body->getSortingOrders()[1] ? " ASC" : " DESC") +
                "\nLIMIT " + std::to_string(limit) + "\nWITH " + retainedAlias + "\n" + cypher;
            result.uses.push_back(std::move(use));
            output.try_emplace(physical.definitionKey, std::move(physical));
          }
        }
      }
    }
    // A global SUM of earlier COUNT values can be partitioned by any live,
    // non-NULL graph identity. Preserve the entire input computation verbatim:
    // this does not flatten MATCH boundaries, omit internal edges, or assume
    // that relationships are unique by endpoint. On representable results,
    // nonnegative partial sums cannot overflow before the original total does.
    // The final SUM also preserves the original empty-input value and type.
    bool partitionedCountSum = pos + 1 == projections.size() && pos > 0 &&
        upper(ts[projections[pos]].text) == "RETURN" &&
        body->getGroupByExpressions().empty() && !body->isDistinct() &&
        !part->hasProjectionBodyPredicate() && !body->hasOrderByExpressions() &&
        !body->hasSkip() && !body->hasLimit() && deferred.empty() &&
        !outputs.empty() && outputs.size() <= limits.columns &&
        original.size() == outputs.size() &&
        originalText.expressions.size() == outputs.size();
    for (const auto& value : outputs) {
      if (value->expressionType != ET::AGGREGATE_FUNCTION ||
          value->getDataType().getLogicalTypeID() != common::LogicalTypeID::INT64) {
        partitionedCountSum = false;
        continue;
      }
      const auto* aggregate = value->constPtrCast<binder::AggregateFunctionExpression>();
      partitionedCountSum &= upper(aggregate->getFunction().name) == "SUM" &&
          !aggregate->isDistinct() && value->getNumChildren() == 1 &&
          countMeasures.contains(value->getChild(0)->getUniqueName());
    }
    if (partitionedCountSum) {
      std::vector<std::string> eligible;
      for (const auto& name : liveNodes)
        if (nonNullNodes.contains(name) && nodes.at(name)->getNumEntries() == 1)
          eligible.push_back(name);
      std::vector<std::vector<std::string>> partitions;
      for (size_t i = 0; i < eligible.size(); ++i) {
        partitions.push_back({eligible[i]});
        for (size_t j = i + 1; j < eligible.size(); ++j)
          partitions.push_back({eligible[i], eligible[j]});
      }
      for (const auto& partition : partitions) {
        if (limits.maxGenerated && result.generated >= limits.maxGenerated) {
          result.truncated = true;
          break;
        }
        ++result.generated;
        Candidate candidate;
        candidate.kind = CandidateKind::Computation;
        std::vector<std::string> projection, returns, finalizers;
        for (size_t i = 0; i < partition.size(); ++i) {
          Node node;
          node.binding = "key_" + std::to_string(i);
          node.labels = {nodes.at(partition[i])->getSingleEntry()->getName()};
          candidate.keys.push_back(node.binding);
          candidate.nodes.push_back(node);
          projection.push_back(identifier(liveNames.at(partition[i])) +
                               " AS " + identifier(temporary + node.binding));
          returns.push_back(identifier(temporary + node.binding) + " AS " + node.binding);
        }
        for (size_t i = 0; i < outputs.size(); ++i) {
          const auto column = "value_" + std::to_string(i);
          const auto alias = projectionName(original[i]);
          projection.push_back(originalText.expressions[i] + "\n AS " + identifier(temporary + column));
          returns.push_back(identifier(temporary + column) + " AS " + column);
          finalizers.push_back("SUM(" + identifier(column) + ") AS " + identifier(alias));
          candidate.columns.push_back({ColumnKind::Projection,
              Expression::variable(alias, "INT64"), "INT64"});
        }
        candidate.sourceQuery = cypher.substr(0, ts[projections[pos]].begin) +
            "WITH " + join(projection) + "\nRETURN " + join(returns);
        candidate.logicalDefinitionKey = "COUNT_SUM_PARTITION:" + fingerprint(candidate.sourceQuery);
        std::set<std::string> seenTargets;
        for (size_t store = 0; store < carriers.size(); ++store) {
          const auto& carrier = carriers[store];
          if (!carrier.uniqueKey || !carrier.coversAllKeys || !carrier.preservesNull ||
              !carrier.preservesGroupPresence ||
              !includes(carrier.sourceLabels, candidate.nodes[0].labels) ||
              (!carrier.scalarTypes.empty() && !carrier.scalarTypes.contains("INT64")))
            continue;
          if ((partition.size() == 1 && carrier.kind != CarrierKind::VertexProperties) ||
              (partition.size() == 2 && (carrier.kind != CarrierKind::DerivedEdge ||
               !includes(carrier.destinationLabels, candidate.nodes[1].labels))))
            continue;
          if (!seenTargets.insert(carrier.structuralKey()).second) continue;
          if (limits.maxCombinations && result.combinations >= limits.maxCombinations) {
            result.truncated = true;
            break;
          }
          ++result.combinations;
          auto physical = candidate;
          physical.storage = carrier;
          physical.definitionKey = physical.logicalDefinitionKey + ":TARGET:" + carrier.structuralKey();
          Usage use;
          use.source = source + "/0/" + std::to_string(pos);
          use.scope = "prefix";
          use.replacedThroughPart = pos;
          use.definitions = {physical.definitionKey};
          IndexRead read;
          read.carriers = {store};
          for (size_t i = 0; i < partition.size(); ++i)
            read.bindings[partition[i]] = physical.keys[i];
          for (size_t i = 0; i < outputs.size(); ++i)
            read.columns[outputs[i]->getUniqueName()] = i;
          use.reads.push_back(std::move(read));
          use.residualQuery = "RETURN " + join(finalizers) + "\n" + cypher.substr(originalText.tail);
          result.uses.push_back(std::move(use));
          output.try_emplace(physical.definitionKey, std::move(physical));
        }
      }
    }
    const bool recoverable = std::all_of(outputs.begin(), outputs.end(), [&](const E& e) {
      return scalar(e) || (e->expressionType == ET::PATTERN && key.contains(e->getUniqueName()));
    });
    Names outputNames;
    for (const auto& e : outputs)
      outputNames.insert(e->getUniqueName());
    std::function<bool(const E&)> tailReadable = [&](const E& e) {
      if (outputNames.contains(e->getUniqueName()) || key.contains(e->getUniqueName()))
        return true;
      if (e->expressionType == ET::PROPERTY)
        return key.contains(e->constPtrCast<binder::PropertyExpression>()->getVariableName());
      if (e->expressionType == ET::AGGREGATE_FUNCTION || e->expressionType == ET::SUBQUERY ||
          e->expressionType == ET::PATTERN || e->expressionType == ET::VARIABLE)
        return false;
      const auto cs = children(e);
      return std::all_of(cs.begin(), cs.end(), tailReadable);
    };
    bool readableTail = !part->hasProjectionBodyPredicate() || tailReadable(part->getProjectionBodyPredicate());
    const auto& orderExpressions = body->getOrderByExpressions();
    for (size_t i = 0; i < orderExpressions.size(); ++i) {
      const auto& e = orderExpressions[i];
      // A bound alias retains its aggregate expression tree. For example,
      // ORDER BY score + friendsScore is ordinary arithmetic over outputs,
      // even though friendsScore's tree contains SUM. Only explicit aggregate
      // syntax needs special restoration once its input binding is dropped.
      bool explicitAggregate = false;
      if (i < parsedBody->getOrderByExpressions().size()) {
        const auto orderTokens = tokens(parsedBody->getOrderByExpressions()[i]->getRawName());
        static const Names aggregateNames = {"COUNT", "SUM", "AVG", "MIN", "MAX", "COLLECT"};
        for (size_t j = 0; j + 1 < orderTokens.size(); ++j)
          explicitAggregate |= aggregateNames.contains(upper(orderTokens[j].text)) && orderTokens[j + 1].text == "(";
      }
      readableTail &= tailReadable(e) && (!explicitAggregate || outputNames.contains(e->getUniqueName()));
    }
    if (uniqueInput && recoverable && readableTail && original.size() == outputs.size() &&
        originalText.expressions.size() == outputs.size()) {
      if (limits.maxGenerated && result.generated >= limits.maxGenerated) {
        result.truncated = true;
        break;
      }
      ++result.generated;
      Candidate candidate;
      candidate.kind = CandidateKind::Computation;
      std::vector<std::string> projection, returns, restored;
      std::vector<size_t> valuePositions;
      for (size_t i = 0; i < outputs.size(); ++i) {
        // A source expression may end in a // comment at EOF. Put generated
        // syntax on a fresh line so it cannot be swallowed by that comment.
        projection.push_back(originalText.expressions[i] + "\n AS " + identifier(temporary + "out_" + std::to_string(i)));
        bool isKey = outputs[i]->expressionType == ET::PROPERTY &&
            key.contains(outputs[i]->constPtrCast<binder::PropertyExpression>()->getVariableName());
        if (scalar(outputs[i]) && !isKey)
          valuePositions.push_back(i);
      }
      size_t k = 0;
      for (const auto& name : key) {
        const auto* n = nodes.at(name);
        Node node;
        node.binding = "key_" + std::to_string(k);
        for (auto* entry : n->getEntries())
          node.labels.push_back(entry->getName());
        candidate.keys.push_back(node.binding);
        candidate.nodes.push_back(node);
        projection.push_back(identifier(liveNames.at(name)) + " AS " + identifier(temporary + "key_" + std::to_string(k)));
        returns.push_back(identifier(temporary + "key_" + std::to_string(k)) + " AS " + node.binding);
        ++k;
      }
      for (auto i : valuePositions) {
        const auto type = outputs[i]->getDataType().toString();
        candidate.columns.push_back({ColumnKind::Projection, Expression::variable(projectionName(original[i]), type), type});
        returns.push_back(identifier(temporary + "out_" + std::to_string(i)) + " AS value_" + std::to_string(candidate.columns.size() - 1));
      }
      std::vector<std::string> transportBindings, resumeBindings;
      k = 0;
      for (const auto& name : key) {
        const auto binding = temporary + "key_" + std::to_string(k);
        transportBindings.push_back("key_" + std::to_string(k++) + " AS " + identifier(binding));
        resumeBindings.push_back(identifier(binding));
        resumeBindings.push_back(identifier(binding) + " AS " + identifier(liveNames.at(name)));
      }
      for (size_t i = 0; i < valuePositions.size(); ++i) {
        const auto binding = temporary + "value_" + std::to_string(i);
        transportBindings.push_back("value_" + std::to_string(i) + " AS " + identifier(binding));
        resumeBindings.push_back(identifier(binding));
      }
      for (size_t i = 0; i < outputs.size(); ++i) {
        std::string value;
        auto column = std::find(valuePositions.begin(), valuePositions.end(), i);
        if (column != valuePositions.end()) {
          value = identifier(temporary + "value_" + std::to_string(column - valuePositions.begin()));
        } else {
          std::string nodeName;
          if (outputs[i]->expressionType == ET::PATTERN)
            nodeName = outputs[i]->getUniqueName();
          else
            nodeName = outputs[i]->constPtrCast<binder::PropertyExpression>()->getVariableName();
          const auto keyNo = std::distance(key.begin(), key.find(nodeName));
          value = identifier(temporary + "key_" + std::to_string(keyNo));
          if (outputs[i]->expressionType == ET::PROPERTY)
            value += "." + identifier(outputs[i]->constPtrCast<binder::PropertyExpression>()->getPropertyName());
        }
        const auto alias = projectionName(original[i]);
        restored.push_back(value + " AS " + identifier(alias));
      }
      // Leave the boundary's WHERE/ORDER/SKIP/LIMIT online, exactly before the
      // remaining original clauses. Earlier boundaries remain in sourceQuery.
      const size_t tail = originalText.tail;
      std::string residualTail = cypher.substr(tail);
      if (parsedBody->hasOrderByExpressions()) {
        const auto& parsedOrder = parsedBody->getOrderByExpressions();
        const auto& boundOrder = body->getOrderByExpressions();
        for (size_t i = projections[pos] + 1; i + 1 < ts.size(); ++i) {
          if (ts[i].depth || upper(ts[i].text) != "ORDER" || upper(ts[i + 1].text) != "BY")
            continue;
          size_t end = cypher.size();
          std::vector<std::pair<size_t, size_t>> intervals;
          size_t item = i + 2;
          int cases = 0;
          for (size_t j = i + 2; j < ts.size(); ++j) {
            if (ts[j].depth) continue;
            if (upper(ts[j].text) == "CASE") ++cases;
            if (upper(ts[j].text) == "END") --cases;
            if (cases) continue;
            if (ts[j].text == ",") {
              intervals.emplace_back(item, j);
              item = j + 1;
            } else if ((clauseBoundaries.contains(upper(ts[j].text)) || ts[j].text == ";") &&
                upper(ts[j - 1].text) != "AS" && ts[j - 1].text != "." &&
                !(upper(ts[j].text) == "WITH" && (upper(ts[j - 1].text) == "STARTS" || upper(ts[j - 1].text) == "ENDS"))) {
              end = ts[j].begin;
              intervals.emplace_back(item, j);
              break;
            }
          }
          if (end == cypher.size()) intervals.emplace_back(item, ts.size());
          if (intervals.size() != parsedOrder.size()) {
            readableTail = false;
            break;
          }
          std::vector<std::string> order;
          for (size_t index = 0; index < intervals.size(); ++index) {
            const auto [first, last] = intervals[index];
            const auto finish = last == ts.size() ? cypher.size() : ts[last].begin;
            std::string expression = cypher.substr(ts[first].begin, finish - ts[first].begin);
            if (index < boundOrder.size())
              for (size_t j = 0; j < outputs.size(); ++j)
                if (outputs[j]->getUniqueName() == boundOrder[index]->getUniqueName()) {
                  expression = identifier(projectionName(original[j])) +
                      (parsedBody->getSortOrders()[index] ? " ASC" : " DESC");
                  break;
                }
            order.push_back(expression);
          }
          residualTail.replace(ts[i].begin - tail, end - ts[i].begin, "ORDER BY " + join(order) + "\n");
          break;
        }
      }
      auto sourcePrefix = cypher.substr(0, ts[projections[pos]].begin);
      if (!deferred.empty()) {
        const auto generalized = removeDeferredPredicates(sourcePrefix, deferred);
        if (!generalized) {
          ++result.rejections["unsupported_parameter_filter_spelling"];
          break;
        }
        sourcePrefix = *generalized;
      }
      candidate.sourceQuery = sourcePrefix +
          "WITH " + (parsedBody->getIsDistinct() ? std::string("DISTINCT ") : std::string{}) +
          join(projection) + "\nRETURN " + join(returns);
      candidate.logicalDefinitionKey = "PREFIX:" + fingerprint(candidate.sourceQuery);
      for (const auto& c : candidate.columns)
        candidate.logicalDefinitionKey += ":" + c.type;
      if (readableTail && !candidate.columns.empty() && candidate.columns.size() <= limits.columns) {
        std::set<std::string> seenTargets;
        std::vector<size_t> stores;
        for (size_t store = 0; store < carriers.size(); ++store) stores.push_back(store);
        auto capabilityKey = [&](size_t store) {
          const auto& carrier = carriers[store];
          std::string value = carrier.structuralKey() + std::to_string(carrier.coversAllKeys) +
              std::to_string(carrier.uniqueKey) + std::to_string(carrier.preservesNull) +
              std::to_string(carrier.preservesGroupPresence) + std::to_string(carrier.resolvesDestinationId);
          for (const auto& type : carrier.scalarTypes) value += std::to_string(type.size()) + ":" + type;
          return value;
        };
        std::sort(stores.begin(), stores.end(), [&](size_t a, size_t b) { return capabilityKey(a) < capabilityKey(b); });
        for (const auto store : stores) {
          const auto& carrier = carriers[store];
          bool covers = carrier.coversAllKeys;
          if (!covers && key.size() == 2 && carrier.kind == CarrierKind::ExistingEdgeProperties) {
            const auto src = *key.begin(), dst = *std::next(key.begin());
            for (const auto& domain : edgeDomains)
              covers |= domain.source == src && domain.destination == dst && !domain.types.empty() &&
                        std::all_of(domain.types.begin(), domain.types.end(), [&](const auto& type) { return type == carrier.relationshipType; });
          }
          if (!carrier.uniqueKey || !covers || !carrier.preservesNull || !carrier.preservesGroupPresence ||
              !includes(carrier.sourceLabels, candidate.nodes[0].labels))
            continue;
          if ((key.size() == 1 && carrier.kind != CarrierKind::VertexProperties) ||
              (key.size() == 2 && (carrier.kind == CarrierKind::VertexProperties || !includes(carrier.destinationLabels, candidate.nodes[1].labels))))
            continue;
          if (std::any_of(candidate.columns.begin(), candidate.columns.end(), [&](const auto& c) { return !carrier.scalarTypes.empty() && !carrier.scalarTypes.contains(c.type); }))
            continue;
          if (!seenTargets.insert(carrier.structuralKey()).second)
            continue;
          if (limits.maxCombinations && result.combinations >= limits.maxCombinations) {
            result.truncated = true;
            break;
          }
          ++result.combinations;
          auto physical = candidate;
          physical.storage = carrier;
          for (auto* labels : {&physical.storage->sourceLabels, &physical.storage->destinationLabels}) {
            std::sort(labels->begin(), labels->end());
            labels->erase(std::unique(labels->begin(), labels->end()), labels->end());
          }
          physical.definitionKey = physical.logicalDefinitionKey + ":TARGET:" + carrier.structuralKey();
          Usage use;
          use.source = source + "/0/" + std::to_string(pos);
          use.scope = "prefix";
          use.replacedThroughPart = pos;
          use.definitions.push_back(physical.definitionKey);
          IndexRead read;
          read.carriers = {store};
          k = 0;
          for (const auto& name : key)
            read.bindings[name] = physical.keys[k++];
          for (size_t i = 0; i < physical.columns.size(); ++i)
            read.columns[outputs[valuePositions[i]]->getUniqueName()] = i;
          use.reads.push_back(std::move(read));
          std::string parameterFilter;
          for (const auto& predicate : deferred)
            parameterFilter += (parameterFilter.empty() ? "WHERE " : " AND ") +
                identifier(predicate.alias) + "." + identifier(predicate.property) + " " + predicate.comparison + " $" + predicate.parameter;
          if (!parameterFilter.empty()) parameterFilter += "\n";
          use.residualQuery = "WITH " + join(transportBindings) + "\nWITH " + join(resumeBindings) + "\n" + parameterFilter +
              (upper(ts[projections[pos]].text) == "RETURN" ? "RETURN " : "WITH ") +
              (parsedBody->getIsDistinct() ? "DISTINCT " : "") +
              join(restored) + "\n" + residualTail;
          result.uses.push_back(std::move(use));
          output.try_emplace(physical.definitionKey, std::move(physical));
        }
      }
    }
    if (!deferred.empty())
      break;
    Names nextCountMeasures;
    for (const auto& value : outputs) {
      if (countMeasures.contains(value->getUniqueName()))
        nextCountMeasures.insert(value->getUniqueName());
      if (value->expressionType != ET::AGGREGATE_FUNCTION ||
          value->getDataType().getLogicalTypeID() != common::LogicalTypeID::INT64)
        continue;
      const auto name = upper(value->constPtrCast<binder::AggregateFunctionExpression>()->getFunction().name);
      if (name == "COUNT" || name == "COUNT_STAR")
        nextCountMeasures.insert(value->getUniqueName());
    }
    countMeasures = std::move(nextCountMeasures);
    // Aggregated values depend on the entire original grouping key. Preserve
    // that functional dependence through later projections, without inlining
    // their expressions into a different stage.
    if (body->hasAggregateExpressions())
      for (const auto& aggregate : body->getAggregateExpressions())
        dependencies[aggregate->getUniqueName()] = groupDependencies;
    for (const auto& e : outputs)
      dependencies[e->getUniqueName()] = deps(e);
    Names nextNodes;
    for (size_t i = 0; i < outputs.size(); ++i) {
      const auto& e = outputs[i];
      if (e->expressionType == ET::PATTERN && nodes.contains(e->getUniqueName())) {
        nextNodes.insert(e->getUniqueName());
        if (i < original.size())
          liveNames[e->getUniqueName()] = projectionName(original[i]);
      }
    }
    liveNodes = std::move(nextNodes);
    for (auto it = liveNames.begin(); it != liveNames.end();)
      if (!liveNodes.contains(it->first)) it = liveNames.erase(it);
      else ++it;
    previousKey = key;
    uniqueInput &= subset(previousKey, liveNodes);
    if (part->hasProjectionBodyPredicate())
      validPrefix &= deterministic(part->getProjectionBodyPredicate(), constraints);
    for (const auto& e : body->getOrderByExpressions())
      validPrefix &= deterministic(e, constraints);
    if (body->hasSkip()) validPrefix &= deterministic(body->getSkipNumber(), constraints);
    if (body->hasLimit()) validPrefix &= deterministic(body->getLimitNumber(), constraints);
  }
  for (auto& [key, candidate] : output)
    result.candidates.push_back(std::move(candidate));
  return result;
}
}  // namespace neug::index

#include "neug/compiler/binder/rewriter/computation_prefix_rewriter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <openssl/sha.h>

#include "glog/logging.h"
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/expression/variable_expression.h"
#include "neug/compiler/binder/expression_visitor.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/binder/query/reading_clause/bound_unwind_clause.h"
#include "neug/compiler/binder/query/updating_clause/bound_set_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/common/constants.h"
#include "neug/compiler/common/pattern_ir.h"
#include "neug/compiler/common/string_utils.h"
#include "neug/compiler/index/bound_index_candidate_adapter.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/parser/parser.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace neug::binder {
namespace {
using namespace common;
using namespace catalog;
using Expr = std::shared_ptr<Expression>;
using Names = std::unordered_map<std::string, std::string>;
using Replacements = std::unordered_map<std::string, Expr>;

std::string sha256(const std::string& text) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
  std::string result;
  for (const auto byte : digest) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

std::unique_ptr<BoundRegularQuery> bindSource(const std::string& text,
                                              main::ClientContext& context) {
  const auto parsed = parser::Parser::parseQuery(text);
  if (parsed.size() != 1 ||
      parsed.front()->getStatementType() != StatementType::QUERY)
    return nullptr;
  Binder binder(&context);
  binder.setAllowIndexRewrite(false);
  auto bound = binder.bind(*parsed.front());
  return std::unique_ptr<BoundRegularQuery>(
      static_cast<BoundRegularQuery*>(bound.release()));
}

bool allowedTarget(const FillIndexAuxInfo& info) {
  const auto* value = std::getenv("NEUG_INDEX_REWRITE_ALLOW_TARGETS");
  if (!value || !*value)
    return true;
  std::istringstream stream(value);
  for (std::string token; std::getline(stream, token, ',');) {
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
    if (token == info.getTableName())
      return true;
    for (const auto& column : info.getTargetColumns())
      if (token == info.getTableName() + "." + column)
        return true;
  }
  return false;
}

// Preserve every clause boundary. The union graph is only an isomorphism
// search aid; optionality and predicate placement are proved separately.
bool prefixGraph(const NormalizedSingleQuery& query, size_t count,
                 QueryGraph& graph) {
  for (size_t p = 0; p < count; ++p) {
    const auto& part = *query.getQueryPart(p);
    if (part.hasUpdatingClause() || !part.hasProjectionBody())
      return false;
    for (size_t c = 0; c < part.getNumReadingClause(); ++c) {
      const auto* reading = part.getReadingClause(c);
      if (reading->getClauseType() != ClauseType::MATCH)
        return false;
      const auto& match = reading->constCast<BoundMatchClause>();
      if (match.hasHint())
        return false;
      const auto* collection = match.getQueryGraphCollection();
      for (size_t g = 0; g < collection->getNumQueryGraphs(); ++g) {
        const auto* local = collection->getQueryGraph(g);
        for (const auto& node : local->getQueryNodes())
          if (!graph.containsQueryNode(node->getUniqueName()))
            graph.addQueryNode(node);
        for (const auto& rel : local->getQueryRels()) {
          if (!graph.containsQueryRel(rel->getUniqueName()))
            graph.addQueryRel(rel);
        }
      }
    }
  }
  return graph.getNumQueryRels() != 0;
}

index::Expression renamed(index::Expression value, const Names& names) {
  if (const auto found = names.find(value.binding); found != names.end())
    value.binding = found->second;
  for (auto& child : value.children)
    child = renamed(std::move(child), names);
  return value;
}
std::set<std::string> signatures(const std::vector<index::Expression>& values,
                                 const Names& names, bool conjunction = false) {
  std::set<std::string> result;
  std::function<void(const index::Expression&)> add = [&](const auto& e) {
    if (conjunction && e.kind == index::ExpressionKind::Call &&
        e.name == "AND") {
      for (const auto& child : e.children)
        add(child);
    } else
      result.insert(renamed(e, names).structuralKey());
  };
  for (const auto& e : values)
    add(e);
  return result;
}
bool stable(const index::Expression& expression) {
  // This replaces exactly the same closed input domain and ordered stages.
  // Partial deterministic operations are allowed: a READY FILL has already
  // evaluated that whole domain. No predicate/domain is moved across stages.
  static const std::set<std::string> aggregates = {"COUNT", "SUM", "AVG", "MIN",
                                                   "MAX"};
  return expression.deterministic &&
         (expression.kind != index::ExpressionKind::Aggregate ||
          aggregates.contains(expression.name)) &&
         expression.kind != index::ExpressionKind::Parameter &&
         expression.kind != index::ExpressionKind::ResidualPredicate &&
         std::all_of(expression.children.begin(), expression.children.end(),
                     stable);
}

// A single-label node identity and its non-null primary key partition exactly
// the same rows. Restrict this equivalence to the already proved carrier keys;
// an arbitrary property must not make two different source groups coalesce.
std::optional<std::string> carrierKeyIdentity(
    const index::Expression& expression,
    const std::vector<std::shared_ptr<NodeExpression>>& keys) {
  for (const auto& key : keys) {
    if (expression.binding != key->getUniqueName()) continue;
    if ((expression.kind == index::ExpressionKind::Variable &&
         expression.type.starts_with("NODE")) ||
        (expression.kind == index::ExpressionKind::Property &&
         expression.name == key->getPrimaryKey(key->getEntries()[0]->getTableID())
                                ->constCast<PropertyExpression>().getPropertyName()))
      return expression.binding;
  }
  return std::nullopt;
}

std::set<std::string> carrierGroupSignatures(
    const std::vector<index::Expression>& values, const Names& names,
    const std::vector<std::shared_ptr<NodeExpression>>& keys) {
  std::set<std::string> result;
  for (const auto& value : values) {
    const auto expression = renamed(value, names);
    const auto identity = carrierKeyIdentity(expression, keys);
    result.insert(identity ? "carrier-key:" + *identity
                           : "expression:" + expression.structuralKey());
  }
  return result;
}

bool sameClauses(const index::QueryPart& query, const index::QueryPart& source,
                 const Names& names) {
  if (query.clauses.size() != source.clauses.size())
    return false;
  for (size_t i = 0; i < source.clauses.size(); ++i) {
    const auto& q = query.clauses[i];
    const auto& s = source.clauses[i];
    if (q.optional != s.optional || signatures(q.predicates, names, true) !=
                                        signatures(s.predicates, {}, true))
      return false;
    std::set<std::string> qnodes, snodes, qrels, srels;
    for (auto n : q.nodes) {
      const auto found = names.find(query.nodes[n].binding);
      if (found == names.end())
        return false;
      qnodes.insert(found->second);
    }
    for (auto n : s.nodes)
      snodes.insert(source.nodes[n].binding);
    for (auto r : q.relationships) {
      const auto found = names.find(query.relationships[r].binding);
      if (found == names.end())
        return false;
      const auto& queryRel = query.relationships[r];
      const auto sourceRel = std::find_if(
          source.relationships.begin(), source.relationships.end(),
          [&](const auto& rel) { return rel.binding == found->second; });
      if (sourceRel == source.relationships.end() ||
          queryRel.recursive != sourceRel->recursive ||
          queryRel.directed != sourceRel->directed ||
          queryRel.lowerBound != sourceRel->lowerBound ||
          queryRel.upperBound != sourceRel->upperBound ||
          queryRel.pathSemantics != sourceRel->pathSemantics)
        return false;
      qrels.insert(found->second);
    }
    for (auto r : s.relationships)
      srels.insert(source.relationships[r].binding);
    if (qnodes != snodes || qrels != srels)
      return false;
  }
  return true;
}

// The physical value is keyed by graph identities. Require a complete group
// on precisely those keys, followed only by scalar projections. This avoids
// treating a stored scalar as a bag of source rows or changing empty groups.
bool uniqueKeys(const std::vector<index::QueryPart>& parts, size_t count,
                const std::vector<std::shared_ptr<NodeExpression>>& keys) {
  std::set<std::string> keyNames;
  for (const auto& key : keys)
    keyNames.insert(key->getUniqueName());
  std::set<std::string> mandatory;
  bool unique = false;
  for (size_t p = 0; p < count; ++p) {
    const auto& part = parts[p];
    if (!part.clauses.empty())
      unique = false;
    for (const auto& clause : part.clauses)
      if (!clause.optional)
        for (auto n : clause.nodes)
          mandatory.insert(part.nodes[n].binding);
    if (part.aggregates.empty() && !part.distinct)
      continue;
    const auto& groups = part.aggregates.empty() ? part.outputs : part.groupBy;
    std::set<std::string> identities;
    bool exact = true;
    for (const auto& expression : groups) {
      if (expression.kind == index::ExpressionKind::Literal)
        continue;
      const auto key =
          std::find_if(keys.begin(), keys.end(), [&](const auto& k) {
            return k->getUniqueName() == expression.binding;
          });
      if (key == keys.end()) {
        exact = false;
        break;
      }
      if (expression.kind == index::ExpressionKind::Variable &&
          expression.type.starts_with("NODE"))
        identities.insert(expression.binding);
      else if (expression.kind == index::ExpressionKind::Property &&
               (*key)->getPrimaryKey((*key)->getEntries()[0]->getTableID())
                       ->constCast<PropertyExpression>()
                       .getPropertyName() == expression.name)
        identities.insert(expression.binding);
      else {
        exact = false;
        break;
      }
    }
    unique = exact && identities == keyNames;
  }
  return unique && std::includes(mandatory.begin(), mandatory.end(),
                                 keyNames.begin(), keyNames.end());
}

// The caller has already proved a closed prefix, one source group per key,
// and preservation of every carrier key. A scan of one single-label node
// cannot repeat that identity; filtering it cannot introduce duplicates.
// Parallel relationship carriers have no such guarantee and keep DISTINCT.
bool uniqueNodeCarrierRead(const FillIndexAuxInfo& info, const QueryGraph& graph,
                           const std::vector<std::shared_ptr<NodeExpression>>& keys,
                           const expression_vector& outputs) {
  if (info.getFillType() != FillEntryType::NODE || keys.size() != 1 ||
      graph.getNumQueryNodes() != 1 || graph.getNumQueryRels() != 0) return false;
  const auto& node = graph.getQueryNode(0);
  if (node->getNumEntries() != 1 || node->getUniqueName() != keys[0]->getUniqueName()) return false;
  return std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
    if (output->getDataType().getLogicalTypeID() == LogicalTypeID::NODE &&
        output->getUniqueName() == node->getUniqueName()) return true;
    if (output->expressionType != ExpressionType::PROPERTY) return false;
    const auto& property = output->template constCast<PropertyExpression>();
    return property.getVariableName() == node->getUniqueName() && property.isSingleLabel() &&
        property.getSingleTableID() == node->getEntries()[0]->getTableID() && property.isPrimaryKey();
  });
}

// Only the canonical COUNT-to-SUM contract is repartitioned here. Its
// persisted signed-integer wrap contract makes addition associative, including
// overflow. An INT64 field alone is not evidence of that arithmetic contract.
bool countSumRollup(const NormalizedSingleQuery& source,
                    const std::vector<index::QueryPart>& parts, size_t count) {
  if (count < 2 || parts[count - 1].aggregates.empty() ||
      !parts[count - 1].postExpressions.empty())
    return false;
  std::set<std::string> nonnegative;
  for (size_t p = 0; p + 1 < count; ++p) {
    std::set<std::string> projected;
    const auto values =
        source.getQueryPart(p)->getProjectionBody()->getProjectionExpressions();
    for (size_t i = 0; i < parts[p].outputs.size(); ++i) {
      const auto& e = parts[p].outputs[i];
      if ((e.kind == index::ExpressionKind::Aggregate && e.name == "COUNT" &&
           e.type == "INT64") ||
          (e.kind == index::ExpressionKind::Variable &&
           nonnegative.contains(e.binding)))
        projected.insert(values[i]->getUniqueName());
    }
    nonnegative = std::move(projected);
  }
  for (const auto& e : parts[count - 1].aggregates)
    if (e.kind != index::ExpressionKind::Aggregate || e.name != "SUM" ||
        e.distinct || e.type != "INT64" || e.children.size() != 1 ||
        e.children[0].kind != index::ExpressionKind::Variable ||
        !nonnegative.contains(e.children[0].binding))
      return false;
  return true;
}

void substitute(Expr& expression, const Replacements& replacements) {
  if (!expression)
    return;
  if (const auto found = replacements.find(expression->getUniqueName());
      found != replacements.end()) {
    expression = found->second;
    return;
  }
  if (expression->expressionType == ExpressionType::CASE_ELSE) {
    auto& c = expression->cast<CaseExpression>();
    for (size_t i = 0; i < c.getNumCaseAlternatives(); ++i) {
      substitute(c.getCaseAlternative(i)->whenExpression, replacements);
      substitute(c.getCaseAlternative(i)->thenExpression, replacements);
    }
    auto otherwise = c.getElseExpression();
    substitute(otherwise, replacements);
    c.setElseExpression(std::move(otherwise));
  } else {
    for (size_t i = 0; i < expression->getNumChildren(); ++i) {
      auto child = expression->getChild(i);
      substitute(child, replacements);
      expression->setChild(i, std::move(child));
    }
  }
}
void copyIdentity(const Expr& from, const Expr& to) {
  to->setUniqueName(from->getUniqueName());
  to->setParsedName(from->getParsedName());
  if (from->hasAlias())
    to->setAlias(from->getAlias());
}
std::string quote(const std::string& text) {
  std::string result = "`";
  for (const auto c : text)
    result += c == '`' ? "``" : std::string(1, c);
  return result + '`';
}

bool rewriteOne(NormalizedSingleQuery& query, const FillIndexAuxInfo& info,
                const std::string& indexName, main::ClientContext& context,
                Binder& binder, size_t branch) {
  // Canonical registration adds maintenance state which is not part of the
  // saved source query's public projection. Never match it by column position.
  std::vector<std::string> valueColumns;
  for (const auto& column : info.getTargetColumns())
    if (!column.starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) &&
        !column.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX))
      valueColumns.push_back(column);
  auto bound = bindSource(info.getSourceQuery(), context);
  if (!bound || bound->getNumSingleQueries() != 1 ||
      !bound->getPreQueryPart().empty() || bound->getPostSingleQuery())
    return false;
  const auto& source = *bound->getSingleQuery(0);
  if (source.getNumQueryParts() < 2)
    return false;
  const auto count = source.getNumQueryParts() - 1;
  if (query.getNumQueryParts() < count)
    return false;
  const auto* wrapper = source.getQueryPart(count);
  if (!wrapper->hasProjectionBody())
    return false;
  const auto* wrapperBody = wrapper->getProjectionBody();
  if (wrapper->hasReadingClause() || wrapper->hasUpdatingClause() ||
      wrapper->hasProjectionBodyPredicate() ||
      wrapperBody->hasAggregateExpressions() || wrapperBody->isDistinct() ||
      wrapperBody->hasOrderByExpressions() || wrapperBody->hasSkipOrLimit())
    return false;
  QueryGraph sourceGraph, queryGraph;
  if (!prefixGraph(source, count, sourceGraph) ||
      !prefixGraph(query, count, queryGraph) ||
      sourceGraph.getNumQueryNodes() != queryGraph.getNumQueryNodes() ||
      sourceGraph.getNumQueryRels() != queryGraph.getNumQueryRels())
    return false;
  const auto keyCount = info.getFillType() == FillEntryType::NODE ? 1u : 2u;
  const auto finalValues = wrapperBody->getProjectionExpressions();
  if (finalValues.size() != keyCount + valueColumns.size())
    return false;
  std::vector<std::shared_ptr<NodeExpression>> sourceKeys;
  for (size_t i = 0; i < keyCount; ++i) {
    if (finalValues[i]->expressionType != ExpressionType::PROPERTY)
      return false;
    const auto& property = finalValues[i]->constCast<PropertyExpression>();
    if (!property.isPrimaryKey() ||
        !sourceGraph.containsQueryNode(property.getVariableName()))
      return false;
    auto key = sourceGraph.getQueryNode(property.getVariableName());
    if (key->getNumEntries() != 1)
      return false;
    sourceKeys.push_back(std::move(key));
  }
  if (keyCount == 2 &&
      sourceKeys[0]->getUniqueName() == sourceKeys[1]->getUniqueName())
    return false;
  index::Constraints expressionContract;
  expressionContract.deterministicFunctions = {
      "ROUND", "SQRT",      "FLOOR",       "CEIL",     "NULLIF",  "TO_DATE",
      "DATE",  "TIMESTAMP", "TO_INTERVAL", "INTERVAL", "DURATION"};
  const auto sourceParts =
      index::extractClosedQueryParts(source, expressionContract);
  const auto queryParts =
      index::extractClosedQueryParts(query, expressionContract);
  for (size_t p = count; p < query.getNumQueryParts(); ++p) {
    const auto* suffix = query.getQueryPart(p);
    if (!suffix->hasProjectionBody() || suffix->hasUpdatingClause())
      return false;
    for (size_t c = 0; c < suffix->getNumReadingClause(); ++c)
      if (suffix->getReadingClause(c)->getClauseType() != ClauseType::MATCH)
        return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const auto* body = source.getQueryPart(i)->getProjectionBody();
    if (!sourceParts[i].unsupportedReason.empty() ||
        !queryParts[i].unsupportedReason.empty() ||
        body->hasOrderByExpressions() || body->hasSkipOrLimit() ||
        !std::all_of(sourceParts[i].outputs.begin(),
                     sourceParts[i].outputs.end(), stable) ||
        !std::all_of(sourceParts[i].postExpressions.begin(),
                     sourceParts[i].postExpressions.end(), stable))
      return false;
    for (const auto& clause : sourceParts[i].clauses)
      if (!std::all_of(clause.predicates.begin(), clause.predicates.end(),
                       stable))
        return false;
  }
  if (!uniqueKeys(sourceParts, count, sourceKeys))
    return false;
  std::optional<size_t> presence;
  for (size_t i = keyCount; i < finalValues.size(); ++i) {
    if (finalValues[i]->expressionType != ExpressionType::LITERAL)
      continue;
    const auto& literal = finalValues[i]->constCast<LiteralExpression>();
    if (literal.getDataType().getLogicalTypeID() == LogicalTypeID::BOOL &&
        !literal.isNull() && literal.getValue().getValue<bool>())
      presence = i - keyCount;
  }
  bool derived = keyCount == 2;
  for (const auto& rel : sourceGraph.getQueryRels())
    for (auto* table : rel->getEntries())
      if (table->getName() == info.getTableName() ||
          table->getLabel(context.getCatalog(), context.getTransaction()) ==
              info.getTableName())
        derived = false;
  // Existing carriers contain unrelated rows. A separate presence field is
  // required even when the materialized value can legitimately be NULL/zero.
  if (!derived && !presence)
    return false;
  auto sourceIR = convertGraphPatternToIR(sourceGraph);
  auto queryIR = convertGraphPatternToIR(queryGraph);
  Names finalNames;
  bool rollup = false;
  const bool canRollup =
      info.getAggregateNumericContract() ==
          FillAggregateNumericContract::SIGNED_INTEGER_WRAP &&
      countSumRollup(source, sourceParts, count);
  std::vector<size_t> outputColumns;
  const auto [found, ignored] =
      PatternIR::findMatch(queryIR, sourceIR, indexName, [&]() {
        Names names;
        for (const auto& node : sourceIR.nodesInfo) {
          if (node.matchedIdx >= queryIR.nodesInfo.size() ||
              !PatternIR::labelsEqual(
                  node.nodeLabels,
                  queryIR.nodesInfo[node.matchedIdx].nodeLabels))
            return false;
          names.emplace(queryIR.nodesInfo[node.matchedIdx].uniqName,
                        node.uniqName);
        }
        for (const auto& rel : sourceIR.relsInfo) {
          if (rel.matchedIdx >= queryIR.relsInfo.size() ||
              queryIR.relsInfo[rel.matchedIdx].edgeType != rel.edgeType ||
              !PatternIR::labelsEqual(
                  rel.relLabels, queryIR.relsInfo[rel.matchedIdx].relLabels))
            return false;
          names.emplace(queryIR.relsInfo[rel.matchedIdx].uniqName,
                        rel.uniqName);
        }
        outputColumns.clear();
        rollup = false;
        for (size_t p = 0; p < count; ++p) {
          const auto& q = queryParts[p];
          const auto& s = sourceParts[p];
          const auto* qb = query.getQueryPart(p)->getProjectionBody();
          const auto* sb = source.getQueryPart(p)->getProjectionBody();
          const auto queryGroups = p + 1 == count
              ? carrierGroupSignatures(q.groupBy, names, sourceKeys)
              : signatures(q.groupBy, names);
          const auto sourceGroups = p + 1 == count
              ? carrierGroupSignatures(s.groupBy, {}, sourceKeys)
              : signatures(s.groupBy, {});
          if (queryGroups != sourceGroups) {
            if (p + 1 != count || !canRollup ||
                !std::includes(sourceGroups.begin(), sourceGroups.end(),
                               queryGroups.begin(), queryGroups.end()))
              return false;
            rollup = true;
          }
          if (!sameClauses(q, s, names) || q.distinct != s.distinct ||
              signatures(q.aggregates, names) != signatures(s.aggregates, {}) ||
              signatures(q.postExpressions, names) !=
                  signatures(s.postExpressions, {}) ||
              qb->hasOrderByExpressions() != sb->hasOrderByExpressions() ||
              qb->hasSkipOrLimit() != sb->hasSkipOrLimit())
            return false;
          if (p + 1 < count &&
              (q.outputs.size() != s.outputs.size() ||
               signatures(q.outputs, names) != signatures(s.outputs, {})))
            return false;
          const auto qo = qb->getProjectionExpressions();
          const auto so = sb->getProjectionExpressions();
          std::vector<size_t> positions;
          for (size_t i = 0; i < q.outputs.size(); ++i) {
            const auto queryOutput = renamed(q.outputs[i], names);
            const auto signature = queryOutput.structuralKey();
            const auto keyIdentity = p + 1 == count
                ? carrierKeyIdentity(queryOutput, sourceKeys) : std::nullopt;
            size_t pos = 0;
            for (; pos < s.outputs.size(); ++pos)
              if (s.outputs[pos].structuralKey() == signature ||
                  (keyIdentity &&
                   carrierKeyIdentity(s.outputs[pos], sourceKeys) == keyIdentity))
                break;
            if (pos == s.outputs.size())
              return false;
            positions.push_back(pos);
            if (p + 1 == count) {
              size_t column = finalValues.size();
              for (size_t j = keyCount; j < finalValues.size(); ++j)
                if (finalValues[j]->getUniqueName() ==
                    so[pos]->getUniqueName()) {
                  column = j;
                  break;
                }
              if (column == finalValues.size()) {
                // Graph keys and their properties can be read directly from the
                // carrier endpoints; other unmaterialized values cannot
                // survive.
                const auto& e = s.outputs[pos];
                if (std::none_of(
                        sourceKeys.begin(), sourceKeys.end(),
                        [&](const auto& key) {
                          return e.binding == key->getUniqueName() &&
                                 (e.kind == index::ExpressionKind::Variable ||
                                  e.kind == index::ExpressionKind::Property);
                        }))
                  return false;
              }
              if (rollup && column != finalValues.size() &&
                  (q.outputs[i].kind != index::ExpressionKind::Aggregate ||
                   q.outputs[i].name != "SUM"))
                return false;
              outputColumns.push_back(column);
            }
          }
          for (size_t i = 0; i < qo.size(); ++i) {
            // The final PK projection is read from the carrier directly; it
            // does not rename the source's whole-node projection binding.
            if (p + 1 == count &&
                renamed(q.outputs[i], names).structuralKey() !=
                    s.outputs[positions[i]].structuralKey()) continue;
            names[qo[i]->getUniqueName()] = so[positions[i]]->getUniqueName();
          }
        }
        // DISTINCT on the read must retain every key; otherwise equal payloads
        // belonging to different source groups would collapse into a single
        // row.
        for (const auto& key : sourceKeys) {
          bool retained = false;
          for (const auto& output : queryParts[count - 1].outputs) {
            const auto e = renamed(output, names);
            if (e.binding != key->getUniqueName())
              continue;
            retained |= (e.kind == index::ExpressionKind::Variable &&
                         e.type.starts_with("NODE")) ||
                        (e.kind == index::ExpressionKind::Property &&
                         e.name == key->getPrimaryKey(
                                          key->getEntries()[0]->getTableID())
                                       ->constCast<PropertyExpression>()
                                       .getPropertyName());
          }
          if (!retained && !rollup)
            return false;
        }
        finalNames = std::move(names);
        return true;
      });
  (void) ignored;
  if (!found)
    return false;
  std::vector<std::shared_ptr<NodeExpression>> queryKeys;
  for (const auto& sourceKey : sourceKeys) {
    auto match = std::find_if(
        queryIR.nodesInfo.begin(), queryIR.nodesInfo.end(), [&](const auto& n) {
          return finalNames.at(n.uniqName) == sourceKey->getUniqueName();
        });
    if (match == queryIR.nodesInfo.end())
      return false;
    queryKeys.push_back(queryGraph.getQueryNode(match->uniqName));
  }
  const auto left = "__fill_prefix_left_" + indexName;
  const auto right = "__fill_prefix_right_" + indexName;
  const auto edge = "__fill_prefix_edge_" + indexName;
  std::string carrierText = "MATCH (" + quote(left) + ":" +
                            quote(sourceKeys[0]->getEntries()[0]->getName()) +
                            ")";
  if (keyCount == 2)
    carrierText += "-[" + quote(edge) + ":" + quote(info.getTableName()) +
                   "]->(" + quote(right) + ":" +
                   quote(sourceKeys[1]->getEntries()[0]->getName()) + ")";
  carrierText += " RETURN " + quote(left);
  auto carrierBound = bindSource(carrierText, context);
  if (!carrierBound)
    return false;
  auto* carrierPart =
      carrierBound->getSingleQueryUnsafe(0)->getQueryPartUnsafe(0);
  auto* match = &carrierPart->getReadingClause(0)->cast<BoundMatchClause>();
  auto* graph = match->getQueryGraphCollectionUnsafe()->getQueryGraphUnsafe(0);
  Expr carrier;
  for (const auto& node : graph->getQueryNodes()) {
    const auto pos = node->getVariableName() == left ? 0u : 1u;
    node->setNodeUniqueName(queryKeys[pos]->getUniqueName());
    if (keyCount == 1)
      carrier = node;
  }
  if (keyCount == 2)
    carrier = graph->getQueryRel(0);
  const auto* carrierPattern = carrier->constPtrCast<NodeOrRelExpression>();
  for (const auto& column : valueColumns)
    if (!carrierPattern->hasPropertyExpression(column))
      return false;
  const auto property = [&](size_t column) -> Expr {
    if (keyCount == 1)
      return carrier->cast<NodeExpression>().getPropertyExpression(
          valueColumns[column]);
    return carrier->cast<RelExpression>().getPropertyExpression(
        valueColumns[column]);
  };
  Expr presenceExpression;
  if (presence) {
    presenceExpression = property(*presence);
    if (presenceExpression->getDataType().getLogicalTypeID() !=
        LogicalTypeID::BOOL)
      return false;
  }
  const auto oldOutputs = query.getQueryPart(count - 1)
                              ->getProjectionBody()
                              ->getProjectionExpressions();
  expression_vector outputs;
  Replacements downstream;
  Replacements rollupReplacements;
  expression_vector rollupValues;
  for (size_t i = 0; i < oldOutputs.size(); ++i) {
    if (outputColumns[i] == finalValues.size()) {
      outputs.push_back(oldOutputs[i]);
      continue;
    }
    auto value = property(outputColumns[i] - keyCount);
    if (!value || value->getDataType() != oldOutputs[i]->getDataType())
      return false;
    if (rollup) {
      if (oldOutputs[i]->expressionType != ExpressionType::AGGREGATE_FUNCTION)
        return false;
      const auto& oldAggregate =
          oldOutputs[i]->constCast<AggregateFunctionExpression>();
      const auto readName =
          "__fill_prefix_value_" + indexName + "_" + std::to_string(i);
      value->setUniqueName(readName);
      auto input = std::make_shared<VariableExpression>(
          value->getDataType().copy(), readName, readName);
      auto aggregate = std::make_shared<AggregateFunctionExpression>(
          oldAggregate.getFunction().copy(), oldAggregate.getBindData()->copy(),
          expression_vector{input}, oldOutputs[i]->getUniqueName());
      copyIdentity(oldOutputs[i], aggregate);
      rollupReplacements.emplace(oldOutputs[i]->getUniqueName(), aggregate);
      rollupValues.push_back(aggregate);
    } else
      copyIdentity(oldOutputs[i], value);
    outputs.push_back(value);
    auto variable = std::make_shared<VariableExpression>(
        oldOutputs[i]->getDataType().copy(), oldOutputs[i]->getUniqueName(),
        oldOutputs[i]->toString());
    copyIdentity(oldOutputs[i], variable);
    downstream.emplace(variable->getUniqueName(), std::move(variable));
  }
  // A prefix replacement is a complete executable rewrite, just like a local
  // V4 substitution. Discover every proved catalog alternative before choosing
  // one: a shorter prefix must not hide a better, overlapping prefix merely
  // because its catalog name sorts first. No original query expression has
  // been mutated at this point.
  if (binder.hasIndexRewriteReplay()) {
    std::string choice = "computation-prefix/" + std::to_string(branch) + "/" +
                         std::to_string(count) + "/";
    const auto append = [&](const std::string& value) {
      choice += std::to_string(value.size()) + ":" + value;
    };
    append(indexName);
    for (const auto& key : queryKeys) append(key->getUniqueName());
    if (!binder.acceptIndexRewriteChoice(choice)) return false;
  }
  // All proofs succeeded. A carrier edge may be parallel, so recover exactly
  // one row per materialized key. A proved unique node scan needs no dedup.
  if (rollup)
    for (const auto& key : queryKeys)
      outputs.push_back(key);
  BoundProjectionBody body(!uniqueNodeCarrierRead(info, *graph, queryKeys, outputs));
  body.setProjectionExpressions(std::move(outputs));
  body.setProjectionNames(
      query.getQueryPart(count - 1)->getProjectionBody()->getProjectionNames());
  QueryGraph carrierGraph;
  for (const auto& node : graph->getQueryNodes())
    carrierGraph.addQueryNode(node);
  for (const auto& rel : graph->getQueryRels())
    carrierGraph.addQueryRel(rel);
  QueryGraphCollection collection;
  collection.addAndMergeQueryGraphIfConnected(std::move(carrierGraph));
  auto replacement = std::make_unique<BoundMatchClause>(std::move(collection),
                                                        MatchClauseType::MATCH);
  if (presenceExpression)
    replacement->setPredicate(presenceExpression);
  auto* cut = query.getQueryPartUnsafe(count - 1);
  while (cut->getNumReadingClause())
    cut->removeReadingClause(0);
  std::optional<NormalizedQueryPart> rollupRead;
  if (rollup) {
    rollupRead.emplace();
    rollupRead->addReadingClause(std::move(replacement));
    rollupRead->setProjectionBody(std::move(body));
    auto originalBody = cut->getProjectionBody()->copy();
    auto projections = originalBody.getProjectionExpressions();
    for (auto& projection : projections)
      substitute(projection, rollupReplacements);
    originalBody.setProjectionExpressions(std::move(projections));
    originalBody.setAggregateExpressions(std::move(rollupValues));
    cut->setProjectionBody(std::move(originalBody));
  } else {
    cut->addReadingClause(std::move(replacement));
    cut->setProjectionBody(std::move(body));
    cut->setProjectionBodyPredicate(nullptr);
  }
  for (size_t p = count; p < query.getNumQueryParts(); ++p) {
    auto* part = query.getQueryPartUnsafe(p);
    const auto rewrite = [&](expression_vector values) {
      for (auto& value : values)
        substitute(value, downstream);
      return values;
    };
    auto* projection = part->getProjectionBodyUnsafe();
    projection->setProjectionExpressions(
        rewrite(projection->getProjectionExpressions()));
    projection->setAggregateExpressions(
        rewrite(projection->getAggregateExpressions()));
    projection->setGroupByExpressions(
        rewrite(projection->getGroupByExpressions()));
    projection->setOrderByExpressions(
        rewrite(projection->getOrderByExpressions()),
        projection->getSortingOrders());
    auto predicate = part->getProjectionBodyPredicate();
    substitute(predicate, downstream);
    part->setProjectionBodyPredicate(predicate);
    for (size_t c = 0; c < part->getNumReadingClause(); ++c) {
      auto* reading = part->getReadingClause(c);
      predicate = reading->getPredicate();
      substitute(predicate, downstream);
      reading->setPredicate(predicate);
    }
  }
  for (size_t p = 1; p < count; ++p)
    query.removeQueryPart(0);
  if (rollupRead)
    query.insertQueryPart(0, std::move(*rollupRead));
  LOG(INFO) << "Successfully applied typed computation prefix index: "
            << indexName;
  return true;
}

const BoundSetPropertyInfo* terminalSet(const BoundStatement& statement) {
  if (statement.getStatementType() != StatementType::QUERY) return nullptr;
  const auto& regular = statement.constCast<BoundRegularQuery>();
  if (regular.getNumSingleQueries() != 1 || !regular.getPreQueryPart().empty() ||
      regular.getPostSingleQuery()) return nullptr;
  const auto& query = *regular.getSingleQuery(0);
  if (query.getNumQueryParts() == 0) return nullptr;
  for (size_t p = 0; p < query.getNumQueryParts(); ++p) {
    const auto& part = *query.getQueryPart(p);
    if (p + 1 < query.getNumQueryParts()) {
      if (part.hasUpdatingClause() || !part.hasProjectionBody()) return nullptr;
    } else {
      if (part.getNumUpdatingClause() != 1 || part.hasProjectionBodyPredicate() ||
          (part.hasProjectionBody() &&
           (!part.getProjectionBody()->getProjectionExpressions().empty() ||
            part.getProjectionBody()->hasOrderByExpressions() ||
            part.getProjectionBody()->hasSkipOrLimit()))) return nullptr;
      const auto* clause = part.getUpdatingClause(0);
      if (clause->getClauseType() != ClauseType::SET) return nullptr;
      const auto& infos = clause->constCast<BoundSetClause>().getInfos();
      if (infos.size() != 1 || infos[0].updatePk ||
          infos[0].column->expressionType != ExpressionType::PROPERTY ||
          infos[0].pattern->expressionType != ExpressionType::PATTERN ||
          infos[0].pattern->constCast<NodeOrRelExpression>().getNumEntries() != 1)
        return nullptr;
      return &infos[0];
    }
  }
  return nullptr;
}

// This is the same typed read/write question used by metadata pricing. Graph
// identities do not read every payload field. Unknown source constructs fail
// closed; ordinary updates cannot write a FILL-owned property either.
bool independentCatalogWrite(const BoundSetPropertyInfo& update,
                             main::ClientContext& context) {
  const auto& target = update.pattern->constCast<NodeOrRelExpression>();
  const auto table = target.getEntries()[0]->getTableID();
  const auto property = StringUtils::getUpper(
      update.column->constCast<PropertyExpression>().getPropertyName());
  for (const auto* entry : context.getCatalog()->getExtendedIndexEntries(context.getTransaction())) {
    if (entry->getIndexType() != "FILL" || !entry->isLoaded()) return false;
    const auto& fill = entry->getAuxInfo().cast<FillIndexAuxInfo>();
    if (fill.getSourceQuery().empty()) return false;
    if (entry->getTableID() == table &&
        std::any_of(fill.getTargetColumns().begin(), fill.getTargetColumns().end(),
          [&](const auto& column) { return StringUtils::getUpper(column) == property; }))
      return false;
    auto source = bindSource(fill.getSourceQuery(), context);
    if (!source) return false;
    std::set<const Expression*> visited;
    bool independent = true;
    std::function<void(const Expr&)> inspect;
    const auto graph = [&](const QueryGraphCollection& collection) {
      for (const auto& rel : collection.getQueryRels()) {
        if (!rel->isRecursive()) continue;
        const auto* recursive = rel->getRecursiveInfo();
        if (!recursive || !recursive->bindData) { independent = false; continue; }
        inspect(recursive->nodePredicate);
        inspect(recursive->relPredicate);
        inspect(recursive->bindData->weightPropertyExpr);
        for (const auto& value : recursive->nodeProjectionList) inspect(value);
        for (const auto& value : recursive->relProjectionList) inspect(value);
      }
    };
    inspect = [&](const Expr& value) {
      if (!value || !visited.insert(value.get()).second) return;
      if (value->expressionType == ExpressionType::PROPERTY) {
        const auto& read = value->constCast<PropertyExpression>();
        if (!read.isInternalID() && read.hasProperty(table) &&
            StringUtils::getUpper(read.getPropertyName()) == property) independent = false;
        return;
      }
      if (value->expressionType == ExpressionType::PATTERN) return;
      if (value->expressionType == ExpressionType::GRAPH ||
          value->expressionType == ExpressionType::LAMBDA) { independent = false; return; }
      if (value->expressionType == ExpressionType::SUBQUERY)
        graph(*value->constCast<SubqueryExpression>().getQueryGraphCollection());
      for (const auto& child : ExpressionChildrenCollector::collectChildren(*value)) inspect(child);
    };
    const auto inspectPart = [&](const NormalizedQueryPart& part) {
      if (part.hasUpdatingClause()) { independent = false; return; }
      for (size_t c = 0; c < part.getNumReadingClause(); ++c) {
        const auto* clause = part.getReadingClause(c);
        if (clause->getClauseType() == ClauseType::MATCH)
          graph(*clause->constCast<BoundMatchClause>().getQueryGraphCollection());
        else if (clause->getClauseType() == ClauseType::UNWIND)
          inspect(clause->constCast<BoundUnwindClause>().getInExpr());
        else independent = false;
        inspect(clause->getPredicate());
      }
      if (part.hasProjectionBody()) {
        for (const auto& value : part.getProjectionBody()->getProjectionExpressions()) inspect(value);
        for (const auto& value : part.getProjectionBody()->getOrderByExpressions()) inspect(value);
      }
      inspect(part.getProjectionBodyPredicate());
    };
    const auto inspectSingle = [&](const NormalizedSingleQuery& single) {
      for (size_t p = 0; p < single.getNumQueryParts(); ++p) inspectPart(*single.getQueryPart(p));
    };
    for (const auto& part : source->getPreQueryPart()) inspectPart(part);
    for (size_t b = 0; b < source->getNumSingleQueries(); ++b) inspectSingle(*source->getSingleQuery(b));
    if (source->getPostSingleQuery()) inspectSingle(*source->getPostSingleQuery());
    if (!independent) return false;
  }
  return true;
}

std::unique_ptr<BoundMatchClause> copyMatch(const BoundMatchClause& match) {
  QueryGraphCollection collection;
  collection.merge(*match.getQueryGraphCollection());
  auto result = std::make_unique<BoundMatchClause>(std::move(collection), match.getMatchClauseType());
  result->setPredicate(match.getPredicate());
  if (match.hasHint()) result->setHint(match.getHint());
  return result;
}

// A separate, read-only proof view of the SET input. Shared bound expressions
// are only inspected. No mutation is made to the executable query until all
// stages, identity, payload and replay checks have succeeded.
std::optional<NormalizedSingleQuery> setProofQuery(
    const NormalizedSingleQuery& query, const BoundSetPropertyInfo& update,
    bool splitCarrierSeed) {
  if (query.getNumQueryParts() < 2 || query.getQueryPart(query.getNumQueryParts() - 1)->hasReadingClause())
    return std::nullopt;
  NormalizedSingleQuery result;
  for (size_t p = 0; p + 1 < query.getNumQueryParts(); ++p) {
    const auto& old = *query.getQueryPart(p);
    if (!old.hasProjectionBody() || old.hasUpdatingClause()) return std::nullopt;
    NormalizedQueryPart part;
    for (size_t c = 0; c < old.getNumReadingClause(); ++c) {
      const auto* clause = old.getReadingClause(c);
      if (clause->getClauseType() != ClauseType::MATCH) return std::nullopt;
      const auto& match = clause->constCast<BoundMatchClause>();
      if (match.hasHint()) return std::nullopt;
      if (p == 0 && c == 0 && splitCarrierSeed) {
        const auto& rel = update.pattern->constCast<RelExpression>();
        const auto* graph = match.getQueryGraphCollection();
        if (match.getMatchClauseType() != MatchClauseType::MATCH || match.hasPredicate() ||
            graph->getQueryRels().size() != 1 || graph->getQueryNodes().size() != 2 ||
            graph->getQueryRels()[0]->getUniqueName() != rel.getUniqueName()) return std::nullopt;
        NormalizedQueryPart seed;
        seed.addReadingClause(copyMatch(match));
        BoundProjectionBody projection(false);
        projection.setProjectionExpressions({rel.getSrcNode(), rel.getDstNode(), update.pattern});
        seed.setProjectionBody(std::move(projection));
        result.appendQueryPart(std::move(seed));
      } else part.addReadingClause(copyMatch(match));
    }
    part.setProjectionBody(old.getProjectionBody()->copy());
    part.setProjectionBodyPredicate(old.getProjectionBodyPredicate());
    result.appendQueryPart(std::move(part));
  }
  NormalizedQueryPart rhs;
  BoundProjectionBody projection(false);
  projection.setProjectionExpressions({update.columnData});
  rhs.setProjectionBody(std::move(projection));
  result.appendQueryPart(std::move(rhs));
  return result;
}

bool identityExpression(const index::Expression& expression, const std::string& binding,
                        const char* type) {
  return expression.kind == index::ExpressionKind::Variable && expression.binding == binding &&
         expression.type.starts_with(type);
}

// Candidate rendering can add one or more alias-only WITH layers between the
// final value and the native primary-key wrapper. Bound aliases retain their
// typed expression identity. Removing only these row-preserving forwarders
// leaves every computation, grouping, OPTIONAL and filtering stage intact.
void removeTrailingProjectionForwarders(NormalizedSingleQuery& source) {
  while (source.getNumQueryParts() > 3) {
    const auto position = source.getNumQueryParts() - 2;
    const auto& part = *source.getQueryPart(position);
    const auto& previous = *source.getQueryPart(position - 1);
    if (part.hasReadingClause() || part.hasUpdatingClause() ||
        part.hasProjectionBodyPredicate() || !part.hasProjectionBody() ||
        !previous.hasProjectionBody()) return;
    const auto& body = *part.getProjectionBody();
    if (body.isDistinct() || body.hasAggregateExpressions() ||
        body.hasOrderByExpressions() || body.hasSkipOrLimit()) return;
    const auto outputs = body.getProjectionExpressions();
    const auto inputs = previous.getProjectionBody()->getProjectionExpressions();
    if (outputs.empty() || !std::all_of(outputs.begin(), outputs.end(), [&](const auto& output) {
          return std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) {
            return output->getUniqueName() == input->getUniqueName() &&
                   output->getDataType() == input->getDataType();
          });
        })) return;
    source.removeQueryPart(position);
  }
}

bool rewriteTerminalSet(NormalizedSingleQuery& query, const BoundSetPropertyInfo& update,
                        const FillIndexAuxInfo& info, const std::string& indexName,
                        main::ClientContext& context, Binder& binder) {
  if (update.tableType != TableType::REL || info.getFillType() != FillEntryType::REL)
    return false;
  const auto target = std::static_pointer_cast<RelExpression>(update.pattern);
  if (target->isRecursive() || target->getDirectionType() != RelDirectionType::SINGLE ||
      target->getSrcNodeName() == target->getDstNodeName()) return false;
  auto* targetTable = target->getEntries()[0];
  if (targetTable->getName() != info.getTableName() &&
      targetTable->getLabel(context.getCatalog(), context.getTransaction()) != info.getTableName())
    return false;
  auto bound = bindSource(info.getSourceQuery(), context);
  if (!bound || bound->getNumSingleQueries() != 1 || !bound->getPreQueryPart().empty() ||
      bound->getPostSingleQuery()) return false;
  auto& source = *bound->getSingleQueryUnsafe(0);
  removeTrailingProjectionForwarders(source);
  if (source.getNumQueryParts() < 3) return false;
  const auto count = source.getNumQueryParts() - 1;
  const auto& wrapper = *source.getQueryPart(count);
  if (!wrapper.hasProjectionBody() || wrapper.hasReadingClause() || wrapper.hasUpdatingClause() ||
      wrapper.hasProjectionBodyPredicate()) return false;
  const auto* wrapperBody = wrapper.getProjectionBody();
  if (wrapperBody->isDistinct() || wrapperBody->hasAggregateExpressions() ||
      wrapperBody->hasOrderByExpressions() || wrapperBody->hasSkipOrLimit()) return false;
  std::vector<std::string> columns;
  for (const auto& column : info.getTargetColumns())
    if (!column.starts_with(InternalKeyword::FILL_AGGREGATE_STATE_PREFIX) &&
        !column.starts_with(InternalKeyword::FILL_ROW_COUNT_STATE_PREFIX)) columns.push_back(column);
  const auto values = wrapperBody->getProjectionExpressions();
  if (values.size() != columns.size() + 2) return false;
  QueryGraph sourceGraph;
  if (!prefixGraph(source, count, sourceGraph)) return false;
  std::vector<std::shared_ptr<NodeExpression>> keys;
  for (size_t k = 0; k < 2; ++k) {
    if (values[k]->expressionType != ExpressionType::PROPERTY) return false;
    const auto& property = values[k]->constCast<PropertyExpression>();
    if (!property.isPrimaryKey() || !sourceGraph.containsQueryNode(property.getVariableName())) return false;
    auto node = sourceGraph.getQueryNode(property.getVariableName());
    if (node->getNumEntries() != 1) return false;
    keys.push_back(std::move(node));
  }
  if (keys[0]->getUniqueName() == keys[1]->getUniqueName()) return false;
  const auto& initial = *source.getQueryPart(0);
  if (!initial.hasReadingClause() || initial.getReadingClause(0)->getClauseType() != ClauseType::MATCH)
    return false;
  const auto& seed = initial.getReadingClause(0)->constCast<BoundMatchClause>();
  const auto seedRels = seed.getQueryGraphCollection()->getQueryRels();
  if (seed.getMatchClauseType() != MatchClauseType::MATCH || seed.hasPredicate() ||
      seedRels.size() != 1 || seed.getQueryGraphCollection()->getQueryNodes().size() != 2)
    return false;
  const auto sourceCarrier = seedRels[0];
  if (sourceCarrier->isRecursive() || sourceCarrier->getDirectionType() != RelDirectionType::SINGLE ||
      sourceCarrier->getNumEntries() != 1 || sourceCarrier->getEntries()[0]->getTableID() != targetTable->getTableID() ||
      sourceCarrier->getSrcNodeName() != keys[0]->getUniqueName() ||
      sourceCarrier->getDstNodeName() != keys[1]->getUniqueName()) return false;
  const bool distinctSeed = initial.getProjectionBody()->isDistinct();
  auto proofQuery = setProofQuery(query, update, distinctSeed);
  if (!proofQuery || proofQuery->getNumQueryParts() != count) return false;
  QueryGraph queryGraph;
  if (!prefixGraph(*proofQuery, count, queryGraph) ||
      queryGraph.getNumQueryNodes() != sourceGraph.getNumQueryNodes() ||
      queryGraph.getNumQueryRels() != sourceGraph.getNumQueryRels()) return false;
  index::Constraints contract;
  contract.deterministicFunctions = {"ROUND", "SQRT", "FLOOR", "CEIL", "NULLIF", "TO_DATE",
      "DATE", "TIMESTAMP", "TO_INTERVAL", "INTERVAL", "DURATION"};
  const auto sourceParts = index::extractClosedQueryParts(source, contract);
  const auto queryParts = index::extractClosedQueryParts(*proofQuery, contract);
  if (!uniqueKeys(sourceParts, count, keys)) return false;
  bool completeCarrier = true;
  for (size_t p = 0; p < count; ++p) {
    // A mandatory carrier row survives every OPTIONAL suffix, scalar WITH
    // and aggregation on its own keys. In this closed grammar the presence
    // field is therefore known true. Do not estimate a fictitious reduction
    // in SET rows by applying a generic boolean-filter selectivity to it.
    if (!sourceParts[p].postExpressions.empty()) completeCarrier = false;
    for (size_t c = 0; c < sourceParts[p].clauses.size(); ++c)
      if (!(p == 0 && c == 0) && !sourceParts[p].clauses[c].optional) completeCarrier = false;
  }
  std::optional<size_t> presence;
  for (size_t i = 2; i < values.size(); ++i)
    if (values[i]->expressionType == ExpressionType::LITERAL) {
      const auto& value = values[i]->constCast<LiteralExpression>();
      if (value.getDataType().getLogicalTypeID() == LogicalTypeID::BOOL &&
          !value.isNull() && value.getValue().getValue<bool>()) presence = i - 2;
    }
  if (!presence) return false;
  for (size_t p = 0; p < count; ++p) {
    const auto& part = sourceParts[p];
    const auto* body = source.getQueryPart(p)->getProjectionBody();
    if (!part.unsupportedReason.empty() || !queryParts[p].unsupportedReason.empty() ||
        body->hasOrderByExpressions() || body->hasSkipOrLimit() ||
        !std::all_of(part.outputs.begin(), part.outputs.end(), stable) ||
        !std::all_of(part.postExpressions.begin(), part.postExpressions.end(), stable)) return false;
    for (const auto& clause : part.clauses)
      if (!std::all_of(clause.predicates.begin(), clause.predicates.end(), stable)) return false;
  }
  auto sourceIR = convertGraphPatternToIR(sourceGraph);
  auto queryIR = convertGraphPatternToIR(queryGraph);
  size_t valueColumn = columns.size();
  std::string valueSignature;
  const auto [matched, ignored] = PatternIR::findMatch(queryIR, sourceIR, indexName, [&]() {
    Names names;
    for (const auto& node : sourceIR.nodesInfo) {
      if (node.matchedIdx >= queryIR.nodesInfo.size() ||
          !PatternIR::labelsEqual(node.nodeLabels, queryIR.nodesInfo[node.matchedIdx].nodeLabels)) return false;
      names.emplace(queryIR.nodesInfo[node.matchedIdx].uniqName, node.uniqName);
    }
    for (const auto& rel : sourceIR.relsInfo) {
      if (rel.matchedIdx >= queryIR.relsInfo.size() || queryIR.relsInfo[rel.matchedIdx].edgeType != rel.edgeType ||
          !PatternIR::labelsEqual(rel.relLabels, queryIR.relsInfo[rel.matchedIdx].relLabels)) return false;
      names.emplace(queryIR.relsInfo[rel.matchedIdx].uniqName, rel.uniqName);
    }
    if (!names.contains(target->getUniqueName()) ||
        names.at(target->getUniqueName()) != sourceCarrier->getUniqueName()) return false;
    const auto sourceKey = [&](const index::Expression& expression) {
      return carrierKeyIdentity(expression, keys).has_value();
    };
    const auto targetKey = [&](const index::Expression& expression) {
      return identityExpression(expression, sourceCarrier->getUniqueName(), "REL") || sourceKey(expression);
    };
    std::function<bool(const index::Expression&)> readsCarrier = [&](const auto& expression) {
      return expression.binding == sourceCarrier->getUniqueName() ||
          std::any_of(expression.children.begin(), expression.children.end(), readsCarrier);
    };
    bool grouped = false;
    for (size_t p = 0; p < count; ++p) {
      const auto& q = queryParts[p];
      const auto& s = sourceParts[p];
      const auto* qb = proofQuery->getQueryPart(p)->getProjectionBody();
      const auto* sb = source.getQueryPart(p)->getProjectionBody();
      if (!sameClauses(q, s, names) ||
          signatures(q.aggregates, names) != signatures(s.aggregates, {}) ||
          signatures(q.postExpressions, names) != signatures(s.postExpressions, {}) ||
          qb->hasOrderByExpressions() || qb->hasSkipOrLimit() ||
          (q.distinct != s.distinct && !(distinctSeed && p == 0))) return false;
      if (p > 0 && !s.clauses.empty() && grouped) return false;
      for (const auto& clause : s.clauses)
        for (const auto& predicate : clause.predicates) if (readsCarrier(predicate)) return false;
      if (distinctSeed && p == 0) {
        if (s.clauses.size() != 1 || !s.aggregates.empty() || !s.postExpressions.empty() ||
            s.outputs.size() != 2 || !std::all_of(s.outputs.begin(), s.outputs.end(), sourceKey)) return false;
      } else if (s.distinct || q.distinct) return false;
      if (!s.aggregates.empty()) {
        bool hasTarget = false;
        for (const auto& group : q.groupBy) {
          const auto value = renamed(group, names);
          hasTarget |= identityExpression(value, sourceCarrier->getUniqueName(), "REL");
          if (!targetKey(value)) return false;
        }
        if (!hasTarget || carrierGroupSignatures(s.groupBy, {}, keys) !=
            std::set<std::string>{"carrier-key:" + keys[0]->getUniqueName(),
                                  "carrier-key:" + keys[1]->getUniqueName()}) return false;
        grouped = true;
        for (const auto& aggregate : s.aggregates) {
          if (readsCarrier(aggregate)) return false;
          // Without a DISTINCT endpoint seed, each physical carrier repeats
          // the same witness bag. Only integer MIN/MAX is proved invariant;
          // COUNT/SUM and floating NaN-sensitive extrema remain rejected.
          if (!distinctSeed && (aggregate.kind != index::ExpressionKind::Aggregate ||
              (aggregate.name != "MIN" && aggregate.name != "MAX") ||
              !aggregate.type.starts_with("INT"))) return false;
        }
      }
      const auto qo = qb->getProjectionExpressions();
      const auto so = sb->getProjectionExpressions();
      std::set<std::string> queryValues, sourceValues;
      for (const auto& output : s.outputs) {
        if (sourceKey(output)) continue;
        if (readsCarrier(output)) return false;
        sourceValues.insert(output.structuralKey());
      }
      bool retainsTarget = p + 1 == count;
      for (size_t i = 0; i < q.outputs.size(); ++i) {
        const auto output = renamed(q.outputs[i], names);
        retainsTarget |= identityExpression(output, sourceCarrier->getUniqueName(), "REL");
        if (targetKey(output)) continue;
        if (readsCarrier(output)) return false;
        queryValues.insert(output.structuralKey());
        const auto found = std::find_if(s.outputs.begin(), s.outputs.end(), [&](const auto& value) {
          return value.structuralKey() == output.structuralKey();
        });
        if (found == s.outputs.end()) return false;
        const auto position = static_cast<size_t>(found - s.outputs.begin());
        if (p + 1 == count) {
          valueColumn = columns.size();
          for (size_t c = 2; c < values.size(); ++c)
            if (values[c]->getUniqueName() == so[position]->getUniqueName()) valueColumn = c - 2;
          valueSignature = output.structuralKey();
        }
        names[qo[i]->getUniqueName()] = so[position]->getUniqueName();
      }
      if (!retainsTarget || queryValues != sourceValues) return false;
    }
    return grouped && valueColumn < columns.size() && valueColumn != *presence;
  });
  (void) ignored;
  if (!matched || !target->hasPropertyExpression(columns[valueColumn]) ||
      !target->hasPropertyExpression(columns[*presence])) return false;
  auto value = target->getPropertyExpression(columns[valueColumn]);
  auto present = target->getPropertyExpression(columns[*presence]);
  if (value->getDataType() != update.columnData->getDataType() ||
      present->getDataType().getLogicalTypeID() != LogicalTypeID::BOOL) return false;
  const auto choice = "terminal-set/0/" + std::to_string(indexName.size()) + ":" + indexName +
      "/" + target->getUniqueName() + "/" + update.column->constCast<PropertyExpression>().getPropertyName();
  if (binder.hasIndexRewriteReplay() && !binder.acceptIndexRewriteChoice(choice)) return false;
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> proof(buffer);
  proof.StartObject();
  proof.Key("version"); proof.Uint(1);
  const auto field = [&](const char* name, const std::string& text) {
    proof.Key(name); proof.String(text.data(), text.size());
  };
  field("recipe", choice);
  field("index_name", indexName);
  field("target_kind", "REL");
  field("target_table", targetTable->getName());
  field("target_binding", target->getUniqueName());
  field("target_property", update.column->constCast<PropertyExpression>().getPropertyName());
  field("target_value_type", update.column->getDataType().toString());
  proof.Key("target_table_id"); proof.Uint64(targetTable->getTableID());
  proof.Key("target_column_id"); proof.Uint64(
      update.column->constCast<PropertyExpression>().getColumnID(*targetTable));
  proof.Key("query_branch"); proof.Uint(0);
  proof.Key("used_native_entries"); proof.StartArray(); proof.String(indexName.c_str()); proof.EndArray();
  field("identity_rule", "same_physical_relationship_one_write_per_group");
  field("multiplicity_rule", distinctSeed ? "distinct_endpoint_seed" : "integer_extrema_duplicate_invariance");
  field("group_presence_rule", completeCarrier ? "all_carrier_keys_preserved_by_optional_suffix" : "materialized_presence_predicate");
  field("source_query", info.getSourceQuery());
  field("source_query_sha256", sha256(info.getSourceQuery()));
  field("value_structural_key", valueSignature);
  proof.Key("set_items"); proof.Uint(1);
  proof.Key("catalog_maintenance_independent"); proof.Bool(true);
  proof.EndObject();
  QueryGraph graph;
  graph.addQueryNode(target->getSrcNode());
  graph.addQueryNode(target->getDstNode());
  graph.addQueryRel(target);
  QueryGraphCollection collection;
  collection.addAndMergeQueryGraphIfConnected(std::move(graph));
  auto read = std::make_unique<BoundMatchClause>(std::move(collection), MatchClauseType::MATCH);
  if (!completeCarrier) read->setPredicate(present);
  auto write = std::make_unique<BoundSetClause>();
  auto retained = update.copy();
  retained.columnData = value;
  write->addInfo(std::move(retained));
  NormalizedQueryPart replacement;
  replacement.addReadingClause(std::move(read));
  replacement.addUpdatingClause(std::move(write));
  const auto* oldLast = query.getQueryPart(query.getNumQueryParts() - 1);
  if (oldLast->hasProjectionBody()) replacement.setProjectionBody(oldLast->getProjectionBody()->copy());
  std::vector<NormalizedQueryPart> replacements;
  replacements.push_back(std::move(replacement));
  query.replaceQueryParts(std::move(replacements));
  binder.setPropertyUpdateReadPrefixProof(buffer.GetString());
  LOG(INFO) << "Successfully applied terminal SET read prefix index: " << indexName;
  return true;
}
}  // namespace

bool isIndependentTerminalPropertySet(const BoundStatement& statement,
                                      main::ClientContext& context) {
  try {
    const auto* update = terminalSet(statement);
    return update && independentCatalogWrite(*update, context);
  } catch (const std::exception&) {
    return false;
  }
}

bool supportsStagedCountSumFill(const NormalizedSingleQuery& source,
                               const std::string& anchorUniqueName) {
  if (source.getNumQueryParts() < 3) return false;
  const auto count = source.getNumQueryParts() - 1;
  const auto* wrapper = source.getQueryPart(count);
  if (!wrapper->hasProjectionBody() || wrapper->hasReadingClause() ||
      wrapper->hasUpdatingClause() || wrapper->hasProjectionBodyPredicate())
    return false;
  const auto* body = wrapper->getProjectionBody();
  if (body->hasAggregateExpressions() || body->isDistinct() ||
      body->hasOrderByExpressions() || body->hasSkipOrLimit()) return false;
  QueryGraph graph;
  if (!prefixGraph(source, count, graph) ||
      !graph.containsQueryNode(anchorUniqueName)) return false;
  const auto anchor = graph.getQueryNode(anchorUniqueName);
  for (const auto& node : graph.getQueryNodes())
    if (node->getNumEntries() != 1) return false;
  for (const auto& rel : graph.getQueryRels())
    if (rel->isRecursive()) return false;
  const auto parts = index::extractClosedQueryParts(source, {});
  if (!uniqueKeys(parts, count, {anchor}) ||
      !countSumRollup(source, parts, count)) return false;
  for (size_t p = 0; p < count; ++p) {
    const auto* projection = source.getQueryPart(p)->getProjectionBody();
    if (!parts[p].unsupportedReason.empty() || parts[p].distinct ||
        projection->hasOrderByExpressions() || projection->hasSkipOrLimit() ||
        !std::all_of(parts[p].outputs.begin(), parts[p].outputs.end(), stable) ||
        !std::all_of(parts[p].postExpressions.begin(),
                     parts[p].postExpressions.end(), stable)) return false;
    for (const auto& clause : parts[p].clauses)
      if (!std::all_of(clause.predicates.begin(), clause.predicates.end(), stable))
        return false;
  }
  const auto values = body->getProjectionExpressions();
  if (values.size() < 3 ||
      values[0]->expressionType != ExpressionType::PROPERTY) return false;
  const auto& key = values[0]->constCast<PropertyExpression>();
  if (key.getVariableName() != anchorUniqueName || !key.isPrimaryKey())
    return false;
  const auto aggregates = source.getQueryPart(count - 1)
                              ->getProjectionBody()->getAggregateExpressions();
  bool presence = false, payload = false;
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i]->expressionType == ExpressionType::LITERAL) {
      const auto& literal = values[i]->constCast<LiteralExpression>();
      if (literal.getDataType().getLogicalTypeID() != LogicalTypeID::BOOL ||
          literal.isNull() || !literal.getValue().getValue<bool>()) return false;
      presence = true;
    } else {
      if (presence || std::none_of(aggregates.begin(), aggregates.end(),
          [&](const auto& aggregate) {
            return values[i]->getUniqueName() == aggregate->getUniqueName();
          })) return false;
      payload = true;
    }
  }
  return payload && presence;
}

void rewriteComputationPrefixes(BoundStatement& statement,
                                main::ClientContext& context, Binder* binder) {
  if (!binder || !binder->allowIndexRewrite() ||
      statement.getStatementType() != StatementType::QUERY)
    return;
  auto& regular = statement.cast<BoundRegularQuery>();
  if (!regular.getPreQueryPart().empty() || regular.getPostSingleQuery())
    return;
  auto entries =
      context.getCatalog()->getExtendedIndexEntries(context.getTransaction());
  std::sort(entries.begin(), entries.end(), [](const auto* a, const auto* b) {
    return a->getName() < b->getName();
  });
  for (size_t branch = 0; branch < regular.getNumSingleQueries(); ++branch) {
    auto& query = *regular.getSingleQueryUnsafe(branch);
    bool readOnly = true;
    for (size_t p = 0; p < query.getNumQueryParts(); ++p)
      if (query.getQueryPart(p)->hasUpdatingClause())
        readOnly = false;
    const auto* update = readOnly ? nullptr : terminalSet(statement);
    if (!readOnly && (!update || !independentCatalogWrite(*update, context))) continue;
    for (const auto* entry : entries) {
      if (entry->getIndexType() != "FILL")
        continue;
      const auto* fillInfo =
          dynamic_cast<const FillIndexAuxInfo*>(&entry->getAuxInfo());
      if (!fillInfo)
        continue;
      const auto& info = *fillInfo;
      if (context.getCatalog()->isExtendedIndexBuilding(entry->getName()) ||
          info.getSourceQuery().empty() || !allowedTarget(info))
        continue;
      try {
        const bool rewritten = readOnly
            ? rewriteOne(query, info, entry->getName(), context, *binder, branch)
            : rewriteTerminalSet(query, *update, info, entry->getName(), context, *binder);
        if (rewritten) {
          binder->markIndexRewriteApplied(query);
          break;
        }
      } catch (const std::exception& error) {
        // Unsupported catalog definitions never prevent the original query
        // from binding. Mutations occur only after structural proof succeeds.
        LOG(INFO) << "Computation prefix not applicable: " << entry->getName()
                  << ": " << error.what();
      }
    }
  }
}
}  // namespace neug::binder

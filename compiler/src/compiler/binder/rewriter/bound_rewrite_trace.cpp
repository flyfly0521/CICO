#include "neug/compiler/binder/rewriter/bound_rewrite_trace.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <unordered_set>

#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/parameter_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/rel_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression/subquery_expression.h"
#include "neug/compiler/binder/query/bound_regular_query.h"
#include "neug/compiler/binder/query/reading_clause/bound_match_clause.h"
#include "neug/compiler/catalog/catalog.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/planner/operator/extend/logical_extend.h"
#include "neug/compiler/planner/operator/logical_aggregate.h"
#include "neug/compiler/planner/operator/logical_distinct.h"
#include "neug/compiler/planner/operator/logical_filter.h"
#include "neug/compiler/planner/operator/logical_get_v.h"
#include "neug/compiler/planner/operator/logical_hash_join.h"
#include "neug/compiler/planner/operator/logical_order_by.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/logical_unwind.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"

namespace neug::binder {
namespace {
using V = rapidjson::Value;
using namespace common;
thread_local BoundRewriteTrace* active = nullptr;

struct Json {
  rapidjson::Document doc;
  main::ClientContext* context = nullptr;
  // Track only the current recursion path: shared expressions are serialized
  // at every use, while cycles/deep unknown shapes remain explicit failures.
  std::unordered_set<const Expression*> expressionPath;
  uint64_t expressionCaptureTruncations = 0;
  static constexpr size_t MAX_EXPRESSION_DEPTH = 128;
  struct ExpressionPathScope {
    std::unordered_set<const Expression*>& path;
    const Expression* expression;
    ~ExpressionPathScope() { path.erase(expression); }
  };
  Json() { doc.SetObject(); }
  V str(const std::string& s) { return V(s.data(), s.size(), doc.GetAllocator()); }
  void put(V& o, const char* k, V v) { o.AddMember(str(k), v, doc.GetAllocator()); }
  void put(V& o, const char* k, const std::string& s) { put(o, k, str(s)); }
  void put(V& o, const char* k, const char* s) { put(o, k, str(s)); }
  void put(V& o, const char* k, bool b) { put(o, k, V(b)); }
  void put(V& o, const char* k, uint64_t n) { put(o, k, V(n)); }
  void append(V& a, V v) { a.PushBack(v, doc.GetAllocator()); }
  template<class T> V ids(const T& values) {
    V a(rapidjson::kArrayType);
    for (auto n : values) append(a, V(uint64_t(n)));
    return a;
  }
  V strings(const std::vector<std::string>& values) {
    V a(rapidjson::kArrayType);
    for (const auto& s : values) append(a, str(s));
    return a;
  }
  V parsed(const std::string& input) {
    rapidjson::Document d;
    d.Parse(input.data(), input.size());
    if (d.HasParseError()) throw std::runtime_error("Invalid internal rewrite trace JSON");
    return V(d, doc.GetAllocator());
  }
  static std::string dump(const V& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return {buffer.GetString(), buffer.GetSize()};
  }
  V tables(const std::vector<catalog::TableCatalogEntry*>& entries) {
    V a(rapidjson::kArrayType);
    for (auto* entry : entries) {
      V t(rapidjson::kObjectType);
      put(t, "id", uint64_t(entry->getTableID()));
      put(t, "name", entry->getName());
      append(a, std::move(t));
    }
    return a;
  }
  V graphEntity(const NodeOrRelExpression& entity) {
    V out(rapidjson::kObjectType);
    put(out, "uid", entity.getUniqueName());
    put(out, "raw", entity.getVariableName());
    put(out, "table_ids", ids(entity.getTableIDs()));
    put(out, "tables", tables(entity.getEntries()));
    if (auto* rel = dynamic_cast<const RelExpression*>(&entity)) {
      put(out, "kind", "REL");
      put(out, "src", rel->getSrcNodeName());
      put(out, "dst", rel->getDstNodeName());
      put(out, "left", rel->getLeftNode() ? str(rel->getLeftNode()->getUniqueName()) : V());
      put(out, "right", rel->getRightNode() ? str(rel->getRightNode()->getUniqueName()) : V());
      put(out, "direction", rel->getDirectionType() == RelDirectionType::SINGLE ? "SINGLE" :
          rel->getDirectionType() == RelDirectionType::BOTH ? "BOTH" : "UNKNOWN");
      put(out, "rel_type", uint64_t(rel->getRelType()));
      put(out, "recursive", rel->isRecursive());
      if (rel->isRecursive()) put(out, "unsupported", true);
      put(out, "direction_expression", expr(rel->getDirectionExpr().get()));
    } else put(out, "kind", "NODE");
    return out;
  }
  V expr(const Expression* e) {
    if (!e) return V();
    V o(rapidjson::kObjectType);
    put(o, "kind", ExpressionTypeUtil::toString(e->expressionType));
    put(o, "uid", e->getUniqueName());
    put(o, "alias", e->getAlias());
    put(o, "type", e->getDataType().toString());
    const bool cycle = expressionPath.contains(e);
    if (cycle || expressionPath.size() >= MAX_EXPRESSION_DEPTH) {
      ++expressionCaptureTruncations;
      put(o, "raw", e->getUniqueName());
      put(o, "raw_omitted", true);
      put(o, "unsupported", true);
      put(o, "serialization_complete", false);
      put(o, "capture_reason", cycle ? "expression_cycle" : "expression_depth_limit");
      put(o, "children", V(rapidjson::kArrayType));
      return o;
    }
    expressionPath.insert(e);
    ExpressionPathScope pathScope{expressionPath, e};
    const auto truncationsBefore = expressionCaptureTruncations;
    const auto* subquery = dynamic_cast<const SubqueryExpression*>(e);
    bool completeSubqueryShape = true;
    bool known = false;
    if (auto* p = dynamic_cast<const PropertyExpression*>(e)) {
      known = true;
      put(o, "owner_uid", p->getVariableName());
      put(o, "owner_raw", p->getRawVariableName());
      put(o, "property", p->getPropertyName());
      V ts(rapidjson::kArrayType);
      if (context) for (auto* t : context->getCatalog()->getTableEntries(context->getTransaction()))
        if (p->hasProperty(t->getTableID())) append(ts, V(uint64_t(t->getTableID())));
      put(o, "table_ids", std::move(ts));
    } else if (auto* a = dynamic_cast<const AggregateFunctionExpression*>(e)) {
      known = true;
      put(o, "function", a->getFunction().name);
      put(o, "distinct", a->isDistinct());
    } else if (auto* s = dynamic_cast<const ScalarFunctionExpression*>(e)) {
      known = true;
      put(o, "function", s->getFunction().name);
    } else if (auto* l = dynamic_cast<const LiteralExpression*>(e)) {
      known = true;
      const auto value = l->getValue();
      V literal(rapidjson::kObjectType);
      put(literal, "is_null", value.isNull());
      put(literal, "text", value.toString());
      put(literal, "type", value.getDataType().toString());
      put(o, "literal", std::move(literal));
    } else if (auto* c = dynamic_cast<const CaseExpression*>(e)) {
      known = true;
      V alternatives(rapidjson::kArrayType);
      for (uint64_t i = 0; i < c->getNumCaseAlternatives(); ++i) {
        V alternative(rapidjson::kObjectType);
        put(alternative, "when", expr(c->getCaseAlternative(i)->whenExpression.get()));
        put(alternative, "then", expr(c->getCaseAlternative(i)->thenExpression.get()));
        append(alternatives, std::move(alternative));
      }
      put(o, "alternatives", std::move(alternatives));
      put(o, "else", expr(c->getElseExpression().get()));
    } else if (auto* n = dynamic_cast<const NodeOrRelExpression*>(e)) {
      known = true;
      put(o, "entity", graphEntity(*n));
    } else if (auto* p = dynamic_cast<const ParameterExpression*>(e)) {
      known = true;
      put(o, "parameter", p->getName());
      put(o, "has_value", p->hasValue());
    } else if (subquery) {
      // Rich evidence is not an expansion of the generic finite proof grammar.
      // Keep unsupported=true; a scoped consumer must validate this explicit
      // shape, its parent boolean tree and exact outer binding identities.
      const auto type = subquery->getSubqueryType();
      const bool knownType = type == SubqueryType::EXISTS || type == SubqueryType::COUNT;
      put(o, "subquery_type", type == SubqueryType::EXISTS ? "EXISTS" :
          type == SubqueryType::COUNT ? "COUNT" : "UNKNOWN");
      put(o, "subquery_type_code", uint64_t(type));
      put(o, "has_hint", subquery->getHint() != nullptr);
      completeSubqueryShape = knownType && subquery->getHint() == nullptr;
      V graphs(rapidjson::kArrayType);
      const auto* collection = subquery->getQueryGraphCollection();
      if (!collection) {
        completeSubqueryShape = false;
      } else {
        for (common::idx_t g = 0; g < collection->getNumQueryGraphs(); ++g) {
          const auto* graph = collection->getQueryGraph(g);
          V item(rapidjson::kObjectType), nodes(rapidjson::kArrayType), rels(rapidjson::kArrayType);
          for (const auto& node : graph->getQueryNodes()) append(nodes, graphEntity(*node));
          for (const auto& rel : graph->getQueryRels()) {
            if (rel->isRecursive() ||
                (rel->getDirectionType() != RelDirectionType::SINGLE &&
                 rel->getDirectionType() != RelDirectionType::BOTH))
              completeSubqueryShape = false;
            append(rels, graphEntity(*rel));
          }
          put(item, "nodes", std::move(nodes));
          put(item, "relationships", std::move(rels));
          append(graphs, std::move(item));
        }
      }
      put(o, "graphs", std::move(graphs));
      put(o, "where", expr(subquery->getWhereExpression().get()));
      put(o, "count_star", expr(subquery->getCountStarExpr().get()));
      put(o, "projection", expr(subquery->getProjectionExpr().get()));
      put(o, "polarity_source", "enclosing_boolean_expression_tree");
    }
    if (!known) put(o, "unsupported", true);
    put(o, "children", exprs(e->getChildren()));
    const bool completeTraversal = truncationsBefore == expressionCaptureTruncations;
    if (subquery) {
      put(o, "serialization_complete", completeSubqueryShape && completeTraversal);
      if (!completeSubqueryShape)
        put(o, "serialization_limitation", "unknown_subquery_kind_or_unserialized_hint_or_recursive_relationship");
    }
    // rawName() may recursively format children; do not call it after finding
    // a cycle or depth cut in that subtree. UIDs/type/partial structure remain.
    put(o, "raw", completeTraversal ? e->rawName() : e->getUniqueName());
    if (!completeTraversal) {
      put(o, "raw_omitted", true);
      if (!subquery) put(o, "serialization_complete", false);
      put(o, "capture_reason", "descendant_capture_truncated");
    }
    return o;
  }
  V exprs(const expression_vector& expressions) {
    V a(rapidjson::kArrayType);
    for (const auto& e : expressions) append(a, expr(e.get()));
    return a;
  }
  V part(const NormalizedQueryPart& p) {
    V out(rapidjson::kObjectType), clauses(rapidjson::kArrayType);
    put(out, "updating_clause_count", uint64_t(p.getNumUpdatingClause()));
    for (uint32_t i = 0; i < p.getNumReadingClause(); ++i) {
      auto* clause = p.getReadingClause(i);
      V c(rapidjson::kObjectType);
      put(c, "clause_type", uint64_t(clause->getClauseType()));
      put(c, "predicate", expr(clause->getPredicate().get()));
      if (clause->getClauseType() != ClauseType::MATCH) {
        put(c, "kind", "UNSUPPORTED"); put(c, "unsupported", true);
      } else {
        put(c, "kind", "MATCH");
        const auto& match = clause->constCast<BoundMatchClause>();
        put(c, "optional", match.getMatchClauseType() == MatchClauseType::OPTIONAL_MATCH);
        V graphs(rapidjson::kArrayType);
        auto* collection = match.getQueryGraphCollection();
        for (uint32_t g = 0; g < collection->getNumQueryGraphs(); ++g) {
          V graph(rapidjson::kObjectType), nodes(rapidjson::kArrayType), rels(rapidjson::kArrayType);
          for (const auto& node : collection->getQueryGraph(g)->getQueryNodes()) append(nodes, graphEntity(*node));
          for (const auto& rel : collection->getQueryGraph(g)->getQueryRels()) append(rels, graphEntity(*rel));
          put(graph, "nodes", std::move(nodes)); put(graph, "relationships", std::move(rels));
          append(graphs, std::move(graph));
        }
        put(c, "graphs", std::move(graphs));
      }
      append(clauses, std::move(c));
    }
    put(out, "reading_clauses", std::move(clauses));
    put(out, "predicate", expr(p.getProjectionBodyPredicate().get()));
    V projection;
    if (p.hasProjectionBody()) {
      projection.SetObject(); const auto* b = p.getProjectionBody();
      put(projection, "distinct", b->isDistinct());
      put(projection, "expressions", exprs(b->getProjectionExpressions()));
      put(projection, "names", strings(b->getProjectionNames()));
      put(projection, "group_by", exprs(b->getGroupByExpressions()));
      put(projection, "aggregates", exprs(b->getAggregateExpressions()));
      put(projection, "order_by", exprs(b->getOrderByExpressions()));
      V orders(rapidjson::kArrayType);
      for (bool order : b->getSortingOrders()) append(orders, V(order));
      put(projection, "sorting_orders", std::move(orders));
      put(projection, "skip", expr(b->getSkipNumber().get()));
      put(projection, "limit", expr(b->getLimitNumber().get()));
    }
    put(out, "projection", std::move(projection)); return out;
  }
  V single(const NormalizedSingleQuery& q) {
    V o(rapidjson::kObjectType), parts(rapidjson::kArrayType);
    for (uint32_t i = 0; i < q.getNumQueryParts(); ++i) append(parts, part(*q.getQueryPart(i)));
    put(o, "parts", std::move(parts)); return o;
  }
  V statement(const BoundStatement& s) {
    V out(rapidjson::kObjectType);
    put(out, "statement_type", uint64_t(s.getStatementType()));
    if (s.getStatementType() != StatementType::QUERY) { put(out, "unsupported", true); return out; }
    const auto& q = s.constCast<BoundRegularQuery>();
    V queries(rapidjson::kArrayType), pre(rapidjson::kArrayType), unions(rapidjson::kArrayType);
    for (uint32_t i = 0; i < q.getNumSingleQueries(); ++i) {
      append(queries, single(*q.getSingleQuery(i)));
      if (i > 0) append(unions, V(q.getIsUnionAll(i - 1)));
    }
    for (const auto& p : q.getPreQueryPart()) append(pre, part(p));
    put(out, "single_queries", std::move(queries)); put(out, "pre_parts", std::move(pre));
    put(out, "union_all", std::move(unions)); put(out, "pre_expressions", exprs(q.getPreQueryExprs()));
    put(out, "post_query", q.getPostSingleQuery() ? single(*q.getPostSingleQuery()) : V());
    return out;
  }
  V index(const catalog::IndexCatalogEntry& entry, const PatternIR& source,
          const PatternIR& current) {
    const auto& info = entry.getAuxInfo().cast<catalog::FillIndexAuxInfo>();
    V out(rapidjson::kObjectType), pattern(rapidjson::kObjectType), nodes(rapidjson::kArrayType), rels(rapidjson::kArrayType);
    put(out, "name", entry.getName()); put(out, "target_table", info.getTableName());
    put(out, "fill_type", info.getFillType() == catalog::FillEntryType::NODE ? "NODE" : "REL");
    put(out, "target_columns", strings(info.getTargetColumns()));
    put(out, "anchor_uid", info.getTableUniqName());
    put(out, "src_key_uid", info.getSrcNodeUniqueName());
    put(out, "dst_key_uid", info.getDstNodeUniqueName());
    put(out, "source_query", info.getSourceQuery());
    put(out, "predicate_templates", strings(info.getPredicateTemplates()));
    put(out, "numeric_contract", uint64_t(info.getAggregateNumericContract()));
    put(out, "aggregate_form", uint64_t(info.getAggregateForm()));
    for (const auto& n : source.nodesInfo) {
      V o(rapidjson::kObjectType);
      put(o, "uid", n.uniqName); put(o, "raw", n.varName); put(o, "table_ids", ids(n.nodeLabels));
      put(o, "matched_query_uid", n.matchedIdx < current.nodesInfo.size() ? str(current.nodesInfo[n.matchedIdx].uniqName) : V());
      append(nodes, std::move(o));
    }
    for (const auto& r : source.relsInfo) {
      V o(rapidjson::kObjectType);
      put(o, "uid", r.uniqName); put(o, "raw", r.varName); put(o, "table_ids", ids(r.relLabels));
      put(o, "src", source.nodesInfo.at(r.srcNodeIdx).uniqName);
      put(o, "dst", source.nodesInfo.at(r.dstNodeIdx).uniqName);
      put(o, "direction", r.edgeType == EdgeType::UNDIRECTED ? "BOTH" : r.edgeType == EdgeType::OUTGOING ? "OUTGOING" : "INCOMING");
      put(o, "rel_type", uint64_t(r.relType));
      put(o, "matched_query_uid", r.matchedIdx < current.relsInfo.size() ? str(current.relsInfo[r.matchedIdx].uniqName) : V());
      append(rels, std::move(o));
    }
    put(pattern, "nodes", std::move(nodes)); put(pattern, "relationships", std::move(rels));
    put(out, "source_pattern", std::move(pattern));
    V columns(rapidjson::kArrayType);
    for (const auto& column : info.getIndexColumnInfos()) {
      V c(rapidjson::kObjectType);
      put(c, "column_type", uint64_t(column->columnType));
      put(c, "node_unique", column->nodeUnique); put(c, "property", column->propertyName);
      if (auto* a = dynamic_cast<const catalog::NodeAggregationIndexColnumInfo*>(column.get())) {
        put(c, "function", a->aggregateFuncName); put(c, "template", a->nameTemplate);
      } else if (auto* a = dynamic_cast<const catalog::RelIndexColnumInfo*>(column.get())) {
        put(c, "function", a->aggregateFuncName); put(c, "template", a->nameTemplate);
      }
      append(columns, std::move(c));
    }
    put(out, "columns", std::move(columns)); return out;
  }
  V logical(const planner::LogicalOperator& op) {
    using namespace planner;
    V out(rapidjson::kObjectType), children(rapidjson::kArrayType);
    put(out, "kind", LogicalOperatorUtils::logicalOperatorTypeToString(op.getOperatorType()));
    put(out, "printed", op.getExpressionsForPrinting());
    const auto columnPredicates = [&](const auto& scan) {
      V predicates(rapidjson::kArrayType);
      uint64_t nonempty = 0;
      for (const auto& set : scan.getPropertyPredicates()) {
        if (!set.isEmpty()) ++nonempty;
        V entry(rapidjson::kObjectType);
        put(entry, "empty", set.isEmpty());
        append(predicates, std::move(entry));
      }
      put(out, "column_predicate_sets", std::move(predicates));
      put(out, "nonempty_column_predicate_sets", nonempty);
    };
    const auto read = [&](const auto& scan) {
      put(out, "properties", exprs(scan.getProperties()));
      put(out, "predicate", expr(scan.getPredicates().get()));
    };
    switch (op.getOperatorType()) {
    case LogicalOperatorType::SCAN_NODE_TABLE: {
      const auto& scan = op.constCast<LogicalScanNodeTable>(); read(scan); columnPredicates(scan);
      put(out, "scan_type", uint64_t(scan.getScanType()));
      put(out, "node_id_scan", scan.isNodeIDScan());
      put(out, "has_extra_info", scan.getExtraInfo() != nullptr);
      if (auto* pk = dynamic_cast<PrimaryKeyScanInfo*>(scan.getExtraInfo()))
        put(out, "primary_key", expr(pk->key.get()));
      else if (scan.getExtraInfo()) put(out, "unsupported", true);
      put(out, "node_id", expr(scan.getNodeID().get())); put(out, "table_ids", ids(scan.getTableIDs())); break;
    }
    case LogicalOperatorType::GET_V: {
      const auto& scan = op.constCast<LogicalGetV>(); read(scan);
      put(out, "node_id", expr(scan.getNodeID().get())); put(out, "table_ids", ids(scan.getTableIDs()));
      put(out, "bound_rel_uid", scan.getStartAliasName()); put(out, "get_v_option", uint64_t(scan.getGetVOpt())); break;
    }
    case LogicalOperatorType::EXTEND: {
      const auto& scan = op.constCast<LogicalExtend>(); read(scan); columnPredicates(scan);
      put(out, "optional", scan.isOptional());
      put(out, "extend_from_source", scan.extendFromSourceNode());
      put(out, "scan_nbr_id", scan.shouldScanNbrID());
      put(out, "relationship", graphEntity(*scan.getRel()));
      put(out, "bound_node", graphEntity(*scan.getBoundNode())); put(out, "nbr_node", graphEntity(*scan.getNbrNode()));
      put(out, "direction", uint64_t(scan.getDirection())); put(out, "extend_option", uint64_t(scan.getExtendOpt())); break;
    }
    case LogicalOperatorType::PROJECTION:
      put(out, "append", op.constCast<LogicalProjection>().append());
      put(out, "expressions", exprs(op.constCast<LogicalProjection>().getExpressionsToProject())); break;
    case LogicalOperatorType::AGGREGATE: {
      const auto& a = op.constCast<LogicalAggregate>();
      put(out, "keys", exprs(a.getAllKeys())); put(out, "aggregates", exprs(a.getAggregates())); break;
    }
    case LogicalOperatorType::FILTER:
      put(out, "predicate", expr(op.constCast<LogicalFilter>().getPredicate().get())); break;
    case LogicalOperatorType::HASH_JOIN: {
      V conditions(rapidjson::kArrayType);
      const auto& join = op.constCast<LogicalHashJoin>();
      for (const auto& [left, right] : join.getJoinConditions()) {
        V c(rapidjson::kObjectType); put(c, "left", expr(left.get())); put(c, "right", expr(right.get())); append(conditions, std::move(c));
      }
      put(out, "join_conditions", std::move(conditions));
      put(out, "join_type", uint64_t(join.getJoinType()));
      put(out, "mark", expr(join.getMark().get()));
      put(out, "pre_query", join.getPreQuery()); break;
    }
    case LogicalOperatorType::DISTINCT: {
      const auto& d = op.constCast<LogicalDistinct>(); put(out, "keys", exprs(d.getKeys())); put(out, "payloads", exprs(d.getPayloads())); break;
    }
    case LogicalOperatorType::ORDER_BY:
      put(out, "expressions", exprs(op.constCast<LogicalOrderBy>().getExpressionsToOrderBy())); break;
    case LogicalOperatorType::UNWIND:
      put(out, "expression", expr(op.constCast<LogicalUnwind>().getInExpr().get())); break;
    case LogicalOperatorType::CROSS_PRODUCT:
    case LogicalOperatorType::DUMMY_SCAN:
    case LogicalOperatorType::DUMMY_SINK:
    case LogicalOperatorType::EMPTY_RESULT:
      break;
    default: put(out, "unsupported", true); break;
    }
    for (const auto& child : op.getChildren()) append(children, logical(*child));
    put(out, "children", std::move(children)); return out;
  }
};
}  // namespace

struct BoundRewriteTrace::Impl {
  Json json;
  BoundRewriteTrace* previous = nullptr;
  size_t depth = 0;
  BoundStatement* statement = nullptr;
  explicit Impl(BoundRewriteTrace* previous) : previous(previous) {
    json.put(json.doc, "version", uint64_t(1));
    json.put(json.doc, "before", V()); json.put(json.doc, "after", V());
    json.put(json.doc, "steps", V(rapidjson::kArrayType));
  }
  std::string snapshot() { return Json::dump(json.statement(*statement)); }
};

BoundRewriteTrace::BoundRewriteTrace() : impl(std::make_unique<Impl>(active)) { active = this; }
BoundRewriteTrace::~BoundRewriteTrace() { active = impl->previous; }
std::string BoundRewriteTrace::toJson() const { return Json::dump(impl->json.doc); }

BoundRewriteTracePause::BoundRewriteTracePause() : previous(active) { active = nullptr; }
BoundRewriteTracePause::~BoundRewriteTracePause() { active = previous; }

BoundRewriteTraceQueryScope::BoundRewriteTraceQueryScope(BoundStatement& s, main::ClientContext& c)
    : recorder(active), statement(&s), context(&c) {
  if (recorder) root = recorder->impl->depth++ == 0;
}
void BoundRewriteTraceQueryScope::normalized() {
  if (!recorder || !root) return;
  auto& state = *recorder->impl;
  state.statement = statement; state.json.context = context;
  state.json.doc["steps"].Clear();
  state.json.doc["before"] = state.json.statement(*statement);
}
BoundRewriteTraceQueryScope::~BoundRewriteTraceQueryScope() {
  if (!recorder) return;
  auto& state = *recorder->impl;
  if (root && state.statement) {
    try { state.json.doc["after"] = state.json.statement(*statement); }
    catch (const std::exception& e) { state.json.put(state.json.doc, "capture_error", e.what()); }
    state.statement = nullptr;
  }
  --state.depth;
}
BoundRewriteTraceStep::BoundRewriteTraceStep(const catalog::IndexCatalogEntry& entry,
    const PatternIR& source, const PatternIR& current) {
  if (!active || active->impl->depth != 1 || !active->impl->statement) return;
  recorder = active;
  before = recorder->impl->snapshot();
  index = Json::dump(recorder->impl->json.index(entry, source, current));
}
void BoundRewriteTraceStep::commit() {
  if (!recorder) return;
  auto& state = *recorder->impl;
  V step(rapidjson::kObjectType);
  state.json.put(step, "rule", "v4_match");
  state.json.put(step, "index", state.json.parsed(index));
  state.json.put(step, "before", state.json.parsed(before));
  state.json.put(step, "after", state.json.statement(*state.statement));
  state.json.append(state.json.doc["steps"], std::move(step));
  recorder = nullptr;
}
std::string structuredBoundStatementJson(const BoundStatement& statement, main::ClientContext& context) {
  Json json; json.context = &context; return Json::dump(json.statement(statement));
}

std::string structuredLogicalPlanJson(const planner::LogicalOperator& plan, main::ClientContext& context) {
  Json json; json.context = &context; return Json::dump(json.logical(plan));
}
}  // namespace neug::binder

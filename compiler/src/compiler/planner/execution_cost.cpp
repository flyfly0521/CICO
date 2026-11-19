#include "neug/compiler/planner/execution_cost.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <string_view>
#include <functional>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "neug/compiler/binder/expression/aggregate_function_expression.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/binder/expression/case_expression.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/gopt/g_alias_manager.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/planner/operator/extend/logical_extend.h"
#include "neug/compiler/planner/operator/extend/logical_recursive_extend.h"
#include "neug/compiler/planner/operator/logical_aggregate.h"
#include "neug/compiler/planner/operator/logical_alias_map.h"
#include "neug/compiler/planner/operator/logical_distinct.h"
#include "neug/compiler/planner/operator/logical_filter.h"
#include "neug/compiler/planner/operator/logical_get_v.h"
#include "neug/compiler/planner/operator/logical_hash_join.h"
#include "neug/compiler/planner/operator/logical_order_by.h"
#include "neug/compiler/planner/operator/logical_plan.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/logical_union.h"
#include "neug/compiler/planner/operator/logical_unwind.h"
#include "neug/compiler/planner/operator/persistent/logical_set.h"
#include "neug/compiler/planner/operator/scan/logical_expressions_scan.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"
#include "neug/compiler/storage/stats_manager.h"
#include "neug/compiler/storage/store/node_table.h"
#include "neug/execution/common/types/value.h"
#include "neug/transaction/undo_log.h"
#include "neug/transaction/wal/wal.h"

namespace neug::planner {
namespace {
using namespace binder;
using common::ExpressionType;
using common::LogicalTypeID;

uint64_t add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    throw std::overflow_error("execution work overflow");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a)
    throw std::overflow_error("execution work overflow");
  return a * b;
}

// The general evaluator constructs tagged Values for expression nodes, whereas
// VertexPropertyExpr reads a typed column directly. Charge the actual Value
// object's words plus dispatch/null handling, not a query-specific multiplier.
constexpr uint64_t VALUE_WORDS = (sizeof(execution::Value) + 7) / 8;
constexpr uint64_t VERTEX_WORDS = (sizeof(execution::VertexRecord) + 7) / 8;
// Compact edge columns can use fewer words. EdgeRecord is a conservative
// physical identity/pointer bound; shuffling an edge does NOT copy a Value.
constexpr uint64_t EDGE_WORDS = (sizeof(execution::EdgeRecord) + 7) / 8;


// Runtime calibration (SF10, labserver, 2026-11-16 micro-benchmarks):
// - a named multi-table edge column materialized by EXTEND costs ~0.2 us
//   per edge without properties and ~0.8 us with properties (single-table
//   edges are cheap), vs ~0.01 us for a
//   vertex expansion (HASCREATOR / LIKES count(e) vs count(DISTINCT v));
// - a SEMI/ANTI hash join spends ~0.5-1 us per build-side row (BI18);
// LIST_CONTAINS / LIST group keys also touch every element (~65 ns, BI13),
// but their estimated list lengths and row counts compound correlated
// cardinality errors (BI13 zombies: 18x), so they are not priced per element.
constexpr uint64_t NAMED_EDGE_WORK = 100;
constexpr uint64_t NAMED_EDGE_PROPERTY_WORK = 300;
constexpr uint64_t SEMI_ANTI_BUILD_ROW_WORK = 250;
// A named edge traversed against its stored direction resolves its canonical
// outgoing identity (CanonicalEdgeIdentityResolver::outgoing_offset): the
// first time a source vertex is seen, every outgoing edge of that source in
// that typed table is encoded into a string-keyed map (~150 ns per adjacency
// entry, BI6 LIKES: 23.1M entries, 3.4 s, independent of the 1.1M vs 7.3M
// traversed edges once the touched sources saturate).
constexpr uint64_t NAMED_EDGE_IDENTITY_ENTRY_WORK = 100;

struct Estimator {
  main::ClientContext* context;
  ExecutionCostOptions options;
  ExecutionCostEstimate result;
  std::unordered_set<std::string> nodeVariables;
  std::unique_ptr<gopt::GAliasManager> aliases;
  using Columns = std::unordered_map<std::string, uint64_t>;
  // Expected list length after a known COLLECT population. It is an estimate
  // under uniform selection of groups, not a maximum group size or an NDV.
  std::unordered_map<std::string, long double> listLengths;
  // EDGE expansions that the runtime fuses with the predicated GET_V above
  // them (EdgeExpandGetVOprBuilder -> EdgeExpandVWith{SP,GP}VertexPredOpr).
  std::unordered_set<const LogicalOperator*> vertexFilterFusedExtends;
  bool chargedUpdateStatement = false;

  void assumption(const std::string& value) {
    if (std::find(result.assumptions.begin(), result.assumptions.end(),
                  value) == result.assumptions.end())
      result.assumptions.push_back(value);
  }

  static uint64_t ceilWork(long double value) {
    if (!std::isfinite(value) || value < 0 ||
        value > std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("execution work overflow");
    return static_cast<uint64_t>(std::ceil(value));
  }

  static uint64_t log2ceil(uint64_t value) {
    uint64_t answer = 0;
    for (auto n = value ? value - 1 : 0; n; n >>= 1)
      ++answer;
    return answer;
  }

  void findVariables(const LogicalOperator& op) {
    if (op.getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
      const auto& scan = op.constCast<LogicalScanNodeTable>();
      if (scan.getNodeID()->expressionType == ExpressionType::PROPERTY)
        nodeVariables.insert(scan.getNodeID()
                                 ->constCast<PropertyExpression>()
                                 .getVariableName());
    } else if (op.getOperatorType() == LogicalOperatorType::EXTEND) {
      const auto& extend = op.constCast<LogicalExtend>();
      nodeVariables.insert(extend.getBoundNode()->getUniqueName());
      nodeVariables.insert(extend.getNbrNode()->getUniqueName());
    } else if (op.getOperatorType() == LogicalOperatorType::GET_V) {
      const auto& getV = op.constCast<LogicalGetV>();
      if (getV.getNodeID()->expressionType == ExpressionType::PROPERTY)
        nodeVariables.insert(getV.getNodeID()
                                 ->constCast<PropertyExpression>()
                                 .getVariableName());
      // Mirrors edge_expand_get_v_fusable for an EDGE expansion: directed,
      // non-optional, no edge predicate, and GET_V takes the far endpoint.
      // The anonymous-edge condition is checked in visit() (needs aliases).
      if (getV.getPredicates() && op.getNumChildren() == 1 &&
          op.getChild(0)->getOperatorType() == LogicalOperatorType::EXTEND) {
        const auto& extend = op.getChild(0)->constCast<LogicalExtend>();
        const auto direction = extend.getDirection();
        if (extend.getExtendOpt() == ExtendOpt::EDGE && !extend.isOptional() &&
            !extend.isRecursive() && !extend.getPredicates() &&
            getV.getStartAliasName() == extend.getRel()->getUniqueName() &&
            ((direction == common::ExtendDirection::FWD && getV.getGetVOpt() == GetVOpt::END) ||
             (direction == common::ExtendDirection::BWD && getV.getGetVOpt() == GetVOpt::START)))
          vertexFilterFusedExtends.insert(op.getChild(0).get());
      }
    } else if (op.getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
      const auto& extend = op.constCast<LogicalRecursiveExtend>();
      if (extend.getBoundNode())
        nodeVariables.insert(extend.getBoundNode()->getUniqueName());
      if (extend.getNbrNode())
        nodeVariables.insert(extend.getNbrNode()->getUniqueName());
    } else if (op.getOperatorType() == LogicalOperatorType::ALIAS_MAP) {
      for (const auto& expression :
           op.constCast<LogicalAliasMap>().getDstExprs())
        if (expression->getDataType().getLogicalTypeID() == LogicalTypeID::NODE)
          nodeVariables.insert(expression->getUniqueName());
    }
    for (const auto& child : op.getChildren())
      findVariables(*child);
  }

  // Expected adjacency entries encoded by the identity resolver: per typed
  // table, sources are touched by `edges` degree-weighted draws, so the
  // touched outgoing mass is M (1 - exp(-edges * E[d^2]/E[d] / M)) (first
  // order: edges * E[d^2]/E[d]; saturating at all M edges of the table).
  uint64_t namedEdgeIdentityWork(const LogicalExtend& extend, uint64_t edges) {
    long double total = 0, mass = 0;
    std::vector<std::pair<long double, long double>> tables;  // (M, E[d^2]/E[d])
    const auto stats = context->getStatsManager();
    for (const auto* entry : extend.getRel()->getEntries()) {
      const auto& rel = entry->constCast<catalog::RelTableCatalogEntry>();
      const storage::EndpointDegreeKey source{rel.getTableID(), rel.getSrcTableID(),
                                              common::RelDataDirection::FWD};
      const auto degrees = stats->getEndpointDegreeStatistics(source);
      if (!degrees || !degrees->edge_count) continue;
      const long double m = degrees->edge_count;
      long double ratio = m / std::max<uint64_t>(1, degrees->nonzero_vertex_count);
      if (const auto pair = stats->getEndpointPairStatistics(source, source))
        ratio = std::max<long double>(ratio, pair->degree_product_sum / m);
      tables.emplace_back(m, ratio);
      total += m;
    }
    if (total <= 0) return 0;
    for (const auto& [m, ratio] : tables) {
      const long double visited = edges * (m / total);
      mass += m * (1 - std::exp(-visited * ratio / m));
    }
    assumption("a named edge traversed against its stored direction encodes the "
               "touched source vertices' full outgoing adjacency once (identity resolution)");
    return mul(ceilWork(mass), NAMED_EDGE_IDENTITY_ENTRY_WORK);
  }

  bool isAnonymousHead(const std::string& name) const {
    // GAliasManager also recognizes anonymous bindings needed downstream,
    // which receive a real tag and must be retained like named columns.
    return aliases && !aliases->hasValidAliasId(name);
  }


  // Mirrors the runtime's special vertex predicates (special_predicates.cc):
  // a typed TIMESTAMP range with constant bounds, one comparison against a
  // typed parameter, or a literal STRING equality, optionally ANDed with BOOL
  // property flags, is evaluated as typed column comparisons per vertex, not
  // through the generic expression evaluator. Returns that per-vertex work.
  std::optional<uint64_t> typedVertexPredicateWork(
      const std::shared_ptr<Expression>& predicate, const std::string& owner) {
    if (!predicate) return std::nullopt;
    std::vector<std::shared_ptr<Expression>> conjuncts;
    std::function<void(const std::shared_ptr<Expression>&)> split = [&](const auto& e) {
      if (e->expressionType == ExpressionType::AND)
        for (const auto& child : e->getChildren()) split(child);
      else conjuncts.push_back(e);
    };
    split(predicate);
    std::function<bool(const std::shared_ptr<Expression>&)> constant = [&](const auto& e) {
      if (e->expressionType == ExpressionType::LITERAL ||
          e->expressionType == ExpressionType::PARAMETER) return true;
      if (e->expressionType != ExpressionType::FUNCTION) return false;
      for (const auto& child : e->getChildren())
        if (!constant(child)) return false;
      return true;
    };
    const auto ownedProperty = [&](const std::shared_ptr<Expression>& e) {
      if (e->expressionType != ExpressionType::PROPERTY) return false;
      const auto& property = e->constCast<PropertyExpression>();
      return !property.isInternalID() && (property.getVariableName() == owner ||
                                          property.getVariableName() == "__default_alias_name__");
    };
    struct Comparison {
      std::string property;
      ExpressionType op;
      LogicalTypeID type;
      bool parameter;
      bool literalString;
    };
    std::vector<Comparison> comparisons;
    uint64_t flags = 0;
    for (const auto& conjunct : conjuncts) {
      if (ownedProperty(conjunct) &&
          conjunct->getDataType().getLogicalTypeID() == LogicalTypeID::BOOL) {
        ++flags;
        continue;
      }
      if (!common::ExpressionTypeUtil::isComparison(conjunct->expressionType) ||
          conjunct->getNumChildren() != 2) return std::nullopt;
      auto lhs = conjunct->getChild(0), rhs = conjunct->getChild(1);
      auto op = conjunct->expressionType;
      if (!ownedProperty(lhs)) {
        std::swap(lhs, rhs);
        switch (op) {
        case ExpressionType::GREATER_THAN: op = ExpressionType::LESS_THAN; break;
        case ExpressionType::GREATER_THAN_EQUALS: op = ExpressionType::LESS_THAN_EQUALS; break;
        case ExpressionType::LESS_THAN: op = ExpressionType::GREATER_THAN; break;
        case ExpressionType::LESS_THAN_EQUALS: op = ExpressionType::GREATER_THAN_EQUALS; break;
        default: break;
        }
      }
      if (!ownedProperty(lhs) || !constant(rhs)) return std::nullopt;
      comparisons.push_back({lhs->constCast<PropertyExpression>().getPropertyName(), op,
                             lhs->getDataType().getLogicalTypeID(),
                             rhs->expressionType == ExpressionType::PARAMETER,
                             rhs->expressionType == ExpressionType::LITERAL &&
                                 rhs->getDataType().getLogicalTypeID() == LogicalTypeID::STRING});
    }
    const auto lower = [](ExpressionType op) {
      return op == ExpressionType::GREATER_THAN || op == ExpressionType::GREATER_THAN_EQUALS;
    };
    const auto upper = [](ExpressionType op) {
      return op == ExpressionType::LESS_THAN || op == ExpressionType::LESS_THAN_EQUALS;
    };
    if (comparisons.size() == 2) {
      const auto& a = comparisons[0];
      const auto& b = comparisons[1];
      if (a.property != b.property || a.type != LogicalTypeID::TIMESTAMP ||
          b.type != LogicalTypeID::TIMESTAMP ||
          !((lower(a.op) && upper(b.op)) || (upper(a.op) && lower(b.op))))
        return std::nullopt;
    } else if (comparisons.size() == 1) {
      const auto& c = comparisons[0];
      const bool typedParameter = c.parameter &&
          (c.type == LogicalTypeID::INT64 || c.type == LogicalTypeID::INT32 ||
           c.type == LogicalTypeID::TIMESTAMP || c.type == LogicalTypeID::STRING);
      const bool stringEquality = c.literalString && c.type == LogicalTypeID::STRING &&
          c.op == ExpressionType::EQUALS;
      if (!typedParameter && !stringEquality) return std::nullopt;
    } else {
      return std::nullopt;
    }
    return 1 + comparisons.size() + flags;
  }

  uint64_t expressionWork(const std::shared_ptr<Expression>& expr,
                          bool typedProperty,
                          const Columns* available = nullptr) {
    if (!expr)
      return 0;
    // The converter first resolves an already materialized unique expression
    // to a column reference. A direct projection aliases it; a reference inside
    // a general expression still transfers a tagged Value to the evaluator.
    if (available && available->contains(columnKey(expr)))
      return typedProperty ? 0 : VALUE_WORDS + 2;
    const auto type = expr->expressionType;
    if (type == ExpressionType::CASE_ELSE) {
      const auto& conditional = expr->constCast<CaseExpression>();
      auto tests = uint64_t{0};
      auto branch =
          expressionWork(conditional.getElseExpression(), false, available);
      for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i) {
        const auto* alternative = conditional.getCaseAlternative(i);
        tests = add(tests, expressionWork(alternative->whenExpression, false,
                                          available));
        branch = std::max(branch, expressionWork(alternative->thenExpression,
                                                 false, available));
      }
      // Conditions are checked in order; only one value branch is evaluated.
      // No selectivity is known, so charge all tests and the most expensive
      // value branch, including generic tagged-value dispatch.
      return add(VALUE_WORDS + 2, add(tests, branch));
    }
    if (type == ExpressionType::PROPERTY) {
      const auto& property = expr->constCast<PropertyExpression>();
      // ID(n) is a computed integer projection, distinct from the physical
      // node column emitted by SCAN/GET_V. It reads and encodes the identity.
      if (property.isInternalID())
        return typedProperty ? 2 : VALUE_WORDS + 4;
      if (typedProperty && nodeVariables.contains(property.getVariableName()))
        return 2;
      // Property lookup, tagged-value transfer, virtual call and null check.
      return VALUE_WORDS + 4;
    }
    if (type == ExpressionType::LITERAL || type == ExpressionType::PARAMETER)
      return typedProperty ? 1 : VALUE_WORDS + 1;
    if (type == ExpressionType::VARIABLE || type == ExpressionType::PATTERN ||
        type == ExpressionType::AGGREGATE_FUNCTION)
      return 0;  // Existing column.
    if (type == ExpressionType::SUBQUERY || type == ExpressionType::LAMBDA ||
        type == ExpressionType::PATH || type == ExpressionType::GRAPH)
      throw std::invalid_argument("unsupported execution-cost expression");
    uint64_t work = VALUE_WORDS + 2;
    for (const auto& child : expr->getChildren())
      work = add(work, expressionWork(child, false, available));
    return work;
  }

  // Physical Context columns survive until a non-append, nonempty projection
  // replaces them. Logical property metadata is not a materialized column.
  using Dimensions = std::set<std::string>;
  struct State {
    uint64_t rows = 0;
    Columns columns;
    std::optional<std::string> anonymousHead;
    // Distinct identity populations and deterministic value dependencies.
    // These are estimation priors/bounds, never rewrite/uniqueness axioms.
    Columns populations;
    std::unordered_map<std::string, Dimensions> dependencies;
    Dimensions nullableBindings;
    // Rows in which a NULL-extendable binding is non-NULL (LEFT join matches,
    // OPTIONAL expansion matches), and for COUNT columns an upper bound on the
    // groups whose count is positive. Used to bound `counts > 0` filters.
    std::unordered_map<std::string, uint64_t> nonNullRows;
    std::unordered_map<std::string, uint64_t> positiveSupport;
  };

  Dimensions dependencies(const std::shared_ptr<Expression>& expr,
                          State& state) {
    const auto key = columnKey(expr);
    if (const auto found = state.dependencies.find(key);
        found != state.dependencies.end())
      return found->second;
    const auto unknown = [&]() {
      state.populations.try_emplace(key, state.rows);
      return Dimensions{key};
    };
    if (expr->expressionType == ExpressionType::LITERAL ||
        expr->expressionType == ExpressionType::PARAMETER)
      return {};
    if (expr->expressionType == ExpressionType::PROPERTY) {
      const auto& prop = expr->constCast<PropertyExpression>();
      const auto owner =
          (nodeVariables.contains(prop.getVariableName()) ? "node:" : "edge:") +
          prop.getVariableName();
      if (const auto found = state.dependencies.find(owner);
          found != state.dependencies.end())
        return found->second;
      if (state.populations.contains(owner))
        return {owner};
      return unknown();
    }
    if (expr->getDataType().getLogicalTypeID() == LogicalTypeID::NODE ||
        expr->getDataType().getLogicalTypeID() == LogicalTypeID::REL)
      return unknown();
    if (expr->expressionType == ExpressionType::FUNCTION) {
      // Only explicitly deterministic functions may inherit group keys.
      // Unknown/UDF/random functions retain their own population dimension.
      static const std::set<std::string> deterministic = {"+",
                                                          "-",
                                                          "*",
                                                          "/",
                                                          "%",
                                                          "NEGATE",
                                                          "ABS",
                                                          "SQRT",
                                                          "ROUND",
                                                          "CAST",
                                                          "COALESCE",
                                                          "DATE_PART",
                                                          "LOWER",
                                                          "UPPER",
                                                          "LENGTH",
                                                          "CHAR_LENGTH",
                                                          "LIST_CREATION",
                                                          "LIST_EXTRACT",
                                                          "LIST_CONTAINS",
                                                          "LABEL",
                                                          "STRUCT_EXTRACT"};
      if (!deterministic.contains(
              expr->constCast<ScalarFunctionExpression>().getFunction().name))
        return unknown();
    } else if (expr->expressionType != ExpressionType::CASE_ELSE &&
               !common::ExpressionTypeUtil::isBoolean(expr->expressionType) &&
               !common::ExpressionTypeUtil::isComparison(
                   expr->expressionType) &&
               !common::ExpressionTypeUtil::isNullOperator(
                   expr->expressionType))
      return unknown();
    Dimensions output;
    auto include = [&](const std::shared_ptr<Expression>& child) {
      const auto value = dependencies(child, state);
      output.insert(value.begin(), value.end());
    };
    if (expr->expressionType == ExpressionType::CASE_ELSE) {
      const auto& conditional = expr->constCast<CaseExpression>();
      include(conditional.getElseExpression());
      for (size_t i = 0; i < conditional.getNumCaseAlternatives(); ++i) {
        include(conditional.getCaseAlternative(i)->whenExpression);
        include(conditional.getCaseAlternative(i)->thenExpression);
      }
    } else
      for (const auto& child : expr->getChildren())
        include(child);
    return output;
  }

  uint64_t population(const Dimensions& keys, const State& state,
                      uint64_t cap) const {
    long double rows = 1;
    for (const auto& key : keys) {
      const auto found = state.populations.find(key);
      if (found == state.populations.end())
        return cap;
      rows *= found->second;
      if (rows >= cap)
        return cap;
    }
    return std::min(cap, ceilWork(rows));
  }

  void identityPopulation(State& state, const std::string& key,
                          const std::vector<common::table_id_t>& tables,
                          bool nullable = false) {
    if (!context)
      return;
    uint64_t domain = 0;
    for (const auto table : tables)
      domain = add(domain, tableRows(table));
    if (nullable)
      domain = add(domain, 1);
    state.populations[key] = std::min(state.rows, domain);
    state.dependencies[key] = {key};
    if (nullable)
      state.nullableBindings.insert(key);
    else
      state.nullableBindings.erase(key);
  }

  uint64_t scaledRows(uint64_t rows, uint64_t oldInput, uint64_t input) {
    if (oldInput == input)
      return rows;
    if (!input)
      return 0;
    if (!oldInput)
      throw std::invalid_argument(
          "unknown population after zero-cardinality planner input");
    return ceilWork(static_cast<long double>(rows) * input / oldInput);
  }

  void dropAnonymousHead(State& state) const {
    if (state.anonymousHead)
      state.columns.erase(*state.anonymousHead);
    state.anonymousHead.reset();
  }

  std::string physicalNodeKey(
      const std::shared_ptr<Expression>& descriptor) const {
    return "node:" +
           (descriptor->expressionType == ExpressionType::PROPERTY
                ? descriptor->constCast<PropertyExpression>().getVariableName()
                : descriptor->getUniqueName());
  }

  std::string columnKey(const std::shared_ptr<Expression>& expr) const {
    if (expr->getDataType().getLogicalTypeID() == LogicalTypeID::NODE)
      return "node:" + expr->getUniqueName();
    if (expr->getDataType().getLogicalTypeID() == LogicalTypeID::REL)
      return "edge:" + expr->getUniqueName();
    return "value:" + expr->getUniqueName();
  }

  uint64_t columnWords(const std::shared_ptr<Expression>& expr) const {
    const auto type = expr->getDataType().getLogicalTypeID();
    switch (type) {
    case LogicalTypeID::NODE:
    case LogicalTypeID::INTERNAL_ID:
      return VERTEX_WORDS;
    case LogicalTypeID::REL:
      return EDGE_WORDS;
    case LogicalTypeID::BOOL:
    case LogicalTypeID::INT8:
    case LogicalTypeID::UINT8:
    case LogicalTypeID::INT16:
    case LogicalTypeID::UINT16:
    case LogicalTypeID::INT32:
    case LogicalTypeID::UINT32:
    case LogicalTypeID::INT64:
    case LogicalTypeID::UINT64:
    case LogicalTypeID::SERIAL:
    case LogicalTypeID::FLOAT:
    case LogicalTypeID::DOUBLE:
    case LogicalTypeID::DATE:
    case LogicalTypeID::DATE32:
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_SEC:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP_NS:
    case LogicalTypeID::TIMESTAMP_TZ:
    case LogicalTypeID::TIMESTAMP64:
    case LogicalTypeID::POINTER:
      return 1;
    case LogicalTypeID::INT128:
    case LogicalTypeID::UUID:
    case LogicalTypeID::INTERVAL:
      return 2;
    default:
      return VALUE_WORDS;
    }
  }

  uint64_t shuffleRowWork(const Columns& columns) const {
    uint64_t work = 1;  // Produce the output offset once.
    for (const auto& [_, words] : columns)
      work =
          add(work, add(1, mul(words, 2)));  // Offset read, payload read/write.
    return work;
  }

  uint64_t keyWords(const std::shared_ptr<Expression>& expr) {
    const auto& type = expr->getDataType();
    switch (type.getLogicalTypeID()) {
    case LogicalTypeID::STRING: {
      uint64_t bytes;
      if (expr->expressionType == ExpressionType::LITERAL) {
        const auto& literal = expr->constCast<LiteralExpression>();
        bytes = literal.isNull()
                    ? 0
                    : literal.getValue().getValue<std::string>().size();
      } else {
        auto extra = type.getExtraTypeInfo();
        if (!extra)
          throw std::invalid_argument("unavailable string key length bound");
        bytes = extra->constPtrCast<common::StringTypeInfo>()->getMaxLength();
        assumption(
            "string hashing/comparison uses declared maximum byte length, not "
            "observed mean length");
      }
      return add(
          2, add(bytes, 7) / 8);  // View (pointer, length) plus payload scan.
    }
    case LogicalTypeID::LIST:
    case LogicalTypeID::MAP:
    case LogicalTypeID::STRUCT:
    case LogicalTypeID::UNION:
    case LogicalTypeID::BLOB:
    case LogicalTypeID::RECURSIVE_REL:
      throw std::invalid_argument(
          "unsupported execution-cost nested/variable-width key");
    default:
      return columnWords(expr);
    }
  }

  std::optional<long double> listLength(
      const std::shared_ptr<Expression>& expression) {
    const auto found = listLengths.find(columnKey(expression));
    if (found != listLengths.end())
      return found->second;
    if (expression->expressionType == ExpressionType::LITERAL) {
      const auto& literal = expression->constCast<LiteralExpression>();
      if (literal.isNull())
        return 0;
      if (expression->getDataType().getLogicalTypeID() == LogicalTypeID::LIST)
        return literal.getValue().getChildrenSize();
    }
    if (expression->expressionType == ExpressionType::FUNCTION) {
      const auto& function =
          expression->constCast<ScalarFunctionExpression>().getFunction();
      if (function.name == "LIST_CREATION")
        return expression->getNumChildren();
    }
    return std::nullopt;
  }

  uint64_t tableRows(common::table_id_t id) {
    if (!context || !context->getStatsManager()->hasTableCardinality(id))
      throw std::invalid_argument(
          "unavailable recursive/update source cardinality statistics");
    return context->getStatsManager()->getTable(id)->getNumTotalRows(
        context->getTransaction());
  }

  uint64_t recursiveWork(const LogicalRecursiveExtend& extend, uint64_t inputs,
                         State& state) {
    const auto rel = extend.getRel();
    if (!rel || !extend.getBoundNode())
      throw std::invalid_argument(
          "unsupported execution-cost recursive descriptor");
    const auto& bind = extend.getBindData();
    if (bind.lowerBound > bind.upperBound)
      throw std::invalid_argument("invalid recursive hop range");
    const auto pathType = rel->getRelType();
    const bool shortest = pathType == common::QueryRelType::SHORTEST;
    const bool weighted = pathType == common::QueryRelType::WEIGHTED_SHORTEST;
    const bool walk = pathType == common::QueryRelType::VARIABLE_LENGTH_WALK;
    if (!shortest && !weighted && !walk)
      throw std::invalid_argument(
          "unsupported execution-cost path enumeration semantics");
    struct Incidence {
      common::table_id_t from, to;
      uint64_t edges;
    };
    std::vector<Incidence> incidences;
    std::map<common::table_id_t, uint64_t> domains;
    auto domain = [&](common::table_id_t id) {
      if (!domains.contains(id))
        domains.emplace(id, tableRows(id));
    };
    uint64_t edgeVisits = 0;
    for (const auto* entry : rel->getEntries()) {
      const auto& table = entry->constCast<catalog::RelTableCatalogEntry>();
      const auto src = table.getSrcTableID(), dst = table.getDstTableID();
      const auto edges = tableRows(table.getTableID());
      domain(src);
      domain(dst);
      if (edges && (!domains.at(src) || !domains.at(dst)))
        throw std::invalid_argument(
            "inconsistent recursive edge/vertex statistics");
      if (extend.getDirection() != common::ExtendDirection::BWD) {
        incidences.push_back({src, dst, edges});
        edgeVisits = add(edgeVisits, edges);
      }
      if (extend.getDirection() != common::ExtendDirection::FWD) {
        incidences.push_back({dst, src, edges});
        edgeVisits = add(edgeVisits, edges);
      }
    }
    if (incidences.empty())
      throw std::invalid_argument("unavailable recursive relationship domain");
    uint64_t vertices = 0, seedDomain = 0;
    for (const auto id : extend.getBoundNode()->getTableIDs()) {
      domain(id);
      seedDomain = add(seedDomain, domains.at(id));
    }
    for (const auto& [_, size] : domains)
      vertices = add(vertices, size);
    if (inputs && !seedDomain)
      throw std::invalid_argument(
          "nonempty recursive seeds in an empty domain");
    const auto recursiveInfo = rel->getRecursiveInfo();
    const auto predicateWork =
        recursiveInfo ? add(expressionWork(recursiveInfo->relPredicate, false),
                            expressionWork(recursiveInfo->nodePredicate, false))
                      : 0;
    uint64_t own = 0;
    if (shortest || weighted) {
      // The executor's visited set expands each typed vertex at most once per
      // seed. With no reachability observations, price a whole reachable-type
      // domain rather than invent a path selectivity. Heap relaxations cannot
      // exceed the directed incidence count. Path::expand shares its prefix;
      // only length() traverses the existing linked path.
      assumption(
          "shortest-path work uses whole typed source V/E per seed; early "
          "termination/reachability reductions are unknown");
      const auto maxDepth =
          std::min<uint64_t>(bind.upperBound, vertices ? vertices - 1 : 0);
      const auto pathNode = add(2, add(VERTEX_WORDS, EDGE_WORDS));
      uint64_t perSeed =
          add(mul(vertices, 3), mul(edgeVisits, add(3, predicateWork)));
      if (weighted) {
        auto weight = bind.weightPropertyExpr;
        if (!weight && recursiveInfo && recursiveInfo->bindData)
          weight = recursiveInfo->bindData->weightPropertyExpr;
        if (!weight)
          throw std::invalid_argument(
              "unavailable weighted-path weight expression");
        const auto heap = mul(2, log2ceil(add(edgeVisits, 1)));
        perSeed =
            add(perSeed, mul(edgeVisits, add(add(heap, pathNode),
                                             expressionWork(weight, false))));
        perSeed = add(perSeed, mul(vertices, maxDepth));
        assumption(
            "weighted shortest-path relaxations allocate shared path nodes; "
            "visited path length reads use V/hop bound; no parallel speedup "
            "assumed");
      } else {
        perSeed = add(
            perSeed, mul(vertices, add(VERTEX_WORDS, mul(maxDepth, pathNode))));
      }
      own = mul(inputs, perSeed);
    } else {
      // A finite WALK keeps multiplicity, including parallel edges, cycles,
      // and both incidences of an undirected same-label relation. Population
      // propagation is a declared first-moment estimate, not an acyclicity or
      // single-valued assumption. Source counts are required even for 0 rows.
      assumption(
          "finite WALK frontier propagation uses typed E/V means; path "
          "correlations and predicate selectivity are unknown");
      // The physical range is exclusive at INT32_MAX. Evaluate the exact
      // finite engine hop limit with a matrix geometric sum in O(log hops),
      // without declaring source data acyclic or capping the query's hops.
      const auto hops = std::min<uint64_t>(bind.upperBound, INT32_MAX - 1u);
      std::map<common::table_id_t, size_t> ids;
      for (const auto& [id, _] : domains)
        ids.emplace(id, ids.size());
      const auto n = ids.size() + 1;  // Last coordinate accumulates visit work.
      using Matrix = std::vector<std::vector<long double>>;
      Matrix transition(n, std::vector<long double>(n));
      const auto payload = extend.getResultOpt() != ResultOpt::END_V
                               ? add(2, add(VERTEX_WORDS, EDGE_WORDS))
                               : VERTEX_WORDS;
      const auto perVisit = add(add(1, payload), predicateWork);
      for (const auto& [id, _] : domains)
        transition[ids.at(id)][n - 1] = 1;
      for (const auto& edge : incidences) {
        if (!domains.at(edge.from))
          continue;
        const auto rate =
            static_cast<long double>(edge.edges) / domains.at(edge.from);
        transition[ids.at(edge.from)][ids.at(edge.to)] += rate;
        transition[ids.at(edge.from)][n - 1] += rate * perVisit;
      }
      transition[n - 1][n - 1] = 1;
      std::vector<long double> frontier(n);
      for (const auto id : extend.getBoundNode()->getTableIDs())
        frontier[ids.at(id)] = seedDomain ? static_cast<long double>(inputs) *
                                                domains.at(id) / seedDomain
                                          : 0;
      const auto bounded = [](long double value) {
        // Retain the full floating-point exponent range, including +infinity.
        // Clipping a transition before multiplying by a fractional seed could
        // hide overflow. Zero factors are skipped explicitly below, so an
        // unreachable divergent component cannot introduce 0 * infinity NaN.
        return value;
      };
      for (auto remaining = hops; remaining; remaining >>= 1) {
        if (remaining & 1) {
          std::vector<long double> next(n);
          for (size_t i = 0; i < n; ++i)
            if (frontier[i])
              for (size_t j = 0; j < n; ++j)
                if (transition[i][j])
                  next[j] = bounded(next[j] + frontier[i] * transition[i][j]);
          frontier = std::move(next);
        }
        if (remaining == 1)
          break;
        Matrix next(n, std::vector<long double>(n));
        for (size_t i = 0; i < n; ++i)
          for (size_t k = 0; k < n; ++k)
            if (transition[i][k])
              for (size_t j = 0; j < n; ++j)
                if (transition[k][j])
                  next[i][j] =
                      bounded(next[i][j] + transition[i][k] * transition[k][j]);
        transition = std::move(next);
      }
      own = ceilWork(frontier[n - 1]);
      assumption(
          "WALK uses the engine's finite exclusive INT32_MAX hop bound and "
          "logarithmic transition sum; divergent/overflowing mean work stays "
          "unsupported");
    }
    // The logical output cardinality remains the planner's estimate. This is
    // separate from the traversal work above (rejected/visited paths cost
    // work).
    dropAnonymousHead(state);
    const auto key = columnKey(rel);
    state.columns.erase(key);
    own = add(own,
              mul(state.rows, add(VALUE_WORDS, shuffleRowWork(state.columns))));
    state.columns[key] = VALUE_WORDS;
    if (context) {
      state.populations[key] = state.rows;
      state.dependencies[key] = {key};
    }
    if (isAnonymousHead(extend.getAliasName()))
      state.anonymousHead = key;
    return own;
  }

  uint64_t setPropertyWork(const LogicalSetProperty& set, uint64_t inputs,
                           const Columns& columns) {
    if (!options.allowPropertyUpdates)
      throw std::invalid_argument(
          "execution cost property update requires explicit opt-in");
    if (!options.assumeNoPropertyMaintenance)
      throw std::invalid_argument(
          "unavailable auxiliary property-maintenance work");
    if (set.getInfos().empty())
      throw std::invalid_argument("empty property update");
    uint64_t own = 0;
    if (!chargedUpdateStatement) {
      // Begin/dispatch, timestamp acquisition, WAL-header and commit-record
      // bookkeeping. This counts operations/transfers, never fsync latency.
      own = 6;
      chargedUpdateStatement = true;
      assumption(
          "scalar SET includes transactional bookkeeping/redo/undo work; WAL "
          "I/O time and auxiliary maintenance are outside this model");
    }
    for (const auto& info : set.getInfos()) {
      if (info.updatePk || !info.pattern || !info.column || !info.columnData ||
          info.column->expressionType != ExpressionType::PROPERTY)
        throw std::invalid_argument(
            "unsupported primary-key or non-property update");
      const auto type = info.column->getDataType().getLogicalTypeID();
      if (type == LogicalTypeID::STRING || type == LogicalTypeID::LIST ||
          type == LogicalTypeID::MAP || type == LogicalTypeID::STRUCT ||
          type == LogicalTypeID::UNION || type == LogicalTypeID::BLOB ||
          type == LogicalTypeID::NODE || type == LogicalTypeID::REL ||
          type == LogicalTypeID::ANY)
        throw std::invalid_argument(
            "unsupported non-fixed-width scalar property update");
      const auto& property = info.column->constCast<PropertyExpression>();
      const auto words = columnWords(info.column);
      uint64_t perRow = add(expressionWork(info.columnData, false, &columns),
                            add(VALUE_WORDS, mul(words, 3)));
      uint64_t schemaLookups = 0;
      if (info.tableType == common::TableType::NODE) {
        const auto& node = info.pattern->constCast<NodeExpression>();
        for (const auto* entry : node.getEntries()) {
          const auto& table =
              entry->constCast<catalog::NodeTableCatalogEntry>();
          if (table.getPrimaryKeyName() == property.getPropertyName())
            throw std::invalid_argument(
                "unsupported primary-key property update");
          schemaLookups =
              std::max<uint64_t>(schemaLookups, table.getNumProperties());
        }
        const auto records =
            (sizeof(UpdateVertexPropRedo) + sizeof(UpdateVertexPropUndo) + 7) /
            8;
        // Vertex identity, validity/type checks, two schema-name scans, old/new
        // typed values, MVCC timestamp and transaction records.
        perRow = add(
            perRow, add(add(VERTEX_WORDS + 5, mul(schemaLookups, 2)), records));
      } else if (info.tableType == common::TableType::REL) {
        const auto& rel = info.pattern->constCast<RelExpression>();
        long double scanPopulation = 0;
        uint64_t population = 0;
        for (const auto* entry : rel.getEntries()) {
          const auto& table = entry->constCast<catalog::RelTableCatalogEntry>();
          // Offset reconstruction, preimage lookup, incoming occurrence
          // resolution and WAL occurrence ordinal scans may each traverse an
          // adjacency list. A uniformly sampled edge sees E[d^2]/E[d], not
          // the degree of a uniformly sampled vertex. For a full typed-edge
          // scan these totals reproduce the exact source degree-square sums.
          const auto count = tableRows(table.getTableID());
          const storage::EndpointDegreeKey out{table.getTableID(),
                                               table.getSrcTableID(),
                                               common::RelDataDirection::FWD};
          const storage::EndpointDegreeKey in{table.getTableID(),
                                              table.getDstTableID(),
                                              common::RelDataDirection::BWD};
          const auto outPairs =
              context->getStatsManager()->getEndpointPairStatistics(out, out);
          const auto inPairs =
              context->getStatsManager()->getEndpointPairStatistics(in, in);
          if (outPairs && inPairs) {
            scanPopulation += 5.0L * outPairs->degree_product_sum +
                              3.0L * inPairs->degree_product_sum;
            assumption(
                "edge SET adjacency scans use source degree-square sums / edge "
                "count (uniform selected-edge prior); filtered-edge "
                "correlation is unknown");
          } else {
            scanPopulation += 8.0L * count * count;
            assumption(
                "missing degree-square observations: edge SET uses typed table "
                "E as each adjacency-scan upper bound");
          }
          population = add(population, count);
          schemaLookups =
              std::max<uint64_t>(schemaLookups, table.getNumProperties());
        }
        const auto records = (sizeof(UpdateEdgePropByOccurrenceRedo) +
                              sizeof(UpdateEdgePropUndo) + 7) /
                             8;
        perRow = add(perRow, add(add(EDGE_WORDS + 8, schemaLookups), records));
        // Two offset lookups, one preimage scan, three physical occurrence
        // matching scans and two WAL ordinal scans. Count endpoint/offset,
        // visibility and payload comparison work in addition to the visits.
        perRow = add(perRow,
                     mul(ceilWork(population ? scanPopulation / population : 0),
                         add(3, mul(words, 2))));
        perRow =
            add(perRow, words);  // Both outgoing and incoming typed writes.
        assumption(
            "edge SET includes eight identity/preimage/occurrence adjacency "
            "scans; AP batch fast-path savings are not assumed");
      } else
        throw std::invalid_argument("unsupported property update table type");
      own = add(own, mul(inputs, perRow));
    }
    return own;
  }


  // Runtime FilteredNeighborPairCountOpr fuses EXTEND(EDGE) -> GET_V(scalar
  // filter on the neighbor) -> identity PROJECTION -> COUNT(input) grouped by
  // (owner, neighbor). It evaluates the filter once per neighbor-label vertex
  // and walks each input's adjacency, counting into owner slots, without
  // materializing expanded rows or hashing them. Price that operator instead
  // of the generic chain, so rewrites that break the fusion are not favoured.
  // Upper bound on rows passing `X > c` (c >= 0), `X >= c` (c >= 1) or
  // `X <> 0`, where X is a nonnegative combination (+, * by positive literal)
  // of COUNT columns with a known positive-group support.
  std::optional<uint64_t> countSupport(const std::shared_ptr<Expression>& e,
                                       const State& state) {
    if (e->expressionType == ExpressionType::LITERAL) {
      const auto& value = e->constCast<LiteralExpression>().getValue();
      if (value.isNull()) return std::nullopt;
      const auto type = value.getDataType().getLogicalTypeID();
      if (type == LogicalTypeID::INT64 && value.getValue<int64_t>() == 0) return uint64_t{0};
      return std::nullopt;
    }
    const auto found = state.positiveSupport.find(columnKey(e));
    if (found != state.positiveSupport.end()) return found->second;
    if (e->expressionType != ExpressionType::FUNCTION) return std::nullopt;
    const auto& name = e->constCast<ScalarFunctionExpression>().getFunction().name;
    if (name == "+") {
      uint64_t sum = 0;
      for (const auto& child : e->getChildren()) {
        const auto part = countSupport(child, state);
        if (!part) return std::nullopt;
        sum = add(sum, *part);
      }
      return sum;
    }
    if (name == "*" && e->getNumChildren() == 2) {
      for (size_t i = 0; i < 2; ++i) {
        const auto& factor = e->getChild(i);
        if (factor->expressionType != ExpressionType::LITERAL) continue;
        const auto& value = factor->constCast<LiteralExpression>().getValue();
        if (value.isNull() || value.getDataType().getLogicalTypeID() != LogicalTypeID::INT64 ||
            value.getValue<int64_t>() <= 0) return std::nullopt;
        return countSupport(e->getChild(1 - i), state);
      }
    }
    return std::nullopt;
  }

  std::optional<uint64_t> positiveFilterBound(const std::shared_ptr<Expression>& predicate,
                                              const State& state) {
    if (!predicate || predicate->getNumChildren() != 2) return std::nullopt;
    const auto kind = predicate->expressionType;
    const auto& lhs = predicate->getChild(0);
    const auto& rhs = predicate->getChild(1);
    if (rhs->expressionType != ExpressionType::LITERAL) return std::nullopt;
    const auto& value = rhs->constCast<LiteralExpression>().getValue();
    if (value.isNull() || value.getDataType().getLogicalTypeID() != LogicalTypeID::INT64)
      return std::nullopt;
    const auto c = value.getValue<int64_t>();
    const bool positive = (kind == ExpressionType::GREATER_THAN && c >= 0) ||
        (kind == ExpressionType::GREATER_THAN_EQUALS && c >= 1) ||
        (kind == ExpressionType::NOT_EQUALS && c == 0);
    if (!positive) return std::nullopt;
    return countSupport(lhs, state);
  }

  std::optional<uint64_t> fusedNeighborPairCount(const LogicalOperator& op,
                                                 uint64_t groups) {
    const auto* disabled = std::getenv("NEUG_DISABLE_NEIGHBOR_PAIR_COUNT");
    if ((disabled && std::string_view(disabled) == "1") || !context ||
        op.getNumChildren() != 1)
      return std::nullopt;
    const auto& aggregate = op.constCast<LogicalAggregate>();
    const auto keys = aggregate.getAllKeys();
    const auto aggregates = aggregate.getAggregates();
    if (keys.size() != 2 || aggregates.size() != 1) return std::nullopt;
    const auto* projection = op.getChild(0).get();
    if (projection->getOperatorType() != LogicalOperatorType::PROJECTION ||
        projection->getNumChildren() != 1)
      return std::nullopt;
    const auto* getVOp = projection->getChild(0).get();
    if (getVOp->getOperatorType() != LogicalOperatorType::GET_V ||
        getVOp->getNumChildren() != 1)
      return std::nullopt;
    const auto* extendOp = getVOp->getChild(0).get();
    if (extendOp->getOperatorType() != LogicalOperatorType::EXTEND)
      return std::nullopt;
    const auto& extend = extendOp->constCast<LogicalExtend>();
    const auto& getV = getVOp->constCast<LogicalGetV>();
    if (extend.getExtendOpt() != ExtendOpt::EDGE || extend.isOptional() ||
        extend.getRel()->getTableIDs().size() != 1 || !getV.getPredicates() ||
        getV.getTableIDs().size() != 1)
      return std::nullopt;
    const auto input = extend.getBoundNode()->getUniqueName();
    const auto neighbor = extend.getNbrNode()->getUniqueName();
    const auto nodeID = getV.getNodeID();
    if (nodeID->expressionType != ExpressionType::PROPERTY ||
        nodeID->constCast<PropertyExpression>().getVariableName() != neighbor)
      return std::nullopt;
    // The runtime accepts only a scalar filter local to the neighbor.
    std::function<bool(const std::shared_ptr<Expression>&)> local =
        [&](const auto& expression) {
      if (expression->expressionType == ExpressionType::PROPERTY) {
        // GOpt rewrites a GET_V filter over its output vertex to the default
        // alias; both spellings denote the neighbor being produced.
        const auto& owner = expression->template constCast<PropertyExpression>().getVariableName();
        return owner == neighbor || owner == "__default_alias_name__";
      }
      if (expression->expressionType == ExpressionType::PATTERN ||
          expression->expressionType == ExpressionType::VARIABLE ||
          expression->expressionType == ExpressionType::AGGREGATE_FUNCTION ||
          expression->expressionType == ExpressionType::SUBQUERY)
        return false;
      for (const auto& child : expression->getChildren())
        if (!local(child)) return false;
      return true;
    };
    if (!local(getV.getPredicates())) return std::nullopt;
    const auto& count = aggregates.front();
    if (count->expressionType != ExpressionType::AGGREGATE_FUNCTION) return std::nullopt;
    const auto& countExpr = count->constCast<AggregateFunctionExpression>();
    if (countExpr.getFunction().name != "COUNT" || countExpr.isDistinct() ||
        count->getNumChildren() != 1 ||
        count->getChild(0)->expressionType != ExpressionType::PATTERN ||
        count->getChild(0)->getUniqueName() != input)
      return std::nullopt;
    std::set<std::string> keyNames;
    for (const auto& key : keys) {
      if (key->expressionType != ExpressionType::PATTERN ||
          key->getDataType().getLogicalTypeID() != LogicalTypeID::NODE)
        return std::nullopt;
      keyNames.insert(key->getUniqueName());
    }
    if (!keyNames.contains(neighbor) || keyNames.contains(input) || keyNames.size() != 2)
      return std::nullopt;
    std::set<std::string> projected;
    for (const auto& expression : projection->constCast<LogicalProjection>().getExpressionsToProject()) {
      if (expression->expressionType != ExpressionType::PATTERN) return std::nullopt;
      projected.insert(expression->getUniqueName());
    }
    auto expected = keyNames;
    expected.insert(input);
    if (projected != expected) return std::nullopt;
    const auto n = result.operators.size();
    if (n < 3 || result.operators[n - 3].type != "EXTEND" ||
        result.operators[n - 2].type != "GET_V" ||
        result.operators[n - 1].type != "PROJECTION")
      return std::nullopt;
    const auto table = getV.getTableIDs().front();
    if (!context->getStatsManager()->hasTableCardinality(table)) return std::nullopt;
    const uint64_t vertices = context->getStatsManager()
                                  ->getTable(table)
                                  ->cast<storage::NodeTable>()
                                  .getStats(context->getTransaction())
                                  .getTableCard();
    const auto extendInputs = result.operators[n - 3].inputRows;
    const auto visited = result.operators[n - 3].outputRows;
    uint64_t replaced = 0;
    for (size_t i = n - 3; i < n; ++i) {
      replaced = add(replaced, result.operators[i].localWork);
      result.operators[i].localWork = 0;
      result.operators[i].type += "(fused)";
    }
    if (result.work != std::numeric_limits<uint64_t>::max())
      result.work = result.work >= replaced ? result.work - replaced : 0;
    uint64_t fused = mul(vertices, add(2, expressionWork(getV.getPredicates(), false)));
    fused = add(fused, mul(extendInputs, 6));  // owner slot hash + adjacency lookup
    fused = add(fused, mul(visited, 2));       // accepted test + counter increment
    fused = add(fused, mul(groups, add(2 * VERTEX_WORDS, 1)));
    assumption(
        "EXTEND->GET_V->PROJECTION->COUNT grouped by (owner, neighbor) is priced "
        "as runtime FilteredNeighborPairCountOpr: one neighbor-label filter pass "
        "plus one adjacency walk, no row materialization or hash aggregation");
    return fused;
  }

  State visit(const LogicalOperator& op, const State* inherited = nullptr) {
    State state;
    uint64_t inputs = 0;
    std::vector<State> children;
    const bool correlated =
        op.getOperatorType() == LogicalOperatorType::HASH_JOIN &&
        op.constCast<LogicalHashJoin>().getPreQuery();
    for (size_t i = 0; i < op.getNumChildren(); ++i) {
      const auto previous =
          visit(*op.getChild(i),
                correlated && i == 1 ? &children.front() : inherited);
      inputs = add(inputs, previous.rows);
      state.columns.insert(previous.columns.begin(), previous.columns.end());
      state.dependencies.insert(previous.dependencies.begin(),
                                previous.dependencies.end());
      state.nullableBindings.insert(previous.nullableBindings.begin(),
                                    previous.nullableBindings.end());
      for (const auto& [key, count] : previous.populations)
        state.populations[key] = std::max(state.populations[key], count);
      for (const auto& [key, count] : previous.nonNullRows)
        state.nonNullRows[key] = std::max(state.nonNullRows[key], count);
      for (const auto& [key, count] : previous.positiveSupport)
        state.positiveSupport[key] = std::max(state.positiveSupport[key], count);
      if (previous.anonymousHead) {
        if (op.getNumChildren() != 1)
          throw std::invalid_argument(
              "unsupported execution-cost anonymous head across a join");
        state.anonymousHead = previous.anonymousHead;
      }
      children.push_back(previous);
    }
    state.rows = op.getCardinality();
    if (context && op.getNumChildren() == 1)
      state.rows =
          scaledRows(state.rows, op.getChild(0)->getCardinality(), inputs);
    uint64_t own = 0;
    const auto kind = op.getOperatorType();
    switch (kind) {
    case LogicalOperatorType::EMPTY_RESULT:
      state.rows = 0;
      break;
    case LogicalOperatorType::DUMMY_SCAN:
      own = 1;
      break;
    case LogicalOperatorType::EXPRESSIONS_SCAN: {
      const auto& scan = op.constCast<LogicalExpressionsScan>();
      if (scan.isExternalInput())
        throw std::invalid_argument(
            "unavailable external Context input population");
      // The converter emits no operator: correlated subplans inherit the
      // actual parent Context, not the planner's one-row placeholder.
      if (inherited)
        state = *inherited;
      else if (context)
        throw std::invalid_argument(
            "unavailable correlated Context population");
      for (const auto& expression : scan.getExpressions()) {
        const bool nodeID =
            expression->expressionType == ExpressionType::PROPERTY &&
            expression->constCast<PropertyExpression>().isInternalID();
        state.columns[nodeID ? physicalNodeKey(expression)
                             : columnKey(expression)] = columnWords(expression);
      }
      assumption(
          "correlated EXPRESSIONS_SCAN inherits the actual pre-query Context "
          "population; RHS is executed once on that Context");
      break;
    }
    case LogicalOperatorType::SCAN_NODE_TABLE: {
      const auto& scan = op.constCast<LogicalScanNodeTable>();
      uint64_t visited = state.rows;
      if (context && scan.getScanType() == LogicalScanNodeTableType::SCAN &&
          !scan.isNodeIDScan()) {
        uint64_t fullDomain = 0;
        for (const auto table : scan.getTableIDs()) {
          if (!context->getStatsManager()->hasTableCardinality(table))
            throw std::invalid_argument(
                "unavailable scan cardinality statistics");
          const auto stats = context->getStatsManager()
                                 ->getTable(table)
                                 ->cast<storage::NodeTable>()
                                 .getStats(context->getTransaction());
          fullDomain = add(fullDomain, stats.getTableCard());
        }
        // An observed empty table is different from missing statistics.
        // hasTableCardinality above establishes which case this is.
        visited = std::max(visited, fullDomain);
      }
      own = add(visited, mul(state.rows, VERTEX_WORDS));
      if (const auto typed = typedVertexPredicateWork(
              scan.getPredicates(), scan.getNodeID()->constCast<PropertyExpression>().getVariableName())) {
        own = add(own, mul(visited, *typed));
        assumption("typed special scan predicates cost one column comparison per vertex");
      } else
        own = add(own, mul(visited, expressionWork(scan.getPredicates(), false)));
      const auto key = physicalNodeKey(scan.getNodeID());
      state.columns[key] = VERTEX_WORDS;
      identityPopulation(state, key, scan.getTableIDs());
      if (isAnonymousHead(scan.getAliasName()))
        state.anonymousHead = key;
      break;
    }
    case LogicalOperatorType::EXTEND: {
      const auto& extend = op.constCast<LogicalExtend>();
      if (context)
        // Statistics are keyed by typed catalog table IDs. Physical edge
        // label IDs use a separate domain and may name unrelated catalog
        // tables (including nodes), or share one label across typed tables.
        for (const auto table : extend.getRel()->getTableIDs())
          if (!context->getStatsManager()->hasTableCardinality(table))
            throw std::invalid_argument(
                "unavailable relationship cardinality statistics");
      if (extend.getExtendOpt() == ExtendOpt::DEGREE)
        throw std::invalid_argument(
            "unsupported execution-cost degree expansion");
      const auto vertex = extend.getExtendOpt() == ExtendOpt::VERTEX;
      const auto key = vertex ? "node:" + extend.getNbrNode()->getUniqueName()
                              : "edge:" + extend.getRel()->getUniqueName();
      const auto words = vertex ? VERTEX_WORDS : EDGE_WORDS;
      // Context::set_with_reshuffle clears an untagged old head before
      // copying tagged columns; only the new output becomes the new head.
      dropAnonymousHead(state);
      state.columns.erase(key);  // set_with_reshuffle replaces this alias.
      // Adjacency lookup, edge visit, new identity write, then shuffle every
      // surviving Context column, including all earlier vertex bindings.
      const auto visited =
          context
              ? scaledRows(extend.getEstimatedUnfilteredCardinality().value_or(
                               op.getCardinality()),
                           op.getChild(0)->getCardinality(), inputs)
              : extend.getEstimatedUnfilteredCardinality().value_or(state.rows);
      // A fused expansion writes no edge column: the predicated GET_V above
      // checks each visited neighbor and shuffles only the surviving rows
      // (BI10: 25.8M visited, 193K kept, 82 ms).
      const bool fused = vertexFilterFusedExtends.contains(&op) &&
                         isAnonymousHead(extend.getAliasName());
      own = add(inputs, visited);
      if (!fused)
        own = add(own, mul(state.rows, add(words, shuffleRowWork(state.columns))));
      else
        assumption("EDGE expansion fused with a predicated GET_V shuffles only GET_V output rows");
      own = add(own, mul(visited, expressionWork(extend.getPredicates(), false,
                                                 &state.columns)));
      // Only multi-table (union-label) edges use the slow generic edge column;
      // single-table edges (e.g. KNOWS, 3.7M edges in 70 ms) stay cheap.
      if (!vertex && !isAnonymousHead(extend.getAliasName()) &&
          extend.getRel()->getTableIDs().size() > 1) {
        bool properties = false;
        for (const auto* entry : extend.getRel()->getEntries())
          for (const auto& property : entry->getProperties())
            properties |= !property.getName().starts_with("_");
        own = add(own, mul(state.rows, NAMED_EDGE_WORK +
                                           (properties ? NAMED_EDGE_PROPERTY_WORK : 0)));
        assumption("a named edge column costs NAMED_EDGE_WORK per edge (+ properties)");
        if (context && extend.getDirection() != common::ExtendDirection::FWD)
          own = add(own, namedEdgeIdentityWork(extend, state.rows));
      }
      state.columns[key] = words;
      if (extend.isOptional())
        state.nonNullRows[key] = std::min(visited, state.rows);
      identityPopulation(state, key,
                         vertex ? extend.getNbrNode()->getTableIDs()
                                : extend.getRel()->getTableIDs(),
                         extend.isOptional());
      if (isAnonymousHead(extend.getAliasName()))
        state.anonymousHead = key;
      break;
    }
    case LogicalOperatorType::GET_V: {
      const auto& getV = op.constCast<LogicalGetV>();
      const auto key = physicalNodeKey(getV.getNodeID());
      dropAnonymousHead(state);
      state.columns.erase(key);
      own = add(inputs, mul(state.rows,
                            add(VERTEX_WORDS, shuffleRowWork(state.columns))));
      if (const auto typed = typedVertexPredicateWork(
              getV.getPredicates(), getV.getNodeID()->constCast<PropertyExpression>().getVariableName())) {
        own = add(own, mul(inputs, *typed));
        assumption("typed special GET_V predicates cost one column comparison per vertex");
      } else
        own = add(own, mul(inputs, expressionWork(getV.getPredicates(), false,
                                                  &state.columns)));
      state.columns[key] = VERTEX_WORDS;
      identityPopulation(
          state, key, getV.getTableIDs(),
          state.nullableBindings.contains("edge:" + getV.getStartAliasName()));
      if (isAnonymousHead(getV.getAliasName()))
        state.anonymousHead = key;
      break;
    }
    case LogicalOperatorType::PROJECTION: {
      const auto& projection = op.constCast<LogicalProjection>();
      state.rows = inputs;
      const auto expressions = projection.getExpressionsToProject();
      // GQueryConvertor skips an empty projection, including the one below
      // global COUNT(*); it does not clear physical Context columns.
      if (expressions.empty())
        break;
      Columns projected;
      std::unordered_set<std::string> computed;
      bool writesColumn = false;
      for (const auto& expr : expressions) {
        if (!computed.insert(expr->getUniqueName()).second)
          continue;
        const auto work = expressionWork(expr, true, &state.columns);
        // Existing-column projections alias a vector; computed values emit it.
        const auto key = columnKey(expr);
        const auto existing = state.columns.find(key);
        writesColumn |= existing == state.columns.end();
        const auto words = existing == state.columns.end() ? columnWords(expr)
                                                           : existing->second;
        if (work)
          own = add(own, mul(inputs, add(work, words)));
        projected[key] = words;
        if (context)
          state.dependencies[key] = dependencies(expr, state);
        if (const auto length = listLength(expr))
          listLengths[key] = *length;
      }
      // An appending identity mapping is skipped by ProjectOpr::Eval. It
      // leaves even an untagged current head in place until an actual set().
      if (!projection.append() || writesColumn)
        dropAnonymousHead(state);
      if (!projection.append())
        state.columns.clear();
      state.columns.insert(projected.begin(), projected.end());
      break;
    }
    case LogicalOperatorType::FILTER:
      if (const auto bound = positiveFilterBound(
              op.constCast<LogicalFilter>().getPredicate(), state)) {
        state.rows = std::min(state.rows, *bound);
        assumption("`nonnegative COUNT combination > 0` keeps at most the groups "
                   "with a positive count (LEFT/OPTIONAL match support)");
      }
      own = add(mul(inputs,
                    expressionWork(op.constCast<LogicalFilter>().getPredicate(),
                                   false, &state.columns)),
                mul(state.rows, shuffleRowWork(state.columns)));
      break;
    case LogicalOperatorType::DISTINCT: {
      const auto& distinct = op.constCast<LogicalDistinct>();
      if (!distinct.getPayloads().empty() || distinct.hasSkipNum() ||
          distinct.hasLimitNum())
        throw std::invalid_argument(
            "unsupported execution-cost DISTINCT payload/limit");
      Columns retained;
      std::unordered_map<std::string, uint64_t> listKeyWords;
      std::vector<std::shared_ptr<Expression>> keys;
      for (const auto& key : distinct.getKeys()) {
        const auto identity = columnKey(key);
        if (retained.contains(identity))
          continue;  // Converter deduplicates aliases.
        const auto found = state.columns.find(identity);
        if (found == state.columns.end())
          throw std::invalid_argument(
              "unsupported execution-cost unevaluated DISTINCT key");
        const auto type = key->getDataType().getLogicalTypeID();
        if (type == LogicalTypeID::LIST) {
          // A LIST of fixed-width identities/scalars whose length is tracked
          // (e.g. COLLECT, BI4's topForums) hashes as one encoded Value of
          // that many elements. Unknown lengths remain unsupported.
          const auto length = listLength(key);
          const auto element =
              common::ListType::getChildType(key->getDataType()).getLogicalTypeID();
          uint64_t elementWords = 0;
          if (element == LogicalTypeID::NODE || element == LogicalTypeID::INTERNAL_ID)
            elementWords = VERTEX_WORDS;
          else if (element == LogicalTypeID::REL)
            elementWords = EDGE_WORDS;
          else if (element == LogicalTypeID::INT64 || element == LogicalTypeID::DOUBLE ||
                   element == LogicalTypeID::BOOL || element == LogicalTypeID::DATE ||
                   element == LogicalTypeID::TIMESTAMP)
            elementWords = 1;
          if (!length || elementWords == 0)
            throw std::invalid_argument(
                "unsupported execution-cost variable-width DISTINCT key");
          const auto words = add(VALUE_WORDS,
              mul(static_cast<uint64_t>(std::ceil(*length)), elementWords));
          retained.emplace(identity, std::max<uint64_t>(found->second, words));
          listKeyWords[identity] = words;
          assumption("LIST DISTINCT keys hash every element of the tracked list length");
          keys.push_back(key);
          continue;
        }
        if (type == LogicalTypeID::MAP ||
            type == LogicalTypeID::STRUCT || type == LogicalTypeID::UNION ||
            type == LogicalTypeID::BLOB)
          throw std::invalid_argument(
              "unsupported execution-cost variable-width DISTINCT key");
        retained.emplace(identity, found->second);
        keys.push_back(key);
      }
      // Dedup::dedup returns its input unchanged for an empty key list.
      if (keys.empty()) {
        state.rows = inputs;
        break;
      }
      uint64_t rowWork = 3;  // Hash/probe and the surviving offset.
      for (const auto& key : keys) {
        const auto listWords = listKeyWords.find(columnKey(key));
        const auto words =
            listWords != listKeyWords.end() ? listWords->second
            : key->getDataType().getLogicalTypeID() == LogicalTypeID::STRING
                ? keyWords(key)
                : retained.at(columnKey(key));
        if (keys.size() == 1 && listWords == listKeyWords.end())
          rowWork = add(rowWork, words);  // Typed column hash path.
        else {
          // The generic multi-column path constructs a tagged Value, encodes
          // each fixed-width key and hashes the combined byte string.
          rowWork = add(rowWork, add(VALUE_WORDS + 3, mul(words, 3)));
        }
      }
      state.rows = std::min(state.rows, inputs);
      if (context) {
        // A correlated Context can repeat a binding many times. Dedup still
        // visits every input row, but can emit only the distinct key domains.
        // Union dependencies so keys determined by the same identity do not
        // multiply its domain; independent bindings retain their product.
        Dimensions keyDimensions;
        for (const auto& key : keys) {
          const auto dims = dependencies(key, state);
          keyDimensions.insert(dims.begin(), dims.end());
        }
        state.rows =
            std::min(state.rows, population(keyDimensions, state, inputs));
        assumption("DISTINCT output populations are capped by distinct "
                   "binding domains; all input rows still incur dedup work");
      }
      own =
          add(mul(inputs, rowWork), mul(state.rows, shuffleRowWork(retained)));
      // Physical Dedup constructs a new Context containing only its keys.
      state.columns = std::move(retained);
      state.anonymousHead.reset();
      break;
    }
    case LogicalOperatorType::AGGREGATE: {
      const auto& aggregate = op.constCast<LogicalAggregate>();
      Dimensions groupDimensions;
      if (context) {
        for (const auto& key : aggregate.getAllKeys()) {
          const auto dims = dependencies(key, state);
          groupDimensions.insert(dims.begin(), dims.end());
        }
        state.rows = aggregate.getAllKeys().empty()
                         ? 1
                         : std::min(state.rows,
                                    population(groupDimensions, state, inputs));
        assumption(
            "group populations are capped by distinct binding domains; prior "
            "aggregate results inherit their grouping-key dependencies");
      }
      uint64_t groupWork =
          3;  // Empty GKey still hashes, probes and writes a row index.
      for (const auto& key : aggregate.getAllKeys())
        groupWork = add(groupWork, add(1, columnWords(key)));
      own = mul(inputs, groupWork);
      Columns projected;
      for (const auto& key : aggregate.getAllKeys())
        projected[columnKey(key)] = columnWords(key);
      if (!projected.empty())
        own = add(own, mul(state.rows, shuffleRowWork(projected)));
      for (const auto& expression : aggregate.getAggregates()) {
        if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION)
          throw std::invalid_argument(
              "unsupported execution-cost nonaggregate reducer");
        const auto& aggregateExpr =
            expression->constCast<AggregateFunctionExpression>();
        const auto& function = aggregateExpr.getFunction();
        // CountStarReducer reads each group's size. Other reducers traverse
        // row indexes and typed values; projection already computed operands.
        if (aggregateExpr.isDistinct()) {
          if ((function.name != "COUNT" && function.name != "COLLECT") ||
              expression->getNumChildren() != 1)
            throw std::invalid_argument(
                "unsupported execution-cost DISTINCT reducer");
          const auto operand = expression->getChild(0);
          const auto words = keyWords(operand);
          // Typed scalar/vertex operands use a hash set. General Value (edge)
          // operands use std::set because Value has no std::hash overload.
          auto perInput =
              add(4, words);  // Group index, NULL test, hash/probe, key read.
          if (function.name == "COLLECT" ||
              operand->getDataType().getLogicalTypeID() == LogicalTypeID::REL) {
            perInput = add(VALUE_WORDS + 2,
                           mul(add(words, 1), log2ceil(add(inputs, 1))));
            if (function.name == "COLLECT") {
              // ToSetReducer emits tagged values from its ordered set into a
              // list. Unknown distinct population is bounded by its input.
              perInput = add(perInput, add(VALUE_WORDS, words));
              assumption(
                  "COLLECT DISTINCT uses ordered set and input-population "
                  "bound for emitted list values");
            } else
              assumption(
                  "general edge COUNT DISTINCT uses ordered Value set with "
                  "input-cardinality comparison bound");
          }
          own = add(own, mul(inputs, perInput));
        } else if (function.name != "COUNT_STAR")
          own = add(own, mul(inputs, 3));
        own = add(own, mul(state.rows, columnWords(expression)));
        projected[columnKey(expression)] = columnWords(expression);
        if (function.name == "COUNT" && expression->getNumChildren() == 1) {
          const auto found = state.nonNullRows.find(columnKey(expression->getChild(0)));
          if (found != state.nonNullRows.end())
            state.positiveSupport[columnKey(expression)] = std::min(state.rows, found->second);
        }
        if (context)
          state.dependencies[columnKey(expression)] = groupDimensions;
        if (function.name == "COLLECT") {
          listLengths[columnKey(expression)] =
              state.rows ? static_cast<long double>(inputs) / state.rows : 0;
          assumption(
              "UNWIND of COLLECT uses input rows / groups; NULL/distinct "
              "elimination and later group selection use a conservative "
              "uniform population prior");
        }
      }
      if (const auto fused = fusedNeighborPairCount(op, state.rows))
        own = *fused;
      state.columns = std::move(projected);
      state.anonymousHead.reset();
      break;
    }
    case LogicalOperatorType::ORDER_BY: {
      const auto& order = op.constCast<LogicalOrderBy>();
      const auto keys = order.getExpressionsToOrderBy();
      if (keys.empty() || keys.size() != order.getIsAscOrders().size())
        throw std::invalid_argument("invalid execution-cost sort keys");
      uint64_t comparisons = 1;
      for (const auto& key : keys) {
        if (!state.columns.contains(columnKey(key)))
          throw std::invalid_argument(
              "unsupported execution-cost unevaluated sort key");
        comparisons = add(comparisons, add(1, mul(2, keyWords(key))));
      }
      auto retained = inputs;
      auto skip = uint64_t{0};
      if (order.hasLimitNum()) {
        skip = order.getSkipNum() == UINT64_MAX ? 0 : order.getSkipNum();
        retained = std::min(inputs, add(skip, order.getLimitNum()));
      }
      state.rows = retained > skip ? retained - skip : 0;
      // Full sort initializes offsets then sorts. Top-k scans every input,
      // maintains a skip+limit heap, and drains it in order. Without tie
      // statistics do not assume savings from typed first-key staging.
      const auto steps =
          retained == inputs && !skip
              ? mul(inputs, log2ceil(inputs))
              : add(mul(inputs, mul(2, log2ceil(add(retained, 1)))),
                    mul(retained, log2ceil(add(retained, 1))));
      own = add(inputs, add(mul(steps, add(comparisons, 2)),
                            mul(state.rows, shuffleRowWork(state.columns))));
      const auto firstType = keys.front()->getDataType().getLogicalTypeID();
      if (firstType != LogicalTypeID::NODE && firstType != LogicalTypeID::REL) {
        // Nonoptional ValueColumn performs TopNGenerator before the generic
        // heap. Nullable columns can skip it; nullability is not supplied by
        // this metadata interface, so retain the extra candidate-stage work.
        const auto stageSteps = add(inputs, mul(add(mul(inputs, 2), retained),
                                                log2ceil(add(retained, 1))));
        own = add(own, mul(stageSteps, add(2, mul(2, keyWords(keys.front())))));
        assumption(
            "ORDER BY includes possible typed first-key TopN stage; unknown "
            "nullability and ties do not receive pruning discounts");
      }
      assumption(
          "ORDER BY charges all keys per comparison; top-k uses skip+limit");
      break;
    }
    case LogicalOperatorType::UNWIND: {
      const auto& unwind = op.constCast<LogicalUnwind>();
      if (!unwind.getInExpr() || !unwind.getOutExpr() ||
          (unwind.hasIDExpr() &&
           (unwind.getIDExpr()->expressionType != ExpressionType::PROPERTY ||
            !unwind.getIDExpr()
                 ->constCast<PropertyExpression>()
                 .isInternalID())))
        throw std::invalid_argument(
            "unsupported execution-cost UNWIND descriptor");
      const auto length = listLength(unwind.getInExpr());
      if (!length)
        throw std::invalid_argument(
            "unavailable UNWIND list population (requires literal/COLLECT "
            "lineage)");
      state.rows = ceilWork(inputs * *length);
      const auto key = columnKey(unwind.getOutExpr());
      const auto words = columnWords(unwind.getOutExpr());
      own = mul(inputs, add(1, expressionWork(unwind.getInExpr(), false,
                                              &state.columns)));
      dropAnonymousHead(state);
      state.columns.erase(key);
      own = add(own, mul(state.rows, add(add(1, mul(words, 2)),
                                         shuffleRowWork(state.columns))));
      state.columns[key] = words;
      if (context) {
        state.populations[key] = state.rows;
        state.dependencies[key] = {key};
        if (unwind.hasIDExpr()) {
          const auto identity = physicalNodeKey(unwind.getIDExpr());
          state.populations[identity] = state.rows;
          state.dependencies[identity] = {key};
          nodeVariables.insert(unwind.getIDExpr()
                                   ->constCast<PropertyExpression>()
                                   .getVariableName());
        }
      }
      if (isAnonymousHead(unwind.getOutExpr()->getUniqueName()))
        state.anonymousHead = key;
      break;
    }
    case LogicalOperatorType::RECURSIVE_EXTEND:
      own =
          recursiveWork(op.constCast<LogicalRecursiveExtend>(), inputs, state);
      break;
    case LogicalOperatorType::SET_PROPERTY:
      state.rows = inputs;
      own = setPropertyWork(op.constCast<LogicalSetProperty>(), inputs,
                            state.columns);
      break;
    case LogicalOperatorType::UNION_ALL: {
      const auto& unionOp = op.constCast<LogicalUnion>();
      if (unionOp.getPreQuery())
        throw std::invalid_argument(
            "unsupported execution-cost correlated UNION input");
      Columns output;
      for (const auto& expression : unionOp.getExpressionsToUnion())
        output[columnKey(expression)] = columnWords(expression);
      state.rows = inputs;
      // The executor moves the first Context, then union_ctx copies the
      // growing prefix and each next branch into new columns.
      uint64_t prefix = 0;
      for (size_t i = 0; i < op.getNumChildren(); ++i) {
        prefix = add(prefix, children[i].rows);
        if (i)
          own = add(own, mul(prefix, shuffleRowWork(output)));
      }
      state.columns = std::move(output);
      if (context) {
        // Aligned UNION aliases may carry different values or typed identities
        // in each branch. A branch-local constant/owner dependency is not a
        // dependency of the combined column. Without a cross-branch contract,
        // retain the full union population as each output's unknown domain.
        state.populations.clear();
        state.dependencies.clear();
        for (const auto& [key, words] : state.columns) {
          state.populations[key] = state.rows;
          state.dependencies[key] = {key};
        }
      }
      state.anonymousHead.reset();
      break;
    }
    case LogicalOperatorType::HASH_JOIN: {
      const auto& join = op.constCast<LogicalHashJoin>();
      if (context && children.size() == 2) {
        Dimensions leftKeys, rightKeys;
        for (const auto& condition : join.getJoinConditions()) {
          const auto left = dependencies(condition.first, children[0]);
          const auto right = dependencies(condition.second, children[1]);
          leftKeys.insert(left.begin(), left.end());
          rightKeys.insert(right.begin(), right.end());
        }
        const auto left = children[0].rows, right = children[1].rows;
        const auto domain = std::max(population(leftKeys, children[0], left),
                                     population(rightKeys, children[1], right));
        const auto matches =
            domain ? ceilWork(static_cast<long double>(left) * right / domain)
                   : 0;
        switch (join.getJoinType()) {
        case common::JoinType::INNER:
          state.rows = matches;
          break;
        case common::JoinType::LEFT:
          state.rows = std::max(left, matches);
          // LEFT preserves every original binding, including an edge key
          // used by a following GROUP BY/SET. RHS-only dimensions gain NULL.
          for (const auto& [key, value] : children[0].populations)
            state.populations[key] = value;
          for (const auto& [key, value] : children[1].populations)
            if (!children[0].populations.contains(key))
              state.nonNullRows[key] = std::min(matches, state.rows);
          for (const auto& [key, value] : children[1].populations) {
            if (!children[0].populations.contains(key)) {
              // A may-null flag does not prove the row-capped domain already
              // includes a NULL slot. This outer join can add an unmatched
              // NULL even after a prior optional/UNION population was clipped.
              state.populations[key] = std::min(state.rows, add(value, 1));
              state.nullableBindings.insert(key);
            }
          }
          // A materialized RHS-only scalar can also gain NULL on unmatched
          // rows. Even a constant (no dependencies), or a value determined by
          // probe keys, no longer has its pre-join dependency contract. Keep
          // an independent conservative domain including the new NULL choice.
          for (const auto& [key, words] : children[1].columns) {
            if (!key.starts_with("value:") || children[0].columns.contains(key))
              continue;
            const auto found = children[1].dependencies.find(key);
            const auto domain =
                found == children[1].dependencies.end()
                    ? right
                    : population(found->second, children[1], right);
            state.populations[key] = std::min(state.rows, add(domain, 1));
            state.dependencies[key] = {key};
          }
          break;
        case common::JoinType::MARK:
        case common::JoinType::COUNT:
          state.rows = left;
          break;
        case common::JoinType::ANTI:
        case common::JoinType::SEMI:
          state.rows = std::min(left, state.rows);
          break;
        }
        assumption(
            "hash join uses compound-key distinct-domain uniform prior; LEFT "
            "preserves its probe population and NULL-extends RHS-only "
            "bindings");
      }
      own = add(mul(inputs, 3), mul(state.rows, shuffleRowWork(state.columns)));
      if ((join.getJoinType() == common::JoinType::ANTI ||
           join.getJoinType() == common::JoinType::SEMI) && children.size() == 2) {
        own = add(own, mul(children[1].rows, SEMI_ANTI_BUILD_ROW_WORK));
        assumption("SEMI/ANTI hash join build rows cost SEMI_ANTI_BUILD_ROW_WORK");
      }
      break;
    }
    case LogicalOperatorType::CROSS_PRODUCT:
      if (context && children.size() == 2)
        state.rows = mul(children[0].rows, children[1].rows);
      own = add(mul(inputs, 3), mul(state.rows, shuffleRowWork(state.columns)));
      break;
    case LogicalOperatorType::INTERSECT:
      own = add(mul(inputs, 3), mul(state.rows, shuffleRowWork(state.columns)));
      break;
    case LogicalOperatorType::FLATTEN:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::ACCUMULATE:
      own = 0;  // Factorized bookkeeping; not another materialized row pass.
      break;
    case LogicalOperatorType::ALIAS_MAP: {
      const auto& mapping = op.constCast<LogicalAliasMap>();
      const auto sources = mapping.getSrcExprs(),
                 destinations = mapping.getDstExprs();
      if (sources.size() != destinations.size())
        throw std::invalid_argument("invalid execution-cost alias map");
      state.rows = inputs;
      if (sources.empty())
        break;  // Physical converter skips an empty map.
      Columns projected;
      for (size_t i = 0; i < sources.size(); ++i) {
        const auto existing = state.columns.find(columnKey(sources[i]));
        const auto words = existing == state.columns.end()
                               ? columnWords(sources[i])
                               : existing->second;
        const auto work = expressionWork(sources[i], true, &state.columns);
        if (work)
          own = add(own, mul(inputs, add(work, words)));
        if (!projected.emplace(columnKey(destinations[i]), words).second)
          throw std::invalid_argument(
              "duplicate execution-cost alias destination");
        if (context)
          state.dependencies[columnKey(destinations[i])] =
              dependencies(sources[i], state);
        // Alias alignment (including UNION branches) must retain possible
        // NULL graph identities for a later relationship-to-vertex GET_V.
        if (state.nullableBindings.contains(columnKey(sources[i])))
          state.nullableBindings.insert(columnKey(destinations[i]));
        if (const auto length = listLength(sources[i]))
          listLengths[columnKey(destinations[i])] = *length;
      }
      state.columns = std::move(projected);
      state.anonymousHead.reset();
      break;
    }
    case LogicalOperatorType::LIMIT:
      own = mul(state.rows, shuffleRowWork(state.columns));
      break;
    default:
      throw std::invalid_argument(
          "unsupported execution-cost operator: " +
          LogicalOperatorUtils::logicalOperatorTypeToString(kind));
    }
    if (context)
      for (auto& [_, count] : state.populations)
        count = std::min(count, state.rows);
    result.work = add(result.work, own);
    result.operators.push_back(
        {LogicalOperatorUtils::logicalOperatorTypeToString(kind), inputs,
         state.rows, own});
    return state;
  }
};
}  // namespace

ExecutionCostEstimate estimateExecutionCost(const LogicalPlan& plan,
                                            main::ClientContext* context,
                                            ExecutionCostOptions options) {
  Estimator estimator{context, options, {}, {}, {}};
  try {
    if (plan.isEmpty() || (plan.hasUpdate() && !options.allowPropertyUpdates))
      throw std::invalid_argument(
          "execution cost requires a read plan or explicit property-update "
          "opt-in");
    estimator.findVariables(plan.getLastOperatorRef());
    if (!estimator.nodeVariables.empty())
      estimator.aliases = std::make_unique<gopt::GAliasManager>(plan);
    estimator.visit(plan.getLastOperatorRef());
  } catch (const std::exception& error) {
    estimator.result.supported = false;
    estimator.result.work = std::numeric_limits<uint64_t>::max();
    estimator.result.reason = error.what();
  }
  return estimator.result;
}

std::string executionCostJson(const ExecutionCostEstimate& estimate) {
  rapidjson::Document json;
  json.SetObject();
  auto& alloc = json.GetAllocator();
  json.AddMember("model", rapidjson::Value(EXECUTION_COST_MODEL, alloc), alloc);
  json.AddMember("model_revision",
                 rapidjson::Value(EXECUTION_COST_REVISION, alloc), alloc);
  json.AddMember("unit", rapidjson::Value(EXECUTION_COST_UNIT, alloc), alloc);
  json.AddMember("stage", "execution_preparation", alloc);
  json.AddMember("supported", estimate.supported, alloc);
  json.AddMember("work", estimate.work, alloc);
  json.AddMember("reason", rapidjson::Value(estimate.reason.c_str(), alloc),
                 alloc);
  json.AddMember("calibrated_to_time", false, alloc);
  json.AddMember("generic_value_words", VALUE_WORDS, alloc);
  json.AddMember("vertex_column_words", VERTEX_WORDS, alloc);
  json.AddMember("edge_column_words_upper_bound", EDGE_WORDS, alloc);
  json.AddMember("physical_column_tracking", true, alloc);
  rapidjson::Value assumptions(rapidjson::kArrayType);
  for (const auto& value : estimate.assumptions)
    assumptions.PushBack(rapidjson::Value(value.c_str(), alloc), alloc);
  json.AddMember("assumptions", assumptions, alloc);
  rapidjson::Value capabilities(rapidjson::kArrayType);
  for (const auto* capability :
       {"order_by_top_k", "typed_count_distinct", "bounded_string_distinct",
        "known_population_unwind",
        "finite_walk_and_source_domain_shortest_path", "uncorrelated_union",
        "opt_in_scalar_property_updates_without_auxiliary_maintenance"})
    capabilities.PushBack(rapidjson::Value(capability, alloc), alloc);
  json.AddMember("capabilities", capabilities, alloc);
  rapidjson::Value operators(rapidjson::kArrayType);
  for (const auto& item : estimate.operators) {
    rapidjson::Value op(rapidjson::kObjectType);
    op.AddMember("type", rapidjson::Value(item.type.c_str(), alloc), alloc);
    op.AddMember("input_rows", item.inputRows, alloc);
    op.AddMember("output_rows", item.outputRows, alloc);
    op.AddMember("local_work", item.localWork, alloc);
    operators.PushBack(op, alloc);
  }
  json.AddMember("operators", operators, alloc);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  json.Accept(writer);
  return buffer.GetString();
}
}  // namespace neug::planner

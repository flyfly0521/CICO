#include "neug/compiler/planner/join_order/cardinality_estimator.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_set>
#include "neug/compiler/binder/binder.h"
#include "neug/compiler/binder/expression/expression_util.h"
#include "neug/compiler/binder/expression/index_predicate_key.h"
#include "neug/compiler/binder/expression/literal_expression.h"
#include "neug/compiler/binder/expression/node_expression.h"
#include "neug/compiler/binder/rewriter/endpoint_projection_contract.h"
#include "neug/compiler/catalog/catalog_entry/fill_index_aux_info.h"
#include "neug/compiler/catalog/catalog_entry/node_table_catalog_entry.h"
#include "neug/compiler/catalog/catalog_entry/rel_table_catalog_entry.h"
#include "neug/compiler/gopt/g_alias_manager.h"
#include "neug/compiler/planner/operator/logical_filter.h"
#include "neug/compiler/planner/operator/logical_get_v.h"
#include "neug/compiler/planner/operator/scan/logical_dummy_scan.h"

#include "neug/compiler/binder/expression/expression.h"
#include "neug/compiler/binder/expression/property_expression.h"
#include "neug/compiler/binder/expression/scalar_function_expression.h"
#include "neug/compiler/binder/expression_evaluator_utils.h"
#include "neug/compiler/common/enums/expression_type.h"
#include "neug/compiler/main/client_context.h"
#include "neug/compiler/planner/join_order/join_order_util.h"
#include "neug/compiler/planner/operator/logical_aggregate.h"
#include "neug/compiler/planner/operator/logical_hash_join.h"
#include "neug/compiler/planner/operator/logical_projection.h"
#include "neug/compiler/planner/operator/scan/logical_scan_node_table.h"
#include "neug/compiler/storage/stats_manager.h"
#include "neug/compiler/storage/store/node_table.h"
#include "neug/compiler/storage/store/rel_table.h"
#include "neug/compiler/transaction/transaction.h"

using namespace neug::binder;
using namespace neug::common;
using namespace neug::transaction;

namespace neug {
namespace planner {

void PlanningCatalogLookup::beginPlanning() {
  enabled = true;
  transaction = nullptr;
  indexesLoaded = false;
  tables.clear();
  observedTableRows.clear();
  indexes.clear();
  correlatedSources.clear();
}

void PlanningCatalogLookup::checkTransaction(const Transaction* current) const {
  if (transaction == current && transactionID == current->getID() &&
      startTS == current->getStartTS()) return;
  // A caller can supply another transaction to a standalone estimator. Never
  // reuse either positive or negative visibility across that boundary, even
  // if a Transaction object was destroyed and a new one reuses its address.
  transaction = current;
  transactionID = current->getID();
  startTS = current->getStartTS();
  indexesLoaded = false;
  tables.clear();
  observedTableRows.clear();
  indexes.clear();
}

catalog::TableCatalogEntry* PlanningCatalogLookup::getTableCatalogEntry(
    const Transaction* current, table_id_t table) const {
  if (!enabled) return context->getCatalog()->getTableCatalogEntry(current, table);
  checkTransaction(current);
  const auto found = tables.find(table);
  if (found != tables.end()) return found->second;
  // Establish each pointer through the original version-chain-aware lookup.
  // Failed lookups retain the original exception and are not cached.
  auto* entry = context->getCatalog()->getTableCatalogEntry(current, table);
  tables.emplace(table, entry);
  return entry;
}

std::optional<uint64_t> PlanningCatalogLookup::getObservedTableCardinality(
    const Transaction* current, table_id_t table) const {
  const auto observe = [&]() -> std::optional<uint64_t> {
    auto stats = context->getStatsManager();
    // Retain the original observation/consistency checks, including the
    // version-chain-aware catalog lookups, on the first access to each table.
    if (!stats->hasTableCardinality(table)) return std::nullopt;
    return stats->getTable(table)->getNumTotalRows(current);
  };
  if (!enabled) return observe();
  checkTransaction(current);
  const auto found = observedTableRows.find(table);
  if (found != observedTableRows.end()) return found->second;
  const auto rows = observe();
  observedTableRows.emplace(table, rows);
  return rows;
}

std::vector<catalog::IndexCatalogEntry*>
PlanningCatalogLookup::getExtendedIndexEntriesForTable(table_id_t table) const {
  const auto current = context->getTransaction();
  if (!enabled) {
    auto entries = context->getCatalog()->getExtendedIndexEntries(current);
    std::erase_if(entries, [table](const auto* entry) { return entry->getTableID() != table; });
    return entries;
  }
  checkTransaction(current);
  if (!indexesLoaded) {
    // Preserve the original visible-entry order within each target. READY and
    // BUILDING checks remain in the proof rules and are not memoized here.
    for (auto* entry : context->getCatalog()->getExtendedIndexEntries(current))
      indexes[entry->getTableID()].push_back(entry);
    indexesLoaded = true;
  }
  const auto found = indexes.find(table);
  return found == indexes.end() ? std::vector<catalog::IndexCatalogEntry*>{} : found->second;
}

static cardinality_t atLeastOne(uint64_t x) { return x == 0 ? 1 : x; }

namespace {
using storage::EndpointDegreeKey;
using WideCount = __uint128_t;

// Observed zero is meaningful. Missing observations take the old estimation
// path; they must not turn into either a fabricated zero or an overflow wrap.
uint64_t observedRatio(uint64_t rows, WideCount numerator, WideCount denominator) {
  if (!rows || !numerator) return 0;
  if (!denominator) return std::numeric_limits<uint64_t>::max();
  WideCount result;
  if (numerator <= std::numeric_limits<WideCount>::max() / rows) {
    result = static_cast<WideCount>(rows) * numerator / denominator;
  } else {
    // A BOTH/BOTH moment sums up to four uint64 counters. Their product
    // with rows can exceed 128 bits even when the divided result fits.
    const auto whole = numerator / denominator;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    if (whole > maximum / rows) return maximum;
    result = whole * rows;
    const auto fraction = numerator % denominator;
    WideCount quotient = 0, remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
      quotient *= 2;
      remainder = remainder * 2 + (((rows >> bit) & 1) ? fraction : 0);
      quotient += remainder / denominator;
      remainder %= denominator;
    }
    result += quotient;
  }
  return result > std::numeric_limits<uint64_t>::max()
      ? std::numeric_limits<uint64_t>::max()
      : std::max<uint64_t>(1, static_cast<uint64_t>(result));
}

// An endpoint set is a sum of directed incidences, not a union of neighbor
// entities. BOTH retains parallel occurrences and both self-loop directions.
// Keep the scalar endpoint() proof below for contracts that require exactly
// one directed source; do not weaken those proofs to infer set-support unions.
std::vector<EndpointDegreeKey> extendEndpoints(
    const PlanningCatalogLookup& context, const LogicalExtend& extend, bool bound) {
  if (!context || extend.isRecursive()) return {};
  const auto nodes = (bound ? extend.getBoundNode() : extend.getNbrNode())->getTableIDs();
  const auto others = (bound ? extend.getNbrNode() : extend.getBoundNode())->getTableIDs();
  if (nodes.empty()) return {};
  // A union-label vertex (e.g. COMMENT:POST) gets one key per vertex table;
  // consumers sum degree moments per table (a vertex is in exactly one).
  const auto includesOther = [&](table_id_t id) {
    return std::find(others.begin(), others.end(), id) != others.end();
  };
  auto direction = extend.getDirection();
  if (!bound && direction != ExtendDirection::BOTH)
    direction = direction == ExtendDirection::FWD ? ExtendDirection::BWD : ExtendDirection::FWD;
  std::vector<EndpointDegreeKey> result;
  for (const auto node : nodes) {
    for (const auto tableID : extend.getRel()->getTableIDs()) {
      const auto* entry = context.getTableCatalogEntry(context->getTransaction(), tableID);
      if (entry->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY) return {};
      const auto& rel = entry->constCast<catalog::RelTableCatalogEntry>();
      if (direction != ExtendDirection::BWD && rel.getSrcTableID() == node && includesOther(rel.getDstTableID()))
        result.push_back({tableID, node, RelDataDirection::FWD});
      if (direction != ExtendDirection::FWD && rel.getDstTableID() == node && includesOther(rel.getSrcTableID()))
        result.push_back({tableID, node, RelDataDirection::BWD});
    }
  }
  return result;
}

struct EndpointTotals {
  uint64_t vertices;
  WideCount edges;
  uint64_t positive;  // Used only for a single endpoint's positive support.
};

struct PropertyEquality {
  const PropertyExpression* property;
  storage::PropertyValue value;
  const Expression* predicate;
  common::Value literalValue;
  // A PK scan stores only its lookup value. Keep a reconstructed typed
  // equality alive when recovering that constraint for downstream costing.
  std::shared_ptr<Expression> ownedPredicate;
};

std::optional<PropertyEquality> propertyEquality(
    const PlanningCatalogLookup& context, const Expression& predicate) {
  if (!context || predicate.expressionType != ExpressionType::EQUALS ||
      predicate.getNumChildren() != 2) return std::nullopt;
  auto property = predicate.getChild(0), literal = predicate.getChild(1);
  if (property->expressionType != ExpressionType::PROPERTY) std::swap(property, literal);
  if (property->expressionType != ExpressionType::PROPERTY ||
      !property->constCast<PropertyExpression>().isSingleLabel()) return std::nullopt;
  // Explicit CAST literals occur in production parameter templates. Restrict
  // evaluation to this closed constant form; never run arbitrary UDFs while
  // estimating a predicate.
  std::function<bool(const Expression&)> literalCast = [&](const Expression& expression) {
    if (expression.expressionType == ExpressionType::LITERAL) return true;
    if (expression.expressionType != ExpressionType::FUNCTION || !expression.getNumChildren() ||
        expression.constCast<ScalarFunctionExpression>().getFunction().name != "CAST") return false;
    for (const auto& child : expression.getChildren())
      if (!literalCast(*child)) return false;
    return true;
  };
  if (!literalCast(*literal)) return std::nullopt;
  try {
    const auto value = literal->expressionType == ExpressionType::LITERAL
        ? literal->constCast<LiteralExpression>().getValue()
        : evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(literal, context.getClientContext());
    if (value.isNull()) return std::nullopt;
    std::optional<storage::PropertyValue> key;
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::STRING: key = value.getValue<std::string>(); break;
    case LogicalTypeID::INT64: key = value.getValue<int64_t>(); break;
    case LogicalTypeID::INT32: key = static_cast<int64_t>(value.getValue<int32_t>()); break;
    case LogicalTypeID::BOOL: key = value.getValue<bool>(); break;
    default: break;
    }
    if (!key) return std::nullopt;
    return PropertyEquality{&property->constCast<PropertyExpression>(), std::move(*key),
        &predicate, value, nullptr};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> propertyEqualityKey(
    const PropertyEquality& equality, const std::string& ownerBinding) {
  auto predicate = equality.predicate->copy();
  const auto literalPosition = predicate->getChild(0)->expressionType == ExpressionType::PROPERTY ? 1u : 0u;
  predicate->setChild(literalPosition,
      std::make_shared<LiteralExpression>(equality.literalValue, "conditional_degree_literal"));
  return makeIndexPredicateKey(*predicate,
      {{equality.property->getVariableName(), ownerBinding}});
}

std::optional<PropertyEquality> primaryKeyEquality(const PlanningCatalogLookup& context,
    const LogicalScanNodeTable& scan, const std::string& owner) {
  if (!context || scan.getScanType() != LogicalScanNodeTableType::PRIMARY_KEY_SCAN ||
      scan.getAliasName() != owner || scan.getTableIDs().size() != 1) return std::nullopt;
  const auto* lookup = dynamic_cast<const PrimaryKeyScanInfo*>(scan.getExtraInfo());
  if (!lookup || !lookup->key) return std::nullopt;
  const auto tableID = scan.getTableIDs().front();
  const auto* entry = context.getTableCatalogEntry(context->getTransaction(), tableID);
  if (entry->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY) return std::nullopt;
  const auto& node = entry->constCast<catalog::NodeTableCatalogEntry>();
  if (node.getPrimaryKeyName().empty()) return std::nullopt;
  const auto& key = node.getPrimaryKeyDefinition();
  // List membership and context-dependent lookups are not scalar equalities.
  if (lookup->key->getDataType() != key.getType()) return std::nullopt;
  table_id_map_t<SingleLabelPropertyInfo> infos;
  infos.emplace(tableID, SingleLabelPropertyInfo(true, true));
  auto property = std::make_shared<PropertyExpression>(key.getType().copy(),
      key.getName(), owner, owner, std::move(infos));
  Binder binder(context.getClientContext());
  auto predicate = binder.getExpressionBinder()->createEqualityComparisonExpression(
      std::move(property), lookup->key->copy());
  auto equality = propertyEquality(context, *predicate);
  if (equality) equality->ownedPredicate = std::move(predicate);
  return equality;
}

std::optional<storage::PropertyValueStatistics> propertyObservation(
    const PlanningCatalogLookup& context, const PropertyEquality& equality) {
  return context->getStatsManager()->getPropertyValueStatistics(
      equality.property->getSingleTableID(), equality.property->getPropertyName(), equality.value);
}

std::optional<EndpointTotals> endpointTotals(
    const PlanningCatalogLookup& context, const std::vector<EndpointDegreeKey>& keys) {
  if (!context || keys.empty()) return std::nullopt;
  EndpointTotals result{};
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].nodeTableID != keys.front().nodeTableID) return std::nullopt;
    const auto observed = context->getStatsManager()->getEndpointDegreeStatistics(keys[i]);
    // Never price only the observed half of an undirected expansion.
    if (!observed || (i && observed->vertex_count != result.vertices)) return std::nullopt;
    result.vertices = observed->vertex_count;
    result.edges += observed->edge_count;
    if (i == 0) result.positive = observed->nonzero_vertex_count;
  }
  return result;
}

// Group-key proofs are local to this preparation. An arrival distribution or
// an estimated input cardinality does not establish which node keys occur.
// In particular, a filtered/optional/previously joined edge population must
// not borrow the complete source relationship's nonzero-owner count.
struct GroupNodeKey {
  const Expression* expression;
  std::string owner;
};

std::optional<GroupNodeKey> groupNodeKey(const Expression& expression) {
  if (ExpressionUtil::isNodePattern(expression))
    return GroupNodeKey{&expression, expression.getUniqueName()};
  if (expression.expressionType != ExpressionType::PROPERTY) return std::nullopt;
  const auto& property = expression.constCast<PropertyExpression>();
  const bool internalID = property.isInternalID() &&
      property.getDataType().getLogicalTypeID() == LogicalTypeID::INTERNAL_ID;
  if (!internalID && !(property.isSingleLabel() && property.isPrimaryKey()))
    return std::nullopt;
  // The origin walker below must still prove that the owner is a node, not a
  // relationship with an identically spelled _ID property.
  return GroupNodeKey{&expression, property.getVariableName()};
}

bool sameNodeTables(std::vector<table_id_t> left, std::vector<table_id_t> right) {
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right && std::adjacent_find(left.begin(), left.end()) == left.end();
}

bool projectsGroupNodeKey(const LogicalProjection& projection, const GroupNodeKey& key) {
  if (projection.append()) return true;
  for (const auto& expression : projection.getExpressionsToProject()) {
    if (expression->getUniqueName() == key.expression->getUniqueName() ||
        (ExpressionUtil::isNodePattern(*expression) &&
         expression->getUniqueName() == key.owner)) return true;
  }
  return false;
}

bool nodeIDHasOwner(const Expression& expression, const std::string& owner) {
  return expression.expressionType == ExpressionType::PROPERTY &&
      expression.constCast<PropertyExpression>().isInternalID() &&
      expression.constCast<PropertyExpression>().getVariableName() == owner &&
      expression.getDataType().getLogicalTypeID() == LogicalTypeID::INTERNAL_ID;
}

struct GroupNodeOrigin {
  std::vector<table_id_t> tables;
  bool nonNull;
};

bool matchingGroupGetV(const LogicalGetV& getV, const LogicalExtend& extend) {
  if (getV.getStartAliasName() != extend.getRel()->getUniqueName() ||
      getV.getNodeID()->getUniqueName() != extend.getNbrNode()->getInternalID()->getUniqueName() ||
      !sameNodeTables(getV.getTableIDs(), extend.getNbrNode()->getTableIDs())) return false;
  const auto expected = extend.getDirection() == ExtendDirection::FWD ? GetVOpt::END :
      extend.getDirection() == ExtendDirection::BWD ? GetVOpt::START : GetVOpt::OTHER;
  return getV.getGetVOpt() == expected ||
      (extend.getExtendOpt() == ExtendOpt::VERTEX && getV.getGetVOpt() == GetVOpt::ITSELF);
}

std::optional<GroupNodeOrigin> groupNodeOrigin(
    const LogicalOperator& input, const GroupNodeKey& key, unsigned depth = 0) {
  // A long or unsupported scope takes the old estimate, rather than spending
  // unbounded time resolving aliases or assuming nullable bindings are present.
  if (depth >= 64) return std::nullopt;
  switch (input.getOperatorType()) {
  case LogicalOperatorType::SCAN_NODE_TABLE: {
    const auto& scan = input.constCast<LogicalScanNodeTable>();
    if (nodeIDHasOwner(*scan.getNodeID(), key.owner))
      return GroupNodeOrigin{scan.getTableIDs(), true};
    return std::nullopt;
  }
  case LogicalOperatorType::PROJECTION:
    if (!projectsGroupNodeKey(input.constCast<LogicalProjection>(), key)) return std::nullopt;
    break;
  case LogicalOperatorType::FILTER:
  case LogicalOperatorType::FLATTEN:
    break;
  case LogicalOperatorType::EXTEND: {
    const auto& extend = input.constCast<LogicalExtend>();
    if (extend.isRecursive() || extend.getExtendOpt() == ExtendOpt::DEGREE) return std::nullopt;
    if (extend.getNbrNode()->getUniqueName() == key.owner)
      return GroupNodeOrigin{extend.getNbrNode()->getTableIDs(), !extend.isOptional()};
    if (extend.getBoundNode()->getUniqueName() == key.owner && !extend.isOptional())
      return GroupNodeOrigin{extend.getBoundNode()->getTableIDs(), true};
    // An optional expansion preserves the bound node's previous nullability;
    // it does not make that node non-NULL just by mentioning its variable.
    break;
  }
  case LogicalOperatorType::GET_V: {
    const auto& getV = input.constCast<LogicalGetV>();
    if (nodeIDHasOwner(*getV.getNodeID(), key.owner)) {
      bool nonNull = false;
      if (input.getNumChildren() == 1 &&
          input.getChild(0)->getOperatorType() == LogicalOperatorType::EXTEND) {
        const auto& extend = input.getChild(0)->constCast<LogicalExtend>();
        nonNull = !extend.isOptional() && matchingGroupGetV(getV, extend);
      }
      return GroupNodeOrigin{getV.getTableIDs(), nonNull};
    }
    break;
  }
  default:
    // ALIAS_MAP/UNION, joins and other scope-changing operators intentionally
    // stay outside this initial proof. Plain WITH aliases keep expression UIDs.
    return std::nullopt;
  }
  if (input.getNumChildren() != 1) return std::nullopt;
  return groupNodeOrigin(*input.getChild(0), key, depth + 1);
}

std::optional<uint64_t> groupNodeDomain(const PlanningCatalogLookup& context,
    const GroupNodeKey& key, const GroupNodeOrigin& origin) {
  if (!context || origin.tables.empty()) return std::nullopt;
  if (ExpressionUtil::isNodePattern(*key.expression)) {
    if (!sameNodeTables(key.expression->constCast<NodeExpression>().getTableIDs(), origin.tables))
      return std::nullopt;
  }
  const auto* property = key.expression->expressionType == ExpressionType::PROPERTY
      ? &key.expression->constCast<PropertyExpression>() : nullptr;
  auto stats = context->getStatsManager();
  WideCount count = 0;
  std::unordered_set<table_id_t> seen;
  for (const auto table : origin.tables) {
    if (!seen.insert(table).second || !stats->hasTableCardinality(table)) return std::nullopt;
    const auto* entry = context.getTableCatalogEntry(context->getTransaction(), table);
    if (entry->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY) return std::nullopt;
    const bool internalID = property && property->isInternalID() &&
        property->getDataType().getLogicalTypeID() == LogicalTypeID::INTERNAL_ID;
    if (property && !internalID &&
        (origin.tables.size() != 1 || !property->isSingleLabel() ||
         property->getSingleTableID() != table || !property->isPrimaryKey(table) ||
         property->getPropertyName() != entry->constCast<catalog::NodeTableCatalogEntry>().getPrimaryKeyName()))
      return std::nullopt;
    count += stats->getTable(table)->getNumTotalRows(context->getTransaction());
  }
  return static_cast<uint64_t>(std::min<WideCount>(count, std::numeric_limits<uint64_t>::max()));
}

bool hasNoGroupColumnPredicates(const std::vector<storage::ColumnPredicateSet>& predicates) {
  return std::all_of(predicates.begin(), predicates.end(),
      [](const auto& predicate) { return predicate.isEmpty(); });
}

const LogicalOperator* groupSupportRoot(const LogicalOperator& input, const GroupNodeKey& key) {
  const auto* current = &input;
  for (unsigned depth = 0; depth < 64; ++depth) {
    switch (current->getOperatorType()) {
    case LogicalOperatorType::PROJECTION:
      if (!projectsGroupNodeKey(current->constCast<LogicalProjection>(), key)) return nullptr;
      break;
    case LogicalOperatorType::FLATTEN:
      break;
    default:
      return current;
    }
    if (current->getNumChildren() != 1) return nullptr;
    current = current->getChild(0).get();
  }
  return nullptr;
}

const LogicalScanNodeTable* completeGroupNodeScan(
    const LogicalOperator& input, const GroupNodeKey& key) {
  const auto* root = groupSupportRoot(input, key);
  if (!root || root->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) return nullptr;
  const auto& scan = root->constCast<LogicalScanNodeTable>();
  if (scan.getNumChildren() || scan.getScanType() != LogicalScanNodeTableType::SCAN ||
      scan.isNodeIDScan() || scan.getExtraInfo() || scan.getPredicates() ||
      !hasNoGroupColumnPredicates(scan.getPropertyPredicates()) ||
      !nodeIDHasOwner(*scan.getNodeID(), key.owner)) return nullptr;
  return &scan;
}

bool groupInputMayBindNode(const LogicalOperator& input, const NodeExpression& node) {
  const auto contains = [&](const expression_vector& expressions) {
    return std::any_of(expressions.begin(), expressions.end(), [&](const auto& expression) {
      return expression->getUniqueName() == node.getUniqueName() ||
          (expression->expressionType == ExpressionType::PROPERTY &&
           expression->template constCast<PropertyExpression>().getVariableName() == node.getUniqueName());
    });
  };
  const auto* current = &input;
  for (unsigned depth = 0; depth < 64; ++depth) {
    if (current->getSchema() && contains(current->getSchema()->getExpressionsInScope())) return true;
    if (current->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE)
      return nodeIDHasOwner(*current->constCast<LogicalScanNodeTable>().getNodeID(), node.getUniqueName());
    if (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
      if (contains(current->constCast<LogicalProjection>().getExpressionsToProject())) return true;
    } else if (current->getOperatorType() != LogicalOperatorType::FLATTEN) {
      return true;
    }
    if (current->getNumChildren() != 1) return true;
    current = current->getChild(0).get();
  }
  return true;
}

std::optional<uint64_t> completeNodeGroupSupport(const PlanningCatalogLookup& context,
    const LogicalOperator& input, const GroupNodeKey& key, uint64_t domain) {
  if (const auto* scan = completeGroupNodeScan(input, key)) {
    const auto observed = groupNodeDomain(context, key, {scan->getTableIDs(), true});
    if (observed && *observed == domain) return domain;
  }
  const auto* root = groupSupportRoot(input, key);
  if (!root) return std::nullopt;
  const LogicalGetV* getV = nullptr;
  if (root->getOperatorType() == LogicalOperatorType::GET_V) {
    getV = &root->constCast<LogicalGetV>();
    if (getV->getPredicates() || root->getNumChildren() != 1) return std::nullopt;
    root = root->getChild(0).get();
  }
  if (root->getOperatorType() != LogicalOperatorType::EXTEND) return std::nullopt;
  const auto& extend = root->constCast<LogicalExtend>();
  if (extend.getNumChildren() != 1 || extend.isRecursive() || extend.isOptional() ||
      extend.getExtendOpt() == ExtendOpt::DEGREE || extend.getPredicates() ||
      !hasNoGroupColumnPredicates(extend.getPropertyPredicates()) ||
      extend.getBoundNode()->getUniqueName() == extend.getNbrNode()->getUniqueName() ||
      (getV && !matchingGroupGetV(*getV, extend))) return std::nullopt;
  const bool bound = key.owner == extend.getBoundNode()->getUniqueName();
  const bool neighbor = key.owner == extend.getNbrNode()->getUniqueName();
  if (!bound && !neighbor) return std::nullopt;
  if (neighbor && !getV && extend.getExtendOpt() == ExtendOpt::EDGE && !extend.shouldScanNbrID())
    return std::nullopt;
  const auto boundKey = groupNodeKey(*extend.getBoundNode());
  if (!boundKey) return std::nullopt;
  const auto* scan = completeGroupNodeScan(*extend.getChild(0), *boundKey);
  if (!scan || !sameNodeTables(scan->getTableIDs(), extend.getBoundNode()->getTableIDs()) ||
      groupInputMayBindNode(*extend.getChild(0), *extend.getNbrNode()))
    return std::nullopt;
  // This root is one full scan of the bound variable, with no other pattern
  // operator. Along with different UIDs it proves the neighbor is fresh.
  const auto keys = extendEndpoints(context, extend, bound);
  if (keys.empty() || keys.size() > 2) return std::nullopt;
  const auto owner = bound ? extend.getBoundNode() : extend.getNbrNode();
  if (owner->getTableIDs().size() != 1) return std::nullopt;
  const auto ownerTable = owner->getTableIDs().front();
  auto stats = context->getStatsManager();
  std::vector<storage::EndpointDegreeStatistics> observed;
  for (const auto& endpoint : keys) {
    if (endpoint.nodeTableID != ownerTable) return std::nullopt;
    const auto value = stats->getEndpointDegreeStatistics(endpoint);
    // StatsManager validates edge counts against the supplied typed source
    // snapshot. Recheck the owner/domain bound here, including the single
    // incidence case, before using support as a group estimate.
    if (!value || value->vertex_count != domain ||
        value->nonzero_vertex_count > domain || value->nonzero_vertex_count > value->edge_count)
      return std::nullopt;
    observed.push_back(*value);
  }
  if (observed.size() == 1) return observed.front().nonzero_vertex_count;
  const auto overlap = stats->getEndpointPairStatistics(keys[0], keys[1]);
  if (!overlap || overlap->positive_both_count >
      std::min(observed[0].nonzero_vertex_count, observed[1].nonzero_vertex_count)) return std::nullopt;
  const auto reverse = stats->getEndpointPairStatistics(keys[1], keys[0]);
  if (reverse && reverse->positive_both_count != overlap->positive_both_count) return std::nullopt;
  const WideCount support = static_cast<WideCount>(observed[0].nonzero_vertex_count) +
      observed[1].nonzero_vertex_count - overlap->positive_both_count;
  return support <= domain ? std::make_optional(static_cast<uint64_t>(support)) : std::nullopt;
}

bool observedEmptyGroupInput(const PlanningCatalogLookup& context,
    const LogicalOperator& input, unsigned depth = 0) {
  // Supplied zero statistics affect costing only. This helper is never an
  // EMPTY_RESULT rewrite or a claim that stale/external statistics are true.
  if (depth >= 64) return false;
  if (input.getOperatorType() == LogicalOperatorType::EMPTY_RESULT) return true;
  const auto observedEmpty = [&](const std::vector<table_id_t>& tables) {
    if (!context || tables.empty()) return false;
    auto stats = context->getStatsManager();
    return std::all_of(tables.begin(), tables.end(), [&](table_id_t table) {
      return stats->hasTableCardinality(table) &&
          stats->getTable(table)->getNumTotalRows(context->getTransaction()) == 0;
    });
  };
  switch (input.getOperatorType()) {
  case LogicalOperatorType::SCAN_NODE_TABLE:
    return observedEmpty(input.constCast<LogicalScanNodeTable>().getTableIDs());
  case LogicalOperatorType::EXTEND: {
    const auto& extend = input.constCast<LogicalExtend>();
    if (extend.isRecursive() || extend.getExtendOpt() == ExtendOpt::DEGREE) return false;
    if (!extend.isOptional() && observedEmpty(extend.getRel()->getTableIDs())) return true;
    break;
  }
  case LogicalOperatorType::PROJECTION:
  case LogicalOperatorType::FILTER:
  case LogicalOperatorType::FLATTEN:
  case LogicalOperatorType::GET_V:
    break;
  default:
    return false;
  }
  return input.getNumChildren() == 1 &&
      observedEmptyGroupInput(context, *input.getChild(0), depth + 1);
}

std::optional<EndpointDegreeKey> endpoint(
    const PlanningCatalogLookup& context, const std::vector<table_id_t>& rels,
    table_id_t node, table_id_t other, ExtendDirection direction) {
  if (!context || rels.size() != 1) return std::nullopt;
  const auto* entry = context.getTableCatalogEntry(
      context->getTransaction(), rels.front());
  if (entry->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY)
    return std::nullopt;
  const auto& rel = entry->constCast<catalog::RelTableCatalogEntry>();
  if (node == other) {
    // This scalar source-contract proof requires one direction. General
    // expansion uses extendEndpoints() and complete IN+OUT moments instead.
    if (direction == ExtendDirection::BOTH) return std::nullopt;
    if (rel.getSrcTableID() != node || rel.getDstTableID() != node)
      return std::nullopt;
    return EndpointDegreeKey{rels.front(), node, direction == ExtendDirection::FWD
        ? RelDataDirection::FWD : RelDataDirection::BWD};
  }
  if (rel.getSrcTableID() == node && rel.getDstTableID() == other)
    return EndpointDegreeKey{rels.front(), node, RelDataDirection::FWD};
  if (rel.getDstTableID() == node && rel.getSrcTableID() == other)
    return EndpointDegreeKey{rels.front(), node, RelDataDirection::BWD};
  return std::nullopt;
}

std::optional<EndpointDegreeKey> extendEndpoint(
    const PlanningCatalogLookup& context, const LogicalExtend& extend, bool bound) {
  if (extend.isRecursive() || extend.getBoundNode()->getTableIDs().size() != 1 ||
      extend.getNbrNode()->getTableIDs().size() != 1) return std::nullopt;
  auto direction = extend.getDirection();
  if (!bound && direction != ExtendDirection::BOTH)
    direction = direction == ExtendDirection::FWD ? ExtendDirection::BWD : ExtendDirection::FWD;
  const auto node = bound ? extend.getBoundNode() : extend.getNbrNode();
  const auto other = bound ? extend.getNbrNode() : extend.getBoundNode();
  return endpoint(context, extend.getRel()->getTableIDs(), node->getTableIDs()[0],
      other->getTableIDs()[0], direction);
}

std::string upperNoSpace(std::string value) {
  std::erase_if(value, [](unsigned char c) { return std::isspace(c); });
  std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return std::toupper(c); });
  return value;
}

struct Presence {
  EndpointDegreeKey endpoint;
  std::string variable;
};

bool matchesFusedNode(const Presence& presence, const std::string& variable,
                      const std::vector<table_id_t>& tables) {
  // RenameDependentVarOpt replaces only a fused operator's output binding
  // with the physical head sentinel. Recover that binding from the operator,
  // and require its typed domain as well; a head is not an arbitrary node.
  return tables.size() == 1 && tables.front() == presence.endpoint.nodeTableID &&
      (presence.variable == variable || presence.variable == gopt::DEFAULT_ALIAS_NAME);
}

std::optional<Presence> countPresence(const PlanningCatalogLookup& context,
                                      const Expression& predicate) {
  if (!context || predicate.expressionType != ExpressionType::NOT_EQUALS ||
      predicate.getNumChildren() != 2) return std::nullopt;
  const Expression* property = predicate.getChild(0).get();
  const Expression* literal = predicate.getChild(1).get();
  if (property->expressionType == ExpressionType::LITERAL) std::swap(property, literal);
  if (property->expressionType != ExpressionType::PROPERTY ||
      literal->expressionType != ExpressionType::LITERAL) return std::nullopt;
  const auto zero = literal->constCast<LiteralExpression>().getValue();
  if (zero.isNull() || zero.getDataType().getLogicalTypeID() != LogicalTypeID::INT64 ||
      zero.getValue<int64_t>() != 0) return std::nullopt;
  const auto& prop = property->constCast<PropertyExpression>();
  if (!prop.isSingleLabel()) return std::nullopt;
  for (auto* entry : context.getExtendedIndexEntriesForTable(prop.getSingleTableID())) {
    if (entry->getIndexType() != "FILL" || entry->getTableID() != prop.getSingleTableID() ||
        context->getCatalog()->isExtendedIndexBuilding(entry->getName())) continue;
    // Reopened READY indexes can retain their serialized auxiliary buffer.
    // Decode locally just as the rewriter does: loading state is not index
    // readiness, and costing must not mutate the shared persistent catalog.
    std::unique_ptr<catalog::FillIndexAuxInfo> restored;
    const catalog::FillIndexAuxInfo* info;
    if (entry->isLoaded()) {
      info = dynamic_cast<const catalog::FillIndexAuxInfo*>(&entry->getAuxInfo());
    } else {
      restored.reset(catalog::FillIndexAuxInfo::deserialize(entry->getAuxBufferReader()));
      info = restored.get();
    }
    if (!info || info->getFillType() != catalog::FillEntryType::NODE || info->isStagedPipeline() ||
        info->getPredicateExpr() || !info->getPredicateConjuncts().empty() ||
        !info->getExistsPredicates().empty()) continue;
    // Unknown contract markers may encode filters or unsupported source
    // shapes; only the plain predicate-free single-edge COUNT is proven here.
    if (!info->getPredicateTemplates().empty()) continue;
    const auto& pattern = info->getPatternIR();
    if (pattern.nodesInfo.size() != 2 || pattern.relsInfo.size() != 1) continue;
    const auto& rel = pattern.relsInfo.front();
    if (rel.relType != QueryRelType::NON_RECURSIVE || rel.srcNodeIdx >= 2 || rel.dstNodeIdx >= 2 ||
        rel.srcNodeIdx == rel.dstNodeIdx) continue;
    size_t anchor = 2;
    for (size_t i = 0; i < 2; ++i)
      if (pattern.nodesInfo[i].uniqName == info->getTableUniqName()) anchor = i;
    if (anchor == 2 || pattern.nodesInfo[anchor].nodeLabels.size() != 1 ||
        pattern.nodesInfo[1-anchor].nodeLabels.size() != 1 ||
        pattern.nodesInfo[anchor].nodeLabels[0] != prop.getSingleTableID()) continue;
    for (size_t i = 0; i < info->getTargetColumns().size() && i < info->getIndexColumnInfos().size(); ++i) {
      if (info->getTargetColumns()[i] != prop.getPropertyName() ||
          info->getTargetColumns()[i].starts_with("__neug_fill_") || !info->getIndexColumnInfos()[i] ||
          info->getIndexColumnInfos()[i]->columnType != catalog::IndexColumnType::NODE_AGGREGATE) continue;
      const auto& column = static_cast<const catalog::NodeAggregationIndexColnumInfo&>(
          *info->getIndexColumnInfos()[i]);
      if (upperNoSpace(column.nameTemplate) != "COUNT(*)" ||
          (upperNoSpace(column.aggregateFuncName) != "COUNT" &&
           upperNoSpace(column.aggregateFuncName) != "COUNT_STAR")) continue;
      auto direction = rel.edgeType == EdgeType::UNDIRECTED ? ExtendDirection::BOTH
          : (anchor == rel.srcNodeIdx ? ExtendDirection::FWD : ExtendDirection::BWD);
      auto key = endpoint(context, rel.relLabels, prop.getSingleTableID(),
          pattern.nodesInfo[1-anchor].nodeLabels[0], direction);
      if (key) return Presence{*key, prop.getVariableName()};
    }
  }
  return std::nullopt;
}

struct EndpointDistribution {
  std::vector<EndpointDegreeKey> endpoints;
  bool presence;  // false: degree-weighted arrival; true: uniform positive support.
};

struct DerivedRelationshipContract {
  std::optional<EndpointDegreeKey> sourceBound;
  std::optional<EndpointDegreeKey> sourceNeighbor;
  std::vector<std::string> publicCountColumns;
};

// Registration emits this marker only after normalizeBagRelationPattern has
// proved a complete, ordered pair of typed node identities (or their primary
// keys). Any intermediate WITH carries exactly those identities, and cannot
// aggregate, order, limit, introduce another MATCH, or apply a scalar filter.
// MATCH/inline predicates have separate templates, so accepting exactly this
// one marker excludes them too. Do not infer this contract from SQL spelling.
bool unfilteredEndpointSetSource(const PlanningCatalogLookup& context,
    const catalog::FillIndexAuxInfo& info, table_id_t target) {
  if (info.getFillType() != catalog::FillEntryType::REL ||
      info.isStagedPipeline() || info.isFillUndirectedRel() ||
      !info.getTargetColumns().empty() || !info.getIndexColumnInfos().empty() ||
      info.getPredicateExpr() || !info.getPredicateConjuncts().empty() ||
      !info.getExistsPredicates().empty() ||
      info.getPredicateTemplates().size() != 1 ||
      info.getPredicateTemplates().front() != ENDPOINT_SET_CONTRACT)
    return false;
  const auto& pattern = info.getPatternIR();
  if (pattern.nodesInfo.size() != 2 || pattern.relsInfo.size() != 1 ||
      pattern.nodesInfo[0].uniqName == pattern.nodesInfo[1].uniqName ||
      pattern.nodesInfo[0].nodeLabels.size() != 1 ||
      pattern.nodesInfo[1].nodeLabels.size() != 1)
    return false;
  const auto& source = pattern.relsInfo.front();
  if (source.relType != QueryRelType::NON_RECURSIVE ||
      source.relLabels.size() != 1 || source.srcNodeIdx >= 2 ||
      source.dstNodeIdx >= 2 || source.srcNodeIdx == source.dstNodeIdx ||
      (source.edgeType != EdgeType::OUTGOING && source.edgeType != EdgeType::INCOMING))
    return false;
  // The materialization must have one definition, and the inherited table
  // must be a base relationship, not another independently maintained FILL.
  for (const auto* entry : context.getExtendedIndexEntriesForTable(source.relLabels.front()))
    if (entry->getIndexType() == "FILL") return false;
  const auto definitions = context.getExtendedIndexEntriesForTable(target);
  return std::count_if(definitions.begin(), definitions.end(), [](const auto* entry) {
    return entry->getIndexType() == "FILL";
  }) == 1;
}

// A group's source distribution is independent of a query's COUNT filter.
// A property-free endpoint set can have the same distribution, but never
// acquires a COUNT-presence contract: publicCountColumns stays empty for it.
std::optional<DerivedRelationshipContract> derivedRelationshipContract(
    const PlanningCatalogLookup& context, const LogicalExtend& extend) {
  if (!context || extend.isRecursive() || extend.isOptional() ||
      extend.getRel()->getTableIDs().size() != 1) return std::nullopt;
  const auto target = extend.getRel()->getTableIDs().front();
  for (auto* entry : context.getExtendedIndexEntriesForTable(target)) {
    if (entry->getIndexType() != "FILL" || entry->getTableID() != target ||
        context->getCatalog()->isExtendedIndexBuilding(entry->getName())) continue;
    std::unique_ptr<catalog::FillIndexAuxInfo> restored;
    const catalog::FillIndexAuxInfo* info;
    if (entry->isLoaded()) {
      info = dynamic_cast<const catalog::FillIndexAuxInfo*>(&entry->getAuxInfo());
    } else {
      restored.reset(catalog::FillIndexAuxInfo::deserialize(entry->getAuxBufferReader()));
      info = restored.get();
    }
    if (!info || info->getFillType() != catalog::FillEntryType::REL ||
        info->isStagedPipeline() || info->getSrcNodeUniqName().empty() ||
        info->getDstNodeUniqName().empty()) continue;
    const bool endpointSet = unfilteredEndpointSetSource(context, *info, target);
    if (!endpointSet && (info->usesWorkloadCompatibility() ||
        info->getAggregateForm() != catalog::FillAggregateForm::CANONICAL_BASE ||
        info->getRowCountColumn().empty())) continue;
    const auto& pattern = info->getPatternIR();
    // A materialized relationship already in the source pattern is a carrier
    // edge. Its lifetime is independent of this aggregate's group existence.
    if (std::any_of(pattern.relsInfo.begin(), pattern.relsInfo.end(), [&](const auto& rel) {
          return std::find(rel.relLabels.begin(), rel.relLabels.end(), target) != rel.relLabels.end();
        })) continue;
    DerivedRelationshipContract result;
    bool hiddenCount = false;
    for (size_t i = 0; !endpointSet && i < info->getTargetColumns().size() && i < info->getIndexColumnInfos().size(); ++i) {
      const auto& metadata = info->getIndexColumnInfos()[i];
      if (!metadata || metadata->columnType != catalog::IndexColumnType::REL_AGGREGATE) continue;
      const auto& column = static_cast<const catalog::RelIndexColnumInfo&>(*metadata);
      if (upperNoSpace(column.nameTemplate) != "COUNT(*)" ||
          (upperNoSpace(column.aggregateFuncName) != "COUNT" &&
           upperNoSpace(column.aggregateFuncName) != "COUNT_STAR")) continue;
      const auto& name = info->getTargetColumns()[i];
      hiddenCount |= name == info->getRowCountColumn();
      if (!name.starts_with("__neug_fill_")) result.publicCountColumns.push_back(name);
    }
    if (!endpointSet && (result.publicCountColumns.empty() || !hiddenCount)) continue;
    // Grouping source rows by both endpoints removes parallel edges. Reuse a
    // source degree distribution only with an observed ordered-pair uniqueness
    // proof. Without it, actual derived statistics remain usable; hypothetical
    // catalogs retain the ordinary missing-statistics fallback.
    if (pattern.nodesInfo.size() != 2 || pattern.relsInfo.size() != 1 ||
        info->getPredicateExpr() || !info->getPredicateConjuncts().empty() ||
        !info->getExistsPredicates().empty() ||
        (!endpointSet && !info->getPredicateTemplates().empty())) return result;
    const auto& sourceRel = pattern.relsInfo.front();
    if (sourceRel.relType != QueryRelType::NON_RECURSIVE || sourceRel.srcNodeIdx >= 2 ||
        sourceRel.dstNodeIdx >= 2 || sourceRel.srcNodeIdx == sourceRel.dstNodeIdx) return result;
    size_t source = 2, destination = 2;
    for (size_t i = 0; i < 2; ++i) {
      if (pattern.nodesInfo[i].uniqName == info->getSrcNodeUniqName()) source = i;
      if (pattern.nodesInfo[i].uniqName == info->getDstNodeUniqName()) destination = i;
    }
    if (source == 2 || destination == 2 || source == destination ||
        pattern.nodesInfo[source].nodeLabels.size() != 1 ||
        pattern.nodesInfo[destination].nodeLabels.size() != 1) return result;
    auto sourceEndpoint = [&](bool bound) -> std::optional<EndpointDegreeKey> {
      const auto physical = extendEndpoint(context, extend, bound);
      if (!physical) return std::nullopt;
      const auto position = physical->direction == RelDataDirection::FWD ? source : destination;
      const auto other = physical->direction == RelDataDirection::FWD ? destination : source;
      const auto otherNode = bound ? extend.getNbrNode() : extend.getBoundNode();
      if (pattern.nodesInfo[position].nodeLabels.front() != physical->nodeTableID ||
          otherNode->getTableIDs().size() != 1 ||
          pattern.nodesInfo[other].nodeLabels.front() != otherNode->getTableIDs().front())
        return std::nullopt;
      auto direction = sourceRel.edgeType == EdgeType::UNDIRECTED ? ExtendDirection::BOTH :
          (position == sourceRel.srcNodeIdx ? ExtendDirection::FWD : ExtendDirection::BWD);
      if (sourceRel.edgeType == EdgeType::INCOMING)
        direction = direction == ExtendDirection::FWD ? ExtendDirection::BWD : ExtendDirection::FWD;
      auto key = endpoint(context, sourceRel.relLabels, physical->nodeTableID,
          pattern.nodesInfo[other].nodeLabels.front(), direction);
      if (!key) return std::nullopt;
      const auto observed = context->getStatsManager()->getEndpointDegreeStatistics(*key);
      if (!observed || !observed->distinct_endpoint_pair_count ||
          *observed->distinct_endpoint_pair_count != observed->edge_count) return std::nullopt;
      return key;
    };
    result.sourceBound = sourceEndpoint(true);
    result.sourceNeighbor = sourceEndpoint(false);
    return result;
  }
  return std::nullopt;
}

// This is only a costing proof, never predicate elimination. An independently
// maintained group exists while its hidden row count is positive, and a public
// exact COUNT(*) has the same contributions. A different property, comparison,
// literal or binding must still receive its normal filter estimate.
bool isDerivedCountPresence(const LogicalExtend& extend,
                            const DerivedRelationshipContract& contract) {
  if (!extend.getPredicates()) return false;
  const auto& predicate = *extend.getPredicates();
  if (predicate.expressionType != ExpressionType::NOT_EQUALS ||
      predicate.getNumChildren() != 2) return false;
  const Expression* property = predicate.getChild(0).get();
  const Expression* literal = predicate.getChild(1).get();
  if (property->expressionType == ExpressionType::LITERAL) std::swap(property, literal);
  if (property->expressionType != ExpressionType::PROPERTY ||
      literal->expressionType != ExpressionType::LITERAL) return false;
  const auto zero = literal->constCast<LiteralExpression>().getValue();
  if (zero.isNull() || zero.getDataType().getLogicalTypeID() != LogicalTypeID::INT64 ||
      zero.getValue<int64_t>() != 0) return false;
  const auto& prop = property->constCast<PropertyExpression>();
  return prop.isSingleLabel() &&
      prop.getSingleTableID() == extend.getRel()->getTableIDs().front() &&
      (prop.getVariableName() == extend.getRel()->getUniqueName() ||
       prop.getVariableName() == gopt::DEFAULT_ALIAS_NAME) &&
      std::find(contract.publicCountColumns.begin(), contract.publicCountColumns.end(),
          prop.getPropertyName()) != contract.publicCountColumns.end();
}

// A first-order conditional estimate, not a complete correlation model. Stop
// at joins/aggregates and unmodelled filtered arrivals rather than carrying a
// distribution through an unrelated binding or a changed query scope.
std::optional<EndpointDistribution> distribution(
    const PlanningCatalogLookup& context, const LogicalOperator& op, const std::string& variable) {
  switch (op.getOperatorType()) {
  case LogicalOperatorType::SCAN_NODE_TABLE: {
    const auto& scan = op.constCast<LogicalScanNodeTable>();
    if (!scan.getPredicates()) return std::nullopt;
    auto presence = countPresence(context, *scan.getPredicates());
    if (presence && scan.getNodeID() &&
        scan.getNodeID()->expressionType == ExpressionType::PROPERTY &&
        scan.getAliasName() == variable &&
        matchesFusedNode(*presence, variable, scan.getTableIDs()))
      return EndpointDistribution{{presence->endpoint}, true};
    return std::nullopt;
  }
  case LogicalOperatorType::EXTEND: {
    const auto& extend = op.constCast<LogicalExtend>();
    const bool bound = extend.getBoundNode()->getUniqueName() == variable;
    const bool nbr = extend.getNbrNode()->getUniqueName() == variable;
    if (bound || nbr) {
      // OPTIONAL NULL-extends unmatched rows; the matched neighbor values
      // still arrive through the edge. A bound vertex is weighted by
      // max(1, degree) instead, which pair moments do not describe.
      if (extend.isOptional() && !nbr) return std::nullopt;
      // A functional edge from the bound vertex (every bound vertex has at
      // most one: edges == vertices having one) does not reweight it: keep
      // the earlier arrival distribution (LDBC: message -HASCREATOR-> person
      // after the message arrived through HASTAG).
      if (bound && !extend.getPredicates() && extend.getExtendOpt() != ExtendOpt::DEGREE &&
          !derivedRelationshipContract(context, extend)) {
        const auto keys = extendEndpoints(context, extend, true);
        const bool functional = !keys.empty() && std::all_of(keys.begin(), keys.end(),
            [&](const auto& key) {
              const auto observed = context->getStatsManager()->getEndpointDegreeStatistics(key);
              return observed && observed->edge_count == observed->nonzero_vertex_count;
            });
        if (functional && extend.getNumChildren() == 1)
          if (auto earlier = distribution(context, *extend.getChild(0), variable)) return earlier;
      }
      const auto count = derivedRelationshipContract(context, extend);
      if (extend.getPredicates() && (!count || !isDerivedCountPresence(extend, *count)))
        return std::nullopt;
      if (count) {
        const auto& source = bound ? count->sourceBound : count->sourceNeighbor;
        if (source) return EndpointDistribution{{*source}, false};
      }
      auto keys = extendEndpoints(context, extend, bound);
      return keys.empty() ? std::nullopt :
          std::make_optional(EndpointDistribution{std::move(keys), false});
    }
    break;
  }
  case LogicalOperatorType::GET_V:
    if (op.constCast<LogicalGetV>().getPredicates()) return std::nullopt;
    break;
  case LogicalOperatorType::FILTER:
    // A conditional third relation needs a higher-order moment. Keep the old
    // fallback instead of pretending that pair moments describe that state.
    return std::nullopt;
  case LogicalOperatorType::PROJECTION:
  case LogicalOperatorType::FLATTEN:
  case LogicalOperatorType::ALIAS_MAP:
  // A DISTINCT over a small arrival sample keeps (approximately) the arrival
  // weighting of each surviving vertex (BI6: 1227 likers -> 1193 distinct).
  case LogicalOperatorType::DISTINCT: break;
  case LogicalOperatorType::HASH_JOIN:
  case LogicalOperatorType::CROSS_PRODUCT: {
    // A vertex variable is produced by exactly one input; a match multiplies
    // rows uniformly on the other side's key, keeping this arrival weighting.
    for (uint32_t i = 0; i < op.getNumChildren(); ++i)
      if (auto found = distribution(context, *op.getChild(i), variable)) return found;
    return std::nullopt;
  }
  case LogicalOperatorType::EXPRESSIONS_SCAN: {
    // A correlated subquery input: inherit from the outer producer.
    const auto& sources = context.getCorrelatedSources();
    for (auto it = sources.rbegin(); it != sources.rend(); ++it)
      if (auto inherited = distribution(context, **it, variable)) return inherited;
    return std::nullopt;
  }
  default: return std::nullopt;
  }
  if (op.getNumChildren() != 1) return std::nullopt;
  return distribution(context, *op.getChild(0), variable);
}

std::optional<uint64_t> extensionRows(const PlanningCatalogLookup& context, uint64_t rows,
    const std::vector<EndpointDegreeKey>& current,
    const std::optional<EndpointDistribution>& previous) {
  // Union-label variables (e.g. COMMENT:POST) are a disjoint union of vertex
  // tables: degree moments are summed per table. A vertex never belongs to two
  // tables, so cross-table endpoint pairs contribute nothing.
  std::map<table_id_t, std::vector<EndpointDegreeKey>> now, before;
  for (const auto& key : current) now[key.nodeTableID].push_back(key);
  if (now.empty()) return std::nullopt;
  if (!previous) {
    WideCount edges = 0, vertices = 0;
    for (const auto& [table, keys] : now) {
      const auto observed = endpointTotals(context, keys);
      if (!observed) return std::nullopt;
      edges += observed->edges;
      vertices += observed->vertices;
    }
    if (!edges) return uint64_t{0};
    if (!vertices && rows) return std::nullopt;
    return observedRatio(rows, edges, vertices);
  }
  for (const auto& key : previous->endpoints) before[key.nodeTableID].push_back(key);
  if (before.size() != now.size()) return std::nullopt;
  WideCount numerator = 0, denominator = 0;
  for (const auto& [table, keys] : now) {
    const auto found = before.find(table);
    if (found == before.end()) return std::nullopt;
    const auto observed = endpointTotals(context, keys);
    const auto prior = endpointTotals(context, found->second);
    if (!observed || !prior || prior->vertices != observed->vertices ||
        (previous->presence && found->second.size() != 1)) return std::nullopt;
    denominator += previous->presence ? WideCount{prior->positive} : prior->edges;
    for (const auto& first : found->second) {
      for (const auto& second : keys) {
        const auto pair = context->getStatsManager()->getEndpointPairStatistics(first, second);
        if (!pair) return std::nullopt;
        numerator += previous->presence ? pair->present_first_degree_second_sum
                                        : pair->degree_product_sum;
      }
    }
  }
  if (!denominator) return rows ? std::nullopt : std::make_optional(uint64_t{0});
  return observedRatio(rows, numerator, denominator);
}

std::optional<uint64_t> presenceRows(const PlanningCatalogLookup& context,
    uint64_t rows, const Expression& predicate, const LogicalOperator* input,
    const std::optional<EndpointDistribution>& forced = std::nullopt) {
  const auto presence = countPresence(context, predicate);
  if (!presence) return std::nullopt;
  auto stats = context->getStatsManager();
  const auto observed = stats->getEndpointDegreeStatistics(presence->endpoint);
  if (!observed) return std::nullopt;
  if (!observed->nonzero_vertex_count) return uint64_t{0};
  auto prior = forced;
  if (!prior && input) prior = distribution(context, *input, presence->variable);
  if (prior) {
    const auto priorStats = endpointTotals(context, prior->endpoints);
    if (!priorStats || priorStats->vertices != observed->vertex_count ||
        prior->endpoints.front().nodeTableID != presence->endpoint.nodeTableID ||
        (prior->presence && prior->endpoints.size() != 1)) return std::nullopt;
    WideCount numerator = 0;
    for (const auto& key : prior->endpoints) {
      const auto pair = stats->getEndpointPairStatistics(presence->endpoint, key);
      if (!pair) return std::nullopt;
      numerator += prior->presence ? pair->positive_both_count : pair->present_first_degree_second_sum;
    }
    const auto denominator = prior->presence ? WideCount{priorStats->positive} : priorStats->edges;
    if (!denominator) return rows == 0 ? std::make_optional(uint64_t{0}) : std::nullopt;
    return observedRatio(rows, numerator, denominator);
  }
  if (!observed->vertex_count) return rows == 0 ? std::make_optional(uint64_t{0}) : std::nullopt;
  return observedRatio(rows, observed->nonzero_vertex_count, observed->vertex_count);
}


// A projection FILL's BOOL presence flag is true exactly for anchors with a
// complete source row. Estimate its support from collected endpoint degree
// statistics along the source tree: each hop keeps the fraction of its start
// vertices having at least one edge (independence across hops). Without this
// the flag falls back to the generic 0.5 selectivity although, e.g., every
// LDBC person has a city and every city a country.
std::optional<uint64_t> projectionPresenceRows(const PlanningCatalogLookup& context,
                                               uint64_t rows, const Expression& predicate) {
  if (!context || predicate.expressionType != ExpressionType::PROPERTY ||
      predicate.getDataType().getLogicalTypeID() != LogicalTypeID::BOOL) return std::nullopt;
  const auto& prop = predicate.constCast<PropertyExpression>();
  if (!prop.isSingleLabel()) return std::nullopt;
  auto stats = context->getStatsManager();
  for (auto* entry : context.getExtendedIndexEntriesForTable(prop.getSingleTableID())) {
    if (entry->getIndexType() != "FILL" || entry->getTableID() != prop.getSingleTableID() ||
        context->getCatalog()->isExtendedIndexBuilding(entry->getName())) continue;
    std::unique_ptr<catalog::FillIndexAuxInfo> restored;
    const catalog::FillIndexAuxInfo* info;
    if (entry->isLoaded()) {
      info = dynamic_cast<const catalog::FillIndexAuxInfo*>(&entry->getAuxInfo());
    } else {
      restored.reset(catalog::FillIndexAuxInfo::deserialize(entry->getAuxBufferReader()));
      info = restored.get();
    }
    if (!info || info->getFillType() != catalog::FillEntryType::NODE) continue;
    const auto& presence = info->getPresenceColumns();
    if (std::find(presence.begin(), presence.end(), prop.getPropertyName()) == presence.end())
      continue;
    const auto& pattern = info->getPatternIR();
    size_t anchor = pattern.nodesInfo.size();
    for (size_t i = 0; i < pattern.nodesInfo.size(); ++i)
      if (pattern.nodesInfo[i].uniqName == info->getTableUniqName()) anchor = i;
    if (anchor == pattern.nodesInfo.size() || pattern.relsInfo.empty()) return std::nullopt;
    for (const auto& node : pattern.nodesInfo)
      if (node.nodeLabels.size() != 1) return std::nullopt;
    std::vector<bool> known(pattern.nodesInfo.size(), false), used(pattern.relsInfo.size(), false);
    known[anchor] = true;
    long double ratio = 1.0L;
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t r = 0; r < pattern.relsInfo.size(); ++r) {
        const auto& rel = pattern.relsInfo[r];
        if (used[r] || rel.relType != QueryRelType::NON_RECURSIVE ||
            rel.edgeType == EdgeType::UNDIRECTED || rel.srcNodeIdx >= known.size() ||
            rel.dstNodeIdx >= known.size()) continue;
        const bool fromSrc = known[rel.srcNodeIdx] && !known[rel.dstNodeIdx];
        const bool fromDst = known[rel.dstNodeIdx] && !known[rel.srcNodeIdx];
        if (!fromSrc && !fromDst) continue;
        const auto start = fromSrc ? rel.srcNodeIdx : rel.dstNodeIdx;
        const auto next = fromSrc ? rel.dstNodeIdx : rel.srcNodeIdx;
        const auto key = endpoint(context, rel.relLabels, pattern.nodesInfo[start].nodeLabels[0],
            pattern.nodesInfo[next].nodeLabels[0],
            fromSrc ? ExtendDirection::FWD : ExtendDirection::BWD);
        if (!key) return std::nullopt;
        const auto observed = stats->getEndpointDegreeStatistics(*key);
        if (!observed || !observed->vertex_count) return std::nullopt;
        ratio *= static_cast<long double>(observed->nonzero_vertex_count) / observed->vertex_count;
        known[next] = used[r] = true;
        progress = true;
      }
    }
    if (std::find(used.begin(), used.end(), false) != used.end()) return std::nullopt;
    return static_cast<uint64_t>(std::llround(static_cast<long double>(rows) * ratio));
  }
  return std::nullopt;
}


// Date-window selectivity from sampled TIMESTAMP quantiles. Comparisons of one
// property against constants are intersected into one interval per property;
// a union-label owner (COMMENT:POST) weights each vertex table by its size.
struct TimestampBound {
  std::optional<int64_t> lower, upper;  // milliseconds; inclusive enough at ms
};

std::optional<int64_t> constantMilliseconds(const PlanningCatalogLookup& context,
                                            const Expression& expression) {
  std::function<bool(const Expression&)> constant = [&](const Expression& e) {
    if (e.expressionType == ExpressionType::LITERAL) return true;
    if (e.expressionType != ExpressionType::FUNCTION || !e.getNumChildren()) return false;
    const auto& name = e.constCast<ScalarFunctionExpression>().getFunction().name;
    if (name != "CAST" && name != "CAST_TO_TIMESTAMP" && name != "TIMESTAMP") return false;
    for (const auto& child : e.getChildren())
      if (!constant(*child)) return false;
    return true;
  };
  if (!constant(expression)) return std::nullopt;
  try {
    const auto value = expression.expressionType == ExpressionType::LITERAL
        ? expression.constCast<LiteralExpression>().getValue()
        : evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(
              const_cast<Expression&>(expression).shared_from_this(), context.getClientContext());
    if (value.isNull() || value.getDataType().getLogicalTypeID() != LogicalTypeID::TIMESTAMP)
      return std::nullopt;
    return value.getValue<common::timestamp_t>().value / 1000;  // micro -> milli
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::pair<std::string, const PropertyExpression*>> timestampComparison(
    const PlanningCatalogLookup& context, const Expression& predicate, TimestampBound& bound) {
  const auto kind = predicate.expressionType;
  if ((kind != ExpressionType::GREATER_THAN && kind != ExpressionType::GREATER_THAN_EQUALS &&
       kind != ExpressionType::LESS_THAN && kind != ExpressionType::LESS_THAN_EQUALS) ||
      predicate.getNumChildren() != 2) return std::nullopt;
  const Expression* property = predicate.getChild(0).get();
  const Expression* constant = predicate.getChild(1).get();
  bool flipped = false;
  if (property->expressionType != ExpressionType::PROPERTY) {
    std::swap(property, constant);
    flipped = true;
  }
  if (property->expressionType != ExpressionType::PROPERTY ||
      property->getDataType().getLogicalTypeID() != LogicalTypeID::TIMESTAMP) return std::nullopt;
  const auto value = constantMilliseconds(context, *constant);
  if (!value) return std::nullopt;
  bool lower = kind == ExpressionType::GREATER_THAN || kind == ExpressionType::GREATER_THAN_EQUALS;
  if (flipped) lower = !lower;
  if (lower) bound.lower = std::max(bound.lower.value_or(*value), *value);
  else bound.upper = std::min(bound.upper.value_or(*value), *value);
  const auto& owner = property->constCast<PropertyExpression>();
  return std::make_pair(owner.getVariableName() + "." + owner.getPropertyName(), &owner);
}

double quantileFraction(const std::vector<int64_t>& q, int64_t x) {
  if (x <= q.front()) return 0.0;
  if (x >= q.back()) return 1.0;
  const auto upper = std::upper_bound(q.begin(), q.end(), x);
  const auto i = static_cast<size_t>(upper - q.begin()) - 1;
  const double span = static_cast<double>(q[i + 1] - q[i]);
  const double within = span > 0 ? static_cast<double>(x - q[i]) / span : 0.0;
  return (static_cast<double>(i) + within) / static_cast<double>(q.size() - 1);
}

std::optional<double> timestampSelectivity(const PlanningCatalogLookup& context,
    const PropertyExpression& property, const TimestampBound& bound) {
  auto stats = context->getStatsManager();
  long double weighted = 0, total = 0;
  bool any = false;
  for (auto* entry : context->getCatalog()->getTableEntries(context->getTransaction(), false)) {
    if ((entry->getType() != catalog::CatalogEntryType::NODE_TABLE_ENTRY &&
         entry->getType() != catalog::CatalogEntryType::REL_TABLE_ENTRY) ||
        !property.hasProperty(entry->getTableID())) continue;
    const auto range = stats->getRangeStatistics(entry->getTableID(), property.getPropertyName());
    if (!range) return std::nullopt;  // every owner table needs observed quantiles
    const double present = 1.0 - static_cast<double>(range->sample_null_count) / range->sample_count;
    const double low = bound.lower ? quantileFraction(range->quantiles, *bound.lower) : 0.0;
    const double high = bound.upper ? quantileFraction(range->quantiles, *bound.upper) : 1.0;
    // At least half a quantile bucket: an empty interpolated interval is a
    // sampling limit, not an observed empty result.
    const double fraction = std::max(high - low, 0.5 / static_cast<double>(range->quantiles.size() - 1));
    weighted += static_cast<long double>(range->vertex_count) * present * std::min(1.0, fraction);
    total += range->vertex_count;
    any = true;
  }
  if (!any || total <= 0) return std::nullopt;
  return static_cast<double>(weighted / total);
}

// Returns rows after applying every TIMESTAMP comparison conjunct, and removes
// those conjuncts from `conjuncts`. nullopt when none apply.
std::optional<uint64_t> timestampRangeRows(const PlanningCatalogLookup& context, uint64_t rows,
    std::vector<const Expression*>& conjuncts) {
  if (!context) return std::nullopt;
  std::map<std::string, std::pair<TimestampBound, const PropertyExpression*>> bounds;
  std::vector<const Expression*> rest;
  for (const auto* conjunct : conjuncts) {
    TimestampBound probe;
    const auto key = timestampComparison(context, *conjunct, probe);
    if (!key) { rest.push_back(conjunct); continue; }
    auto& slot = bounds[key->first];
    slot.second = key->second;
    if (probe.lower) slot.first.lower = std::max(slot.first.lower.value_or(*probe.lower), *probe.lower);
    if (probe.upper) slot.first.upper = std::min(slot.first.upper.value_or(*probe.upper), *probe.upper);
  }
  if (bounds.empty()) return std::nullopt;
  long double selected = rows;
  std::vector<const Expression*> unresolved;
  for (const auto* conjunct : conjuncts) {
    TimestampBound probe;
    const auto key = timestampComparison(context, *conjunct, probe);
    if (key && !timestampSelectivity(context, *bounds.at(key->first).second,
                                     bounds.at(key->first).first))
      unresolved.push_back(conjunct);
  }
  bool applied = false;
  for (const auto& [key, value] : bounds) {
    const auto selectivity = timestampSelectivity(context, *value.second, value.first);
    if (!selectivity) continue;
    selected *= *selectivity;
    applied = true;
  }
  if (!applied) return std::nullopt;
  rest.insert(rest.end(), unresolved.begin(), unresolved.end());
  conjuncts = std::move(rest);
  return static_cast<uint64_t>(std::max<long double>(1, std::llround(selected)));
}

std::optional<EndpointDegreeKey> filteredEndpointSource(const PlanningCatalogLookup& context,
    const EndpointDegreeKey& physical, const PropertyEquality& equality);

std::optional<uint64_t> propertyRows(const PlanningCatalogLookup& context, uint64_t rows,
    const Expression& predicate, const LogicalOperator* input,
    std::optional<EndpointDistribution> prior = std::nullopt) {
  const auto equality = propertyEquality(context, predicate);
  if (!equality) return std::nullopt;
  if (!prior && input) prior = distribution(context, *input, equality->property->getVariableName());
  // A mandatory arrival through this FILL already satisfies its owner
  // equality, whichever endpoint was chosen as the traversal's starting side.
  if (prior && prior->endpoints.size() == 1 &&
      filteredEndpointSource(context, prior->endpoints.front(), *equality)) return rows;
  const auto observed = propertyObservation(context, *equality);
  if (!observed) return std::nullopt;
  if (!observed->value_count) return uint64_t{0};
  if (prior) {
    const auto total = endpointTotals(context, prior->endpoints);
    if (total && (!prior->presence || prior->endpoints.size() == 1)) {
      WideCount selected = 0;
      bool complete = true;
      for (const auto& endpoint : prior->endpoints) {
        const auto found = observed->endpoint_degrees.find(endpoint);
        if (found == observed->endpoint_degrees.end()) { complete = false; break; }
        selected += prior->presence ? found->second.nonzero_vertex_count : found->second.edge_count;
      }
      if (complete) return observedRatio(rows, selected, prior->presence ? WideCount{total->positive} : total->edges);
    }
  }
  return observedRatio(rows, observed->value_count, observed->vertex_count);
}

std::optional<PropertyEquality> boundPropertyEquality(const PlanningCatalogLookup& context,
    const LogicalOperator& op, const std::string& owner) {
  const Expression* predicate = nullptr;
  switch (op.getOperatorType()) {
  case LogicalOperatorType::FILTER: predicate = op.constCast<LogicalFilter>().getPredicate().get(); break;
  case LogicalOperatorType::SCAN_NODE_TABLE: predicate = op.constCast<LogicalScanNodeTable>().getPredicates().get(); break;
  case LogicalOperatorType::GET_V: predicate = op.constCast<LogicalGetV>().getPredicates().get(); break;
  case LogicalOperatorType::EXTEND:
    if (op.constCast<LogicalExtend>().isOptional()) return std::nullopt;
    predicate = op.constCast<LogicalExtend>().getPredicates().get(); break;
  case LogicalOperatorType::PROJECTION:
  case LogicalOperatorType::DISTINCT:
  case LogicalOperatorType::FLATTEN: break;
  default: return std::nullopt;
  }
  if (predicate) {
    if (const auto equality = propertyEquality(context, *predicate)) {
      if (equality->property->getVariableName() == owner) return equality;
      if (equality->property->getVariableName() == gopt::DEFAULT_ALIAS_NAME) {
        const bool isOwner =
            (op.getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE &&
             op.constCast<LogicalScanNodeTable>().getAliasName() == owner) ||
            (op.getOperatorType() == LogicalOperatorType::GET_V &&
             op.constCast<LogicalGetV>().getAliasName() == owner) ||
            (op.getOperatorType() == LogicalOperatorType::EXTEND &&
             op.constCast<LogicalExtend>().getNbrNode()->getUniqueName() == owner);
        if (isOwner) return equality;
      }
    }
  }
  if (op.getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE)
    return primaryKeyEquality(context, op.constCast<LogicalScanNodeTable>(), owner);
  return op.getNumChildren() == 1 ? boundPropertyEquality(context, *op.getChild(0), owner) : std::nullopt;
}

std::optional<uint64_t> propertyExtensionRows(const PlanningCatalogLookup& context,
    const LogicalExtend& extend, const std::vector<EndpointDegreeKey>& endpoints) {
  if (endpoints.empty()) return std::nullopt;
  const auto equality = boundPropertyEquality(context, *extend.getChild(0), extend.getBoundNode()->getUniqueName());
  if (!equality) return std::nullopt;
  const auto observed = propertyObservation(context, *equality);
  if (!observed) return std::nullopt;
  if (!observed->value_count) return uint64_t{0};
  WideCount edges = 0;
  for (const auto& endpoint : endpoints) {
    const auto found = observed->endpoint_degrees.find(endpoint);
    if (found == observed->endpoint_degrees.end()) return std::nullopt;
    edges += found->second.edge_count;
  }
  return observedRatio(extend.getChild(0)->getCardinality(), edges, observed->value_count);
}

std::optional<EndpointDegreeKey> filteredEndpointSource(const PlanningCatalogLookup& context,
    const EndpointDegreeKey& physical, const PropertyEquality& equality) {
  if (!context || equality.property->getSingleTableID() != physical.nodeTableID) return std::nullopt;
  const auto target = physical.relTableID;
  const auto entries = context.getExtendedIndexEntriesForTable(target);
  if (std::count_if(entries.begin(), entries.end(), [&](const auto* entry) {
        return entry->getIndexType() == "FILL" && entry->getTableID() == target;
      }) != 1) return std::nullopt;
  for (const auto* entry : entries) {
    if (entry->getIndexType() != "FILL" || entry->getTableID() != target ||
        context->getCatalog()->isExtendedIndexBuilding(entry->getName())) continue;
    std::unique_ptr<catalog::FillIndexAuxInfo> restored;
    const catalog::FillIndexAuxInfo* info;
    if (entry->isLoaded()) info = dynamic_cast<const catalog::FillIndexAuxInfo*>(&entry->getAuxInfo());
    else { restored.reset(catalog::FillIndexAuxInfo::deserialize(entry->getAuxBufferReader())); info = restored.get(); }
    if (!info || info->getFillType() != catalog::FillEntryType::REL || info->isStagedPipeline() ||
        info->isFillUndirectedRel() || !info->getTargetColumns().empty() ||
        !info->getExistsPredicates().empty() || info->getPredicateTemplates().size() != 2 ||
        std::find(info->getPredicateTemplates().begin(), info->getPredicateTemplates().end(),
            ENDPOINT_SET_CONTRACT) == info->getPredicateTemplates().end()) continue;
    const auto& pattern = info->getPatternIR();
    if (pattern.nodesInfo.size() != 2 || pattern.relsInfo.size() != 1) continue;
    const auto& rel = pattern.relsInfo.front();
    if (rel.relType != QueryRelType::NON_RECURSIVE || rel.srcNodeIdx >= 2 || rel.dstNodeIdx >= 2 ||
        rel.srcNodeIdx == rel.dstNodeIdx || rel.edgeType == EdgeType::UNDIRECTED ||
        std::find(rel.relLabels.begin(), rel.relLabels.end(), target) != rel.relLabels.end()) continue;
    const auto& ownerName = physical.direction == RelDataDirection::FWD
        ? info->getSrcNodeUniqName() : info->getDstNodeUniqName();
    size_t owner = 2;
    for (size_t i = 0; i < 2; ++i) if (pattern.nodesInfo[i].uniqName == ownerName) owner = i;
    if (owner == 2 || pattern.nodesInfo[owner].nodeLabels != std::vector<table_id_t>{physical.nodeTableID} ||
        pattern.nodesInfo[1-owner].nodeLabels.size() != 1) continue;
    // Compatibility FILLs persist typed structural predicate keys, but do not
    // necessarily persist executable predicate ASTs. Compare the same typed
    // key used at registration, replacing only a proved constant CAST with
    // its evaluated literal. Never parse a printed predicate or raw SQL.
    const auto key = propertyEqualityKey(equality, "node:" + std::to_string(owner));
    if (!key || std::find(info->getPredicateTemplates().begin(),
        info->getPredicateTemplates().end(), *key) == info->getPredicateTemplates().end()) continue;
    const auto source = endpoint(context, rel.relLabels, physical.nodeTableID,
        pattern.nodesInfo[1-owner].nodeLabels.front(), owner == rel.srcNodeIdx ? ExtendDirection::FWD : ExtendDirection::BWD);
    if (source) return source;
  }
  return std::nullopt;
}

std::optional<uint64_t> filteredEndpointExtensionRows(const PlanningCatalogLookup& context,
    const LogicalExtend& extend) {
  if (!context || extend.isOptional() || extend.isRecursive() ||
      extend.getRel()->getTableIDs().size() != 1) return std::nullopt;
  const auto equality = boundPropertyEquality(context, *extend.getChild(0), extend.getBoundNode()->getUniqueName());
  if (!equality) return std::nullopt;
  const auto observed = propertyObservation(context, *equality);
  const auto physical = extendEndpoint(context, extend, true);
  if (!observed || !physical) return std::nullopt;
  const auto source = filteredEndpointSource(context, *physical, *equality);
  if (!source) return std::nullopt;
  const auto degree = context->getStatsManager()->getEndpointDegreeStatistics(*source);
  // Endpoint-set DISTINCT may erase parallel source edges. Transfer exact
  // conditional degrees only when this snapshot observes unique pairs.
  if (!degree || !degree->distinct_endpoint_pair_count ||
      *degree->distinct_endpoint_pair_count != degree->edge_count) return std::nullopt;
  if (!observed->value_count) return uint64_t{0};
  const auto conditional = observed->endpoint_degrees.find(*source);
  if (conditional == observed->endpoint_degrees.end()) return std::nullopt;
  return observedRatio(extend.getChild(0)->getCardinality(),
      conditional->second.edge_count, observed->value_count);
}
}  // namespace


void CardinalityEstimator::initNodeIDDom(const Transaction* transaction,
                                         const QueryGraph& queryGraph) {
  perQueryGraphNodeIDName2dom.clear();
  for (uint64_t i = 0u; i < queryGraph.getNumQueryNodes(); ++i) {
    auto node = queryGraph.getQueryNode(i).get();
    addNodeIDDomAndStats(transaction, *node->getInternalID(),
                         node->getTableIDs());
  }
  for (uint64_t i = 0u; i < queryGraph.getNumQueryRels(); ++i) {
    auto rel = queryGraph.getQueryRel(i);
    if (QueryRelTypeUtils::isRecursive(rel->getRelType())) {
      auto node = rel->getRecursiveInfo()->node.get();
      addNodeIDDomAndStats(transaction, *node->getInternalID(),
                           node->getTableIDs());
    }
  }
}

void CardinalityEstimator::addNodeIDDomAndStats(
    const Transaction* transaction, const binder::Expression& nodeID,
    const std::vector<common::table_id_t>& tableIDs) {
  auto key = nodeID.getUniqueName();
  cardinality_t numNodes = 0u;
  for (auto tableID : tableIDs) {
    auto stats = context->getStatsManager()
                     ->getTable(tableID)
                     ->cast<storage::NodeTable>()
                     .getStats(transaction);
    numNodes += stats.getTableCard();
    if (!nodeTableStats.contains(tableID)) {
      nodeTableStats.insert({tableID, std::move(stats)});
    }
  }
  if (!nodeIDName2dom.contains(key)) {
    nodeIDName2dom.insert({key, numNodes});
  }
}

void CardinalityEstimator::addPerQueryGraphNodeIDDom(
    const binder::Expression& nodeID, cardinality_t numNodes) {
  const auto key = nodeID.getUniqueName();
  if (!perQueryGraphNodeIDName2dom.contains(key)) {
    perQueryGraphNodeIDName2dom.insert({key, numNodes});
  }
}

void CardinalityEstimator::clearPerQueryGraphStats() {
  perQueryGraphNodeIDName2dom.clear();
}

cardinality_t CardinalityEstimator::getNodeIDDom(
    const std::string& nodeIDName) const {
  if (!nodeIDName2dom.contains(nodeIDName)) {
    std::cerr << "[CardEst] missing nodeIDDom for '" << nodeIDName << "'\n";
  }
  NEUG_ASSERT(nodeIDName2dom.contains(nodeIDName));
  cardinality_t dom = nodeIDName2dom.at(nodeIDName);
  if (perQueryGraphNodeIDName2dom.contains(nodeIDName)) {
    dom = std::min(dom, perQueryGraphNodeIDName2dom.at(nodeIDName));
  }
  return dom;
}

uint64_t CardinalityEstimator::estimateScanNode(
    const LogicalOperator& op) const {
  const auto& scan = op.constCast<const LogicalScanNodeTable&>();
  switch (scan.getScanType()) {
  case LogicalScanNodeTableType::PRIMARY_KEY_SCAN:
    return 1;
  default: {
    const auto rows = getNodeIDDom(scan.getNodeID()->getUniqueName());
    if (scan.getPredicates()) {
      if (const auto observed = presenceRows(context, rows, *scan.getPredicates(), nullptr))
        return *observed;
      // A fused predicate still filters rows; it is not a bare full scan.
      LogicalDummyScan input;
      input.setCardinality(rows);
      return estimateFilter(input, *scan.getPredicates());
    }
    const auto tables = scan.getTableIDs();
    if (!rows && context && std::all_of(tables.begin(), tables.end(),
        [&](table_id_t table) { return context->getStatsManager()->hasTableCardinality(table); }))
      return 0;
    return atLeastOne(rows);
  }
  }
}

uint64_t CardinalityEstimator::estimateAggregate(
    const LogicalAggregate& op) const {
  const auto keys = op.getKeys();
  if (keys.empty()) return 1;  // A global aggregate returns one row on empty input.
  const auto& input = *op.getChild(0);
  const auto previous = input.getCardinality();
  if (!context || keys.size() != 1) return previous;
  const auto key = groupNodeKey(*keys.front());
  if (!key) return previous;
  for (const auto& dependent : op.getDependentKeys()) {
    const bool sameNode = ExpressionUtil::isNodePattern(*dependent) &&
        dependent->getUniqueName() == key->owner;
    const bool samePropertyOwner = dependent->expressionType == ExpressionType::PROPERTY &&
        dependent->constCast<PropertyExpression>().getVariableName() == key->owner;
    if (!sameNode && !samePropertyOwner) return previous;
  }
  const auto origin = groupNodeOrigin(input, *key);
  if (!origin) return previous;
  const auto domain = groupNodeDomain(context, *key, *origin);
  if (!domain) return previous;
  if (observedEmptyGroupInput(context, input)) return 0;
  if (const auto support = completeNodeGroupSupport(context, input, *key, *domain))
    return *support;
  const auto cap = !origin->nonNull && *domain != std::numeric_limits<uint64_t>::max()
      ? *domain + 1 : *domain;
  // A filtered key can have any support within the domain. Keep the previous
  // point estimate subject to that cap, without mistaking unknown zero for a
  // proved empty group population or a nullable key for a mandatory one.
  return std::min(cap, atLeastOne(previous));
}

cardinality_t CardinalityEstimator::multiply(double extensionRate,
                                             cardinality_t card) const {
  if (!card) return 0;
  const auto result = static_cast<long double>(extensionRate) * card;
  if (!std::isfinite(result) || result >= std::numeric_limits<cardinality_t>::max())
    return std::numeric_limits<cardinality_t>::max();
  return atLeastOne(result <= 0 ? 0 : static_cast<cardinality_t>(result));
}

uint64_t CardinalityEstimator::estimateExtend(const LogicalExtend& extend, bool applyPredicate) const {
  const auto input = extend.getChild(0)->getCardinality();
  auto rows = extend.isRecursive()
      ? multiply(getDirectionalExtensionRate(*extend.getRel(), extend.getRel()->getTableIDs(),
            *extend.getBoundNode(), extend.getNbrNode()->getTableIDs(), extend.getDirection(),
            context->getTransaction()), input)
      : observedRatio(input,
            getDirectionalRelationshipRows(extend.getRel()->getTableIDs(),
                extend.getBoundNode()->getTableIDs(), extend.getNbrNode()->getTableIDs(),
                extend.getDirection(), context->getTransaction()),
            getNumNodes(context->getTransaction(), extend.getBoundNode()->getTableIDs()));
  const auto relations = extend.getRel()->getTableIDs();
  const auto knownEmpty = !extend.isRecursive() && !relations.empty() &&
      std::all_of(relations.begin(), relations.end(), [&](table_id_t table) {
        const auto& stats = getRelationshipRows(context->getTransaction(), table);
        return stats.observed && stats.rows == 0;
      });
  if (knownEmpty) return extend.isOptional() ? input : 0;
  const auto count = derivedRelationshipContract(context, extend);
  const auto current = count && count->sourceBound ? std::vector<EndpointDegreeKey>{*count->sourceBound} :
      extendEndpoints(context, extend, true);
  const auto previous = distribution(context, *extend.getChild(0), extend.getBoundNode()->getUniqueName());
  if (auto estimated = extensionRows(context, input, current, previous)) rows = *estimated;
  if (auto conditional = propertyExtensionRows(context, extend, current)) rows = *conditional;
  if (auto conditional = filteredEndpointExtensionRows(context, extend)) rows = *conditional;
  if (applyPredicate && extend.getPredicates() &&
      (!count || !isDerivedCountPresence(extend, *count))) {
    auto forced = std::optional<EndpointDistribution>{};
    auto presence = countPresence(context, *extend.getPredicates());
    if (presence && extend.getExtendOpt() == ExtendOpt::VERTEX &&
        matchesFusedNode(*presence, extend.getNbrNode()->getUniqueName(),
            extend.getNbrNode()->getTableIDs())) {
      auto arrival = count && count->sourceNeighbor ? std::vector<EndpointDegreeKey>{*count->sourceNeighbor} :
          extendEndpoints(context, extend, false);
      if (!arrival.empty()) forced = EndpointDistribution{std::move(arrival), false};
    }
    std::vector<const Expression*> conjuncts;
    std::function<void(const Expression&)> split = [&](const Expression& expression) {
      if (expression.expressionType == ExpressionType::AND)
        for (const auto& child : expression.getChildren()) split(*child);
      else conjuncts.push_back(&expression);
    };
    split(*extend.getPredicates());
    // Edge TIMESTAMP windows (BI11 KNOWS.creationDate) from sampled quantiles.
    const auto windowed = timestampRangeRows(context, rows, conjuncts);
    if (windowed && conjuncts.empty())
      rows = *windowed;
    else if (windowed)
      rows = multiply(std::pow(PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY,
                               static_cast<double>(conjuncts.size())), *windowed);
    else if (const auto filtered = presenceRows(context, rows, *extend.getPredicates(), nullptr, forced))
      rows = *filtered;
    else if (const auto filtered = propertyRows(context, rows, *extend.getPredicates(), nullptr,
                 EndpointDistribution{extendEndpoints(context, extend, false), false}))
      rows = *filtered;
    else rows = multiply(PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY, rows);
  }
  return extend.isOptional() ? std::max(rows, input) : rows;
}

namespace {
// Expected walks per seed whose length lies in [lower, upper], and the typed
// endpoint domain. SHORTEST callers cap the walk count by the domain.
struct RecursiveReach { long double perSeed, targetDomain; };
std::optional<RecursiveReach> recursiveReach(const PlanningCatalogLookup& context,
                                             const LogicalRecursiveExtend& extend,
                                             uint64_t lower, uint64_t upper) {
  const auto rel = extend.getRel();
  const auto bound = extend.getBoundNode(), neighbor = extend.getNbrNode();
  if (!context || !rel || !bound || !neighbor) return std::nullopt;
  // Propagate typed E/V means over the actual finite hop interval. A scalar
  // mean over the seed label loses intermediate labels, and counting only
  // outgoing edges loses half of an undirected traversal. These are cardinality
  // estimates, not proofs of uniqueness, reachability, or path independence.
  struct Incidence { table_id_t from, to; uint64_t edges; };
  std::vector<Incidence> incidences;
  std::map<table_id_t, uint64_t> domains;
  const auto transaction = context->getTransaction();
  auto addDomain = [&](table_id_t id) {
    const auto rows = context.getObservedTableCardinality(transaction, id);
    if (!rows) return false;
    domains.emplace(id, *rows);
    return true;
  };
  for (const auto id : bound->getTableIDs()) if (!addDomain(id)) return std::nullopt;
  for (const auto id : neighbor->getTableIDs()) if (!addDomain(id)) return std::nullopt;
  for (const auto* entry : rel->getEntries()) {
    const auto& table = entry->constCast<catalog::RelTableCatalogEntry>();
    const auto source = table.getSrcTableID(), destination = table.getDstTableID();
    if (!addDomain(source) || !addDomain(destination)) return std::nullopt;
    const auto observed = context.getObservedTableCardinality(transaction, table.getTableID());
    if (!observed) return std::nullopt;
    const auto edges = *observed;
    if (edges && (!domains.at(source) || !domains.at(destination))) return std::nullopt;
    if (extend.getDirection() != ExtendDirection::BWD)
      incidences.push_back({source, destination, edges});
    if (extend.getDirection() != ExtendDirection::FWD)
      incidences.push_back({destination, source, edges});
  }
  long double seedDomain = 0, targetDomain = 0;
  for (const auto id : bound->getTableIDs()) seedDomain += domains.at(id);
  for (const auto id : neighbor->getTableIDs()) targetDomain += domains.at(id);
  if (!seedDomain || !targetDomain) return RecursiveReach{0, targetDomain};
  std::map<table_id_t, size_t> ids;
  for (const auto& [id, _] : domains) ids.emplace(id, ids.size());
  const auto n = ids.size() + 1;
  using Matrix = std::vector<std::vector<long double>>;
  Matrix transition(n, std::vector<long double>(n));
  for (const auto& edge : incidences)
    if (domains.at(edge.from))
      transition[ids.at(edge.from)][ids.at(edge.to)] +=
          static_cast<long double>(edge.edges) / domains.at(edge.from);
  std::vector<long double> frontier(n);
  for (const auto id : bound->getTableIDs())
    frontier[ids.at(id)] = domains.at(id) / seedDomain;
  auto advance = [&](Matrix power, uint64_t hops) {
    for (; hops; hops >>= 1) {
      if (hops & 1) {
        std::vector<long double> next(n);
        for (size_t i = 0; i < n; ++i)
          if (frontier[i])
            for (size_t j = 0; j < n; ++j)
              if (power[i][j]) next[j] += frontier[i] * power[i][j];
        frontier = std::move(next);
      }
      if (hops == 1) break;
      Matrix next(n, std::vector<long double>(n));
      for (size_t i = 0; i < n; ++i)
        for (size_t k = 0; k < n; ++k)
          if (power[i][k])
            for (size_t j = 0; j < n; ++j)
              if (power[k][j]) next[i][j] += power[i][k] * power[k][j];
      power = std::move(next);
    }
  };
  // Advance to the first accepted length, then accumulate just the accepted
  // interval. Subtracting two huge geometric sums would lose short intervals.
  advance(transition, lower);
  for (const auto id : neighbor->getTableIDs()) frontier.back() += frontier[ids.at(id)];
  for (size_t i = 0; i + 1 < n; ++i)
    for (const auto id : neighbor->getTableIDs()) transition[i].back() += transition[i][ids.at(id)];
  transition.back().back() = 1;
  advance(transition, upper - lower);
  return RecursiveReach{frontier.back(), targetDomain};
}

bool shortestEndpoints(const LogicalRecursiveExtend& extend) {
  const auto type = extend.getRel()->getRelType();
  return type == QueryRelType::SHORTEST || type == QueryRelType::WEIGHTED_SHORTEST;
}

// Endpoints reached by accepted lengths in [lower, upper]. A SHORTEST endpoint
// is reported once, at its first (shortest) accepted length, so the share of
// a narrower length window is the growth of the capped reachable population.
std::optional<long double> reachedEndpoints(const PlanningCatalogLookup& context,
                                            const LogicalRecursiveExtend& extend,
                                            uint64_t lower, uint64_t upper) {
  const auto& bind = extend.getBindData();
  if (!shortestEndpoints(extend)) {
    if (lower > upper) return 0;
    const auto reach = recursiveReach(context, extend, lower, upper);
    if (!reach) return std::nullopt;
    return reach->perSeed;
  }
  auto capped = [&](uint64_t last) -> std::optional<long double> {
    if (last < bind.lowerBound) return 0;
    const auto reach = recursiveReach(context, extend, bind.lowerBound, last);
    if (!reach) return std::nullopt;
    return std::min(reach->perSeed, reach->targetDomain);
  };
  if (lower > upper) return 0;
  const auto through = capped(upper), before = capped(lower - 1);
  if (!through || !before) return std::nullopt;
  return std::max<long double>(*through - *before, 0);
}

const LogicalRecursiveExtend* findRecursiveExtend(const LogicalOperator& op,
                                                  const std::string& relName) {
  if (op.getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
    const auto& extend = op.constCast<LogicalRecursiveExtend>();
    if (extend.getRel() && extend.getRel()->getUniqueName() == relName) return &extend;
  }
  for (const auto& child : op.getChildren())
    if (const auto found = findRecursiveExtend(*child, relName)) return found;
  return nullptr;
}

// `path._LENGTH <cmp> k` on a finite RECURSIVE_EXTEND below the filter keeps
// the endpoints reached by lengths in the narrowed interval (same typed hop
// propagation that sized the extension; uniform over seeds).
std::optional<uint64_t> pathLengthRows(const PlanningCatalogLookup& context,
                                       const LogicalOperator& childPlan,
                                       const Expression& predicate) {
  if (!context || !ExpressionTypeUtil::isComparison(predicate.expressionType) ||
      predicate.getNumChildren() != 2)
    return std::nullopt;
  auto type = predicate.expressionType;
  auto length = predicate.getChild(0), literal = predicate.getChild(1);
  if (literal->expressionType == ExpressionType::PROPERTY) {
    std::swap(length, literal);
    switch (type) {
    case ExpressionType::GREATER_THAN: type = ExpressionType::LESS_THAN; break;
    case ExpressionType::GREATER_THAN_EQUALS: type = ExpressionType::LESS_THAN_EQUALS; break;
    case ExpressionType::LESS_THAN: type = ExpressionType::GREATER_THAN; break;
    case ExpressionType::LESS_THAN_EQUALS: type = ExpressionType::GREATER_THAN_EQUALS; break;
    default: break;
    }
  }
  if (length->expressionType != ExpressionType::PROPERTY ||
      literal->expressionType != ExpressionType::LITERAL)
    return std::nullopt;
  const auto& property = length->constCast<PropertyExpression>();
  if (property.getPropertyName() != InternalKeyword::LENGTH) return std::nullopt;
  const auto& value = literal->constCast<LiteralExpression>().getValue();
  if (value.isNull()) return 0;
  if (!LogicalTypeUtils::isIntegral(value.getDataType().getLogicalTypeID())) return std::nullopt;
  const auto k = static_cast<long double>(std::stod(value.toString()));
  const auto* extend = findRecursiveExtend(childPlan, property.getVariableName());
  if (!extend) return std::nullopt;
  const auto& bind = extend->getBindData();
  const long double lower = bind.lowerBound;
  const long double upper = std::min<uint64_t>(bind.upperBound, INT32_MAX - 1u);
  long double from = lower, to = upper;
  switch (type) {
  case ExpressionType::EQUALS: from = std::max(from, k); to = std::min(to, k); break;
  case ExpressionType::GREATER_THAN: from = std::max(from, k + 1); break;
  case ExpressionType::GREATER_THAN_EQUALS: from = std::max(from, k); break;
  case ExpressionType::LESS_THAN: to = std::min(to, k - 1); break;
  case ExpressionType::LESS_THAN_EQUALS: to = std::min(to, k); break;
  default: return std::nullopt;
  }
  if (from == lower && to == upper) return childPlan.getCardinality();
  if (from > to) return 0;
  const auto all = reachedEndpoints(context, *extend, lower, upper);
  const auto kept = reachedEndpoints(context, *extend, static_cast<uint64_t>(from), static_cast<uint64_t>(to));
  if (!all || !kept || *all <= 0) return std::nullopt;
  const auto rows = std::ceil(childPlan.getCardinality() * std::min<long double>(*kept / *all, 1));
  return static_cast<uint64_t>(rows);
}
}  // namespace

cardinality_t CardinalityEstimator::estimateRecursiveExtend(
    const LogicalRecursiveExtend& extend) const {
  const auto input = extend.getChild(0)->getCardinality();
  if (!input) return 0;
  const auto rel = extend.getRel();
  const auto bound = extend.getBoundNode(), neighbor = extend.getNbrNode();
  if (!context || !rel || !bound || !neighbor) return input;
  const auto& bind = extend.getBindData();
  const auto upper = std::min<uint64_t>(bind.upperBound, INT32_MAX - 1u);
  if (bind.lowerBound > upper) return 0;
  const auto reach = recursiveReach(context, extend, bind.lowerBound, upper);
  if (!reach)
    return multiply(getExtensionRate(*rel, *bound, context->getTransaction()), input);
  auto perSeed = reach->perSeed;
  if (shortestEndpoints(extend))
    // One selected shortest path per reachable endpoint. ALL SHORTEST retains
    // path multiplicity and must not use this endpoint population cap.
    perSeed = std::min(perSeed, reach->targetDomain);
  const auto result = perSeed * input;
  if (!std::isfinite(result) || result >= std::numeric_limits<cardinality_t>::max())
    return std::numeric_limits<cardinality_t>::max();
  return static_cast<cardinality_t>(std::ceil(result));
}

uint64_t CardinalityEstimator::estimateGetV(const LogicalGetV& getV) const {
  const auto& input = *getV.getChild(0);
  if (!getV.getPredicates()) return input.getCardinality();
  auto presence = countPresence(context, *getV.getPredicates());
  if (presence && getV.getNodeID() &&
      getV.getNodeID()->expressionType == ExpressionType::PROPERTY &&
      matchesFusedNode(*presence, getV.getAliasName(), getV.getTableIDs())) {
    auto prior = distribution(context, input, getV.getAliasName());
    if (auto rows = presenceRows(context, input.getCardinality(),
            *getV.getPredicates(), nullptr, prior)) return *rows;
  }
  const auto equality = propertyEquality(context, *getV.getPredicates());
  if (equality && getV.getNodeID() &&
      getV.getNodeID()->expressionType == ExpressionType::PROPERTY &&
      (equality->property->getVariableName() == getV.getAliasName() ||
       equality->property->getVariableName() == gopt::DEFAULT_ALIAS_NAME)) {
    const auto tables = getV.getTableIDs();
    if (tables.size() == 1 && tables.front() == equality->property->getSingleTableID()) {
      // Fusion rewrites the property owner to the current head. Recover the
      // GET_V output identity before looking up its arriving edge distribution.
      const auto prior = distribution(context, input, getV.getAliasName());
      if (auto rows = propertyRows(context, input.getCardinality(),
              *getV.getPredicates(), nullptr, prior)) return *rows;
    }
  }
  return estimateFilter(input, *getV.getPredicates());
}

uint64_t CardinalityEstimator::estimateHashJoin(
    const std::vector<binder::expression_pair>& joinConditions,
    const LogicalOperator& probeOp, const LogicalOperator& buildOp) const {
  if (LogicalHashJoin::isNodeIDOnlyJoin(joinConditions)) {
    cardinality_t denominator = 1u;
    auto joinKeys = LogicalHashJoin::getJoinNodeIDs(joinConditions);
    for (auto& joinKey : joinKeys) {
      if (nodeIDName2dom.contains(joinKey->getUniqueName())) {
        denominator *= getNodeIDDom(joinKey->getUniqueName());
      }
    }
    return atLeastOne(
        probeOp.getCardinality() *
        JoinOrderUtil::getJoinKeysFlatCardinality(joinKeys, buildOp) /
        atLeastOne(denominator));
  } else {
    // Naively estimate the cardinality if the join is non-ID based
    cardinality_t estCardinality =
        probeOp.getCardinality() * buildOp.getCardinality();
    for (size_t i = 0; i < joinConditions.size(); ++i) {
      estCardinality *= PlannerKnobs::EQUALITY_PREDICATE_SELECTIVITY;
    }
    return atLeastOne(estCardinality);
  }
}

uint64_t CardinalityEstimator::estimateCrossProduct(
    const LogicalOperator& probeOp, const LogicalOperator& buildOp) const {
  return atLeastOne(probeOp.getCardinality() * buildOp.getCardinality());
}

uint64_t CardinalityEstimator::estimateIntersect(
    const expression_vector& joinNodeIDs, const LogicalOperator& probeOp,
    const std::vector<LogicalOperator*>& buildOps) const {
  // Formula 1: treat intersect as a Filter on probe side.
  uint64_t estCardinality1 = probeOp.getCardinality() *
                             PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY;
  // Formula 2: assume independence on join conditions.
  cardinality_t denominator = 1u;
  for (auto& joinNodeID : joinNodeIDs) {
    denominator *= getNodeIDDom(joinNodeID->getUniqueName());
  }
  auto numerator = probeOp.getCardinality();
  for (auto& buildOp : buildOps) {
    numerator *= buildOp->getCardinality();
  }
  auto estCardinality2 = numerator / atLeastOne(denominator);
  // Pick minimum between the two formulas.
  return atLeastOne(std::min<uint64_t>(estCardinality1, estCardinality2));
}

uint64_t CardinalityEstimator::estimateFlatten(
    const LogicalOperator& childOp, f_group_pos groupPosToFlatten) const {
  auto group = childOp.getSchema()->getGroup(groupPosToFlatten);
  return atLeastOne(childOp.getCardinality() * group->cardinalityMultiplier);
}

static bool isPrimaryKey(const Expression& expression) {
  if (expression.expressionType != ExpressionType::PROPERTY) {
    return false;
  }
  return ((PropertyExpression&) expression).isPrimaryKey();
}

static bool isSingleLabelledProperty(const Expression& expression) {
  if (expression.expressionType != ExpressionType::PROPERTY) {
    return false;
  }
  return expression.constCast<PropertyExpression>().isSingleLabel();
}

uint64_t CardinalityEstimator::estimateFilter(
    const LogicalOperator& childPlan, const Expression& predicate) const {
  if (!childPlan.getCardinality()) return 0;
  if (predicate.expressionType == ExpressionType::LITERAL) {
    // Folded constant predicates (e.g. BI10 `minPathDistance <> 1`) are exact.
    const auto& value = predicate.constCast<LiteralExpression>().getValue();
    if (value.isNull()) return 0;
    if (value.getDataType().getLogicalTypeID() == LogicalTypeID::BOOL)
      return value.getValue<bool>() ? childPlan.getCardinality() : 0;
  }
  if (const auto observed = pathLengthRows(context, childPlan, predicate))
    return *observed;
  if (const auto observed = presenceRows(context, childPlan.getCardinality(), predicate, &childPlan))
    return *observed;
  if (const auto observed = projectionPresenceRows(context, childPlan.getCardinality(), predicate))
    return *observed;
  if (const auto observed = propertyRows(context, childPlan.getCardinality(), predicate, &childPlan))
    return *observed;
  {
    // TIMESTAMP windows (one comparison or a conjunction) use sampled quantiles.
    std::vector<const Expression*> conjuncts;
    std::function<void(const Expression&)> split = [&](const Expression& expression) {
      if (expression.expressionType == ExpressionType::AND)
        for (const auto& child : expression.getChildren()) split(*child);
      else conjuncts.push_back(&expression);
    };
    split(predicate);
    const auto before = conjuncts.size();
    if (const auto windowed = timestampRangeRows(context, childPlan.getCardinality(), conjuncts)) {
      if (conjuncts.empty()) return *windowed;
      if (conjuncts.size() < before) {
        LogicalDummyScan narrowed;
        narrowed.setCardinality(*windowed);
        auto rows = *windowed;
        for (const auto* conjunct : conjuncts)
          rows = observedRatio(rows, estimateFilter(narrowed, *conjunct), narrowed.getCardinality());
        return rows;
      }
    }
  }
  if (predicate.expressionType == ExpressionType::AND) {
    std::vector<const Expression*> conjuncts;
    std::function<void(const Expression&)> split = [&](const Expression& expression) {
      if (expression.expressionType == ExpressionType::AND)
        for (const auto& child : expression.getChildren()) split(*child);
      else conjuncts.push_back(&expression);
    };
    split(predicate);
    std::unordered_set<std::string> seen;
    auto rows = childPlan.getCardinality();
    for (const auto* conjunct : conjuncts) {
      // Literal expression UIDs differ even for the same value. A typed
      // structural key preserves the owner's binding identity while comparing
      // literal bytes, including equivalent constant CASTs and swapped sides.
      if (const auto equality = propertyEquality(context, *conjunct)) {
        const auto key = propertyEqualityKey(*equality, equality->property->getVariableName());
        if (key && !seen.insert(*key).second) continue;
      }
      const auto filtered = estimateFilter(childPlan, *conjunct);
      rows = observedRatio(rows, filtered, childPlan.getCardinality());
    }
    return rows;
  }
  if (predicate.expressionType == ExpressionType::OR) {
    bool allEquals = true;
    for (auto& child : predicate.getChildren()) {
      if (child->expressionType != ExpressionType::EQUALS) {
        allEquals = false;
        break;
      }
    }
    if (allEquals && !predicate.getChildren().empty()) {
      return estimateFilter(childPlan, *predicate.getChildren()[0]);
    }
  }
  if (predicate.expressionType == ExpressionType::EQUALS) {
    if (isPrimaryKey(*predicate.getChild(0)) ||
        isPrimaryKey(*predicate.getChild(1))) {
      return 1;
    } else {
      return atLeastOne(childPlan.getCardinality() *
                        PlannerKnobs::EQUALITY_PREDICATE_SELECTIVITY);
    }
  } else {
    return atLeastOne(childPlan.getCardinality() *
                      PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY);
  }
}

// Both extension estimators already account for the typed neighbor domain.
// Completing their endpoint identity does not traverse the relationship again.
// In particular, the legacy scalar path-length heuristic would overwrite the
// typed finite-hop recursive population before the optimizer can see it.
cardinality_t CardinalityEstimator::estimateGetV(
    const planner::LogicalExtend& extend) const {
  return extend.getCardinality();
}

cardinality_t CardinalityEstimator::estimateGetV(
    const planner::LogicalRecursiveExtend& extend) const {
  return extend.getCardinality();
}

uint64_t CardinalityEstimator::getNumNodes(
    const Transaction*, const std::vector<table_id_t>& tableIDs) const {
  cardinality_t numNodes = 0u;
  for (auto& tableID : tableIDs) {
    if (!nodeTableStats.contains(tableID)) {
      std::cerr << "[CardEst] missing table stat for tableID " << tableID
                << "\n";
    }
    NEUG_ASSERT(nodeTableStats.contains(tableID));
    numNodes += nodeTableStats.at(tableID).getTableCard();
  }
  return atLeastOne(numNodes);
}

const CardinalityEstimator::RelationshipRows& CardinalityEstimator::getRelationshipRows(
    const Transaction* transaction, table_id_t tableID) const {
  auto found = relTableRows.find(tableID);
  if (found == relTableRows.end()) {
    auto stats = context->getStatsManager();
    const auto rows = stats->getTable(tableID)->getNumTotalRows(transaction);
    const auto& table = context.getTableCatalogEntry(transaction, tableID)
        ->constCast<catalog::RelTableCatalogEntry>();
    found = relTableRows.emplace(tableID,
        RelationshipRows{rows, stats->hasTableCardinality(tableID),
            table.getSrcTableID(), table.getDstTableID()}).first;
  }
  return found->second;
}

uint64_t CardinalityEstimator::getNumRels(
    const Transaction* transaction,
    const std::vector<table_id_t>& tableIDs) const {
  cardinality_t numRels = 0u;
  for (auto tableID : tableIDs) {
    numRels += getRelationshipRows(transaction, tableID).rows;
  }
  return atLeastOne(numRels);
}

double CardinalityEstimator::getExtensionRate(
    const RelExpression& rel, const NodeExpression& boundNode,
    const Transaction* transaction) const {
  return getExtensionRate(rel, rel.getTableIDs(), boundNode, transaction);
}

double CardinalityEstimator::getExtensionRate(
    const RelExpression& rel, const table_id_vector_t& tableIDs,
    const NodeExpression& boundNode, const Transaction* transaction) const {
  const bool source = rel.getSrcNodeName() == boundNode.getUniqueName();
  const auto neighbor = source ? rel.getDstNode() : rel.getSrcNode();
  const auto direction = rel.getDirectionType() == RelDirectionType::BOTH ? ExtendDirection::BOTH :
      (source ? ExtendDirection::FWD : ExtendDirection::BWD);
  return getDirectionalExtensionRate(rel, tableIDs, boundNode, neighbor->getTableIDs(), direction, transaction);
}

__uint128_t CardinalityEstimator::getDirectionalRelationshipRows(
    const table_id_vector_t& tableIDs, const table_id_vector_t& boundTables,
    const table_id_vector_t& neighborTables,
    ExtendDirection direction, const Transaction* transaction) const {
  WideCount numRels = 0;
  bool unknown = false;
  const auto includes = [](const table_id_vector_t& tables, table_id_t value) {
    return std::find(tables.begin(), tables.end(), value) != tables.end();
  };
  for (const auto tableID : tableIDs) {
    const auto& table = getRelationshipRows(transaction, tableID);
    const auto outgoing = direction != ExtendDirection::BWD && includes(boundTables, table.sourceTable) &&
        includes(neighborTables, table.destinationTable);
    const auto incoming = direction != ExtendDirection::FWD && includes(boundTables, table.destinationTable) &&
        includes(neighborTables, table.sourceTable);
    if (outgoing) numRels += table.rows;
    if (incoming) numRels += table.rows;
    if (outgoing || incoming) unknown |= !table.observed;
  }
  // Keep unknown distinct from an observed empty relationship, including for
  // WCOJoin, which consumes the extension rate without estimateExtend().
  return numRels || !unknown ? numRels : 1;
}

double CardinalityEstimator::getDirectionalExtensionRate(
    const RelExpression& rel, const table_id_vector_t& tableIDs,
    const NodeExpression& boundNode, const table_id_vector_t& neighborTables,
    ExtendDirection direction, const Transaction* transaction) const {
  const auto numBoundNodes = getNumNodes(transaction, boundNode.getTableIDs());
  // Recursive traversal keeps its existing path-length heuristic.
  const auto numRels = rel.getRelType() == QueryRelType::NON_RECURSIVE
      ? getDirectionalRelationshipRows(tableIDs, boundNode.getTableIDs(), neighborTables, direction, transaction)
      : static_cast<WideCount>(getNumRels(transaction, tableIDs));
  NEUG_ASSERT(numBoundNodes > 0);
  const auto oneHopExtensionRate = static_cast<double>(static_cast<long double>(numRels) / numBoundNodes);
  switch (rel.getRelType()) {
  case QueryRelType::NON_RECURSIVE: {
    return oneHopExtensionRate;
  }
  case QueryRelType::VARIABLE_LENGTH_WALK:
  case QueryRelType::VARIABLE_LENGTH_TRAIL:
  case QueryRelType::VARIABLE_LENGTH_ACYCLIC:
  case QueryRelType::SHORTEST:
  case QueryRelType::ALL_SHORTEST:
  case QueryRelType::WEIGHTED_SHORTEST:
  case QueryRelType::ALL_WEIGHTED_SHORTEST: {
    double rate = std::pow(
        std::max<double>(1, std::floor(oneHopExtensionRate)),
        std::max<uint32_t>(rel.getRecursiveInfo()->bindData->upperBound, 1));
    double bound =
        oneHopExtensionRate *
        std::max<uint32_t>(rel.getRecursiveInfo()->bindData->upperBound, 1) *
        context->getClientConfig()->recursivePatternCardinalityScaleFactor;
    return std::min<double>(rate, bound);
  }
  default:
    NEUG_UNREACHABLE;
  }
}

}  // namespace planner
}  // namespace neug

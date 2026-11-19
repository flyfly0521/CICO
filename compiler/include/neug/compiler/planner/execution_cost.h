#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "neug/utils/api.h"

namespace neug::main {
class ClientContext;
}
namespace neug::planner {
class LogicalPlan;

inline constexpr const char* EXECUTION_COST_MODEL = "execution-work-v2";
inline constexpr const char* EXECUTION_COST_UNIT = "estimated_execution_work";
// Revision also records population propagation through correlated contexts and
// deterministic grouping/DISTINCT dependencies; units and JSON contract are
// unchanged.
inline constexpr const char* EXECUTION_COST_REVISION = "source-property-recursive-cardinality-5";

struct ExecutionCostOperator {
  std::string type;
  uint64_t inputRows = 0;
  uint64_t outputRows = 0;
  uint64_t localWork = 0;
};

struct ExecutionCostEstimate {
  uint64_t work = 0;
  bool supported = true;
  std::string reason;
  std::vector<ExecutionCostOperator> operators;
  std::vector<std::string> assumptions;
};

struct ExecutionCostOptions {
  bool allowPropertyUpdates = false;
  // The caller must establish that the updated properties have no auxiliary
  // maintenance callbacks. The scalar-write model does not price those jobs.
  bool assumeNoPropertyMaintenance = false;
};

// Shared by normal preparation and offline selection. Call only after the
// ordinary optimizer. Units count scalar operations and eight-byte transfers;
// they are not milliseconds or a claim of globally accurate cardinalities.
// A context supplies full scan domains even when a pushed predicate lowers the
// estimated output. Unsupported operators/expressions never receive zero cost.
NEUG_API ExecutionCostEstimate estimateExecutionCost(
    const LogicalPlan& plan, main::ClientContext* context = nullptr,
    ExecutionCostOptions options = {});
NEUG_API std::string executionCostJson(const ExecutionCostEstimate& estimate);
}  // namespace neug::planner

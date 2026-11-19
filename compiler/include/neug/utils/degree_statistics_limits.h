#pragma once
#include <cstdint>
namespace neug {
// Resource budgets for source graph statistics. A zero is an explicit bound,
// not a request for an unlimited scan. Missing observations remain unknown.
struct DegreeStatisticsLimits {
  uint64_t vertex_slots = 20'000'000;
  uint64_t edge_visits = 25'000'000;
  uint64_t pair_vertex_visits = 40'000'000;
  // Exact property frequencies are opt-in: ordinary metadata refreshes must
  // not acquire an additional full-property scan implicitly.
  uint64_t property_vertex_visits = 0;
  uint64_t property_max_distinct_values = 65'536;
};
}  // namespace neug

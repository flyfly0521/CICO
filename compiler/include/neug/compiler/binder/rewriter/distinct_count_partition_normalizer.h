#pragma once

#include <memory>

namespace neug::main { class ClientContext; }
namespace neug::binder {
class Binder;
class BoundStatement;
struct DistinctCountPartitions;

// COUNT(DISTINCT w) grouped by K equals SUM over (K, a) of COUNT(DISTINCT w)
// when w functionally determines a previously bound node a: the per-anchor
// sets are disjoint. The bound comes only from relationship multiplicities
// declared (NEUG_ASSUME_FUNCTIONAL_V1) by a FILL on a's label (LDBC BI6:
// a like determines its message, which has one creator). The split only
// exposes the complete per-anchor FILL count; unconsumed splits are restored.
std::shared_ptr<DistinctCountPartitions> normalizeDistinctCountPartitions(
    BoundStatement& statement, main::ClientContext& context, Binder* binder);
void restoreUnusedDistinctCountPartitions(const std::shared_ptr<DistinctCountPartitions>& state);
}  // namespace neug::binder

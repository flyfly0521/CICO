#pragma once

#include <memory>

namespace neug::main { class ClientContext; }
namespace neug::binder {
class Binder;
class BoundStatement;
struct FilteredCountNormalizations;

// SUM(CASE WHEN x IS NOT NULL AND <region filter> THEN 1 ELSE 0 END) over a
// final OPTIONAL MATCH region equals COUNT(x) after moving the filter into the
// OPTIONAL WHERE, when every aggregate of that part uses the same filter and
// nothing else reads the region (LDBC BI13). The normalization exists only to
// expose a filtered FILL COUNT to the index rewriter: whatever no rewrite
// consumed is restored, so the original plan is unchanged without an index.
std::shared_ptr<FilteredCountNormalizations> normalizeFilteredCounts(
    BoundStatement& statement, main::ClientContext& context, Binder* binder);
void restoreUnusedFilteredCounts(const std::shared_ptr<FilteredCountNormalizations>& state);
}  // namespace neug::binder

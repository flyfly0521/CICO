#pragma once

#include <string>

namespace neug::main {
class ClientContext;
}
namespace neug::binder {
class Binder;
class BoundStatement;
class NormalizedSingleQuery;
// A bounded scalar SET grammar whose writes are independent of every FILL
// source and target in the current catalog. Unknown provenance fails closed.
// This permits pricing complete read/write alternatives; it does not itself
// certify any substitution.
bool isIndependentTerminalPropertySet(const BoundStatement& statement,
                                      main::ClientContext& context);
// Prove an identity-keyed, deterministic COUNT-to-SUM pipeline including its
// exact OPTIONAL and grouping boundaries. This grants no flat-graph contract.
bool supportsStagedCountSumFill(const NormalizedSingleQuery& source,
                               const std::string& anchorUniqueName);
// Match an ordered, typed computation prefix against a registered FILL source.
// Discovery requires only the original query and the ready index catalog.
void rewriteComputationPrefixes(BoundStatement& statement,
                                main::ClientContext& context, Binder* binder);
}  // namespace neug::binder

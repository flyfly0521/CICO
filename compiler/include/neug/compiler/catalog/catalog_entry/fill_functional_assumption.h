#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "neug/compiler/common/types/types.h"

namespace neug {
namespace binder {
class PropertyExpression;
class QueryGraphCollection;
}
namespace main {
class ClientContext;
}
namespace catalog {
class TableCatalogEntry;

// An explicit, source-local assumption, not a catalog multiplicity change.
// Returns a signature only after matching the declaration to the actual bound
// graph and proving <=1 source ROW (including relationship multiplicity) per
// anchor. Absence returns nullopt; malformed or inapplicable declarations fail
// binding. The declaration remains in sourceQuery for persistence/rebinding.
std::optional<std::string> proveFillFunctionalAssumption(
    const std::string& sourceQuery,
    const binder::QueryGraphCollection& graph,
    const binder::PropertyExpression& anchorKey,
    const TableCatalogEntry& target, main::ClientContext& context);

// A declared multiplicity bound on the complete union of relationship tables.
struct DeclaredRelationshipMultiplicity {
  std::set<common::table_id_t> tables;
  bool atMostOneOutgoing = false;
  bool atMostOneIncoming = false;
};

// Relationship multiplicities declared by a FILL source with
// NEUG_ASSUME_FUNCTIONAL_V1, resolved against the catalog. Unlike the
// projection proof above, no closure over the source is required: consumers
// use individual bounds (e.g. a counted object determining its key). Absent or
// malformed declarations return nullopt, never a partial list.
std::optional<std::vector<DeclaredRelationshipMultiplicity>>
parseFillRelationshipDeclarations(const std::string& sourceQuery, main::ClientContext& context);

}  // namespace catalog
}  // namespace neug

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "neug/utils/api.h"

namespace neug::binder {
class Expression;

// Bind unique graph identities to positional tokens (for example node:0 and
// rel:1). Both FILL registration and the matched query use the same positions.
// Raw names, aliases and printed SQL are deliberately absent from the key.
using IndexPredicateBindings = std::unordered_map<std::string, std::string>;

// A typed structural key for scalar predicates. Equality/inequality operands
// are symmetric; all other operator and CASE ordering is preserved. An unknown
// binding, parameter, aggregate, subquery or unsupported scalar returns nullopt
// so a caller cannot mistake two unsupported expressions for equivalent ones.
NEUG_API std::optional<std::string> makeIndexPredicateKey(
    const Expression& expression, const IndexPredicateBindings& bindings);

// Preserve the v1 wire format while giving restored and freshly generated
// typed Boolean predicates the same canonical key. Unknown, malformed or
// excessively nested inputs remain opaque and are returned unchanged.
NEUG_API std::string normalizeIndexPredicateKey(const std::string& key);
}  // namespace neug::binder

#pragma once

namespace neug {
namespace catalog { class FillIndexAuxInfo; }
namespace main { class ClientContext; }
namespace binder {

// Prove that a derived aggregate relation has exactly one row for each
// nonempty pair of endpoint identities. Its stored aggregate values are not
// part of this contract and must not be substituted in a presence-only read.
// The caller must separately prove that the query consumes only the endpoint
// set and that every physical member of a label family satisfies this proof.
bool provesAggregateEndpointSetSource(
    const catalog::FillIndexAuxInfo& info, main::ClientContext& context);

}
}

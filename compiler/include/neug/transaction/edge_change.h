#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "neug/utils/property/property.h"
#include "neug/utils/property/types.h"

namespace neug {

enum class EdgeChangeKind : uint8_t { INSERT = 0, DELETE = 1 };

// Stable identity of one physical edge inside a concrete CSR table. The
// outgoing offset is the canonical component; the incoming offset is retained
// for precise transactional writes and undo. Endpoints alone are not an edge
// identity because NeuG permits parallel edges.
struct PhysicalEdgeIdentity {
  label_t edgeLabel{std::numeric_limits<label_t>::max()};
  label_t srcLabel{std::numeric_limits<label_t>::max()};
  label_t dstLabel{std::numeric_limits<label_t>::max()};
  vid_t src{std::numeric_limits<vid_t>::max()};
  vid_t dst{std::numeric_limits<vid_t>::max()};
  int32_t oeOffset{-1};
  int32_t ieOffset{-1};
  // Non-identity payload pointer for synchronous expression evaluation. This
  // deliberately does not participate in equality, ordering, or hashing.
  const void* propertyData{nullptr};

  // Either CSR offset makes the edge locatable (out-expand supplies the
  // outgoing offset, in-expand the incoming one).
  bool valid() const { return oeOffset >= 0 || ieOffset >= 0; }

  bool operator==(const PhysicalEdgeIdentity& other) const {
    return std::tie(edgeLabel, srcLabel, dstLabel, src, dst, oeOffset) ==
           std::tie(other.edgeLabel, other.srcLabel, other.dstLabel, other.src,
                    other.dst, other.oeOffset);
  }

  bool operator<(const PhysicalEdgeIdentity& other) const {
    return std::tie(edgeLabel, srcLabel, dstLabel, src, dst, oeOffset) <
           std::tie(other.edgeLabel, other.srcLabel, other.dstLabel, other.src,
                    other.dst, other.oeOffset);
  }
};

// Hash exactly the fields used by PhysicalEdgeIdentity::operator==. The
// incoming offset is deliberately excluded: the outgoing CSR offset is the
// canonical identity component.
struct PhysicalEdgeIdentityHash {
  size_t operator()(const PhysicalEdgeIdentity& edge) const noexcept {
    size_t seed = 0;
    auto mix = [&seed](uint64_t value) {
      seed ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL +
              (seed << 6) + (seed >> 2);
    };
    mix(static_cast<uint64_t>(edge.edgeLabel));
    mix(static_cast<uint64_t>(edge.srcLabel));
    mix(static_cast<uint64_t>(edge.dstLabel));
    mix(static_cast<uint64_t>(edge.src));
    mix(static_cast<uint64_t>(edge.dst));
    mix(static_cast<uint64_t>(static_cast<int64_t>(edge.oeOffset)));
    return seed;
  }
};

using EdgeOwnershipMap =
    std::unordered_map<PhysicalEdgeIdentity, uint64_t,
                       PhysicalEdgeIdentityHash>;

// One row in a logical mutation batch. propertyData points at the storage row
// for the duration of the synchronous maintenance callback. propertyImage is
// an owning pre/post image when the caller already has one; current node-only
// delta plans do not require it, but carrying it here avoids another event API
// migration when relationship-property maintenance is enabled.
struct EdgeChange {
  EdgeChangeKind kind{EdgeChangeKind::INSERT};
  PhysicalEdgeIdentity identity;
  const void* propertyData{nullptr};
  std::vector<Property> propertyImage;
  uint64_t eventId{0};
};

using EdgeChangeBatch = std::vector<EdgeChange>;

}  // namespace neug

#pragma once
#include <optional>
#include <unordered_map>

#include "pdnsol/common.hpp"
#include "pdnsol/io/parser_utils.hpp"

namespace pdnsol {
class NetId {
  public:
    static const NetId Invalid;

    NetId()
        : NetId(-1) {}
    constexpr explicit NetId(int v)
        : value{v} {}

    constexpr int      get() const noexcept { return value; }
    constexpr explicit operator bool() const noexcept { return value >= 0; }

    // Comparisons
    friend constexpr bool operator==(NetId a, NetId b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(NetId a, NetId b) noexcept {
        return a.value != b.value;
    }

  private:
    int value;
};
inline const NetId NetId::Invalid{-1};
// -------------------------------
// Domain model
// -------------------------------
struct NetKey {
    IdString layer;
    IdString netName;
    bool     isPower;
    bool     isGround;

    struct Hash {
        std::size_t operator()(const NetKey& v) const {
            std::size_t h1 = IdString::Hash{}(v.layer);
            std::size_t h2 = IdString::Hash{}(v.netName);
            // Boost-style combine
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };

    bool operator==(const NetKey& other) const noexcept {
        return layer == other.layer && netName == other.netName;
    }
};

struct Node {
    IdString mName;
    NetId    mNet; // Layer-(VDD/VSS) combination
    Tick     mX;   // micrometers, in FPN format
    Tick     mY;   // micrometers, in FPN format
};

struct MetalRes {
    IdString   mName;
    NetId      mNet; // Layer-(VDD/VSS) combination
    IdString   mN1;  // Node_1
    IdString   mN2;  // Node_2
    ScalarType mR = 0.0;
};

struct ViaRes {
    IdString   mName;
    IdString   mN1;
    IdString   mN2;
    ScalarType mR = 0.0;
};

struct PkgRes {
    IdString   mName;
    IdString   mN1;
    IdString   mN2;
    ScalarType mR = 0.0; // Resistance, Ohm
};

struct Vsrc {
    IdString mName;
    IdString mFromNode;
    IdString mToNode;
    // subtypes: "global", "via", "package", "other"
    enum Type { GLOBAL, VIA, PACKAGE, OTHER };
    Type       mType = OTHER;
    ScalarType mV    = 0.0;
};

struct Isrc {
    IdString mName;
    IdString mFromNode;
    IdString mToNode;
    // subtypes: "iB", "other"
    enum Type { IB, OTHER };
    Type       mType = OTHER;
    ScalarType mI    = 0.0; // Current, Amp
};

struct SectionMeta {
    IdString    mType; // "layer" or "vias"
    IdString    mName;
    IdString    mNet;
    IdString    mFromNet;
    IdString    mToNet;
    std::string mRaw;
};

struct CircuitGraph {
    enum Unit { UM, MM }; // Input unit
    Unit                     mCoordinateUnit = UM;
    IdString::Map<Node>      mNodes;
    std::vector<MetalRes>    mMetalResistors;
    std::vector<ViaRes>      mViaResistors;
    std::vector<PkgRes>      mPkgResistors;
    std::vector<Vsrc>        mVsrcs;
    std::vector<Isrc>        mIsrcs;
    std::vector<SectionMeta> mSections;
    IdString::Map<IdString>  mMetadata;

    std::unordered_map<NetKey, NetId, NetKey::Hash> mNet2Id; // Net to its id
    std::vector<NetKey>                             mId2Net;

    NetId  netId(IdString layer, IdString name) const;
    NetKey netKey(NetId netId) const;
    NetId  registerNet(IdString layer, IdString name, bool isPwr, bool isGnd);

    // Ensure a node exists, similar to getOrCreate
    Node& ensureNode(const IdString& name, int32_t net = -1,
                     std::optional<double> x = std::nullopt,
                     std::optional<double> y = std::nullopt);

    // Convenience: ensure all referenced nodes exist
    void ensureAllReferencedNodesExist();

    bool refersToGround() const;

    void validateReadyForMna() const;

    void purgeParallelElements();

    // Purge nodes that have no incident elements (degree == 0).
    // Returns the number of nodes removed.
    std::size_t purgeIsolatedNodes();
};
} // namespace pdnsol
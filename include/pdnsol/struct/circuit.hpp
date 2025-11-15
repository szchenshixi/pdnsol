#pragma once
#include <optional>
#include <unordered_set>

#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
struct Node;
using NodeMap = std::unordered_map<IdString, Node, IdString::Hash>;
using Tick = FPN::Rep;

// -------------------------------
// Domain model
// -------------------------------
struct Node {
    IdString mName;
    Tick mX; // meters
    Tick mY; // meters
    IdString mNet;
    IdString mLayer;
};

struct MetalRes {
    IdString mId;
    IdString mN1; // Node_1
    IdString mN2; // Node_2
    double mR = 0.0;
    IdString mLayer;
    IdString mNet;
};

struct ViaRes {
    IdString mId;
    IdString mN1;
    IdString mN2;
    double mR = 0.0;
    IdString mFromNet;
    IdString mToNet;
};

struct PkgRes {
    IdString mId;
    IdString mN1;
    IdString mN2;
    double mR = 0.0; // Resistance, Ohm
};

struct Vsrc {
    IdString mId;
    IdString mNPlus;
    IdString mNMinus;
    double mV = 0.0;
    // subtypes: "global", "via", "package", "other"
    enum Type { GLOBAL, VIA, PACKAGE, OTHER };
    Type mType = OTHER;
};

struct Isrc {
    IdString mId;
    IdString mNPlus;
    IdString mNMinus;
    double mI = 0.0; // Current, Amp
    // subtypes: "iB", "other"
    enum Type { IB, OTHER };
    Type mType = OTHER;
};

struct SectionMeta {
    IdString mType; // "layer" or "vias"
    IdString mName;
    IdString mNet;
    IdString mFromNet;
    IdString mToNet;
    std::optional<IdString> mRaw;
};

struct CircuitGraph {
    enum Unit { UM, MM }; // Input unit
    Unit mCoordinateUnit = UM;
    NodeMap mNodes;
    std::vector<MetalRes> mMetalResistors;
    std::vector<ViaRes> mViaResistors;
    std::vector<PkgRes> mPkgResistors;
    std::vector<Vsrc> mVsrcs;
    std::vector<Isrc> mIsrcs;
    std::vector<SectionMeta> mSections;
    std::unordered_map<IdString, IdString, IdString::Hash> mMetadata;

    // Return all nodes (const access)
    const NodeMap& allNodes() const;

    // Ensure a node exists, similar to getOrCreate
    Node& ensureNode(const IdString& name, IdString net = IdString(),
                     IdString layer = IdString(),
                     std::optional<double> x = std::nullopt,
                     std::optional<double> y = std::nullopt);

    std::size_t countVoltageSources() const;

    // Convenience: ensure all referenced nodes exist
    void ensureAllReferencedNodesExist();

    bool refersToGround() const;

    void validateReadyForMna() const;
};
} // namespace pdnsol
#pragma once
#include <optional>

#include "pdnsol/common.hpp"

namespace pdnsol {
// -------------------------------
// Domain model
// -------------------------------
struct Node {
    IdString mName;
    int32_t  mNet; // Layer-(VDD/VSS) combination
    Tick     mX;   // micrometers, in FPN format
    Tick     mY;   // micrometers, in FPN format
};

struct MetalRes {
    IdString   mName;
    int32_t    mNet; // Layer-(VDD/VSS) combination
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
    IdString                mType; // "layer" or "vias"
    IdString                mName;
    IdString                mNet;
    IdString                mFromNet;
    IdString                mToNet;
    std::optional<IdString> mRaw;
};

struct CircuitGraph {
    enum Unit { UM, MM }; // Input unit
    Unit                     mCoordinateUnit = UM;
    NodeMap                  mNodes;
    std::vector<MetalRes>    mMetalResistors;
    std::vector<ViaRes>      mViaResistors;
    std::vector<PkgRes>      mPkgResistors;
    std::vector<Vsrc>        mVsrcs;
    std::vector<Isrc>        mIsrcs;
    std::vector<SectionMeta> mSections;
    IdStringMap              mMetadata;

    // Return all nodes (const access)
    const NodeMap& allNodes() const;

    // Ensure a node exists, similar to getOrCreate
    Node& ensureNode(const IdString& name, int32_t net = -1,
                     std::optional<double> x = std::nullopt,
                     std::optional<double> y = std::nullopt);

    // Convenience: ensure all referenced nodes exist
    void ensureAllReferencedNodesExist();

    bool refersToGround() const;

    void validateReadyForMna() const;

    void purge_parallel_elements();
};
} // namespace pdnsol
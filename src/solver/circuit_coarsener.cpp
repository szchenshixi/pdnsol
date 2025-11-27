#include "pdnsol/solver/circuit_coarsener.hpp"

#include <algorithm> // std::max
#include <utility>   // std::move

namespace pdnsol {

// -----------------------------------------------------------------------------
// CircuitCoarsener public API
// -----------------------------------------------------------------------------

CircuitCoarsener::CircuitCoarsener(const CircuitGraph&      inGraph,
                                   const CoarseModelConfig& cfg)
    : mIn(inGraph)
    , mCfg(cfg) {
}

CircuitGraph CircuitCoarsener::build() {
    CircuitGraph out;
    out.mCoordinateUnit = mIn.mCoordinateUnit;
    out.mMetadata       = mIn.mMetadata;
    out.mSections       = mIn.mSections; // logical PDN description is reused

    if (mIn.mNodes.empty()) return out; // trivial

    mOutPtr = &out;

    computeBoundingBox();
    computeTileSize();
    initLayerModes();
    classifyNodeModes();

    // Build resistive network first (this will create coarse nodes).
    buildMetalResistors();
    buildViaResistors();
    buildPkgResistors();

    // Then remap all sources.
    buildVsrcs();
    buildIsrcs();

    mOutPtr = nullptr;
    return out;
}

// -----------------------------------------------------------------------------
// Geometry / tiling helpers
// -----------------------------------------------------------------------------

void CircuitCoarsener::computeBoundingBox() {
    bool first = true;
    for (const auto& kv : mIn.mNodes) {
        const Node& n = kv.second;
        if (n.mX < 0 || n.mY < 0) continue;
        if (first) {
            mMinX = n.mX;
            mMinY = n.mY;
            first = false;
        } else {
            if (n.mX < mMinX) mMinX = n.mX;
            if (n.mY < mMinY) mMinY = n.mY;
        }
    }
}

void CircuitCoarsener::computeTileSize() {
    // Requested tile size is in micrometers.
    ScalarType tileSizeUm = (mCfg.tileSizeUm > 0.0) ? mCfg.tileSizeUm : 100.0;

    // Tick is documented as "micro meters, 1e-6 m".
    ScalarType tileSizeTicks = tileSizeUm;
    if (mIn.mCoordinateUnit == CircuitGraph::MM) {
        // Coordinates are in mm; convert tile size to same unit:
        //   1 mm = 1000 um
        tileSizeTicks = tileSizeUm / 1000.0;
    }

    // At least 1 tick.
    mTileSizeTicks = FPN::toRep(tileSizeTicks);
}

int64_t CircuitCoarsener::tileIndex(Tick coord, Tick origin) const {
    return static_cast<int64_t>((coord - origin) / mTileSizeTicks);
}

detail::TileKey CircuitCoarsener::computeTileKey(const Node& n) const {
    detail::TileKey key;
    key.net = n.mNet;
    key.tx  = n.mX < 0 ? -1 : tileIndex(n.mX, mMinX);
    key.ty  = n.mY < 0 ? -1 : tileIndex(n.mY, mMinY);
    return key;
}

// -----------------------------------------------------------------------------
// Layer / node classification
// -----------------------------------------------------------------------------

void CircuitCoarsener::initLayerModes() {
    // Start from user overrides.
    mLayerMode = mCfg.perLayerMode;

    // Any MetalRes layer not explicitly mentioned gets default Approximate.
    for (const auto& r : mIn.mMetalResistors) {
        if (mLayerMode.find(r.mName) == mLayerMode.end()) {
            mLayerMode[r.mName] = LayerMode::Approximate;
        }
    }
}

void CircuitCoarsener::classifyNodeModes() {
    // Initialize all nodes as Unknown.
    for (const auto& kv : mIn.mNodes) {
        mNodeMode.emplace(kv.first, detail::NodeMode::Unknown);
    }

    // Mark nodes touched by metal resistors.
    for (const auto& r : mIn.mMetalResistors) {
        LayerMode layerMode = LayerMode::Approximate;
        auto      it        = mLayerMode.find(r.mName);
        if (it != mLayerMode.end()) {
            layerMode = it->second;
        }

        detail::NodeMode nm = (layerMode == LayerMode::Accurate)
                                ? detail::NodeMode::Accurate
                                : detail::NodeMode::Approximate;

        bumpNodeMode(r.mN1, nm);
        bumpNodeMode(r.mN2, nm);
    }

    // Any remaining Unknown nodes (only vias / sources / package, no metal)
    // are treated as Accurate so we don't accidentally collapse pins, etc.
    for (auto& kv : mNodeMode) {
        if (kv.second == detail::NodeMode::Unknown) {
            kv.second = detail::NodeMode::Accurate;
        }
    }
}

void CircuitCoarsener::bumpNodeMode(IdString         nodeId,
                                    detail::NodeMode newMode) {
    auto it = mNodeMode.find(nodeId);
    if (it == mNodeMode.end()) {
        mNodeMode.emplace(nodeId, newMode);
        return;
    }
    detail::NodeMode& cur = it->second;
    if (cur == detail::NodeMode::Unknown) {
        cur = newMode;
    } else if (cur == detail::NodeMode::Approximate &&
               newMode == detail::NodeMode::Accurate) {
        cur = newMode; // Accurate wins over Approximate
    }
    // If already Accurate, keep it Accurate.
}

// -----------------------------------------------------------------------------
// Node mapping
// -----------------------------------------------------------------------------

NodeMap& CircuitCoarsener::outNodes() {
    assert(mOutPtr && "Output graph pointer not set");
    return mOutPtr->mNodes;
}

IdString CircuitCoarsener::mapNode(IdString inId) {
    auto mapIt = mNodeMap.find(inId);
    if (mapIt != mNodeMap.end()) {
        return mapIt->second;
    }

    auto             nmIt = mNodeMode.find(inId);
    detail::NodeMode mode =
      (nmIt != mNodeMode.end()) ? nmIt->second : detail::NodeMode::Accurate;

    if (mode == detail::NodeMode::Accurate) {
        return createAccurateNode(inId);
    } else { // Approximate
        return createCoarseNode(inId);
    }
}

IdString CircuitCoarsener::createAccurateNode(IdString inId) {
    // Check if we already created it.
    auto mapIt = mNodeMap.find(inId);
    if (mapIt != mNodeMap.end()) {
        return mapIt->second;
    }

    auto nodeIt = mIn.mNodes.find(inId);
    assert(nodeIt != mIn.mNodes.end() &&
           "Unknown node id in createAccurateNode");

    const Node& inNode = nodeIt->second;

    // We reuse the same IdString as the original node name.
    IdString outId   = inId;
    Node     outNode = inNode; // copy geometry, net, etc.

    mNodeMap.emplace(inId, outId);
    outNodes().emplace(outId, outNode);
    return outId;
}

IdString CircuitCoarsener::createCoarseNode(IdString inId) {
    // If this fine node has already been mapped, reuse.
    auto mapIt = mNodeMap.find(inId);
    if (mapIt != mNodeMap.end()) {
        return mapIt->second;
    }

    auto nodeIt = mIn.mNodes.find(inId);
    assert(nodeIt != mIn.mNodes.end() &&
           "Unknown node id in createCoarseNode");

    const Node&     inNode = nodeIt->second;
    detail::TileKey key    = computeTileKey(inNode);

    // Check if this tile already has a coarse node.
    auto tileIt = mTileNodeMap.find(key);
    if (tileIt != mTileNodeMap.end()) {
        IdString coarseId = tileIt->second;
        mNodeMap.emplace(inId, coarseId);
        return coarseId;
    }

    // Create a brand-new coarse node for this tile.
    IdString coarseId("COARSE_" + std::to_string(key.net) + "_" +
                      std::to_string(key.tx) + "_" + std::to_string(key.ty));

    Node coarseNode;
    coarseNode.mName = coarseId;
    coarseNode.mNet  = inNode.mNet;

    // Place coarse node at the tile center.
    coarseNode.mX =
      mMinX + static_cast<Tick>(key.tx * mTileSizeTicks + mTileSizeTicks / 2);
    coarseNode.mY =
      mMinY + static_cast<Tick>(key.ty * mTileSizeTicks + mTileSizeTicks / 2);

    mTileNodeMap.emplace(key, coarseId);
    mNodeMap.emplace(inId, coarseId);
    outNodes().emplace(coarseId, coarseNode);

    return coarseId;
}

// -----------------------------------------------------------------------------
// Build resistive network
// -----------------------------------------------------------------------------

void CircuitCoarsener::buildMetalResistors() {
    assert(mOutPtr);
    auto& out = *mOutPtr;
    out.mMetalResistors.clear();
    out.mMetalResistors.reserve(mIn.mMetalResistors.size());

    for (const auto& r : mIn.mMetalResistors) {
        // Decide how the endpoints map (accurate vs coarse) based on node
        // mode.
        IdString n1 = mapNode(r.mN1);
        IdString n2 = mapNode(r.mN2);

        if (n1 == n2) {
            // Entire segment collapsed inside one tile; drop.
            // Physically, this is sub-tile detail which we ignore at coarse
            // scale.
            continue;
        }

        MetalRes cr = r;
        cr.mN1      = n1;
        cr.mN2      = n2;

        // We keep cr.mName and cr.mNet unchanged; the layer identity is
        // preserved.
        out.mMetalResistors.push_back(std::move(cr));
    }
}

void CircuitCoarsener::buildViaResistors() {
    assert(mOutPtr);
    auto& out = *mOutPtr;
    out.mViaResistors.clear();
    out.mViaResistors.reserve(mIn.mViaResistors.size());

    for (const auto& v : mIn.mViaResistors) {
        IdString n1 = mapNode(v.mN1);
        IdString n2 = mapNode(v.mN2);

        if (n1 == n2) {
            // Via endpoints collapsed into same coarse node (local vertical
            // connection within a tile); ignore at this scale.
            continue;
        }

        ViaRes cv = v;
        cv.mN1    = n1;
        cv.mN2    = n2;
        out.mViaResistors.push_back(std::move(cv));
    }
}

void CircuitCoarsener::buildPkgResistors() {
    assert(mOutPtr);
    auto& out = *mOutPtr;
    out.mPkgResistors.clear();
    out.mPkgResistors.reserve(mIn.mPkgResistors.size());

    for (const auto& p : mIn.mPkgResistors) {
        IdString n1 = mapNode(p.mN1);
        IdString n2 = mapNode(p.mN2);

        if (n1 == n2) continue;

        PkgRes cp = p;
        cp.mN1    = n1;
        cp.mN2    = n2;
        out.mPkgResistors.push_back(std::move(cp));
    }
}

// -----------------------------------------------------------------------------
// Build sources
// -----------------------------------------------------------------------------

void CircuitCoarsener::buildVsrcs() {
    assert(mOutPtr);
    auto& out = *mOutPtr;
    out.mVsrcs.clear();
    out.mVsrcs.reserve(mIn.mVsrcs.size());

    for (const auto& vs : mIn.mVsrcs) {
        IdString from = mapNode(vs.mFromNode);
        IdString to   = mapNode(vs.mToNode);

        if (from == to) {
            // Source between same node is ineffective; drop.
            continue;
        }

        Vsrc nvs      = vs;
        nvs.mFromNode = from;
        nvs.mToNode   = to;
        out.mVsrcs.push_back(std::move(nvs));
    }
}

void CircuitCoarsener::buildIsrcs() {
    assert(mOutPtr);
    auto& out = *mOutPtr;
    out.mIsrcs.clear();
    out.mIsrcs.reserve(mIn.mIsrcs.size());

    for (const auto& is : mIn.mIsrcs) {
        IdString from = mapNode(is.mFromNode);
        IdString to   = mapNode(is.mToNode);

        if (from == to) {
            continue;
        }

        Isrc nis      = is;
        nis.mFromNode = from;
        nis.mToNode   = to;
        out.mIsrcs.push_back(std::move(nis));
    }
}

} // namespace pdnsol
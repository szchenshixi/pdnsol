#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

// How a PDN "layer" (identified by MetalRes::mName) should be treated.
enum class LayerMode : uint8_t {
    Accurate,   //< Keep all original geometry (no spatial coarsening)
    Approximate //< Merge nodes into tiles, drop intra-tile detail
};

// Configuration for coarse-model construction.
struct CoarseModelConfig {
    // Tile size in micrometers (um) used for approximate layers.
    // All nodes of an approximate layer whose (x,y) fall into the same
    // tile are merged into one coarse node.
    ScalarType tileSizeUm = 100.0;

    // Per-layer overrides:
    //   key:   MetalRes::mName (i.e., the PDN section / layer name)
    //   value: Accurate or Approximate
    //
    // Any MetalRes with mName not present here will default to Approximate.
    // Typically you'll explicitly mark:
    //   - top-most metals (e.g., M9 VDD, M9 VSS) as Accurate
    //   - bottom rails (e.g., M1 straps) as Accurate
    //   - all middle/global distribution metals as Approximate
    IdString::Map<LayerMode> perLayerMode;
};

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

namespace detail {

inline void hash_combine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

// At the node level we only distinguish "kept as-is" vs "merged into tiles".
enum class NodeMode : uint8_t { Unknown = 0, Approximate = 1, Accurate = 2 };

// Key that identifies a coarse tile node: (net, tileX, tileY).
// net is Node::mNet (which encodes rail + layer/stack index in your model).
struct TileKey {
    int32_t net; //< Node::mNet
    int64_t tx;  //< tile index in X
    int64_t ty;  //< tile index in Y

    bool operator==(const TileKey& o) const noexcept {
        return net == o.net && tx == o.tx && ty == o.ty;
    }
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept {
        std::size_t h = std::hash<int32_t>{}(k.net);
        hash_combine(h, std::hash<int64_t>{}(k.tx));
        hash_combine(h, std::hash<int64_t>{}(k.ty));
        return h;
    }
};

} // namespace detail

// -----------------------------------------------------------------------------
// CircuitCoarsener
// -----------------------------------------------------------------------------

// Build a coarse PDN model from a detailed CircuitGraph.
//
// - Accurate layers: nodes and metal resistors are kept at original
// granularity.
// - Approximate layers: nodes are merged into tiles; metal segments fully
//   inside one tile are dropped; cross-tile segments are kept but connect
//   coarse tile nodes.
//
// Vias, package resistors, and sources (Vsrc/Isrc) are remapped onto the
// new node set, so VDD and VSS nets are both fully represented at coarse
// level.
class CircuitCoarsener {
  public:
    CircuitCoarsener(const CircuitGraph&      inGraph,
                     const CoarseModelConfig& cfg);

    // Construct and return the coarse CircuitGraph.
    CircuitGraph build();

  private:
    // Input / configuration
    const CircuitGraph& mIn;
    CoarseModelConfig   mCfg;

    // Output graph under construction (non-owning)
    CircuitGraph* mOutPtr = nullptr;

    // Derived configuration:
    // (MetalRes::mName) -> LayerMode
    IdString::Map<LayerMode> mLayerMode;

    // Node classification: (Node::mName) -> NodeMode
    IdString::Map<detail::NodeMode> mNodeMode;

    // Geometry and tiling
    Tick mMinX          = 0;
    Tick mMinY          = 0;
    Tick mTileSizeTicks = 1; // tile size in "Tick" units

    // Node mapping:
    //   original node id -> output node id
    IdString::Map<IdString> mNodeMap;

    // Tile -> coarse node id (for approximate layers)
    std::unordered_map<detail::TileKey, IdString, detail::TileKeyHash>
      mTileNodeMap;

  private:
    // ----------------------------
    // Geometry / tiling helpers
    // ----------------------------

    void            computeBoundingBox();
    void            computeTileSize();
    int64_t         tileIndex(Tick coord, Tick origin) const;
    detail::TileKey computeTileKey(const Node& n) const;

    // ----------------------------
    // Layer / node classification
    // ----------------------------

    void initLayerModes();
    void classifyNodeModes();
    void bumpNodeMode(IdString nodeId, detail::NodeMode newMode);

    // ----------------------------
    // Node mapping
    // ----------------------------

    IdString::Map<Node>& outNodes();
    IdString             mapNode(IdString inId);
    IdString             createAccurateNode(IdString inId);
    IdString             createCoarseNode(IdString inId);

    // ----------------------------
    // Build resistive network
    // ----------------------------

    void buildMetalResistors();
    void buildViaResistors();
    void buildPkgResistors();

    // ----------------------------
    // Build sources
    // ----------------------------

    void buildVsrcs();
    void buildIsrcs();
};

} // namespace pdnsol
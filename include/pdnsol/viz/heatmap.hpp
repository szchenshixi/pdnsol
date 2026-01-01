#pragma once

// Multi-layer / multi-net IR-drop heatmap generation
// ---------------------------------------------------
//
// This header declares utilities to:
//   1. Bin IR-drop values per (layer, VDD/VSS) net (netId).
//   2. Generate a separate 2D heatmap for each netId.
//       - netId = Node::mNet is encoded as:
//           layerIndex = netId / 2
//           supplyType = netId % 2   (0 -> VDD, 1 -> VSS)
//   3. Export each heatmap as a color-coded PNG image (via stb_image_write).
//
// Integration:
//   - Include this header in translation units where you want to
//     build or write heatmaps.
//   - Add heatmap.cpp to your build.
//   - Ensure stb_image_write.h is available in your include path.
//
// Example usage (after solveMNA):
//
//   IRDropHeatmapConfig cfg;
//   cfg.width = 4096;
//   cfg.height = 4096;
//   cfg.metric = IRDropHeatmapConfig::Metric::IR_DROP;
//   cfg.vddNominal = 1.0;
//   cfg.vssNominal = 0.0;
//   cfg.includeVdd = true;
//   cfg.includeVss = false; // VDD only, for example
//
//   auto heatmaps = buildIRDropHeatmapsMultiNet(circ, sol, cfg);
//   writeAllHeatmapsToPng(heatmaps, "output_dir", /*useMaxValue=*/true);
//
// You will get one PNG image per netId, named like "M3_VDD.png", etc.
//
// For backward compatibility, PPM-named helpers
//   - writeHeatmapToPpm
//   - writeAllHeatmapsToPpm
// are provided. They are thin wrappers around the PNG writers and
// still generate PNG files on disk.

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {

// Forward declarations to avoid pulling large headers into users of this API
class CircuitGraph;
class MNASolution;

// -------------------------
// Configuration & basic types
// -------------------------

struct IRDropHeatmapConfig {
    // Output resolution (same for all nets).
    int width  = 32;
    int height = 32;

    // Bounding box in layout coordinates (meters)
    // If useCustomBBox == false, the bounding box is inferred
    // from all nodes in CircuitGraph
    bool   useCustomBBox = false;
    double minX          = 0.0;
    double minY          = 0.0;
    double maxX          = 0.0;
    double maxY          = 0.0;

    // Metric to visualize.
    //   IR_DROP:
    //       For VDD nets:  vddNominal - Vnode  (drop > 0 means worse)
    //       For VSS nets:  Vnode - vssNominal  (bounce > 0 means worse)
    //
    //   RAW_VOLTAGE:
    //       Visualize the raw node voltage Vnode
    enum class Metric { IR_DROP, RAW_VOLTAGE };
    Metric metric = Metric::IR_DROP;

    // Nominal voltages for VDD and VSS (for IR_DROP metric)
    double vddNominal = 1.0; // e.g. 1.0 V
    double vssNominal = 0.0; // e.g. 0.0 V

    // Filters for which nets to include.
    bool includeVdd = true; // nets with netId % 2 == 0
    bool includeVss = true; // nets with netId % 2 == 1

    // Optional filter: layer indices to include.
    // If empty, all layers are included
    std::vector<IdString> includedLayers;
};

// Per-pixel aggregated data.
struct IRDropCell {
    float    minVal; // minimum metric in this cell
    float    maxVal; // maximum metric in this cell
    double   sumVal; // sum of metric values (for averaging)
    uint32_t count;  // how many samples fell into this cell

    IRDropCell()
        : minVal(std::numeric_limits<float>::infinity())
        , maxVal(-std::numeric_limits<float>::infinity())
        , sumVal(0.0)
        , count(0) {}
};

// Single heatmap: 2D grid over a rectangular region
struct IRDropHeatmap {
    int width  = 0;
    int height = 0;

    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;

    // Row-major storage: cells[y * width + x]
    std::vector<IRDropCell> cells;

    IRDropHeatmap() = default;

    IRDropHeatmap(int w, int h, double x0, double y0, double x1, double y1)
        : width(w)
        , height(h)
        , minX(x0)
        , minY(y0)
        , maxX(x1)
        , maxY(y1)
        , cells(static_cast<size_t>(w) * static_cast<size_t>(h)) {}
};

// One heatmap per netId (layer-(VDD/VSS) combination).
using HeatmapByNet = std::unordered_map<int, IRDropHeatmap>;

// Layout bounding box (meters).
struct LayoutBBox {
    double minX;
    double minY;
    double maxX;
    double maxY;
};

// Decode netId into (layerIndex, isVdd)
struct NetDecomposition {
    int32_t layer; // netId / 2
    bool    isVdd; // true if netId % 2 == 0
};

// -------------------------
// Helpers for geometry & net decoding
// -------------------------

// Compute a global bounding box over all nodes in the circuit
LayoutBBox computeLayoutBoundingBox(const CircuitGraph& circ,
                                    bool                skipGndNode = true);

// Decode netId into (layerIndex, isVdd).
inline NetDecomposition decodeNetId(int32_t netId) {
    NetDecomposition d;
    d.layer = netId / 2 + 1;
    d.isVdd = ((netId % 2) == 1);
    return d;
}

// Make a human-readable label for file naming, e.g. "M3_VDD"
std::string makeNetLabel(int32_t netId);

// Compute the IR-drop-related metric for a node voltage
inline double computeMetric(double Vnode, bool isVdd,
                            const IRDropHeatmapConfig& cfg) {
    if (cfg.metric == IRDropHeatmapConfig::Metric::RAW_VOLTAGE) {
        return Vnode;
    }

    // IR_DROP:
    //   For VDD: drop = vddNominal - Vnode  (positive = worse IR drop)
    //   For VSS: bounce = Vnode - vssNominal (positive = worse ground bounce)
    if (isVdd) {
        return cfg.vddNominal - Vnode;
    } else {
        return Vnode - cfg.vssNominal;
    }
}

// Build an empty IRDropHeatmap object for a given bounding box and resolution.
inline IRDropHeatmap makeEmptyHeatmap(const LayoutBBox& bbox, int width,
                                      int height) {
    return IRDropHeatmap(
      width, height, bbox.minX, bbox.minY, bbox.maxX, bbox.maxY);
}

// -------------------------
// Heatmap construction: multi-layer / multi-net binning
// -------------------------

// Main entry: build IR-drop heatmaps, one per netId (layer-net combination).
//
// Returns: map from netId -> IRDropHeatmap.
// Each heatmap covers the same bounding box.
HeatmapByNet buildIRDropHeatmapsMultiNet(const CircuitGraph&        circ,
                                         const MNASolution&         sol,
                                         const IRDropHeatmapConfig& cfg);

// -------------------------
// Scalar extraction & colormap
// -------------------------

// Extract one scalar per pixel from a heatmap.
// If useMaxValue == true, use maximum metric in each cell;
// otherwise, use average (sum/count).
void extractScalarImage(const IRDropHeatmap& hm, std::vector<float>& out,
                        bool useMaxValue);

// Simple RGB struct for image output.
struct RGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// A simple blue->green->red "jet-like" colormap.
// v is normalized between [vmin, vmax].
RGB applyColormap(float v, float vmin, float vmax);

// -------------------------
// PNG writers
// -------------------------

// Write a single heatmap to a PNG image file using stb_image_write.
void writeHeatmapToPng(const IRDropHeatmap& hm, const std::string& filename,
                       bool useMaxValue);

// Write all heatmaps (one per netId) to PNG files in the given directory.
// Files are named as "<outputDir>/<netLabel>.png", e.g. "M3_VDD.png".
void writeAllHeatmapsToPng(const HeatmapByNet& heatmaps,
                           const std::string& outputDir, bool useMaxValue);

// -------------------------
// Legacy PPM-named wrappers (still output PNG)
// -------------------------

inline void writeHeatmapToPpm(const IRDropHeatmap& hm,
                              const std::string& filename, bool useMaxValue) {
    writeHeatmapToPng(hm, filename, useMaxValue);
}

inline void writeAllHeatmapsToPpm(const HeatmapByNet& heatmaps,
                                  const std::string&  outputDir,
                                  bool                useMaxValue) {
    writeAllHeatmapsToPng(heatmaps, outputDir, useMaxValue);
}

} // namespace pdnsol
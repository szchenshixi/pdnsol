// Multi-layer / multi-net IR-drop heatmap generation - implementation
// -------------------------------------------------------------------
//
// See heatmap.hpp for public API and usage notes. This file
// contains the implementation, including PNG writing via stb_image_write.

#include "pdnsol/viz/heatmap.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// stb_image_write is a single-header library.
// Make sure this is the only translation unit in your project that defines
// STB_IMAGE_WRITE_IMPLEMENTATION, or adjust as needed.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <../3rdparty/stb/stb_image_write.h>

#include "pdnsol/common.hpp"
#include "pdnsol/solver/solver_basic.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {

// -------------------------
// Helpers for geometry & net decoding
// -------------------------

// Compute a global bounding box over all nodes in the circuit.
LayoutBBox computeLayoutBoundingBox(const CircuitGraph& circ,
                                    bool                skipGndNode) {
    LayoutBBox bbox;
    bbox.minX = bbox.minY = std::numeric_limits<double>::infinity();
    bbox.maxX = bbox.maxY = -std::numeric_limits<double>::infinity();

    for (const auto& kv : circ.mNodes) {
        const IdString& name = kv.first;
        const Node&     node = kv.second;

        if (skipGndNode && name == "GND") {
            continue;
        }
        // The node has no geometry information
        if (node.mX < 0 || node.mY < 0) {
            continue;
        }

        double x = FPN::fromRep(node.mX);
        double y = FPN::fromRep(node.mY);

        if (x < bbox.minX) bbox.minX = x;
        if (y < bbox.minY) bbox.minY = y;
        if (x > bbox.maxX) bbox.maxX = x;
        if (y > bbox.maxY) bbox.maxY = y;
    }

    if (!std::isfinite(bbox.minX) || !std::isfinite(bbox.minY) ||
        !std::isfinite(bbox.maxX) || !std::isfinite(bbox.maxY)) {
        // If no valid nodes, fall back to a zero-sized box.
        bbox.minX = bbox.minY = 0.0;
        bbox.maxX = bbox.maxY = 0.0;
    }

    return bbox;
}

// Make a human-readable label for file naming, e.g. "M3_VDD".
std::string makeNetLabel(int32_t netId) {
    NetDecomposition d = decodeNetId(netId);

    // NOTE: You can adjust the layer numbering if your M1 is layer=0 or 1.
    // Here we assume M0 corresponds to layer index 0.
    std::ostringstream oss;
    oss << "M" << d.layer << (d.isVdd ? "_VDD" : "_VSS");
    return oss.str();
}

// -------------------------
// Heatmap construction: multi-layer / multi-net binning
// -------------------------

HeatmapByNet buildIRDropHeatmapsMultiNet(const CircuitGraph&        circ,
                                         const MNASolution&         sol,
                                         const IRDropHeatmapConfig& cfg) {
    // 1. Determine bounding box
    LayoutBBox bbox;
    if (cfg.useCustomBBox) {
        bbox.minX = cfg.minX;
        bbox.minY = cfg.minY;
        bbox.maxX = cfg.maxX;
        bbox.maxY = cfg.maxY;
    } else {
        bbox = computeLayoutBoundingBox(circ, /*skipGndNode=*/true);
    }

    if (bbox.maxX <= bbox.minX || bbox.maxY <= bbox.minY) {
        throw std::runtime_error(
          "IR-drop heatmap: invalid bounding box (zero or negative size).");
    }

    if (cfg.width <= 0 || cfg.height <= 0) {
        throw std::runtime_error(
          "IR-drop heatmap: width/height must be positive.");
    }

    const double dx = (bbox.maxX - bbox.minX) / static_cast<double>(cfg.width);
    const double dy =
      (bbox.maxY - bbox.minY) / static_cast<double>(cfg.height);

    if (dx <= 0.0 || dy <= 0.0) {
        throw std::runtime_error(
          "IR-drop heatmap: invalid pixel size (dx/dy <= 0).");
    }

    // Layer filter (optional).
    IdString::Set<IdString> layerFilter;
    if (!cfg.includedLayers.empty()) {
        layerFilter.insert(cfg.includedLayers.begin(),
                           cfg.includedLayers.end());
    }

    HeatmapByNet heatmaps;
    heatmaps.reserve(16); // heuristic

    // 2. Single pass over all solved nodes
    for (const auto& kv : sol.mNodeIndex) {
        const IdString& nodeName = kv.first;
        int64_t         solIndex = kv.second;

        // We skip GND as it's typically reference (0V) and not a physical node
        // in the grid for IR-drop visualization. Adjust if needed.
        if (nodeName == "GND") {
            continue;
        }

        // Find the node in circuit graph to get geometry & netId.
        auto itNode = circ.mNodes.find(nodeName);
        if (itNode == circ.mNodes.end()) {
            // Should not happen if the graph and MNA are consistent.
            continue;
        }
        const Node& node = itNode->second;
        const NetId& netId = node.mNet;

        if (!netId) {
            // Ignore invalid netId if any.
            continue;
        }
        const NetKey& netKey = circ.netKey(node.mNet);
        const bool isVdd = netKey.isPower;

        // Apply filters: VDD/VSS
        if (isVdd && !cfg.includeVdd) continue;
        if (!isVdd && !cfg.includeVss) continue;

        // Apply layer filter (if any).
        if (!layerFilter.empty() &&
            layerFilter.find(netKey.layer) == layerFilter.end()) {
            continue;
        }

        // Convert coordinates to meters and then to pixel indices.
        double x = FPN::fromRep(node.mX);
        double y = FPN::fromRep(node.mY);

        int ix = static_cast<int>((x - bbox.minX) / dx);
        int iy = static_cast<int>((y - bbox.minY) / dy);

        // Guard against boundary issues (clamp or skip).
        if (ix < 0 || ix >= cfg.width || iy < 0 || iy >= cfg.height) {
            // Node outside bounding box; ignore.
            continue;
        }

        // Voltage from MNA solution.
        double Vnode = sol.mXFull(solIndex);

        // Metric to be visualized.
        double metric = computeMetric(Vnode, isVdd, cfg);
        float  fVal   = static_cast<float>(metric);

        // 3. Get or create heatmap for this netId.
        auto itHm = heatmaps.find(netId.get());
        if (itHm == heatmaps.end()) {
            IRDropHeatmap hm  = makeEmptyHeatmap(bbox, cfg.width, cfg.height);
            auto          res = heatmaps.emplace(netId.get(), std::move(hm));
            itHm              = res.first;
        }

        IRDropHeatmap& hm = itHm->second;
        IRDropCell&    cell =
          hm.cells[static_cast<size_t>(iy) * static_cast<size_t>(hm.width) +
                   static_cast<size_t>(ix)];

        // 4. Aggregate into the cell.
        if (fVal < cell.minVal) cell.minVal = fVal;
        if (fVal > cell.maxVal) cell.maxVal = fVal;
        cell.sumVal += metric;
        cell.count += 1;
    }

    return heatmaps;
}

// -------------------------
// Scalar extraction & colormap
// -------------------------

void extractScalarImage(const IRDropHeatmap& hm, std::vector<float>& out,
                        bool useMaxValue) {
    const size_t n = hm.cells.size();
    out.resize(n);

    for (size_t i = 0; i < n; ++i) {
        const IRDropCell& c = hm.cells[i];
        if (c.count == 0) {
            out[i] = std::numeric_limits<float>::quiet_NaN();
        } else if (useMaxValue) {
            out[i] = c.maxVal;
        } else {
            out[i] =
              static_cast<float>(c.sumVal / static_cast<double>(c.count));
        }
    }
}

// A smooth "jet"-style colormap similar to MATLAB/Matplotlib "jet":
// low values -> dark blue, then cyan/green, then yellow, then red.
inline RGB applyColormap(float v, float vmin, float vmax) {
    if (std::isnan(v)) {
        // No data -> black
        return RGB{0, 0, 0};
    }

    if (vmax <= vmin) {
        // Avoid division by zero; fallback to mid-gray.
        return RGB{127, 127, 127};
    }

    // v is normalized between [vmin, vmax].
    float t = (v - vmin) / (vmax - vmin);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    auto clip01 = [](float x) -> float {
        if (x < 0.0f) return 0.0f;
        if (x > 1.0f) return 1.0f;
        return x;
    };

    // Jet-like approximation (see e.g. Matplotlib/MATLAB "jet" discussions):
    //   r ~ peak around t=0.75
    //   g ~ peak around t=0.50
    //   b ~ peak around t=0.25
    float r = clip01(1.5f - 4.0f * std::fabs(t - 0.75f));
    float g = clip01(1.5f - 4.0f * std::fabs(t - 0.50f));
    float b = clip01(1.5f - 4.0f * std::fabs(t - 0.25f));

    auto toByte = [](float c) -> uint8_t {
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        return static_cast<uint8_t>(c * 255.0f + 0.5f);
    };

    RGB color;
    color.r = toByte(r);
    color.g = toByte(g);
    color.b = toByte(b);
    return color;
}

// -------------------------
// PNG writers
// -------------------------

// Write a single heatmap to a PNG image file using stb_image_write.
void writeHeatmapToPng(const IRDropHeatmap& hm, const std::string& filename,
                       bool useMaxValue) {
    const int HEATMAP_W = hm.width;
    const int HEATMAP_H = hm.height;
    const int OUTPUT_W  = 1024;
    const int OUTPUT_H  = 1024;

    if (HEATMAP_W <= 0 || HEATMAP_H <= 0) {
        throw std::runtime_error(
          "IR-drop heatmap: invalid dimensions for PNG.");
    }

    std::vector<float> scalar;
    extractScalarImage(hm, scalar, useMaxValue);

    // Compute global min/max (ignoring NaNs).
    float vmin = std::numeric_limits<float>::infinity();
    float vmax = -std::numeric_limits<float>::infinity();

    for (float v : scalar) {
        if (std::isnan(v)) continue;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    if (!std::isfinite(vmin) || !std::isfinite(vmax)) {
        // If no valid data, write a black image instead of failing.
        vmin = 0.0f;
        vmax = 1.0f;
    }

    // Allocate RGB buffer for 1024x1024 output (row-major, top-to-bottom).
    std::vector<uint8_t> pixels(static_cast<size_t>(OUTPUT_W) *
                                static_cast<size_t>(OUTPUT_H) * 3u);

    for (int y = 0; y < OUTPUT_H; ++y) {
        for (int x = 0; x < OUTPUT_W; ++x) {
            // Map output coordinates to heatmap coordinates
            float heatmap_x = (static_cast<float>(x) / OUTPUT_W) * HEATMAP_W;
            float heatmap_y = (static_cast<float>(y) / OUTPUT_H) * HEATMAP_H;

            // Get the nearest neighbor in the heatmap
            int heatmap_xi = static_cast<int>(heatmap_x);
            int heatmap_yi = static_cast<int>(heatmap_y);

            // Clamp to valid range
            heatmap_xi = std::clamp(heatmap_xi, 0, HEATMAP_W - 1);
            heatmap_yi = std::clamp(heatmap_yi, 0, HEATMAP_H - 1);

            size_t heatmap_idx = static_cast<size_t>(heatmap_yi) *
                                   static_cast<size_t>(HEATMAP_W) +
                                 static_cast<size_t>(heatmap_xi);
            RGB color = applyColormap(scalar[heatmap_idx], vmin, vmax);

            size_t output_base =
              (static_cast<size_t>(y) * OUTPUT_W + static_cast<size_t>(x)) *
              3u;
            pixels[output_base + 0] = color.r;
            pixels[output_base + 1] = color.g;
            pixels[output_base + 2] = color.b;
        }
    }

    const int strideBytes = OUTPUT_W * 3;

    if (!stbi_write_png(filename.c_str(),
                        OUTPUT_W,
                        OUTPUT_H,
                        /*comp=*/3,
                        pixels.data(),
                        strideBytes)) {
        throw std::runtime_error(
          "IR-drop heatmap: failed to write PNG file: " + filename);
    }
}

// Write all heatmaps (one per netId) to PNG files in the given directory.
// Files are named as "<outputDir>/<netLabel>.png", e.g. "M3_VDD.png".
void writeAllHeatmapsToPng(const HeatmapByNet& heatmaps,
                           const std::string& outputDir, bool useMaxValue) {
    std::string dir = outputDir;
    if (!dir.empty()) {
        char last = dir.back();
        if (last != '/' && last != '\\') {
            dir.push_back('/');
        }
    }

    if (!std::filesystem::exists(outputDir)) {
        if (std::filesystem::create_directory(outputDir)) {
            PDN_INFO("Created directory %s", outputDir.c_str());
        } else {
            PDN_ERROR("Failed to create directory %s", outputDir.c_str());
        }
    }

    for (const auto& kv : heatmaps) {
        int32_t              netId = kv.first;
        const IRDropHeatmap& hm    = kv.second;

        std::string label    = makeNetLabel(netId);
        std::string filename = dir + label + ".png";

        writeHeatmapToPng(hm, filename, useMaxValue);
    }
}

} // namespace pdnsol

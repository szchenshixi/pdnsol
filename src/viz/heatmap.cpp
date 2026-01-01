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
#include <unordered_set>
#include <vector>

// stb_image_write is a single-header library.
// Make sure this is the only translation unit in the project that defines
// STB_IMAGE_WRITE_IMPLEMENTATION, or adjust as needed.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <../3rdparty/stb/stb_image_write.h>

#include "pdnsol/common.hpp"
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
std::string makeNetLabel(const NetKey& netKey) {

    // NOTE: You can adjust the layer numbering if your M1 is layer=0 or 1.
    // Here we assume M0 corresponds to layer index 0.
    std::ostringstream oss;
    oss << netKey.layer.c_str() << "_" << netKey.netName.c_str();
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

        NetKey netKey = circ.netKey(node.mNet);
        if (!netKey.layer.valid() || !netKey.netName.valid()) {
            // Ignore invalid netKey if any.
            continue;
        }

        // NetDecomposition nd    = decodeNetId(netId);
        bool isPower = netKey.isPower;

        // Apply filters: VDD/VSS
        if (isPower && !cfg.includeVdd) continue;
        if (!isPower && !cfg.includeVss) continue;

        // Apply layer filter (if any).
        if (!layerFilter.empty() &&
            layerFilter.find(netKey.netName) == layerFilter.end()) {
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
        double metric = computeMetric(Vnode, isPower, cfg);
        float  fVal   = static_cast<float>(metric);

        // 3. Get or create heatmap for this netId.
        auto itHm = heatmaps.find(netKey);
        if (itHm == heatmaps.end()) {
            IRDropHeatmap hm  = makeEmptyHeatmap(bbox, cfg.width, cfg.height);
            auto          res = heatmaps.emplace(netKey, std::move(hm));
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

namespace {

// Safe pixel write into an RGB (3-channel) byte buffer.
inline void setPixel(std::vector<uint8_t>& img, int W, int H, int x, int y,
                     const RGB& c) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(W) +
                        static_cast<size_t>(x)) *
                       3u;
    img[idx + 0] = c.r;
    img[idx + 1] = c.g;
    img[idx + 2] = c.b;
}

inline void fillRect(std::vector<uint8_t>& img, int W, int H, int x0, int y0,
                     int w, int h, const RGB& c) {
    if (w <= 0 || h <= 0) return;

    const int x0c = std::max(x0, 0);
    const int y0c = std::max(y0, 0);
    const int x1c = std::min(x0 + w, W);
    const int y1c = std::min(y0 + h, H);
    if (x1c <= x0c || y1c <= y0c) return;

    for (int y = y0c; y < y1c; ++y) {
        size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(W) +
                      static_cast<size_t>(x0c)) *
                     3u;
        for (int x = x0c; x < x1c; ++x) {
            img[idx + 0] = c.r;
            img[idx + 1] = c.g;
            img[idx + 2] = c.b;
            idx += 3u;
        }
    }
}

inline void drawRect(std::vector<uint8_t>& img, int W, int H, int x0, int y0,
                     int w, int h, const RGB& c) {
    if (w <= 0 || h <= 0) return;
    fillRect(img, W, H, x0, y0, w, 1, c);
    fillRect(img, W, H, x0, y0 + h - 1, w, 1, c);
    fillRect(img, W, H, x0, y0, 1, h, c);
    fillRect(img, W, H, x0 + w - 1, y0, 1, h, c);
}

// Minimal 5x7 bitmap font for tick labels (digits, '.', '-', '+', 'e', space).
// Each row is 5 bits wide (bit 4 = leftmost pixel).
inline const uint8_t* glyph5x7(char ch) {
    static constexpr uint8_t SPACE[7] = {0, 0, 0, 0, 0, 0, 0};
    static constexpr uint8_t DOT[7]   = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04};
    static constexpr uint8_t MINUS[7] = {
      0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static constexpr uint8_t PLUS[7] = {
      0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    static constexpr uint8_t ELOW[7] = {
      0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E};
    static constexpr uint8_t QMARK[7] = {
      0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};

    // Letters needed for the unit "Volt"
    static constexpr uint8_t VCAP[7] = {
      0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x00}; // 'V'
    static constexpr uint8_t OLOW[7] = {
      0x00, 0x00, 0x0E, 0x11, 0x11, 0x0E, 0x00}; // 'o'
    static constexpr uint8_t LLOW[7] = {
      0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00}; // 'l'
    static constexpr uint8_t TLOW[7] = {
      0x04, 0x04, 0x1F, 0x04, 0x04, 0x06, 0x00}; // 't'

    static constexpr uint8_t D0[7] = {
      0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static constexpr uint8_t D1[7] = {
      0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static constexpr uint8_t D2[7] = {
      0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static constexpr uint8_t D3[7] = {
      0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E};
    static constexpr uint8_t D4[7] = {
      0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static constexpr uint8_t D5[7] = {
      0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    static constexpr uint8_t D6[7] = {
      0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static constexpr uint8_t D7[7] = {
      0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static constexpr uint8_t D8[7] = {
      0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static constexpr uint8_t D9[7] = {
      0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};

    switch (ch) {
    case '0': return D0;
    case '1': return D1;
    case '2': return D2;
    case '3': return D3;
    case '4': return D4;
    case '5': return D5;
    case '6': return D6;
    case '7': return D7;
    case '8': return D8;
    case '9': return D9;
    case '.': return DOT;
    case '-': return MINUS;
    case '+': return PLUS;
    case 'e': return ELOW;
    case 'V': return VCAP;
    case 'o': return OLOW;
    case 'l': return LLOW;
    case 't': return TLOW;
    case ' ': return SPACE;
    default: return QMARK;
    }
}

inline void drawChar5x7(std::vector<uint8_t>& img, int W, int H, int x0,
                        int y0, char ch, int scale, const RGB& fg) {
    const uint8_t* g = glyph5x7(ch);
    for (int row = 0; row < 7; ++row) {
        const uint8_t bits = g[row];
        for (int col = 0; col < 5; ++col) {
            const bool on = (bits & (1u << (4 - col))) != 0;
            if (!on) continue;

            const int px = x0 + col * scale;
            const int py = y0 + row * scale;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    setPixel(img, W, H, px + sx, py + sy, fg);
                }
            }
        }
    }
}

inline void drawText5x7(std::vector<uint8_t>& img, int W, int H, int x0,
                        int y0, const std::string& s, int scale,
                        const RGB& fg) {
    int       x   = x0;
    int       y   = y0;
    const int adv = (5 + 1) * scale;
    for (char ch : s) {
        if (ch == '\n') {
            y += (7 + 1) * scale;
            x = x0;
            continue;
        }
        drawChar5x7(img, W, H, x, y, ch, scale, fg);
        x += adv;
    }
}

inline std::string formatTickValue(double v) {
    // Use fixed for "normal" ranges, scientific for very small/large.
    const double av = std::fabs(v);

    std::ostringstream oss;
    if (av != 0.0 && (av < 1e-3 || av >= 1e4)) {
        oss.setf(std::ios::scientific);
        oss << std::setprecision(3) << v; // e.g. 1.234e-06
    } else {
        oss.setf(std::ios::fixed);
        oss << std::setprecision(4) << v; // e.g. 0.0123
    }

    std::string s = oss.str();

    // Trim trailing zeros for fixed format (keep scientific as-is).
    if (s.find('e') == std::string::npos) {
        const auto dot = s.find('.');
        if (dot != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
    }
    return s;
}

// Internal PNG writer that can optionally use a caller-provided legend range.
// If overrideVmin/overrideVmax are not finite, the range is auto-computed from
// this heatmap.
static void writeHeatmapToPngImpl(const IRDropHeatmap& hm,
                                  const std::string&   filename,
                                  bool useMaxValue, float overrideVmin,
                                  float overrideVmax) {
    const int HEATMAP_W = hm.width;
    const int HEATMAP_H = hm.height;

    // Keep the heatmap itself 1024x1024, then add a right-side legend.
    const int MAP_W = 1024;
    const int MAP_H = 1024;

    const int CB_PAD_L = 24;
    const int CB_W     = 24;
    const int CB_PAD_R = 12;
    // Wider label area to accommodate " Volt"
    const int LABEL_W  = 220;

    const int OUTPUT_W = MAP_W + CB_PAD_L + CB_W + CB_PAD_R + LABEL_W;
    const int OUTPUT_H = MAP_H;

    if (HEATMAP_W <= 0 || HEATMAP_H <= 0) {
        throw std::runtime_error(
          "IR-drop heatmap: invalid dimensions for PNG.");
    }

    std::vector<float> scalar;
    extractScalarImage(hm, scalar, useMaxValue);

    float vmin = overrideVmin;
    float vmax = overrideVmax;

    // Auto-range if caller didn't provide a valid override range.
    if (!std::isfinite(vmin) || !std::isfinite(vmax)) {
        vmin = std::numeric_limits<float>::infinity();
        vmax = -std::numeric_limits<float>::infinity();

        for (float v : scalar) {
            if (std::isnan(v)) continue;
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }

        if (!std::isfinite(vmin) || !std::isfinite(vmax)) {
            vmin = 0.0f;
            vmax = 1.0f;
        }
    }

    // Avoid degenerate ranges (keeps colormap stable).
    if (vmax <= vmin) {
        const float eps =
          (std::fabs(vmin) > 0.0f) ? (1e-6f * std::fabs(vmin)) : 1e-6f;
        vmin -= eps;
        vmax += eps;
    }

    // Start with a white canvas so labels are readable.
    std::vector<uint8_t> pixels(static_cast<size_t>(OUTPUT_W) *
                                  static_cast<size_t>(OUTPUT_H) * 3u,
                                255u);

    // 1) Render heatmap into the left MAP_W x MAP_H region.
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            float heatmap_x = (static_cast<float>(x) / MAP_W) * HEATMAP_W;
            float heatmap_y = (static_cast<float>(y) / MAP_H) * HEATMAP_H;

            int heatmap_xi =
              std::clamp(static_cast<int>(heatmap_x), 0, HEATMAP_W - 1);
            int heatmap_yi =
              std::clamp(static_cast<int>(heatmap_y), 0, HEATMAP_H - 1);

            const size_t heatmap_idx = static_cast<size_t>(heatmap_yi) *
                                         static_cast<size_t>(HEATMAP_W) +
                                       static_cast<size_t>(heatmap_xi);

            const RGB color = applyColormap(scalar[heatmap_idx], vmin, vmax);

            const size_t out_idx =
              (static_cast<size_t>(y) * static_cast<size_t>(OUTPUT_W) +
               static_cast<size_t>(x)) *
              3u;
            pixels[out_idx + 0] = color.r;
            pixels[out_idx + 1] = color.g;
            pixels[out_idx + 2] = color.b;
        }
    }

    // 2) Draw colorbar on the right.
    const int CB_TOP    = 32;
    const int CB_BOTTOM = 32;

    const int cbX0 = MAP_W + CB_PAD_L;
    const int cbY0 = CB_TOP;
    const int cbH  = OUTPUT_H - CB_TOP - CB_BOTTOM;

    if (cbH > 1) {
        for (int i = 0; i < cbH; ++i) {
            // i=0 (top) => vmax, i=cbH-1 (bottom) => vmin
            const float ft =
              static_cast<float>(i) / static_cast<float>(cbH - 1);
            const float v = vmax - ft * (vmax - vmin);
            const RGB   c = applyColormap(v, vmin, vmax);
            fillRect(pixels, OUTPUT_W, OUTPUT_H, cbX0, cbY0 + i, CB_W, 1, c);
        }
    }

    const RGB BLACK{0, 0, 0};
    drawRect(pixels, OUTPUT_W, OUTPUT_H, cbX0, cbY0, CB_W, cbH, BLACK);

    // 3) Tick marks + numeric labels (with unit).
    const int NUM_TICKS  = 6;
    const int TICK_LEN   = 8;
    const int TEXT_SCALE = 2;

    for (int t = 0; t < NUM_TICKS; ++t) {
        const float ft =
          (NUM_TICKS == 1)
            ? 0.0f
            : (static_cast<float>(t) / static_cast<float>(NUM_TICKS - 1));
        const int yTick =
          cbY0 + static_cast<int>(ft * static_cast<float>(cbH - 1) + 0.5f);

        // Tick value: top=vmax, bottom=vmin
        const double vTick =
          static_cast<double>(vmax) -
          static_cast<double>(ft) *
            (static_cast<double>(vmax) - static_cast<double>(vmin));

        // Tick line to the right of the bar
        const int x0 = cbX0 + CB_W;
        const int x1 = x0 + TICK_LEN;
        fillRect(pixels, OUTPUT_W, OUTPUT_H, x0, yTick, (x1 - x0), 1, BLACK);

        // Label + unit
        const std::string label = formatTickValue(vTick) + " Volt";
        int               xText = x1 + 4;
        int               yText = yTick - (7 * TEXT_SCALE) / 2;
        yText = std::clamp(yText, 0, OUTPUT_H - 7 * TEXT_SCALE);

        drawText5x7(
          pixels, OUTPUT_W, OUTPUT_H, xText, yText, label, TEXT_SCALE, BLACK);
    }

    const int strideBytes = OUTPUT_W * 3;

    if (!stbi_write_png(filename.c_str(),
                        OUTPUT_W,
                        OUTPUT_H,
                        /*comp=*/3,
                        pixels.data(),
                        strideBytes)) {
        PDN_FATAL("IR-drop heatmap: failed to write PNG file: %s",
                  filename.c_str());
    }
}

} // namespace

void writeHeatmapToPng(const IRDropHeatmap& hm, const std::string& filename,
                       bool useMaxValue) {
    writeHeatmapToPngImpl(
      hm,
      filename,
      useMaxValue,
      /*overrideVmin=*/std::numeric_limits<float>::quiet_NaN(),
      /*overrideVmax=*/std::numeric_limits<float>::quiet_NaN());
}

// Write all heatmaps (one per netId) to PNG files in the given directory.
// Files are named as "<outputDir>/<netLabel>.png", e.g. "M3_VDD.png".
void writeAllHeatmapsToPng(const HeatmapByNet& heatmaps,
                           const std::string& outputDir, bool useMaxValue) {
    std::string dir = outputDir;
    if (!std::filesystem::exists(dir)) {
        PDN_INFO("Creating directory %s/...", dir.c_str());
        std::filesystem::create_directory(dir);
    }
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

    // Compute ONE shared legend range for all net-layer heatmaps
    float legendVmin = std::numeric_limits<float>::infinity();
    float legendVmax = -std::numeric_limits<float>::infinity();
    for (const auto& kv : heatmaps) {
        const IRDropHeatmap& hm = kv.second;
        for (const IRDropCell& c : hm.cells) {
            if (c.count == 0) continue;
            const float v =
              useMaxValue
                ? c.maxVal
                : static_cast<float>(c.sumVal / static_cast<double>(c.count));
            if (!std::isfinite(v)) continue;
            legendVmin = std::min(legendVmin, v);
            legendVmax = std::max(legendVmax, v);
        }
    }
    if (!std::isfinite(legendVmin) || !std::isfinite(legendVmax)) {
        legendVmin = 0.0f;
        legendVmax = 1.0f;

    } else if (legendVmax <= legendVmin) {
        const float eps = +(std::fabs(legendVmin) > 0.0f)
                            ? (1e-6f * std::fabs(legendVmin))
                            : 1e-6f;
        legendVmin -= eps;
        legendVmax += eps;
    }

    for (const auto& kv : heatmaps) {
        NetKey               netKey = kv.first;
        const IRDropHeatmap& hm     = kv.second;

        std::string label    = makeNetLabel(netKey);
        std::string filename = dir + label + ".png";

        writeHeatmapToPngImpl(
          hm, filename, useMaxValue, legendVmin, legendVmax);
    }
}

} // namespace pdnsol
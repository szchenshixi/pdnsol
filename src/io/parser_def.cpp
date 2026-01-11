#include "pdnsol/io/parser_def.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pdnsol/io/parser_utils.hpp" // for trim, startsWithIgnoreCase, clamp
#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {

// -----------------------------------------------------------------------------
// Small helper functions for DEF parsing
// -----------------------------------------------------------------------------

// Strip optional DEF double quotes from a name token, e.g. "VDD" -> VDD.
static std::string stripDefQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Robust integer parser for DEF coordinate tokens.
static bool parseIntSafe(const std::string& s, int& out) {
    try {
        std::size_t pos = 0;
        int         v   = std::stoi(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Simple tokenizer for DEF lines.
// Splits on whitespace and also makes '(', ')', ';', '+' separate tokens.
static std::vector<std::string> tokenizeDef(const std::string& s) {
    std::vector<std::string> tokens;
    std::string              cur;

    auto flush = [&]() {
        if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    };

    for (char c : s) {
        switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case '\n': flush(); break;
        case '(':
        case ')':
        case ';':
        case '+':
            flush();
            tokens.emplace_back(1, c);
            break;
        default: cur.push_back(c); break;
        }
    }
    flush();
    return tokens;
}

// -----------------------------------------------------------------------------
// ConductanceGrid2D implementation
// -----------------------------------------------------------------------------

void ConductanceGrid2D::init(int nx_, int ny_, double xMin_, double xMax_,
                             double yMin_, double yMax_) {
    nx   = nx_;
    ny   = ny_;
    xMin = xMin_;
    yMin = yMin_;
    dx   = (xMax_ - xMin_) / static_cast<double>(nx);
    dy   = (yMax_ - yMin_) / static_cast<double>(ny);
    Gx.assign(static_cast<std::size_t>((nx - 1) * ny), 0.0);
    Gy.assign(static_cast<std::size_t>(nx * (ny - 1)), 0.0);
}

// -----------------------------------------------------------------------------
// ConductanceGrid3D implementation
// -----------------------------------------------------------------------------

void ConductanceGrid3D::init(int netIdx, int lb, int lt, int nxB, int nyB,
                             int nxT, int nyT, bool isTsv) {
    netIndex       = netIdx;
    bottomLayerIdx = lb;
    topLayerIdx    = lt;

    this->nxB = nxB;
    this->nyB = nyB;
    this->nxT = nxT;
    this->nyT = nyT;

    this->isTsv = isTsv;

    edgeG.clear();
}

void ConductanceGrid3D::addConductance(int ixB, int iyB, int ixT, int iyT,
                                       double dG) {
    if (dG <= 0.0) return;

    PDN_FATAL_IF(ixB < 0 || ixB >= nxB,
                 "Bottom-X index %d beyond scope (0, %d)",
                 ixB,
                 nxB);
    PDN_FATAL_IF(iyB < 0 || iyB >= nyB,
                 "Bottom-Y index %d beyond scope (0, %d)",
                 iyB,
                 nyB);
    PDN_FATAL_IF(
      ixT < 0 || ixT >= nxT, "Top-X index %d beyond scope (0, %d)", ixT, nxT);
    PDN_FATAL_IF(
      iyT < 0 || iyT >= nyT, "Top-Y index %d beyond scope (0, %d)", iyT, nyT);

    const std::uint32_t flatB = static_cast<std::uint32_t>(iyB * nxB + ixB);
    const std::uint32_t flatT = static_cast<std::uint32_t>(iyT * nxT + ixT);

    edgeG[packEdge(flatB, flatT)] += dG; // parallel vias add conductance
}

struct TileNodeIdGrid {
    int                   nx = 0;
    int                   ny = 0;
    std::vector<IdString> ids; // size = nx*ny

    void init(int nx_, int ny_) {
        nx = nx_;
        ny = ny_;
        ids.assign(static_cast<std::size_t>(nx * ny), IdString());
    }

    IdString& at(int ix, int iy) {
        return ids[static_cast<std::size_t>(iy * nx + ix)];
    }
    const IdString& at(int ix, int iy) const {
        return ids[static_cast<std::size_t>(iy * nx + ix)];
    }
};

// -----------------------------------------------------------------------------
// CoarsePdnBuilder3D implementation
// -----------------------------------------------------------------------------

CoarsePdnBuilder3D::CoarsePdnBuilder3D(
  TechDatabase& techDb, LayerGridResolution defaultRes,
  const IdString::Map<LayerGridResolution>& perLayerRes,
  const std::vector<std::string>&           powerNetNames,
  const std::vector<std::string>&           groundNetNames,
  const std::vector<std::string>&           layerOrder)
    : mTechDb(techDb)
    , mDefaultRes(defaultRes) {

    // 1) Register PDN nets
    for (const std::string& n : powerNetNames) {
        addNetIfAbsent(n, /*isPower=*/true, /*isGround=*/false);
    }
    for (const std::string& n : groundNetNames) {
        addNetIfAbsent(n, /*isPower=*/false, /*isGround=*/true);
    }
    mNumNets = static_cast<int>(mNetByIndex.size());

    // 2) Register PDN layers and build mapping from layer name to index
    mLayerNameToIndex.reserve(layerOrder.size());
    mLayerOrder.reserve(layerOrder.size()); // fixed: reserve target size
    for (std::size_t i = 0; i < layerOrder.size(); ++i) {
        IdString layerId(layerOrder[i]);
        mLayerOrder.push_back(layerId);
        mLayerNameToIndex[layerId] = static_cast<int>(i);
    }
    mNumLayers       = static_cast<int>(mLayerOrder.size());
    mNumNetLayerComb = mNumNets * mNumLayers;
    // 3) Resolve per-layer grid resolutions (NEW)
    mLayerGridRes.resize(static_cast<std::size_t>(mNumLayers));
    for (int l = 0; l < mNumLayers; ++l) {
        IdString lname = mLayerOrder[l];
        auto     it    = perLayerRes.find(lname);
        if (it != perLayerRes.end()) {
            mLayerGridRes[l] = it->second;
        } else {
            mLayerGridRes[l] = mDefaultRes;
        }

        if (mLayerGridRes[l].sx <= 0 || mLayerGridRes[l].sy <= 0) {
            PDN_FATAL("Invalid grid stride for layer %s", lname.c_str());
        }
    }
}

int CoarsePdnBuilder3D::encodeNetLayer(int netIndex, int layerIndex) const {
    return layerIndex * mNumNets + netIndex;
}

bool CoarsePdnBuilder3D::buildCoarsePdnFromDef(const std::string& defPath,
                                               CircuitGraph&      outGraph) {
    // Backwards compatible: no filtering => all PDN nets
    NetFilter f;
    return buildCoarsePdnFromDef(defPath, outGraph, f);
}

bool CoarsePdnBuilder3D::buildCoarsePdnFromDef(const std::string& defPath,
                                               CircuitGraph&      outGraph,
                                               const NetFilter&   filter) {
    // Ensure we start clean for each DEF
    resetForNewBuild();

    // Make sure output is clean too
    outGraph = CircuitGraph{};

    // Precompute selection once and keep it active during parsing/recording.
    const std::vector<int> netIndices = selectNetIndices(filter);
    setNetSelectedMask(netIndices);

    if (!parseDefGeometry(defPath)) {
        std::cerr << "ERROR: Failed to parse geometry from DEF: " << defPath
                  << "\n";
        return false;
    }

    initInPlaneGrids();

    if (!parseDefPdnAndBumps(defPath)) {
        std::cerr << "ERROR: Failed to parse PDN & bumps from DEF: " << defPath
                  << "\n";
        return false;
    }

    finalizeRecordedPdnGeometry();

    buildCircuitGraph(outGraph, netIndices);
    return true;
}

bool CoarsePdnBuilder3D::buildCoarsePdnGraphsByNetFromDef(
  const std::string& defPath, IdString::Map<CircuitGraph>& outGraphs,
  const NetFilter& filter) {

    resetForNewBuild();
    outGraphs.clear();

    if (!parseDefGeometry(defPath)) {
        std::cerr << "ERROR: Failed to parse geometry from DEF: " << defPath
                  << "\n";
        return false;
    }

    initInPlaneGrids();

    if (!parseDefPdnAndBumps(defPath)) {
        std::cerr << "ERROR: Failed to parse PDN & bumps from DEF: " << defPath
                  << "\n";
        return false;
    }

    finalizeRecordedPdnGeometry();

    const std::vector<int> nets = selectNetIndices(filter);
    for (int netIndex : nets) {
        if (netIndex < 0 || netIndex >= mNumNets) continue;

        CircuitGraph g;
        buildCircuitGraph(g, std::vector<int>{netIndex});
        outGraphs.emplace(mNetByIndex[netIndex].name, std::move(g));
    }

    return true;
}

// -----------------------------------------------------------------------------
// DEF parsing: geometry (UNITS, DIEAREA)
// -----------------------------------------------------------------------------

bool CoarsePdnBuilder3D::parseDefGeometry(const std::string& defPath) {
    std::ifstream fin(defPath);
    if (!fin) {
        std::cerr << "ERROR: Cannot open DEF file: " << defPath << "\n";
        return false;
    }

    bool   haveUnits   = false;
    bool   haveDieArea = false;
    double dbu         = 1.0;

    std::string line;
    while (std::getline(fin, line)) {
        std::string tline = trim(line);
        if (tline.empty()) continue;

        // UNITS DISTANCE MICRONS <dbu> ;
        if (!haveUnits && startsWithIgnoreCase(tline, "UNITS")) {
            std::istringstream iss(tline);
            std::string        tok;
            iss >> tok; // UNITS
            iss >> tok; // DISTANCE
            iss >> tok; // MICRONS
            if (tok != "MICRONS") {
                std::cerr << "ERROR: Only MICRONS units supported.\n";
                return false;
            }
            int dbuInt = 1;
            iss >> dbuInt;
            dbu       = static_cast<double>(dbuInt);
            haveUnits = true;
        }

        // DIEAREA ( x0 y0 ) ( x1 y1 ) ;
        if (!haveDieArea && startsWithIgnoreCase(tline, "DIEAREA")) {
            int                x0, y0, x1, y1;
            std::string        token;
            std::istringstream iss(tline);
            iss >> token;    // DIEAREA
            iss >> token;    // "("
            iss >> x0 >> y0; // x0 y0
            iss >> token;    // ")"
            iss >> token;    // "("
            iss >> x1 >> y1; // x1 y1
            // trailing ) ;
            mDieXMinUm  = static_cast<double>(x0) / dbu;
            mDieYMinUm  = static_cast<double>(y0) / dbu;
            mDieXMaxUm  = static_cast<double>(x1) / dbu;
            mDieYMaxUm  = static_cast<double>(y1) / dbu;
            haveDieArea = true;
        }

        if (haveUnits && haveDieArea) break;
    }

    if (!haveUnits || !haveDieArea) {
        std::cerr << "ERROR: Missing UNITS or DIEAREA in DEF.\n";
        return false;
    }

    mDbuPerMicron = dbu;
    return true;
}

// -----------------------------------------------------------------------------
// Initialize in-plane grids for each (net, layer)
// -----------------------------------------------------------------------------

static int gcd(int a, int b) {
    while (b) {
        int t = b;
        b     = a % b;
        a     = t;
    }
    return a;
}

static int lcm(int a, int b) { return (a / gcd(a, b)) * b; }

void CoarsePdnBuilder3D::initInPlaneGrids() {
    m2DGrids.assign(mNumNets, std::vector<ConductanceGrid2D>(mNumLayers));

    // Calculate die dimensions once
    double dieWidth  = mDieXMaxUm - mDieXMinUm;
    double dieHeight = mDieYMaxUm - mDieYMinUm;

    // Step 1: Check if all tile sizes are integer multiples of a common
    // divisor
    int commonDivisorX = 0;
    int commonDivisorY = 0;

    for (int l = 0; l < mNumLayers; ++l) {
        if (l == 0) {
            commonDivisorX = mLayerGridRes[l].sx;
            commonDivisorY = mLayerGridRes[l].sy;
        } else {
            commonDivisorX = gcd(commonDivisorX, mLayerGridRes[l].sx);
            commonDivisorY = gcd(commonDivisorY, mLayerGridRes[l].sy);
        }
    }

    if (commonDivisorX <= 0 || commonDivisorY <= 0) {
        PDN_FATAL("Invalid tile sizes found (non-positive values)");
    }

    // Verify all tile sizes are integer multiples of the common divisors
    for (int l = 0; l < mNumLayers; ++l) {
        if (mLayerGridRes[l].sx % commonDivisorX != 0) {
            PDN_FATAL("Tile size sx=%d is not an integer multiple of common "
                      "divisor %d",
                      mLayerGridRes[l].sx,
                      commonDivisorX);
        }
        if (mLayerGridRes[l].sy % commonDivisorY != 0) {
            PDN_FATAL("Tile size sy=%d is not an integer multiple of common "
                      "divisor %d",
                      mLayerGridRes[l].sy,
                      commonDivisorY);
        }
    }

    // Step 2: Find the least common multiple of tile size ratios
    // This ensures all tile counts will be integer multiples
    int lcmX = 1;
    int lcmY = 1;

    for (int l = 0; l < mNumLayers; ++l) {
        int ratioX = mLayerGridRes[l].sx / commonDivisorX;
        int ratioY = mLayerGridRes[l].sy / commonDivisorY;

        lcmX = lcm(lcmX, ratioX);
        lcmY = lcm(lcmY, ratioY);
    }

    // Step 3: Calculate base grid dimensions using the common divisor
    // This is the finest grid that can accommodate all tile sizes
    double baseTileSizeX = commonDivisorX;
    double baseTileSizeY = commonDivisorY;

    // Calculate required number of base tiles to cover the die
    int baseNx = static_cast<int>(std::ceil(dieWidth / baseTileSizeX));
    int baseNy = static_cast<int>(std::ceil(dieHeight / baseTileSizeY));

    // Adjust base grid to be a multiple of LCM to ensure integer tile counts
    baseNx = ((baseNx + lcmX - 1) / lcmX) * lcmX;
    baseNy = ((baseNy + lcmY - 1) / lcmY) * lcmY;

    // Step 4: Compute nx/ny for each layer and check/correct for integer
    // multiples
    for (int l = 0; l < mNumLayers; ++l) {
        int ratioX = mLayerGridRes[l].sx / commonDivisorX;
        int ratioY = mLayerGridRes[l].sy / commonDivisorY;

        // Calculate initial tile counts
        int nx = baseNx / ratioX;
        int ny = baseNy / ratioY;

        // Verify these are integer multiples (should be due to LCM adjustment)
        if (baseNx % ratioX != 0 || baseNy % ratioY != 0) {
            // This should not happen if LCM was calculated correctly
            // But if it does, adjust by padding
            if (baseNx % ratioX != 0) {
                // Need to increase baseNx to make it divisible by ratioX
                int requiredBaseNx = ((baseNx / ratioX) + 1) * ratioX;
                baseNx             = requiredBaseNx;
                nx                 = baseNx / ratioX;

                // Recalculate all previous layers
                for (int prev = 0; prev < l; ++prev) {
                    int prevRatioX = mLayerGridRes[prev].sx / commonDivisorX;
                    mLayerGridRes[prev].nx = baseNx / prevRatioX;
                }
            }

            if (baseNy % ratioY != 0) {
                // Need to increase baseNy to make it divisible by ratioY
                int requiredBaseNy = ((baseNy / ratioY) + 1) * ratioY;
                baseNy             = requiredBaseNy;
                ny                 = baseNy / ratioY;

                // Recalculate all previous layers
                for (int prev = 0; prev < l; ++prev) {
                    int prevRatioY = mLayerGridRes[prev].sy / commonDivisorY;
                    mLayerGridRes[prev].ny = baseNy / prevRatioY;
                }
            }
        }

        // Ensure at least 1 tile
        mLayerGridRes[l].nx = std::max(1, nx);
        mLayerGridRes[l].ny = std::max(1, ny);

        // Double-check integer multiple relationship
        if (mLayerGridRes[l].nx * ratioX != baseNx) {
            // Adjust by increasing nx (adding padding)
            mLayerGridRes[l].nx = (baseNx + ratioX - 1) / ratioX;
        }
        if (mLayerGridRes[l].ny * ratioY != baseNy) {
            // Adjust by increasing ny (adding padding)
            mLayerGridRes[l].ny = (baseNy + ratioY - 1) / ratioY;
        }
    }

    // Optional: Verify all layers align properly
    for (int l = 0; l < mNumLayers; ++l) {
        int ratioX = mLayerGridRes[l].sx / commonDivisorX;
        int ratioY = mLayerGridRes[l].sy / commonDivisorY;

        if (mLayerGridRes[l].nx * ratioX != baseNx) {
            PDN_ERROR("Warning: Layer %d X-dimension not properly aligned. "
                      "Expected baseNx=%d, got %d",
                      l,
                      baseNx,
                      mLayerGridRes[l].nx * ratioX);
        }
        if (mLayerGridRes[l].ny * ratioY != baseNy) {
            PDN_ERROR("Warning: Layer %d X-dimension not properly aligned. "
                      "Expected baseNx=%d, got %d",
                      l,
                      baseNy,
                      mLayerGridRes[l].ny * ratioY);
        }
    }

    // Calculate the actual padded die dimensions
    double paddedDieWidth  = baseNx * baseTileSizeX;
    double paddedDieHeight = baseNy * baseTileSizeY;

    PDN_INFO("Grid Configuration:");
    PDN_INFO("  Base tile:    %.2f x %.2fum", baseTileSizeX, baseTileSizeY);
    PDN_INFO("  Base grid:    %d x %d tiles", baseNx, baseNy);
    PDN_INFO("  Original die: %.2f x %.2fum", dieWidth, dieHeight);
    PDN_INFO("  Padded die:   %.2f x %.2fum", paddedDieWidth, paddedDieHeight);

    for (int n = 0; n < mNumNets; ++n) {
        for (int l = 0; l < mNumLayers; ++l) {
            m2DGrids[n][l].init(mLayerGridRes[l].nx,
                                mLayerGridRes[l].ny,
                                mDieXMinUm,
                                //  mDieXMaxUm,
                                mDieXMinUm + paddedDieWidth,
                                mDieYMinUm,
                                //  mDieYMaxUm,
                                mDieYMinUm + paddedDieHeight);
        }
    }
}

void CoarsePdnBuilder3D::resetForNewBuild() {
    m2DGrids.clear();
    m3DGrids.clear();
    m3DGridLookup.clear();
    mRecordedStripes.clear();
    mRecordedVias.clear();
    mRecordedTsvs.clear();
    mNetSelectedMask.clear();
}

void CoarsePdnBuilder3D::setNetSelectedMask(
  const std::vector<int>& netIndices) {
    mNetSelectedMask.assign(static_cast<std::size_t>(mNumNets), 0);
    for (int n : netIndices) {
        if (n >= 0 && n < mNumNets) {
            mNetSelectedMask[static_cast<std::size_t>(n)] = 1;
        }
    }
}

std::vector<int>
CoarsePdnBuilder3D::selectNetIndices(const NetFilter& filter) const {
    // Optional: warn if user requested unknown nets
    for (const std::string& s : filter.include) {
        IdString id(stripDefQuotes(s));
        if (mNetByName.find(id) == mNetByName.end()) {
            PDN_WARN("Net filter requested '%s' but it is not a "
                     "registered PDN net",
                     id.c_str());
        }
    }

    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(mNumNets));

    for (const NetInfo& ni : mNetByIndex) {
        if (!filter.allows(ni.name, ni.isPower, ni.isGround)) continue;

        out.push_back(ni.index);
    }
    return out;
}

bool CoarsePdnBuilder3D::isNetSelected(int netIndex) const noexcept {
    if (mNetSelectedMask.empty()) return true;
    if (netIndex < 0 || netIndex >= mNumNets) return false;
    return mNetSelectedMask[static_cast<std::size_t>(netIndex)] != 0;
}

// -----------------------------------------------------------------------------
// Helpers for SPECIALNETS routing
// -----------------------------------------------------------------------------

bool CoarsePdnBuilder3D::parseDefPoint(const std::vector<std::string>& tokens,
                                       std::size_t startIdx, int prevX,
                                       int prevY, int& x, int& y,
                                       std::size_t& consumed) {

    consumed            = 0;
    const std::size_t n = tokens.size();
    if (startIdx + 3 >= n) return false;
    if (tokens[startIdx] != "(") return false;
    if (tokens[startIdx + 3] != ")") return false;

    const std::string& xs = tokens[startIdx + 1];
    const std::string& ys = tokens[startIdx + 2];

    if (xs == "*") {
        x = prevX;
    } else if (!parseIntSafe(xs, x)) {
        return false;
    }

    if (ys == "*") {
        y = prevY;
    } else if (!parseIntSafe(ys, y)) {
        return false;
    }

    consumed = 4; // "(", x, y, ")"
    return true;
}

// Convert a routed segment into a rectangle and feed it into the
// existing rectangle-based PDN builder.
// widthDbu is the wire width from "ROUTED/NEW" (in DBU).
void CoarsePdnBuilder3D::recordStripeFromSegment(const std::string& netName,
                                                 const std::string& layerName,
                                                 int x0, int y0, int x1,
                                                 int y1, int widthDbu) {
    if (widthDbu <= 0) return;

    const int halfLo = widthDbu / 2;
    const int halfHi = widthDbu - halfLo;

    if (x0 == x1) {
        // Vertical stripe
        int yMin = std::min(y0, y1);
        int yMax = std::max(y0, y1);
        // addStripeRectangle(
        //   netName, layerName, x0 - half, yMin, x0 + half, yMax);
        recordStripeRectangle(
          netName, layerName, x0 - halfLo, yMin, x0 + halfHi, yMax);
    } else if (y0 == y1) {
        // Horizontal stripe
        int xMin = std::min(x0, x1);
        int xMax = std::max(x0, x1);
        // addStripeRectangle(
        //   netName, layerName, xMin, y0 - half, xMax, y0 + half);
        recordStripeRectangle(
          netName, layerName, xMin, y0 - halfLo, xMax, y0 + halfHi);
    } else {
        // Non-Manhattan (unlikely in PDN); fall back to bounding box.
        int xMin = std::min(x0, x1);
        int xMax = std::max(x0, x1);
        int yMin = std::min(y0, y1);
        int yMax = std::max(y0, y1);
        // addStripeRectangle(netName, layerName, xMin, yMin, xMax, yMax);
        recordStripeRectangle(netName, layerName, xMin, yMin, xMax, yMax);
    }
}

// -----------------------------------------------------------------------------
// DEF parsing: PDN stripes, vias, and bumps
// -----------------------------------------------------------------------------

bool CoarsePdnBuilder3D::parseDefPdnAndBumps(const std::string& defPath) {
    std::ifstream fin(defPath);
    if (!fin) {
        std::cerr << "ERROR: Cannot open DEF file: " << defPath << "\n";
        return false;
    }

    Section section = Section::NONE;

    std::string currentNetName;
    bool        currentNetIsPdn = false;
    std::string currentLayerName;         // updated by "ROUTED <layer>"
    int         currentRouteWidthDbu = 0; // from ROUTED/NEW

    // State for COMPONENTS
    std::string currentCompInstName;
    std::string currentCompMacroName;
    int         currentCompX = -1;
    int         currentCompY = -1;

    std::string line;
    while (std::getline(fin, line)) {
        std::string tline = trim(line);
        if (tline.empty()) continue;
        if (!tline.empty() && tline[0] == '#') continue; // skip comments

        if (startsWithIgnoreCase(tline, "SPECIALNETS")) {
            section = Section::SPECIALNETS;
            currentNetName.clear();
            currentNetIsPdn = false;
            currentLayerName.clear();
            currentRouteWidthDbu = 0;
            continue;
        }
        if (startsWithIgnoreCase(tline, "END SPECIALNETS")) {
            section = Section::NONE;
            currentNetName.clear();
            currentNetIsPdn = false;
            currentLayerName.clear();
            currentRouteWidthDbu = 0;
            continue;
        }

        // VIAS section
        if (startsWithIgnoreCase(tline, "VIAS")) {
            section = Section::VIAS;
            continue;
        }
        if (startsWithIgnoreCase(tline, "END VIAS")) {
            section = Section::NONE;
            continue;
        }

        // For TSVs
        if (startsWithIgnoreCase(tline, "COMPONENTS")) {
            section = Section::COMPONENTS;
            currentCompInstName.clear();
            currentCompMacroName.clear();
            currentCompX = -1;
            currentCompY = -1;
            continue;
        }
        if (startsWithIgnoreCase(tline, "END COMPONENTS")) {
            section = Section::NONE;
            if (!currentCompInstName.empty() &&
                !currentCompMacroName.empty()) {
                // addTsvInstance(currentCompInstName,
                //                currentCompMacroName,
                //                currentCompX,
                //                currentCompY);
                recordTsvInstance(currentCompInstName,
                                  currentCompMacroName,
                                  currentCompX,
                                  currentCompY);
            }
            continue;
        }

        switch (section) {
        case Section::SPECIALNETS:
            handleSpecialNetsLine(tline,
                                  currentNetName,
                                  currentNetIsPdn,
                                  currentLayerName,
                                  currentRouteWidthDbu);
            break;
        // case Section::PINS: handlePinsLine(tline); break;
        case Section::VIAS: handleViasLine(tline); break;
        case Section::COMPONENTS:
            handleComponentsLine(tline,
                                 currentCompInstName,
                                 currentCompMacroName,
                                 currentCompX,
                                 currentCompY);
            break;
        default: break;
        }
    }

    return true;
}

void CoarsePdnBuilder3D::handleSpecialNetsLine(const std::string& line,
                                               std::string& currentNetName,
                                               bool&        currentNetIsPdn,
                                               std::string& currentLayerName,
                                               int& currentRouteWidthDbu) {

    std::vector<std::string> tokens = tokenizeDef(line);
    if (tokens.empty()) return;

    // Start of a new special net: "- <netName> ..."
    if (tokens[0] == "-") {
        if (tokens.size() >= 2) {
            currentNetName = stripDefQuotes(tokens[1]);

            auto it = mNetByName.find(IdString::tryLookup(currentNetName));
            currentNetIsPdn =
              (it != mNetByName.end()) && isNetSelected(it->second.index);
        } else {
            currentNetName.clear();
            currentNetIsPdn = false;
        }
        currentLayerName.clear();
        currentRouteWidthDbu = 0;
        return;
    }

    if (!currentNetIsPdn) {
        // Skip geometry for non-PDN nets
        return;
    }

    std::size_t       i = 0;
    const std::size_t n = tokens.size();

    int  lastX         = 0;
    int  lastY         = 0;
    bool haveLastPoint = false;
    bool inShapeBlock  = false; // Track if we're in a + SHAPE ... block

    while (i < n) {
        const std::string& tok = tokens[i];

        if (tok == "+" || tok == ";") {
            ++i;
            continue;
        }

        // ROUTED or NEW statement
        if (tok == "ROUTED" || tok == "NEW") {
            ++i;
            if (i < n) {
                currentLayerName = stripDefQuotes(tokens[i++]);
            } else {
                break;
            }

            // Optional width
            currentRouteWidthDbu = 0;
            if (i < n) {
                int width = 0;
                if (parseIntSafe(tokens[i], width)) {
                    currentRouteWidthDbu = width;
                    ++i;
                }
            }

            haveLastPoint = false;
            inShapeBlock  = false; // Reset shape block flag

            // Check if the next token is "+" followed by "SHAPE"
            if (i < n && tokens[i] == "+") {
                ++i;
                if (i < n && tokens[i] == "SHAPE") {
                    inShapeBlock = true;
                    ++i; // Skip SHAPE
                }
            }

            // If not in a shape block, check for geometry directly
            if (!inShapeBlock && i < n && tokens[i] == "(") {
                // Handle geometry directly without SHAPE keyword
                // Parse first point
                int         x0 = 0, y0 = 0;
                std::size_t consumed = 0;
                if (!parseDefPoint(tokens, i, 0, 0, x0, y0, consumed)) {
                    break;
                }
                i += consumed;

                // Check if this is a via instantiation or a wire
                if (i < n && tokens[i] != "(" && tokens[i] != "+" &&
                    tokens[i] != ";") {
                    // Via instantiation: (x y) viaName
                    const std::string& viaName = tokens[i++];
                    // addViaInstance(currentNetName, viaName, x0, y0);
                    recordViaInstance(currentNetName, viaName, x0, y0);
                } else if (i < n && tokens[i] == "(") {
                    // Wire segment: (x0 y0) (x1 y1) ...
                    int x1 = 0, y1 = 0;
                    if (!parseDefPoint(tokens, i, x0, y0, x1, y1, consumed)) {
                        break;
                    }
                    i += consumed;

                    if (!currentLayerName.empty() &&
                        currentRouteWidthDbu > 0) {
                        recordStripeFromSegment(currentNetName,
                                                currentLayerName,
                                                x0,
                                                y0,
                                                x1,
                                                y1,
                                                currentRouteWidthDbu);
                    }

                    lastX         = x1;
                    lastY         = y1;
                    haveLastPoint = true;

                    // Additional segments
                    while (i < n && tokens[i] == "(") {
                        int x2 = 0, y2 = 0;
                        if (!parseDefPoint(
                              tokens, i, lastX, lastY, x2, y2, consumed)) {
                            break;
                        }
                        i += consumed;

                        if (!currentLayerName.empty() &&
                            currentRouteWidthDbu > 0) {
                            recordStripeFromSegment(currentNetName,
                                                    currentLayerName,
                                                    lastX,
                                                    lastY,
                                                    x2,
                                                    y2,
                                                    currentRouteWidthDbu);
                        }

                        lastX = x2;
                        lastY = y2;
                    }
                }
            }
            continue;
        }

        // If we're in a SHAPE block, handle its contents
        if (inShapeBlock) {
            // SHAPE type (STRIPE, FOLLOWPIN, etc.)
            if (tokens[i] == "STRIPE" || tokens[i] == "FOLLOWPIN" ||
                tokens[i] == "RING" || tokens[i] == "PADRING") {
                const std::string& shapeType = tokens[i++];
                (void)shapeType; // currently not used beyond parsing

                // Geometry comes right after shape type
                if (i >= n || tokens[i] != "(") {
                    inShapeBlock = false;
                    continue;
                }

                // First point
                int         x0 = 0, y0 = 0;
                std::size_t consumed = 0;
                if (!parseDefPoint(tokens,
                                   i,
                                   haveLastPoint ? lastX : 0,
                                   haveLastPoint ? lastY : 0,
                                   x0,
                                   y0,
                                   consumed)) {
                    break;
                }
                i += consumed;

                // Case 1: via instantiation: (x y) <viaName>
                if (i < n && tokens[i] != "(" && tokens[i] != "+" &&
                    tokens[i] != ";") {
                    const std::string& viaName = tokens[i++];
                    // addViaInstance(currentNetName, viaName, x0, y0);
                    recordViaInstance(currentNetName, viaName, x0, y0);
                    inShapeBlock = false; // End of shape block
                    continue;
                }

                // Case 2: wire segments
                if (i < n && tokens[i] == "(") {
                    int x1 = 0, y1 = 0;
                    if (!parseDefPoint(tokens, i, x0, y0, x1, y1, consumed)) {
                        break;
                    }
                    i += consumed;

                    if (!currentLayerName.empty() &&
                        currentRouteWidthDbu > 0) {
                        recordStripeFromSegment(currentNetName,
                                                currentLayerName,
                                                x0,
                                                y0,
                                                x1,
                                                y1,
                                                currentRouteWidthDbu);
                    }

                    lastX         = x1;
                    lastY         = y1;
                    haveLastPoint = true;

                    // Additional segments
                    while (i < n && tokens[i] == "(") {
                        int x2 = 0, y2 = 0;
                        if (!parseDefPoint(
                              tokens, i, lastX, lastY, x2, y2, consumed)) {
                            break;
                        }
                        i += consumed;

                        if (!currentLayerName.empty() &&
                            currentRouteWidthDbu > 0) {
                            recordStripeFromSegment(currentNetName,
                                                    currentLayerName,
                                                    lastX,
                                                    lastY,
                                                    x2,
                                                    y2,
                                                    currentRouteWidthDbu);
                        }

                        lastX = x2;
                        lastY = y2;
                    }
                }

                inShapeBlock = false; // End of shape block
                continue;
            }

            // If we're still in shape block but didn't match a shape type,
            // something is wrong - exit shape block
            inShapeBlock = false;
        }

        // Optional "LAYER <name>" override (outside shape blocks)
        if (tok == "LAYER") {
            ++i;
            if (i < n) {
                currentLayerName = stripDefQuotes(tokens[i++]);
            }
            continue;
        }

        // Ignore everything else (USE, WIDTH, VIA, RECT, etc.)
        ++i;
    }
}

// void CoarsePdnBuilder3D::handlePinsLine(const std::string& line) {
//     std::vector<std::string> tokens = tokenizeDef(line);
//     if (tokens.empty()) return;

//     std::size_t       i = 0;
//     const std::size_t n = tokens.size();

//     // Start of a new PIN definition
//     if (!mPinParseState.inPin) {
//         if (tokens[0] != "-") {
//             // Ignore lines until we see "- pinName"
//             return;
//         }
//         if (n < 2) {
//             return;
//         }
//         mPinParseState.reset();
//         mPinParseState.inPin   = true;
//         mPinParseState.pinName = IdString(stripDefQuotes(tokens[1]));
//         i = 2; // Continue parsing remaining tokens on this line
//     }

//     for (; i < n; ++i) {
//         const std::string& tok = tokens[i];

//         if (tok == ";") {
//             // End of this PIN definition
//             if (mPinParseState.isPowerOrGround &&
//             mPinParseState.hasLocation
//             &&
//                 mPinParseState.netName.valid()) {
//                 auto it = mNetByName.find(mPinParseState.netName);
//                 if (it != mNetByName.end()) {
//                     Bump b;
//                     b.netName = mPinParseState.netName;
//                     b.x_um =
//                       static_cast<double>(mPinParseState.xDbu) /
//                       mDbuPerMicron;
//                     b.y_um =
//                       static_cast<double>(mPinParseState.yDbu) /
//                       mDbuPerMicron;
//                     mBumps.push_back(std::move(b));
//                 }
//             }
//             mPinParseState.reset();
//             break;
//         } else if (tok == "NET") {
//             if (i + 1 < n) {
//                 mPinParseState.netName =
//                 IdString(stripDefQuotes(tokens[++i]));
//                 // If the net is in our PDN set, treat as power/ground
//                 auto it = mNetByName.find(mPinParseState.netName);
//                 if (it != mNetByName.end()) {
//                     mPinParseState.isPowerOrGround = true;
//                 }
//             }
//         } else if (tok == "USE") {
//             if (i + 1 < n) {
//                 const std::string& useType = tokens[++i];
//                 if (useType == "POWER" || useType == "GROUND") {
//                     mPinParseState.isPowerOrGround = true;
//                 }
//             }
//         } else if (tok == "PLACED" || tok == "FIXED") {
//             // Look ahead for "( x y )"
//             int         x = 0, y = 0;
//             bool        found = false;
//             std::size_t j     = i + 1;
//             for (; j + 3 < n; ++j) {
//                 if (tokens[j] == "(") {
//                     if (parseIntSafe(tokens[j + 1], x) &&
//                         parseIntSafe(tokens[j + 2], y)) {
//                         found = true;
//                     }
//                     break;
//                 }
//             }
//             if (found) {
//                 mPinParseState.xDbu        = x;
//                 mPinParseState.yDbu        = y;
//                 mPinParseState.hasLocation = true;
//             }
//         } else {
//             // Ignore other tokens (DIRECTION, LAYER, PORT, '+', etc.)
//             continue;
//         }
//     }
// }

void CoarsePdnBuilder3D::handleViasLine(const std::string& line) {
    std::vector<std::string> tokens = tokenizeDef(line);
    if (tokens.empty()) return;

    // We only care about lines that start a via definition:
    //   - via4_1600x1600 + VIARULE ... + CUTSIZE ... + LAYERS ...
    if (tokens[0] != "-" || tokens.size() < 2) {
        return;
    }

    std::string viaName = stripDefQuotes(tokens[1]);

    std::string viaRuleName;
    std::string bottomLayer;
    std::string cutLayer;
    std::string topLayer;

    int cutSizeX         = 0;
    int cutSizeY         = 0;
    int cutSpacingX      = 0;
    int cutSpacingY      = 0;
    int enclosureBottomX = 0;
    int enclosureBottomY = 0;
    int enclosureTopX    = 0;
    int enclosureTopY    = 0;
    int rows             = 1;
    int cols             = 1;

    std::size_t       i = 2;
    const std::size_t n = tokens.size();

    while (i < n) {
        const std::string& tok = tokens[i];

        if (tok == "+") {
            ++i;
            continue;
        }
        if (tok == ";") {
            break;
        }

        if (tok == "VIARULE" && i + 1 < n) {
            viaRuleName = stripDefQuotes(tokens[i + 1]);
            i += 2;
        } else if (tok == "CUTSIZE" && i + 2 < n) {
            parseIntSafe(tokens[i + 1], cutSizeX);
            parseIntSafe(tokens[i + 2], cutSizeY);
            i += 3;
        } else if (tok == "LAYERS" && i + 3 < n) {
            bottomLayer = stripDefQuotes(tokens[i + 1]);
            cutLayer    = stripDefQuotes(tokens[i + 2]);
            topLayer    = stripDefQuotes(tokens[i + 3]);
            i += 4;
        } else if (tok == "CUTSPACING" && i + 2 < n) {
            parseIntSafe(tokens[i + 1], cutSpacingX);
            parseIntSafe(tokens[i + 2], cutSpacingY);
            i += 3;
        } else if (tok == "ENCLOSURE" && i + 4 < n) {
            // DEF syntax: ENCLOSURE <botX> <botY> <topX> <topY>
            parseIntSafe(tokens[i + 1], enclosureBottomX);
            parseIntSafe(tokens[i + 2], enclosureBottomY);
            parseIntSafe(tokens[i + 3], enclosureTopX);
            parseIntSafe(tokens[i + 4], enclosureTopY);
            i += 5;
        } else if (tok == "ROWCOL" && i + 2 < n) {
            parseIntSafe(tokens[i + 1], rows);
            parseIntSafe(tokens[i + 2], cols);
            i += 3;
        } else {
            // Ignore other keywords we don't need for coarse modeling
            ++i;
        }
    }

    // Register via geometry in TechDatabase if we have basic connectivity
    if (!bottomLayer.empty() && !topLayer.empty()) {
        mTechDb.addViaGeometryFromDef(viaName,
                                      viaRuleName,
                                      bottomLayer,
                                      cutLayer,
                                      topLayer,
                                      cutSizeX,
                                      cutSizeY,
                                      cutSpacingX,
                                      cutSpacingY,
                                      enclosureBottomX,
                                      enclosureBottomY,
                                      enclosureTopX,
                                      enclosureTopY,
                                      rows,
                                      cols);
    }
}

// For example:
// - U_TSV_PG_Power1_VDD_PERI_MEDIA1_0 TSV_PG_POWER1
// + FIXED ( 100 1993000 ) N ;
void CoarsePdnBuilder3D::handleComponentsLine(const std::string& line,
                                              std::string& currentInstName,
                                              std::string& currentMacroName,
                                              int& currentX, int& currentY) {
    std::vector<std::string> tokens = tokenizeDef(line);
    if (tokens.empty()) return;

    // // Start of a new component: "- <instName> <macroName> ..."
    // if (tokens[0] == "-") {
    //     // Finalize previous component (if any)
    //     if (!currentInstName.empty() && !currentMacroName.empty()) {
    //         addTsvInstance(
    //           currentInstName, currentMacroName, currentX, currentY);
    //     }

    //     currentInstName.clear();
    //     currentMacroName.clear();
    //     currentX = currentY = 0;

    //     if (tokens.size() >= 3) {
    //         currentInstName  = stripDefQuotes(tokens[1]);
    //         currentMacroName = stripDefQuotes(tokens[2]);
    //     }
    //     return;
    // }
    std::size_t       i = 0;
    const std::size_t n = tokens.size();

    // Start of a new component: "- <instName> <macroName> ..."
    if (tokens[0] == "-") {
        // Finalize previous component (if any)
        if (!currentInstName.empty() && !currentMacroName.empty()) {
            recordTsvInstance(
              currentInstName, currentMacroName, currentX, currentY);
        }

        currentInstName.clear();
        currentMacroName.clear();
        // IMPORTANT: unknown until we parse PLACED/FIXED
        currentX = currentY = -1;

        if (n >= 3) {
            currentInstName  = stripDefQuotes(tokens[1]);
            currentMacroName = stripDefQuotes(tokens[2]);
            i = 3; // keep parsing same line for "+ PLACED/FIXED ..."
        } else {
            return;
        }
    }

    // Look for "+ FIXED ( x y )" or "+ PLACED ( x y )"
    // std::size_t       i = 0;
    // const std::size_t n = tokens.size();
    while (i < n) {
        const std::string& tok = tokens[i];

        if ((tok == "FIXED" || tok == "PLACED") && i + 4 < n) {
            // Expect: FIXED ( x y ) <orient>
            if (tokens[i + 1] != "(") {
                ++i;
                continue;
            }
            int x = 0, y = 0;
            if (!parseIntSafe(tokens[i + 2], x) ||
                !parseIntSafe(tokens[i + 3], y)) {
                break;
            }
            currentX = x;
            currentY = y;
            break;
        }

        ++i;
    }
}

// -----------------------------------------------------------------------------
// Representative coordinate helpers
// -----------------------------------------------------------------------------

static int representativeRow(const ConductanceGrid2D& grid, double y0,
                             double y1) {
    const int    ny   = grid.ny;
    const double dy   = grid.dy;
    double       yMid = (y0 + y1) / 2;
    int          jMid =
      clamp(static_cast<int>(std::floor((yMid - grid.yMin) / dy)), 0, ny - 1);
    return jMid;
}

static int representativeCol(const ConductanceGrid2D& grid, double x0,
                             double x1) {
    const int    nx   = grid.nx;
    const double dx   = grid.dx;
    double       xMid = (x0 + x1) / 2;
    int          iMid =
      clamp(static_cast<int>(std::floor((xMid - grid.xMin) / dx)), 0, nx - 1);
    return iMid;
}

// -----------------------------------------------------------------------------
// Stripe accumulation per (net,layer)
// -----------------------------------------------------------------------------

void CoarsePdnBuilder3D::recordViaInstance(const std::string& netName,
                                           const std::string& viaName,
                                           int xDbu, int yDbu) {
    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) return;
    const int netIndex = netIt->second.index;
    if (!isNetSelected(netIndex)) return;

    const TechVia* via = mTechDb.getVia(IdString::tryLookup(viaName));
    if (!via) return;
    if (via->resistance <= 0.0) return;

    auto blIt = mLayerNameToIndex.find(via->bottomLayer);
    auto tlIt = mLayerNameToIndex.find(via->topLayer);
    if (blIt == mLayerNameToIndex.end() || tlIt == mLayerNameToIndex.end())
        return;

    int lb = blIt->second;
    int lt = tlIt->second;
    if (lb < 0 || lb >= mNumLayers || lt < 0 || lt >= mNumLayers) return;
    if (lb == lt) return;

    ViaRec v;
    v.netIndex = netIndex;
    v.x        = xDbu;
    v.y        = yDbu;
    v.viaName  = IdString::tryLookup(viaName);
    v.lb       = lb;
    v.lt       = lt;
    v.g        = 1.0 / via->resistance;

    mRecordedVias.push_back(v);
}

// void CoarsePdnBuilder3D::addStripeRectangle(const std::string& netName,
//                                             const std::string&
//                                             layerName, int x0Dbu, int
//                                             y0Dbu, int x1Dbu, int y1Dbu)
//                                             {
//     auto netIt = mNetByName.find(IdString::tryLookup(netName));
//     if (netIt == mNetByName.end()) return;
//     int netIndex = netIt->second.index;

//     auto layerIdxIt =
//     mLayerNameToIndex.find(IdString::tryLookup(layerName)); if
//     (layerIdxIt
//     == mLayerNameToIndex.end()) return; int layerIndex =
//     layerIdxIt->second;

//     const TechLayer* layer =
//     mTechDb.getLayer(IdString::tryLookup(layerName)); if (!layer)
//     return;

//     // Convert to µm
//     double x0 = static_cast<double>(x0Dbu) / mDbuPerMicron;
//     double y0 = static_cast<double>(y0Dbu) / mDbuPerMicron;
//     double x1 = static_cast<double>(x1Dbu) / mDbuPerMicron;
//     double y1 = static_cast<double>(y1Dbu) / mDbuPerMicron;

//     if (x1 < x0) std::swap(x0, x1);
//     if (y1 < y0) std::swap(y0, y1);

//     double widthX = x1 - x0;
//     double widthY = y1 - y0;
//     if (widthX <= 0.0 || widthY <= 0.0) return;

//     bool isHorizontal = (widthX >= widthY);

//     ConductanceGrid2D& grid = mInPlaneGrids[netIndex][layerIndex];
//     if (isHorizontal) {
//         accumulateHorizontalStripe(grid, *layer, x0, y0, x1, y1);
//     } else {
//         accumulateVerticalStripe(grid, *layer, x0, y0, x1, y1);
//     }
// }

void CoarsePdnBuilder3D::accumulateHorizontalStripe(ConductanceGrid2D& grid,
                                                    const TechLayer&   layer,
                                                    double x0, double y0,
                                                    double x1, double y1) {
    double stripeWidth = y1 - y0; // µm
    if (stripeWidth <= 0.0) return;

    // Ohm/µm
    double RL = layer.resistivity / (layer.thickness * stripeWidth + 1e-30);

    const int    nx = grid.nx;
    const int    ny = grid.ny;
    const double dx = grid.dx;
    const double dy = grid.dy;

    // Rows [jStart..jEnd] where band intersects [y0,y1]
    // int jStart =
    //   clamp(static_cast<int>(std::floor((y0 - grid.yMin) / dy)), 0, ny -
    //   1);
    // int jEnd =
    //   clamp(static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 0, ny -
    //   1);
    // Representative row
    int iy = representativeRow(grid, y0, y1);

    // Vertical boundaries [kStart..kEnd] where xMin + k*dx in [x0,x1], 1
    // <= k <= nx-1
    int kStart =
      clamp(static_cast<int>(std::ceil((x0 - grid.xMin) / dx)), 1, nx - 1);
    int kEnd =
      clamp(static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 1, nx - 1);

    // if (jEnd < jStart || kEnd < kStart) return;
    if (kEnd < kStart) return;

    double segmentLength = dx;
    double G_perSegment  = 1.0 / (RL * segmentLength); // Siemens

    // for (int j = jStart; j <= jEnd; ++j) {
    for (int k = kStart; k <= kEnd; ++k) {
        int ix = k - 1;
        grid.gx(ix, iy) += G_perSegment;
    }
    // }
}

void CoarsePdnBuilder3D::accumulateVerticalStripe(ConductanceGrid2D& grid,
                                                  const TechLayer&   layer,
                                                  double x0, double y0,
                                                  double x1, double y1) {
    double stripeWidth = x1 - x0; // µm
    if (stripeWidth <= 0.0) return;

    double RL =
      layer.resistivity / (layer.thickness * stripeWidth + 1e-30); // Ohm/µm

    const int    nx = grid.nx;
    const int    ny = grid.ny;
    const double dx = grid.dx;
    const double dy = grid.dy;

    // Columns [iStart..iEnd]
    // int iStart =
    //   clamp(static_cast<int>(std::floor((x0 - grid.xMin) / dx)), 0, nx -
    //   1);
    // int iEnd =
    //   clamp(static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 0, nx -
    //   1);
    int ix = representativeCol(grid, x0, x1);

    // Horizontal boundaries [lStart..lEnd], 1 <= l <= ny-1
    int lStart =
      clamp(static_cast<int>(std::ceil((y0 - grid.yMin) / dy)), 1, ny - 1);
    int lEnd =
      clamp(static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 1, ny - 1);

    // if (iEnd < iStart || lEnd < lStart) return;
    if (lEnd < lStart) return;

    double segmentLength = dy;
    double G_perSegment  = 1.0 / (RL * segmentLength); // Siemens

    // for (int i = iStart; i <= iEnd; ++i) {
    for (int l = lStart; l <= lEnd; ++l) {
        int iy = l - 1;
        grid.gy(ix, iy) += G_perSegment;
    }
    // }
}

// -----------------------------------------------------------------------------
// Via accumulation
// -----------------------------------------------------------------------------

void CoarsePdnBuilder3D::recordStripeRectangle(const std::string& netName,
                                               const std::string& layerName,
                                               int x0Dbu, int y0Dbu, int x1Dbu,
                                               int y1Dbu) {
    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) return;
    const int netIndex = netIt->second.index;
    if (!isNetSelected(netIndex)) return;

    auto layerIdxIt = mLayerNameToIndex.find(IdString::tryLookup(layerName));
    if (layerIdxIt == mLayerNameToIndex.end()) return;
    const int layerIndex = layerIdxIt->second;

    // normalize
    int x0 = std::min(x0Dbu, x1Dbu);
    int x1 = std::max(x0Dbu, x1Dbu);
    int y0 = std::min(y0Dbu, y1Dbu);
    int y1 = std::max(y0Dbu, y1Dbu);
    if (x1 <= x0 || y1 <= y0) return;

    StripeRec s;
    s.netIndex   = netIndex;
    s.layerIndex = layerIndex;
    s.x0         = x0;
    s.y0         = y0;
    s.x1         = x1;
    s.y1         = y1;

    mRecordedStripes.push_back(s);
}

// Get the "VDD_PERI_MEDIA1" part out of U_TSV_PG_Power1_VDD_PERI_MEDIA1_0
static std::string extractNetNameFromInstance(const std::string& instName,
                                              const std::string& macroName) {
    // Expected pattern (example):
    //   U_<macroName>_<netName>_<number>
    //
    // We do a case-insensitive search for <macroName>, then take the
    // substring between the macro and the final "_<digits>" suffix.

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };

    const std::string instLower  = toLower(instName);
    const std::string macroLower = toLower(macroName);

    // Require "U_" prefix (case-insensitive).
    if (instLower.size() < 2 || instLower.rfind("u_", 0) != 0) {
        return "";
    }

    // Find macro name in the instance (case-insensitive), starting after
    // "U_".
    const std::size_t searchStart = 2;
    const std::size_t macroPos    = instLower.find(macroLower, searchStart);
    if (macroPos == std::string::npos) return "";

    std::size_t afterMacro = macroPos + macroLower.size();
    if (afterMacro > instName.size()) return "";

    // Skip '_' after macro name.
    while (afterMacro < instName.size() && instName[afterMacro] == '_') {
        ++afterMacro;
    }

    // We expect a numeric suffix "_<digits>".
    const std::size_t lastUnderscore = instName.rfind('_');
    if (lastUnderscore == std::string::npos) return "";
    if (lastUnderscore <= afterMacro) return "";

    // Verify suffix is all digits.
    if (lastUnderscore + 1 >= instName.size()) return "";
    for (std::size_t i = lastUnderscore + 1; i < instName.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(instName[i]))) {
            return "";
        }
    }

    return instName.substr(afterMacro, lastUnderscore - afterMacro);
}

void CoarsePdnBuilder3D::recordTsvInstance(const std::string& instName,
                                           const std::string& macroName,
                                           int xDbu, int yDbu) {
    // Only TSV macros should be recorded.
    const TechTsv* tsv = mTechDb.getTsv(IdString(macroName));
    if (!tsv) return;

    // Coordinates are required for correct tile snapping.
    if (xDbu < 0 || yDbu < 0) {
        PDN_WARN("Found a TSV without coordinates. Skip. inst=%s macro=%s",
                 instName.c_str(),
                 macroName.c_str());
        return;
    }

    // Derive net name from the instance naming convention.
    std::string netName = extractNetNameFromInstance(instName, macroName);
    if (netName.empty()) {
        PDN_WARN("Failed to extract TSV net name from instance: %s (macro=%s)",
                 instName.c_str(),
                 macroName.c_str());
        return;
    }

    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) {
        // Not a PDN net we care about.
        return;
    }
    const int netIndex = netIt->second.index;
    if (!isNetSelected(netIndex)) return;

    // Resolve layers
    auto blIt = mLayerNameToIndex.find(tsv->bottomLayer);
    auto tlIt = mLayerNameToIndex.find(tsv->topLayer);
    if (blIt == mLayerNameToIndex.end() || tlIt == mLayerNameToIndex.end()) {
        return;
    }

    const int lb = blIt->second;
    const int lt = tlIt->second;

    if (lb < 0 || lb >= mNumLayers || lt < 0 || lt >= mNumLayers) return;
    if (lb == lt) return;
    if (tsv->resistance <= 0.0) return;

    TsvRec t;
    t.netIndex = netIndex;
    t.x        = xDbu;
    t.y        = yDbu;
    t.tsvName  = IdString(macroName); // stored only for debugging/metadata
    t.lb       = lb;
    t.lt       = lt;
    t.g        = 1.0 / tsv->resistance;

    mRecordedTsvs.push_back(std::move(t));
}

ConductanceGrid3D& CoarsePdnBuilder3D::getOrCreateViaGrid(int netIndex, int lb,
                                                          int lt) {
    return getOrCreateGrid(netIndex, lb, lt, /* isTsv */ false);
}

ConductanceGrid3D& CoarsePdnBuilder3D::getOrCreateTsvGrid(int netIndex, int lb,
                                                          int lt) {
    return getOrCreateGrid(netIndex, lb, lt, /* isTsv */ true);
}

ConductanceGrid3D& CoarsePdnBuilder3D::getOrCreateGrid(int netIndex, int lb,
                                                       int lt, bool isTsv) {
    std::uint64_t key = makeGridKey(netIndex, lb, lt);
    auto          it  = m3DGridLookup.find(key);
    if (it != m3DGridLookup.end()) {
        ConductanceGrid3D& grid = m3DGrids[it->second];
        PDN_ERROR_IF(grid.isTsv != isTsv,
                     "Inconsistent tsv config found. Skip");
        return grid;
    }

    const LayerGridResolution& bRes = mLayerGridRes[lb];
    const LayerGridResolution& tRes = mLayerGridRes[lt];

    ConductanceGrid3D vg;
    vg.init(netIndex, lb, lt, bRes.nx, bRes.ny, tRes.nx, tRes.ny, isTsv);

    int idx = static_cast<int>(m3DGrids.size());
    m3DGrids.push_back(std::move(vg));
    m3DGridLookup[key] = idx;
    return m3DGrids.back();
}

std::uint64_t CoarsePdnBuilder3D::makeGridKey(int netIndex, int lb, int lt) {
    // Pack (netIndex, lb, lt) into 64 bits (assuming < 65536 of each)
    std::uint64_t k = 0;
    k |= (static_cast<std::uint64_t>(netIndex) & 0xFFFFu) << 32;
    k |= (static_cast<std::uint64_t>(lb) & 0xFFFFu) << 16;
    k |= (static_cast<std::uint64_t>(lt) & 0xFFFFu);
    return k;
}

// -----------------------------------------------------------------------------
// CircuitGraph construction
// -----------------------------------------------------------------------------
void CoarsePdnBuilder3D::finalizeRecordedPdnGeometry() {
    if (mRecordedStripes.empty() && mRecordedVias.empty() &&
        mRecordedTsvs.empty()) {
        return;
    }

    // ---------------------------------------------------------------------
    // 0) Build a per-(net,layer) spatial lookup of stripes on the coarse
    // tiles
    // ---------------------------------------------------------------------
    struct StripeSpatialIndex {
        int                           nx = 0;
        int                           ny = 0;
        // buckets[iy*nx+ix] -> stripe indices
        std::vector<std::vector<int>> buckets;

        void init(int nx_, int ny_) {
            nx = nx_;
            ny = ny_;
            buckets.assign(static_cast<std::size_t>(nx * ny), {});
        }

        std::vector<int>& at(int ix, int iy) {
            return buckets[static_cast<std::size_t>(iy * nx + ix)];
        }
        const std::vector<int>& at(int ix, int iy) const {
            return buckets[static_cast<std::size_t>(iy * nx + ix)];
        }
    };

    std::vector<std::vector<StripeSpatialIndex>> stripeIndex;
    stripeIndex.resize(static_cast<std::size_t>(mNumNets));
    for (int n = 0; n < mNumNets; ++n) {
        stripeIndex[n].resize(static_cast<std::size_t>(mNumLayers));
        for (int l = 0; l < mNumLayers; ++l) {
            const ConductanceGrid2D& g = m2DGrids[n][l];
            stripeIndex[n][l].init(g.nx, g.ny);
        }
    }

    auto tileIndexOfPoint = [&](const ConductanceGrid2D& g,
                                double                   x_um,
                                double y_um) -> std::pair<int, int> {
        const int ix = clamp(
          static_cast<int>(std::floor((x_um - g.xMin) / g.dx)), 0, g.nx - 1);
        const int iy = clamp(
          static_cast<int>(std::floor((y_um - g.yMin) / g.dy)), 0, g.ny - 1);
        return {ix, iy};
    };

    // Insert stripes into the buckets of tiles it overlaps (bbox of tiles)
    for (std::size_t sIdx = 0; sIdx < mRecordedStripes.size(); ++sIdx) {
        const StripeRec& s = mRecordedStripes[sIdx];
        if (s.netIndex < 0 || s.netIndex >= mNumNets) continue;
        if (s.layerIndex < 0 || s.layerIndex >= mNumLayers) continue;

        const ConductanceGrid2D& g = m2DGrids[s.netIndex][s.layerIndex];

        const double x0_um = static_cast<double>(s.x0) / mDbuPerMicron;
        const double y0_um = static_cast<double>(s.y0) / mDbuPerMicron;
        const double x1_um = static_cast<double>(s.x1) / mDbuPerMicron;
        const double y1_um = static_cast<double>(s.y1) / mDbuPerMicron;

        // Convert stripe bbox to a tile index range (inclusive).
        const int ix0 = clamp(
          static_cast<int>(std::floor((x0_um - g.xMin) / g.dx)), 0, g.nx - 1);
        const int ix1 = clamp(
          static_cast<int>(std::floor((x1_um - g.xMin) / g.dx)), 0, g.nx - 1);
        const int iy0 = clamp(
          static_cast<int>(std::floor((y0_um - g.yMin) / g.dy)), 0, g.ny - 1);
        const int iy1 = clamp(
          static_cast<int>(std::floor((y1_um - g.yMin) / g.dy)), 0, g.ny - 1);

        for (int iy = iy0; iy <= iy1; ++iy) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                stripeIndex[s.netIndex][s.layerIndex].at(ix, iy).push_back(
                  static_cast<int>(sIdx));
            }
        }
    }

    // ---------------------------------------------------------------------
    // 1) Discretize all stripes into in-plane conductance grids
    // ---------------------------------------------------------------------

    for (const StripeRec& s : mRecordedStripes) {
        if (s.netIndex < 0 || s.netIndex >= mNumNets) continue;
        if (s.layerIndex < 0 || s.layerIndex >= mNumLayers) continue;

        const TechLayer* layer = mTechDb.getLayer(mLayerOrder[s.layerIndex]);
        if (!layer) continue;

        // Convert to µm
        double x0 = static_cast<double>(s.x0) / mDbuPerMicron;
        double y0 = static_cast<double>(s.y0) / mDbuPerMicron;
        double x1 = static_cast<double>(s.x1) / mDbuPerMicron;
        double y1 = static_cast<double>(s.y1) / mDbuPerMicron;

        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);

        const double wX = x1 - x0;
        const double wY = y1 - y0;
        if (wX <= 0.0 || wY <= 0.0) continue;

        ConductanceGrid2D& grid         = m2DGrids[s.netIndex][s.layerIndex];
        const bool         isHorizontal = (wX >= wY);
        if (isHorizontal) {
            accumulateHorizontalStripe(grid, *layer, x0, y0, x1, y1);
        } else {
            accumulateVerticalStripe(grid, *layer, x0, y0, x1, y1);
        }
    }

    // ---------------------------------------------------------------------
    // 2) Allocate vias/tsvs to the "metal tile" they belong to, then add G
    // ---------------------------------------------------------------------

    // Find the tiles index for a given via/tsv coordinate
    auto allocateEndpoint = [&](int netIndex,
                                int layerIndex,
                                int xDbu,
                                int yDbu) -> std::pair<int, int> {
        const ConductanceGrid2D& g = m2DGrids[netIndex][layerIndex];

        const double x_um = static_cast<double>(xDbu) / mDbuPerMicron;
        const double y_um = static_cast<double>(yDbu) / mDbuPerMicron;

        auto [ixRaw, iyRaw] = tileIndexOfPoint(g, x_um, y_um);

        const std::vector<int>& candidates =
          stripeIndex[netIndex][layerIndex].at(ixRaw, iyRaw);

        // Find best horizontal stripe (closest centerline) and best
        // vertical stripe.
        int       bestH      = -1;
        long long bestHDist  = std::numeric_limits<long long>::max();
        long long bestHWidth = std::numeric_limits<long long>::max();

        int       bestV      = -1;
        long long bestVDist  = std::numeric_limits<long long>::max();
        long long bestVWidth = std::numeric_limits<long long>::max();

        // Tolerance in DBU to avoid "on-the-edge" misses due to integer
        // half-width truncation.
        constexpr int tolDbu = 1;

        for (int sIdx : candidates) {
            const StripeRec& s =
              mRecordedStripes[static_cast<std::size_t>(sIdx)];
            // (Should already match net/layer, but keep defensive)
            if (s.netIndex != netIndex || s.layerIndex != layerIndex) continue;

            if (xDbu < s.x0 - tolDbu || xDbu > s.x1 + tolDbu ||
                yDbu < s.y0 - tolDbu || yDbu > s.y1 + tolDbu) {
                continue;
            }

            const long long dx           = static_cast<long long>(s.x1) - s.x0;
            const long long dy           = static_cast<long long>(s.y1) - s.y0;
            const bool      isHorizontal = (dx >= dy);

            if (isHorizontal) {
                // distance to horizontal stripe centerline in y, in
                // "2*DBU" units
                const long long dist =
                  std::llabs(2LL * static_cast<long long>(yDbu) -
                             (static_cast<long long>(s.y0) + s.y1));
                const long long width = dy;

                if (dist < bestHDist ||
                    (dist == bestHDist && width < bestHWidth)) {
                    bestHDist  = dist;
                    bestHWidth = width;
                    bestH      = sIdx;
                }
            } else {
                // distance to vertical stripe centerline in x
                const long long dist =
                  std::llabs(2LL * static_cast<long long>(xDbu) -
                             (static_cast<long long>(s.x0) + s.x1));
                const long long width = dx;

                if (dist < bestVDist ||
                    (dist == bestVDist && width < bestVWidth)) {
                    bestVDist  = dist;
                    bestVWidth = width;
                    bestV      = sIdx;
                }
            }
        }

        int ix = ixRaw;
        int iy = iyRaw;

        // Snap row to the horizontal stripe's representative row (if any).
        if (bestH >= 0) {
            const StripeRec& s =
              mRecordedStripes[static_cast<std::size_t>(bestH)];
            const double y0_um = static_cast<double>(s.y0) / mDbuPerMicron;
            const double y1_um = static_cast<double>(s.y1) / mDbuPerMicron;
            iy                 = representativeRow(g, y0_um, y1_um);
        }

        // Snap col to the vertical stripe's representative col (if any).
        if (bestV >= 0) {
            const StripeRec& s =
              mRecordedStripes[static_cast<std::size_t>(bestV)];
            const double x0_um = static_cast<double>(s.x0) / mDbuPerMicron;
            const double x1_um = static_cast<double>(s.x1) / mDbuPerMicron;
            ix                 = representativeCol(g, x0_um, x1_um);
        }

        return {ix, iy};
    };

    for (const ViaRec& v : mRecordedVias) {
        if (v.netIndex < 0 || v.netIndex >= mNumNets) continue;
        if (v.lb < 0 || v.lb >= mNumLayers) continue;
        if (v.lt < 0 || v.lt >= mNumLayers) continue;
        if (v.g <= 0.0) continue;

        auto [ixB, iyB] = allocateEndpoint(v.netIndex, v.lb, v.x, v.y);
        auto [ixT, iyT] = allocateEndpoint(v.netIndex, v.lt, v.x, v.y);

        ConductanceGrid3D& vg = getOrCreateViaGrid(v.netIndex, v.lb, v.lt);
        vg.addConductance(ixB, iyB, ixT, iyT, v.g);
    }

    for (const TsvRec& t : mRecordedTsvs) {
        if (t.netIndex < 0 || t.netIndex >= mNumNets) continue;
        if (t.lb < 0 || t.lb >= mNumLayers) continue;
        if (t.lt < 0 || t.lt >= mNumLayers) continue;
        if (t.g <= 0.0) continue;

        auto [ixB, iyB] = allocateEndpoint(t.netIndex, t.lb, t.x, t.y);
        auto [ixT, iyT] = allocateEndpoint(t.netIndex, t.lt, t.x, t.y);

        ConductanceGrid3D& tg = getOrCreateTsvGrid(t.netIndex, t.lb, t.lt);
        tg.addConductance(ixB, iyB, ixT, iyT, t.g);
    }

    // Free memory since I don't need the raw records after discretization
    mRecordedStripes.clear();
    mRecordedVias.clear();
    mRecordedTsvs.clear();
}

void CoarsePdnBuilder3D::buildCircuitGraph(
  CircuitGraph& graph, const std::vector<int>& netIndices) {
    graph.mCoordinateUnit = CircuitGraph::UM;

    if (netIndices.empty()) {
        // Nothing selected: return an empty graph
        return;
    }

    // Map global netIndex -> local selection index
    std::vector<int> netToLocal(static_cast<std::size_t>(mNumNets), -1);
    std::vector<int> nets; // sanitized copy
    nets.reserve(netIndices.size());

    for (int n : netIndices) {
        if (n < 0 || n >= mNumNets) continue;
        if (netToLocal[static_cast<std::size_t>(n)] >= 0) continue; // dedup
        netToLocal[static_cast<std::size_t>(n)] =
          static_cast<int>(nets.size());
        nets.push_back(n);
    }

    // tileNodeIds[localNetIdx][layerIndex] -> 2D grid of node IDs
    std::vector<std::vector<TileNodeIdGrid>> tileNodeIds;
    tileNodeIds.resize(static_cast<std::size_t>(nets.size()));

    for (std::size_t ln = 0; ln < nets.size(); ++ln) {
        const int netIndex = nets[ln];
        tileNodeIds[ln].resize(static_cast<std::size_t>(mNumLayers));
        for (int l = 0; l < mNumLayers; ++l) {
            const ConductanceGrid2D& grid = m2DGrids[netIndex][l];
            tileNodeIds[ln][static_cast<std::size_t>(l)].init(grid.nx,
                                                              grid.ny);
        }
    }

    // Register nets per layer and store the resulting NetId
    std::vector<std::vector<NetId>> netLayerIds;
    netLayerIds.resize(static_cast<std::size_t>(nets.size()),
                       std::vector<NetId>(static_cast<std::size_t>(mNumLayers),
                                          NetId::Invalid));

    for (int l = 0; l < mNumLayers; ++l) {
        IdString layerName = mLayerOrder[l];
        for (std::size_t ln = 0; ln < nets.size(); ++ln) {
            const NetInfo& ni = mNetByIndex[nets[ln]];
            netLayerIds[ln][static_cast<std::size_t>(l)] =
              graph.registerNet(layerName, ni.name, ni.isPower, ni.isGround);
        }
    }

    // 1) Create nodes and in-plane metal resistors
    for (std::size_t ln = 0; ln < nets.size(); ++ln) {
        const int      netIndex = nets[ln];
        const NetInfo& ni       = mNetByIndex[netIndex];

        for (int l = 0; l < mNumLayers; ++l) {
            IdString                 layerName = mLayerOrder[l];
            const ConductanceGrid2D& grid      = m2DGrids[netIndex][l];

            const int nx = grid.nx;
            const int ny = grid.ny;

            const NetId netId = netLayerIds[ln][static_cast<std::size_t>(l)];

            // 1.a) Create tile nodes
            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    double xCenterUm = grid.xMin + (ix + 0.5) * grid.dx;
                    double yCenterUm = grid.yMin + (iy + 0.5) * grid.dy;

                    std::ostringstream ossName;
                    ossName << ni.name.str() << "_" << layerName.str() << "_T_"
                            << ix << "_" << iy;

                    IdString nodeId = IdString(ossName.str());
                    Node     node;
                    node.name = nodeId;
                    node.net  = netId;
                    node.x    = FPN::toRep(xCenterUm);
                    node.y    = FPN::toRep(yCenterUm);

                    graph.mNodes.emplace(nodeId, node);
                    tileNodeIds[ln][static_cast<std::size_t>(l)].at(ix, iy) =
                      nodeId;
                }
            }

            // 1.b) Horizontal resistors (Gx)
            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx - 1; ++ix) {
                    double G = grid.gx(ix, iy);
                    if (G <= 0.0) continue;

                    double   R = 1.0 / G;
                    IdString n1 =
                      tileNodeIds[ln][static_cast<std::size_t>(l)].at(ix, iy);
                    IdString n2 =
                      tileNodeIds[ln][static_cast<std::size_t>(l)].at(ix + 1,
                                                                      iy);

                    std::ostringstream ossResName;
                    ossResName << ni.name.str() << "_" << layerName.str()
                               << "_RH_" << ix << "_" << iy;
                    IdString resId = IdString(ossResName.str());

                    MetalRes mr;
                    mr.name = resId;
                    mr.net  = netId;
                    mr.n1   = n1;
                    mr.n2   = n2;
                    mr.R    = R;
                    graph.mMetalResistors.push_back(std::move(mr));
                }
            }

            // 1.c) Vertical resistors (Gy)
            for (int iy = 0; iy < ny - 1; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    double G = grid.gy(ix, iy);
                    if (G <= 0.0) continue;

                    double   R = 1.0 / G;
                    IdString n1 =
                      tileNodeIds[ln][static_cast<std::size_t>(l)].at(ix, iy);
                    IdString n2 =
                      tileNodeIds[ln][static_cast<std::size_t>(l)].at(ix,
                                                                      iy + 1);

                    std::ostringstream ossResName;
                    ossResName << ni.name.str() << "_" << layerName.str()
                               << "_RV_" << ix << "_" << iy;
                    IdString resId = IdString(ossResName.str());

                    MetalRes mr;
                    mr.name = resId;
                    mr.net  = netId;
                    mr.n1   = n1;
                    mr.n2   = n2;
                    mr.R    = R;
                    graph.mMetalResistors.push_back(std::move(mr));
                }
            }
        }
    }

    // 2) Via/Tsv resistors
    int viaCounter = 0;
    int tsvCounter = 0;
    for (const ConductanceGrid3D& g : m3DGrids) {
        const int netIndex = g.netIndex;
        if (netIndex < 0 || netIndex >= mNumNets) continue;

        const int ln = netToLocal[static_cast<std::size_t>(netIndex)];
        if (ln < 0) continue; // not selected

        const int  lb    = g.bottomLayerIdx;
        const int  lt    = g.topLayerIdx;
        const bool isTsv = g.isTsv;

        const NetInfo& ni     = mNetByIndex[netIndex];
        IdString       blName = mLayerOrder[lb];
        IdString       tlName = mLayerOrder[lt];

        for (const auto& kv : g.edgeG) {
            const std::uint64_t edgeKey = kv.first;
            const double        G       = kv.second;
            if (G <= 0.0) continue;

            const double R = 1.0 / G;

            std::uint32_t flatB = 0, flatT = 0;
            ConductanceGrid3D::unpackEdge(edgeKey, flatB, flatT);

            const int ixB =
              static_cast<int>(flatB % static_cast<std::uint32_t>(g.nxB));
            const int iyB =
              static_cast<int>(flatB / static_cast<std::uint32_t>(g.nxB));
            const int ixT =
              static_cast<int>(flatT % static_cast<std::uint32_t>(g.nxT));
            const int iyT =
              static_cast<int>(flatT / static_cast<std::uint32_t>(g.nxT));

            IdString bottomId = tileNodeIds[static_cast<std::size_t>(ln)]
                                           [static_cast<std::size_t>(lb)]
                                             .at(ixB, iyB);
            IdString topId = tileNodeIds[static_cast<std::size_t>(ln)]
                                        [static_cast<std::size_t>(lt)]
                                          .at(ixT, iyT);

            std::ostringstream ossName;
            if (isTsv) {
                ossName << ni.name.str() << "_TSV" << blName.str() << "_to_"
                        << tlName.str() << "_B_" << ixB << "_" << iyB << "_T_"
                        << ixT << "_" << iyT << "_" << tsvCounter++;

                TsvRes tr;
                tr.name = IdString(ossName.str());
                tr.n1   = bottomId;
                tr.n2   = topId;
                tr.R    = R;
                graph.mTsvResistors.push_back(std::move(tr));
            } else {
                ossName << ni.name.str() << "_VIA_" << blName.str() << "_to_"
                        << tlName.str() << "_B_" << ixB << "_" << iyB << "_T_"
                        << ixT << "_" << iyT << "_" << viaCounter++;

                ViaRes vr;
                vr.name = IdString(ossName.str());
                vr.n1   = bottomId;
                vr.n2   = topId;
                vr.R    = R;
                graph.mViaResistors.push_back(std::move(vr));
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Rect & VIA token parsing helpers (currently unused, but kept for future
// use)
// -----------------------------------------------------------------------------

bool CoarsePdnBuilder3D::parseRectFromTokens(
  const std::vector<std::string>& tokens, std::size_t startIdx, int& x0,
  int& y0, int& x1, int& y1, std::size_t& consumed) const {

    consumed            = 0;
    const std::size_t n = tokens.size();
    if (startIdx >= n) return false;

    // Case 1: RECT ( x0 y0 ) ( x1 y1 )
    if (tokens[startIdx] == "(") {
        if (startIdx + 7 >= n) return false; // need: ( x0 y0 ) ( x1 y1 )
        int tx0, ty0, tx1, ty1;
        if (!parseIntSafe(tokens[startIdx + 1], tx0) ||
            !parseIntSafe(tokens[startIdx + 2], ty0) ||
            tokens[startIdx + 3] != ")" || tokens[startIdx + 4] != "(" ||
            !parseIntSafe(tokens[startIdx + 5], tx1) ||
            !parseIntSafe(tokens[startIdx + 6], ty1) ||
            tokens[startIdx + 7] != ")") {
            return false;
        }
        x0       = tx0;
        y0       = ty0;
        x1       = tx1;
        y1       = ty1;
        consumed = 8;
        return true;
    }

    // Case 2: RECT x0 y0 x1 y1
    if (startIdx + 3 >= n) return false;
    int tx0, ty0, tx1, ty1;
    if (!parseIntSafe(tokens[startIdx], tx0) ||
        !parseIntSafe(tokens[startIdx + 1], ty0) ||
        !parseIntSafe(tokens[startIdx + 2], tx1) ||
        !parseIntSafe(tokens[startIdx + 3], ty1)) {
        return false;
    }
    x0       = tx0;
    y0       = ty0;
    x1       = tx1;
    y1       = ty1;
    consumed = 4;
    return true;
}

bool CoarsePdnBuilder3D::parseViaFromTokens(
  const std::vector<std::string>& tokens, std::size_t startIdx,
  std::string& viaName, int& x, int& y, std::size_t& consumed) const {

    consumed            = 0;
    const std::size_t n = tokens.size();
    if (startIdx >= n) return false;

    viaName         = stripDefQuotes(tokens[startIdx]);
    std::size_t idx = startIdx + 1;
    if (idx >= n) return false;

    int tx = 0, ty = 0;

    if (tokens[idx] == "(") {
        // VIA viaName ( x y )
        if (idx + 3 >= n) return false;
        if (!parseIntSafe(tokens[idx + 1], tx) ||
            !parseIntSafe(tokens[idx + 2], ty)) {
            return false;
        }
        x        = tx;
        y        = ty;
        // viaName, '(', x, y, ')'
        consumed = (idx + 4) - startIdx;
        return true;
    } else {
        // VIA viaName x y
        if (idx + 1 >= n) return false;
        if (!parseIntSafe(tokens[idx], tx) ||
            !parseIntSafe(tokens[idx + 1], ty)) {
            return false;
        }
        x        = tx;
        y        = ty;
        // viaName, x, y
        consumed = (idx + 2) - startIdx;
        return true;
    }
}

// -----------------------------------------------------------------------------
// Net registration helper
// -----------------------------------------------------------------------------

void CoarsePdnBuilder3D::addNetIfAbsent(const std::string& name, bool isPower,
                                        bool isGround) {
    IdString nameId(name);
    auto     it = mNetByName.find(nameId);
    if (it != mNetByName.end()) {
        if (isPower) it->second.isPower = true;
        if (isGround) it->second.isGround = true;
        // Update vector copy as well
        int idx = it->second.index;
        if (idx >= 0 && idx < static_cast<int>(mNetByIndex.size())) {
            if (isPower) mNetByIndex[idx].isPower = true;
            if (isGround) mNetByIndex[idx].isGround = true;
        }
        return;
    }
    NetInfo info;
    info.index         = static_cast<int>(mNetByIndex.size());
    info.name          = nameId;
    info.isPower       = isPower;
    info.isGround      = isGround;
    mNetByName[nameId] = info;
    mNetByIndex.push_back(info);
}

} // namespace pdnsol

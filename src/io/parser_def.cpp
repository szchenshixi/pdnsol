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
std::string stripDefQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Robust integer parser for DEF coordinate tokens.
bool parseIntSafe(const std::string& s, int& out) {
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
std::vector<std::string> tokenizeDef(const std::string& s) {
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
// ViaGrid3D implementation
// -----------------------------------------------------------------------------

void ViaGrid3D::init(int netIdx, int lb, int lt, int bNx, int bNy, int tNx,
                     int tNy) {
    netIndex       = netIdx;
    bottomLayerIdx = lb;
    topLayerIdx    = lt;

    bottomNx = bNx;
    bottomNy = bNy;
    topNx    = tNx;
    topNy    = tNy;

    edgeG.clear();
}

void ViaGrid3D::addConductance(int bIx, int bIy, int tIx, int tIy, double dG) {
    if (dG <= 0.0) return;

    PDN_FATAL_IF(bIx < 0 || bIx >= bottomNx,
                 "Bottom-X index %d beyond scope (0, %d)",
                 bIx,
                 bottomNx);
    PDN_FATAL_IF(bIy < 0 || bIy >= bottomNy,
                 "Bottom-Y index %d beyond scope (0, %d)",
                 bIy,
                 bottomNy);
    PDN_FATAL_IF(tIx < 0 || tIx >= topNx,
                 "Top-X index %d beyond scope (0, %d)",
                 tIx,
                 topNx);
    PDN_FATAL_IF(tIy < 0 || tIy >= topNy,
                 "Top-Y index %d beyond scope (0, %d)",
                 tIy,
                 topNy);

    const std::uint32_t bFlat =
      static_cast<std::uint32_t>(bIy * bottomNx + bIx);
    const std::uint32_t tFlat = static_cast<std::uint32_t>(tIy * topNx + tIx);

    edgeG[packEdge(bFlat, tFlat)] += dG; // parallel vias add conductance
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
  TechDatabase& techDb, int defaultGridNx, int defaultGridNy,
  const IdString::Map<LayerGridResolution>& perLayerRes,
  const std::vector<std::string>&           powerNetNames,
  const std::vector<std::string>&           groundNetNames,
  const std::vector<std::string>&           layerOrder)
    : mTechDb(techDb)
    , mDefaultGridNx(defaultGridNx)
    , mDefaultGridNy(defaultGridNy) {

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
            mLayerGridRes[l] =
              LayerGridResolution{mDefaultGridNx, mDefaultGridNy};
        }

        if (mLayerGridRes[l].nx <= 0 || mLayerGridRes[l].ny <= 0) {
            throw std::runtime_error("Invalid grid resolution for layer " +
                                     lname.str());
        }
    }
}

int CoarsePdnBuilder3D::encodeNetLayer(int netIndex, int layerIndex) const {
    return layerIndex * mNumNets + netIndex;
}

bool CoarsePdnBuilder3D::buildCoarsePdnFromDef(const std::string& defPath,
                                               CircuitGraph&      outGraph) {
    // Ensure we start clean for each DEF
    resetForNewBuild();

    // First pass: parse units and die area
    if (!parseDefGeometry(defPath)) {
        std::cerr << "ERROR: Failed to parse geometry from DEF: " << defPath
                  << "\n";
        return false;
    }

    // Initialize all per-(net,layer) conductance grids
    initInPlaneGrids();

    // Second pass: parse PDN stripes, vias, and bumps
    if (!parseDefPdnAndBumps(defPath)) {
        std::cerr << "ERROR: Failed to parse PDN & bumps from DEF: " << defPath
                  << "\n";
        return false;
    }

    // Build CircuitGraph from all grids
    buildCircuitGraph(outGraph);
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

void CoarsePdnBuilder3D::initInPlaneGrids() {
    mInPlaneGrids.assign(
      mNumNets,
      std::vector<ConductanceGrid2D>(static_cast<std::size_t>(mNumLayers)));

    for (int n = 0; n < mNumNets; ++n) {
        for (int l = 0; l < mNumLayers; ++l) {
            mInPlaneGrids[n][l].init(mLayerGridRes[l].nx,
                                     mLayerGridRes[l].ny,
                                     mDieXMinUm,
                                     mDieXMaxUm,
                                     mDieYMinUm,
                                     mDieYMaxUm);
        }
    }
}

void CoarsePdnBuilder3D::resetForNewBuild() {
    mInPlaneGrids.clear();
    mViaGrids.clear();
    mViaGridLookup.clear();
    // mBumps.clear();
    // mPinParseState.reset();
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
void CoarsePdnBuilder3D::addStripeFromSegment(const std::string& netName,
                                              const std::string& layerName,
                                              int x0, int y0, int x1, int y1,
                                              int widthDbu) {
    if (widthDbu <= 0) return;

    const int half = widthDbu / 2;

    if (x0 == x1) {
        // Vertical stripe
        int yMin = std::min(y0, y1);
        int yMax = std::max(y0, y1);
        addStripeRectangle(
          netName, layerName, x0 - half, yMin, x0 + half, yMax);
    } else if (y0 == y1) {
        // Horizontal stripe
        int xMin = std::min(x0, x1);
        int xMax = std::max(x0, x1);
        addStripeRectangle(
          netName, layerName, xMin, y0 - half, xMax, y0 + half);
    } else {
        // Non-Manhattan (unlikely in PDN); fall back to bounding box.
        int xMin = std::min(x0, x1);
        int xMax = std::max(x0, x1);
        int yMin = std::min(y0, y1);
        int yMax = std::max(y0, y1);
        addStripeRectangle(netName, layerName, xMin, yMin, xMax, yMax);
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
                addTsvInstance(currentCompInstName,
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
            currentNetIsPdn = (it != mNetByName.end());
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
                currentLayerName = tokens[i++];
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
                    addViaInstance(currentNetName, viaName, x0, y0);
                } else if (i < n && tokens[i] == "(") {
                    // Wire segment: (x0 y0) (x1 y1) ...
                    int x1 = 0, y1 = 0;
                    if (!parseDefPoint(tokens, i, x0, y0, x1, y1, consumed)) {
                        break;
                    }
                    i += consumed;

                    if (!currentLayerName.empty() &&
                        currentRouteWidthDbu > 0) {
                        addStripeFromSegment(currentNetName,
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
                            addStripeFromSegment(currentNetName,
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
                    addViaInstance(currentNetName, viaName, x0, y0);
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
                        addStripeFromSegment(currentNetName,
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
                            addStripeFromSegment(currentNetName,
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
                currentLayerName = tokens[i++];
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
//             if (mPinParseState.isPowerOrGround && mPinParseState.hasLocation
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

    // Start of a new component: "- <instName> <macroName> ..."
    if (tokens[0] == "-") {
        // Finalize previous component (if any)
        if (!currentInstName.empty() && !currentMacroName.empty()) {
            addTsvInstance(
              currentInstName, currentMacroName, currentX, currentY);
        }

        currentInstName.clear();
        currentMacroName.clear();
        currentX = currentY = 0;

        if (tokens.size() >= 3) {
            currentInstName  = stripDefQuotes(tokens[1]);
            currentMacroName = stripDefQuotes(tokens[2]);
        }
        return;
    }

    // Look for "+ FIXED ( x y )" or "+ PLACED ( x y )"
    std::size_t       i = 0;
    const std::size_t n = tokens.size();
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
// Stripe accumulation per (net,layer)
// -----------------------------------------------------------------------------

void CoarsePdnBuilder3D::addStripeRectangle(const std::string& netName,
                                            const std::string& layerName,
                                            int x0Dbu, int y0Dbu, int x1Dbu,
                                            int y1Dbu) {
    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) return;
    int netIndex = netIt->second.index;

    auto layerIdxIt = mLayerNameToIndex.find(IdString::tryLookup(layerName));
    if (layerIdxIt == mLayerNameToIndex.end()) return;
    int layerIndex = layerIdxIt->second;

    const TechLayer* layer = mTechDb.getLayer(IdString::tryLookup(layerName));
    if (!layer) return;

    // Convert to µm
    double x0 = static_cast<double>(x0Dbu) / mDbuPerMicron;
    double y0 = static_cast<double>(y0Dbu) / mDbuPerMicron;
    double x1 = static_cast<double>(x1Dbu) / mDbuPerMicron;
    double y1 = static_cast<double>(y1Dbu) / mDbuPerMicron;

    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);

    double widthX = x1 - x0;
    double widthY = y1 - y0;
    if (widthX <= 0.0 || widthY <= 0.0) return;

    bool isHorizontal = (widthX >= widthY);

    ConductanceGrid2D& grid = mInPlaneGrids[netIndex][layerIndex];
    if (isHorizontal) {
        accumulateHorizontalStripe(grid, *layer, x0, y0, x1, y1);
    } else {
        accumulateVerticalStripe(grid, *layer, x0, y0, x1, y1);
    }
}

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
    int jStart =
      clamp(static_cast<int>(std::floor((y0 - grid.yMin) / dy)), 0, ny - 1);
    int jEnd =
      clamp(static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 0, ny - 1);

    // Vertical boundaries [kStart..kEnd] where xMin + k*dx in [x0,x1], 1
    // <= k <= nx-1
    int kStart =
      clamp(static_cast<int>(std::ceil((x0 - grid.xMin) / dx)), 1, nx - 1);
    int kEnd =
      clamp(static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 1, nx - 1);

    if (jEnd < jStart || kEnd < kStart) return;

    double segmentLength = dx;
    double G_perSegment  = 1.0 / (RL * segmentLength); // Siemens

    for (int j = jStart; j <= jEnd; ++j) {
        for (int k = kStart; k <= kEnd; ++k) {
            int ix = k - 1;
            grid.gx(ix, j) += G_perSegment;
        }
    }
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
    int iStart =
      clamp(static_cast<int>(std::floor((x0 - grid.xMin) / dx)), 0, nx - 1);
    int iEnd =
      clamp(static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 0, nx - 1);

    // Horizontal boundaries [lStart..lEnd], 1 <= l <= ny-1
    int lStart =
      clamp(static_cast<int>(std::ceil((y0 - grid.yMin) / dy)), 1, ny - 1);
    int lEnd =
      clamp(static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 1, ny - 1);

    if (iEnd < iStart || lEnd < lStart) return;

    double segmentLength = dy;
    double G_perSegment  = 1.0 / (RL * segmentLength); // Siemens

    for (int i = iStart; i <= iEnd; ++i) {
        for (int l = lStart; l <= lEnd; ++l) {
            int iy = l - 1;
            grid.gy(i, iy) += G_perSegment;
        }
    }
}

// -----------------------------------------------------------------------------
// Via accumulation
// -----------------------------------------------------------------------------

void CoarsePdnBuilder3D::addViaInstance(const std::string& netName,
                                        const std::string& viaName, int xDbu,
                                        int yDbu) {
    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) return;
    int netIndex = netIt->second.index;

    const TechVia* via = mTechDb.getVia(IdString::tryLookup(viaName));
    if (!via) return;

    auto blIt = mLayerNameToIndex.find(via->bottomLayer);
    auto tlIt = mLayerNameToIndex.find(via->topLayer);
    if (blIt == mLayerNameToIndex.end() || tlIt == mLayerNameToIndex.end())
        return;

    int lb = blIt->second;
    int lt = tlIt->second;

    if (lb < 0 || lb >= mNumLayers || lt < 0 || lt >= mNumLayers) return;
    if (mInPlaneGrids.empty()) return;

    ViaGrid3D& vg = getOrCreateViaGrid(netIndex, lb, lt);

    double x_um = static_cast<double>(xDbu) / mDbuPerMicron;
    double y_um = static_cast<double>(yDbu) / mDbuPerMicron;

    // Use the per-layer grids’ geometry assuming all grids share die bbox
    const ConductanceGrid2D& gb = mInPlaneGrids[netIndex][lb];
    const ConductanceGrid2D& gt = mInPlaneGrids[netIndex][lt];

    const int bIx =
      clamp(static_cast<int>((x_um - gb.xMin) / gb.dx), 0, gb.nx - 1);
    const int bIy =
      clamp(static_cast<int>((y_um - gb.yMin) / gb.dy), 0, gb.ny - 1);

    const int tIx =
      clamp(static_cast<int>((x_um - gt.xMin) / gt.dx), 0, gt.nx - 1);
    const int tIy =
      clamp(static_cast<int>((y_um - gt.yMin) / gt.dy), 0, gt.ny - 1);

    if (via->resistance <= 0.0) return;

    vg.addConductance(bIx, bIy, tIx, tIy, 1.0 / via->resistance);
}

// Get the "VDD_PERI_MEDIA1" part out of U_TSV_PG_Power1_VDD_PERI_MEDIA1_0
static std::string extractNetNameFromInstance(const std::string& instName,
                                              const std::string& macroName) {
    // The pattern is: U_{macro}_{net}_X (where X is usually a number)

    // Remove "U_" prefix
    if (instName.substr(0, 2) != "U_") {
        return "";
    }

    // Find the position after the macro name
    std::string macroPattern = macroName;
    std::transform(macroPattern.begin(),
                   macroPattern.end(),
                   macroPattern.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string instLower = instName.substr(2); // Skip "U_"
    std::transform(instLower.begin(),
                   instLower.end(),
                   instLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Find macro name in the instance (case-insensitive)
    size_t macroPos = instLower.find(macroPattern);
    if (macroPos == std::string::npos) {
        return "";
    }

    // Position after macro name
    size_t afterMacro = macroPos + macroPattern.length();

    // Skip underscores after macro name
    while (afterMacro < instName.length() - 2 && instName[afterMacro] == '_') {
        afterMacro++;
    }

    // Find the last underscore before the trailing number
    size_t lastUnderscore = instName.rfind('_');
    if (lastUnderscore <= afterMacro) {
        return "";
    }

    // Extract net name
    return instName.substr(afterMacro, lastUnderscore - afterMacro);
}

// For example:
// U_TSV_PG_Power1_VDD_PERI_MEDIA1_0 TSV_PG_POWER1 100 1993000
void CoarsePdnBuilder3D::addTsvInstance(const std::string& instName,
                                        const std::string& macroName, int xDbu,
                                        int yDbu) {
    const TechTsv* tsv = mTechDb.getTsv(IdString::tryLookup(macroName));
    if (!tsv) return; // Not a TSV

    if (xDbu < 0 || yDbu < 0) {
        PDN_WARN("Found a TSV without coordinates. Skip.");
    }

    std::string netName = extractNetNameFromInstance(instName, macroName);
    if (netName.empty()) {
        PDN_WARN("Failed to extract net name from instance: %s",
                 instName.c_str());
        return;
    }

    auto netIt = mNetByName.find(IdString::tryLookup(netName));
    if (netIt == mNetByName.end()) return;
    int netIndex = netIt->second.index;

    auto blIt = mLayerNameToIndex.find(tsv->bottomLayer);
    auto tlIt = mLayerNameToIndex.find(tsv->topLayer);
    if (blIt == mLayerNameToIndex.end() || tlIt == mLayerNameToIndex.end())
        return;

    int lb = blIt->second;
    int lt = tlIt->second;

    if (mInPlaneGrids.empty() || lb < 0 || lb >= mNumLayers) return;

    ViaGrid3D& vg = getOrCreateViaGrid(netIndex, lb, lt);

    double x_um = static_cast<double>(xDbu) / mDbuPerMicron;
    double y_um = static_cast<double>(yDbu) / mDbuPerMicron;

        // Use the per-layer grids’ geometry assuming all grids share die bbox
        const ConductanceGrid2D& gb = mInPlaneGrids[netIndex][lb];
        const ConductanceGrid2D& gt = mInPlaneGrids[netIndex][lt];
    
        const int bIx =
          clamp(static_cast<int>((x_um - gb.xMin) / gb.dx), 0, gb.nx - 1);
        const int bIy =
          clamp(static_cast<int>((y_um - gb.yMin) / gb.dy), 0, gb.ny - 1);
    
        const int tIx =
          clamp(static_cast<int>((x_um - gt.xMin) / gt.dx), 0, gt.nx - 1);
        const int tIy =
          clamp(static_cast<int>((y_um - gt.yMin) / gt.dy), 0, gt.ny - 1);
    
        if (tsv->resistance <= 0.0) return;
    
        vg.addConductance(bIx, bIy, tIx, tIy, 1.0 / tsv->resistance);
}

ViaGrid3D& CoarsePdnBuilder3D::getOrCreateViaGrid(int netIndex, int lb,
                                                  int lt) {
    std::uint64_t key = makeViaKey(netIndex, lb, lt);
    auto          it  = mViaGridLookup.find(key);
    if (it != mViaGridLookup.end()) {
        return mViaGrids[it->second];
    }

    const LayerGridResolution& bRes = mLayerGridRes[lb];
    const LayerGridResolution& tRes = mLayerGridRes[lt];

    ViaGrid3D vg;
    vg.init(netIndex, lb, lt, bRes.nx, bRes.ny, tRes.nx, tRes.ny);

    int idx = static_cast<int>(mViaGrids.size());
    mViaGrids.push_back(std::move(vg));
    mViaGridLookup[key] = idx;
    return mViaGrids.back();
}

std::uint64_t CoarsePdnBuilder3D::makeViaKey(int netIndex, int lb, int lt) {
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

void CoarsePdnBuilder3D::buildCircuitGraph(CircuitGraph& graph) {
    graph.mCoordinateUnit = CircuitGraph::UM;

    // tileNodeIds[netIndex][layerIndex] is a 2D id-grid with its own nx/ny
    std::vector<std::vector<TileNodeIdGrid>> tileNodeIds;
    tileNodeIds.resize(static_cast<std::size_t>(mNumNets));
    for (int n = 0; n < mNumNets; ++n) {
        tileNodeIds[n].resize(static_cast<std::size_t>(mNumLayers));
        for (int l = 0; l < mNumLayers; ++l) {
            const ConductanceGrid2D& grid = mInPlaneGrids[n][l];
            tileNodeIds[n][l].init(grid.nx, grid.ny);
        }
    }

    // Register PDN nets per layer
    for (int l = 0; l < mNumLayers; ++l) {
        IdString layerName = mLayerOrder[l];
        for (int n = 0; n < mNumNets; ++n) {
            IdString netName  = mNetByIndex[n].name;
            bool     isPower  = mNetByIndex[n].isPower;
            bool     isGround = mNetByIndex[n].isGround;
            graph.registerNet(layerName, netName, isPower, isGround);
        }
    }

    // 1) Create per-(net,layer,tile) nodes and in-plane metal resistors
    for (int n = 0; n < mNumNets; ++n) {
        const NetInfo& ni = mNetByIndex[n];

        for (int l = 0; l < mNumLayers; ++l) {
            IdString           layerName = mLayerOrder[l];
            ConductanceGrid2D& grid      = mInPlaneGrids[n][l];

            const int nx = grid.nx;
            const int ny = grid.ny;

            const int netLayerCode = encodeNetLayer(n, l);

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
                    node.mName = nodeId;
                    node.mNet  = NetId(netLayerCode);
                    node.mX    = FPN::toRep(xCenterUm);
                    node.mY    = FPN::toRep(yCenterUm);

                    graph.mNodes.emplace(nodeId, node);
                    tileNodeIds[n][l].at(ix, iy) = nodeId;
                }
            }

            // 1.b) Horizontal resistors (Gx)
            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx - 1; ++ix) {
                    double G = grid.gx(ix, iy);
                    if (G <= 0.0) continue;

                    double   R  = 1.0 / G;
                    IdString n1 = tileNodeIds[n][l].at(ix, iy);
                    IdString n2 = tileNodeIds[n][l].at(ix + 1, iy);

                    std::ostringstream ossResName;
                    ossResName << ni.name.str() << "_" << layerName.str()
                               << "_RH_" << ix << "_" << iy;
                    IdString resId = IdString(ossResName.str());

                    MetalRes mr;
                    mr.mName = resId;
                    mr.mNet  = NetId(netLayerCode);
                    mr.mN1   = n1;
                    mr.mN2   = n2;
                    mr.mR    = R;
                    graph.mMetalResistors.push_back(std::move(mr));
                }
            }

            // 1.c) Vertical resistors (Gy)
            for (int iy = 0; iy < ny - 1; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    double G = grid.gy(ix, iy);
                    if (G <= 0.0) continue;

                    double   R  = 1.0 / G;
                    IdString n1 = tileNodeIds[n][l].at(ix, iy);
                    IdString n2 = tileNodeIds[n][l].at(ix, iy + 1);

                    std::ostringstream ossResName;
                    ossResName << ni.name.str() << "_" << layerName.str()
                               << "_RV_" << ix << "_" << iy;
                    IdString resId = IdString(ossResName.str());

                    MetalRes mr;
                    mr.mName = resId;
                    mr.mNet  = NetId(netLayerCode);
                    mr.mN1   = n1;
                    mr.mN2   = n2;
                    mr.mR    = R;
                    graph.mMetalResistors.push_back(std::move(mr));
                }
            }
        }
    }

    // 2) Create ViaRes from aggregated via grids (includes TSVs if added
    // via addTsvInstance)
    int viaCounter = 0;
    for (const ViaGrid3D& vg : mViaGrids) {
        const int netIndex = vg.netIndex;
        const int lb       = vg.bottomLayerIdx;
        const int lt       = vg.topLayerIdx;

        const NetInfo& ni     = mNetByIndex[netIndex];
        IdString       blName = mLayerOrder[lb];
        IdString       tlName = mLayerOrder[lt];

        for (const auto& kv : vg.edgeG) {
            const std::uint64_t edgeKey = kv.first;
            const double        G       = kv.second;

            if (G <= 0.0) continue;

            const double R = 1.0 / G;

            std::uint32_t bFlat = 0, tFlat = 0;
            ViaGrid3D::unpackEdge(edgeKey, bFlat, tFlat);

            const int bIx = static_cast<int>(
              bFlat % static_cast<std::uint32_t>(vg.bottomNx));
            const int bIy = static_cast<int>(
              bFlat / static_cast<std::uint32_t>(vg.bottomNx));
            const int tIx =
              static_cast<int>(tFlat % static_cast<std::uint32_t>(vg.topNx));
            const int tIy =
              static_cast<int>(tFlat / static_cast<std::uint32_t>(vg.topNx));

            IdString bottomId = tileNodeIds[netIndex][lb].at(bIx, bIy);
            IdString topId    = tileNodeIds[netIndex][lt].at(tIx, tIy);

            std::ostringstream ossName;
            ossName << ni.name.str() << "_VIA_" << blName.str() << "_to_"
                    << tlName.str() << "_B_" << bIx << "_" << bIy << "_T_"
                    << tIx << "_" << tIy << "_" << viaCounter++;

            ViaRes vr;
            vr.mName = IdString(ossName.str());
            vr.mN1   = bottomId;
            vr.mN2   = topId;
            vr.mR    = R;
            graph.mViaResistors.push_back(std::move(vr));
        }
    }

    // 3) Create bump nodes and package resistors
    // int bumpCounter = 0;
    // for (const Bump& b : mBumps) {
    //     auto netIt = mNetByName.find(b.netName);
    //     if (netIt == mNetByName.end()) continue;

    //     const NetInfo& ni       = netIt->second;
    //     int            netIndex = ni.index;

    //     ConductanceGrid2D& attachGrid =
    //       mInPlaneGrids[netIndex][mBumpLayerIndex];
    //     const int nx = attachGrid.nx;
    //     const int ny = attachGrid.ny;

    //     int ix =
    //       clamp(static_cast<int>((b.x_um - attachGrid.xMin) /
    //       attachGrid.dx),
    //             0,
    //             nx - 1);
    //     int iy =
    //       clamp(static_cast<int>((b.y_um - attachGrid.yMin) /
    //       attachGrid.dy),
    //             0,
    //             ny - 1);

    //     IdString tileId = tileNodeIds[netIndex][mBumpLayerIndex]
    //                                  [static_cast<std::size_t>(iy * nx +
    //                                  ix)];

    //     // Bump node
    //     std::ostringstream ossBumpName;
    //     ossBumpName << ni.name.str() << "_BUMP_" << bumpCounter;
    //     IdString bumpNodeId(ossBumpName.str());

    //     int netLayerCode = encodeNetLayer(netIndex, mBumpLayerIndex);

    //     Node bumpNode;
    //     bumpNode.mName = bumpNodeId;
    //     bumpNode.mNet  = NetId(netLayerCode);
    //     bumpNode.mX    = FPN::toRep(b.x_um);
    //     bumpNode.mY    = FPN::toRep(b.y_um);
    //     graph.mNodes.emplace(bumpNodeId, bumpNode);

    //     // Package resistor
    //     std::ostringstream ossPkgName;
    //     ossPkgName << ni.name.str() << "_PKG_" << bumpCounter;
    //     IdString pkgId(ossPkgName.str());

    //     PkgRes pr;
    //     pr.mName = pkgId;
    //     pr.mN1   = bumpNodeId;
    //     pr.mN2   = tileId;
    //     pr.mR    = mDefaultPkgR;
    //     graph.mPkgResistors.push_back(std::move(pr));

    //     ++bumpCounter;
    // }
}

// -----------------------------------------------------------------------------
// Rect & VIA token parsing helpers (currently unused, but kept for future use)
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
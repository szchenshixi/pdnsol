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

#include "pdnsol/io/parser_utils.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
// Strip optional DEF double quotes from a name token, e.g. "VDD" -> VDD.
static inline std::string stripDefQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Robust integer parser for DEF coordinate tokens.
static inline bool parseIntSafe(const std::string& s, int& out) {
    try {
        size_t pos = 0;
        int    v   = std::stoi(s, &pos);
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
static inline std::vector<std::string> tokenizeDef(const std::string& s) {
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

// Keeps context while parsing routed SPECIALNETS (layer, width, shape, last
// point)
struct SpecialNetRouteState {
    bool inRoute        = false; // inside a ROUTED/NEW/FIXED/COVER statement
    bool prevPointValid = false; // we have a previous (x,y) to connect from
    int  widthDbu       = 0;     // current routing width in DBU, if specified
    std::string shape;           // e.g. "STRIPE", "FOLLOWPIN"
    int         prevX = 0;       // previous routed point (DBU)
    int         prevY = 0;
};

// -----------------------------------------------------------------------------
// Technology database for metal layers, vias, and TSVs
// -----------------------------------------------------------------------------

struct TechLayer {
    IdString name;
    double   resistivity; // Ω·µm (Ohm * micron)
    double   thickness;   // µm
};

struct TechVia {
    IdString name;
    IdString bottomLayer;
    IdString topLayer;
    double   resistance; // Ohms per via instance (or per cut)
};

struct TechTsv {
    IdString name;
    IdString bottomLayer;
    IdString topLayer;
    double   resistance; // Ohms per TSV
};

struct TechViaGeom {
    IdString name;
    IdString viaRuleName;

    IdString bottomLayer;
    IdString cutLayer;
    IdString topLayer;

    // All geometry values are in DEF DBU units
    int cutSizeX         = 0;
    int cutSizeY         = 0;
    int cutSpacingX      = 0;
    int cutSpacingY      = 0;
    int enclosureBottomX = 0; // overhang on bottom layer, x-direction
    int enclosureBottomY = 0; // overhang on bottom layer, y-direction
    int enclosureTopX    = 0; // overhang on top layer, x-direction
    int enclosureTopY    = 0; // overhang on top layer, y-direction
    int rows             = 1; // ROWCOL
    int cols             = 1; // ROWCOL
};

class TechDatabase {
  public:
    // Metal layers
    void addLayer(std::string_view name, double resistivity_ohm_um,
                  double thickness_um) {
        IdString  nameId(name);
        TechLayer layer{nameId, resistivity_ohm_um, thickness_um};
        mLayers[nameId] = layer;
    }

    const TechLayer* getLayer(IdString name) const {
        auto it = mLayers.find(name);
        if (it == mLayers.end()) return nullptr;
        return &it->second;
    }

    // Vias
    void addVia(std::string_view viaName, std::string_view bottomLayer,
                std::string_view topLayer, double resistance_ohm) {
        IdString viaNameId(viaName);
        IdString bottomLayerId(bottomLayer);
        IdString topLayerId(topLayer);
        TechVia  v{viaNameId, bottomLayerId, topLayerId, resistance_ohm};
        mVias[viaNameId] = v;
    }

    const TechVia* getVia(IdString viaName) const {
        auto it = mVias.find(viaName);
        if (it == mVias.end()) return nullptr;
        return &it->second;
    }

    // TSVs (extension point)
    void addTsv(std::string_view tsvName, std::string_view bottomLayer,
                std::string_view topLayer, double resistance_ohm) {
        IdString tsvNameId(tsvName);
        IdString bottomLayerId(bottomLayer);
        IdString topLayerId(topLayer);
        TechTsv  t{tsvNameId, bottomLayerId, topLayerId, resistance_ohm};
        mTsvs[tsvNameId] = t;
    }

    const TechTsv* getTsv(IdString tsvName) const {
        auto it = mTsvs.find(tsvName);
        if (it == mTsvs.end()) return nullptr;
        return &it->second;
    }

    // Via geometry from DEF "VIAS" section
    void addViaGeometryFromDef(
      std::string_view viaName, std::string_view viaRuleName,
      std::string_view bottomLayer, std::string_view cutLayer,
      std::string_view topLayer, int cutSizeX, int cutSizeY, int cutSpacingX,
      int cutSpacingY, int enclosureBottomX, int enclosureBottomY,
      int enclosureTopX, int enclosureTopY, int rows, int cols) {
        IdString viaId(viaName);

        TechViaGeom g;
        g.name             = viaId;
        g.viaRuleName      = IdString(viaRuleName);
        g.bottomLayer      = IdString(bottomLayer);
        g.cutLayer         = IdString(cutLayer);
        g.topLayer         = IdString(topLayer);
        g.cutSizeX         = cutSizeX;
        g.cutSizeY         = cutSizeY;
        g.cutSpacingX      = cutSpacingX;
        g.cutSpacingY      = cutSpacingY;
        g.enclosureBottomX = enclosureBottomX;
        g.enclosureBottomY = enclosureBottomY;
        g.enclosureTopX    = enclosureTopX;
        g.enclosureTopY    = enclosureTopY;
        g.rows             = rows;
        g.cols             = cols;

        mViaGeometries[viaId] = g;

        // Make sure basic via connectivity exists as TechVia as well.
        auto it = mVias.find(viaId);
        if (it == mVias.end()) {
            TechVia v;
            v.name        = viaId;
            v.bottomLayer = g.bottomLayer;
            v.topLayer    = g.topLayer;
            v.resistance  = 0.0; // placeholder, can be filled separately
            mVias.emplace(viaId, v);
        } else {
            // Keep connectivity consistent if already present
            it->second.bottomLayer = g.bottomLayer;
            it->second.topLayer    = g.topLayer;
        }
    }

    const TechViaGeom* getViaGeometry(IdString viaName) const {
        auto it = mViaGeometries.find(viaName);
        if (it == mViaGeometries.end()) return nullptr;
        return &it->second;
    }

    // TSV geometry extension point (minimal, but allows you to fill later)
    struct TechTsvGeom {
        IdString name;
        double   diameter_um = 0.0;
        double   height_um   = 0.0;
    };

    void addTsvGeometry(std::string_view tsvName, double diameter_um,
                        double height_um) {
        IdString    id(tsvName);
        TechTsvGeom g;
        g.name             = id;
        g.diameter_um      = diameter_um;
        g.height_um        = height_um;
        mTsvGeometries[id] = g;
    }

    const TechTsvGeom* getTsvGeometry(IdString tsvName) const {
        auto it = mTsvGeometries.find(IdString(tsvName));
        if (it == mTsvGeometries.end()) return nullptr;
        return &it->second;
    }

  private:
    std::unordered_map<IdString, TechLayer, IdString::Hash> mLayers;
    std::unordered_map<IdString, TechVia, IdString::Hash>   mVias;
    std::unordered_map<IdString, TechTsv, IdString::Hash>   mTsvs;

    std::unordered_map<IdString, TechViaGeom, IdString::Hash> mViaGeometries;
    std::unordered_map<IdString, TechTsvGeom, IdString::Hash> mTsvGeometries;
};

// -----------------------------------------------------------------------------
// 2D conductance grid for in-plane metal
// -----------------------------------------------------------------------------

struct ConductanceGrid2D {
    int    nx   = 0;
    int    ny   = 0;
    double xMin = 0.0;
    double yMin = 0.0;
    double dx   = 1.0;
    double dy   = 1.0;

    // Gx[ix,iy] : conductance between (ix,iy) and (ix+1,iy),
    // ix in [0..nx-2], iy in [0..ny-1]
    // Gy[ix,iy] : conductance between (ix,iy) and (ix,iy+1),
    // ix in [0..nx-1], iy in [0..ny-2]
    std::vector<double> Gx;
    std::vector<double> Gy;

    void init(int nx_, int ny_, double xMin_, double xMax_, double yMin_,
              double yMax_) {
        nx   = nx_;
        ny   = ny_;
        xMin = xMin_;
        yMin = yMin_;
        dx   = (xMax_ - xMin_) / static_cast<double>(nx);
        dy   = (yMax_ - yMin_) / static_cast<double>(ny);
        Gx.assign((nx - 1) * ny, 0.0);
        Gy.assign(nx * (ny - 1), 0.0);
    }

    inline double& gx(int ix, int iy) { return Gx[ix + (nx - 1) * iy]; }

    inline double& gy(int ix, int iy) { return Gy[ix + nx * iy]; }

    inline const double& gx(int ix, int iy) const {
        return Gx[ix + (nx - 1) * iy];
    }

    inline const double& gy(int ix, int iy) const { return Gy[ix + nx * iy]; }
};

// -----------------------------------------------------------------------------
// Vertical conductance grid for via/TSV aggregation
// -----------------------------------------------------------------------------

struct ViaGrid3D {
    int                 netIndex       = -1;
    int                 bottomLayerIdx = -1;
    int                 topLayerIdx    = -1;
    int                 nx             = 0;
    int                 ny             = 0;
    // G_tile[ix,iy]: total conductance between (bottomLayerIdx, ix,iy)
    // and (topLayerIdx, ix,iy) for this net
    std::vector<double> G; // size = nx*ny

    void init(int netIdx, int lb, int lt, int nx_, int ny_) {
        netIndex       = netIdx;
        bottomLayerIdx = lb;
        topLayerIdx    = lt;
        nx             = nx_;
        ny             = ny_;
        G.assign(static_cast<size_t>(nx * ny), 0.0);
    }

    inline double& g(int ix, int iy) { return G[iy * nx + ix]; }

    inline const double& g(int ix, int iy) const { return G[iy * nx + ix]; }
};

// -----------------------------------------------------------------------------
// Net and bump representations
// -----------------------------------------------------------------------------

struct NetInfo {
    int      index = -1; // small integer (0..numNets-1)
    IdString name;
    bool     isPower  = false;
    bool     isGround = false;
};

struct Bump {
    IdString netName;
    double   x_um = 0.0;
    double   y_um = 0.0;
};

// -----------------------------------------------------------------------------
// CoarsePdnBuilder3D
// -----------------------------------------------------------------------------

class CoarsePdnBuilder3D {
  public:
    CoarsePdnBuilder3D(TechDatabase& techDb, int gridNx, int gridNy,
                       const std::vector<std::string>& powerNetNames,
                       const std::vector<std::string>& groundNetNames,
                       const std::vector<std::string>& layerOrder,
                       double defaultPkgResistanceOhm = 0.0,
                       int    bumpLayerIndex          = -1)
        : mTechDb(techDb)
        , mGridNx(gridNx)
        , mGridNy(gridNy)
        , mDefaultPkgR(defaultPkgResistanceOhm) {
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
        mLayerOrder.reserve(mLayerOrder.size());
        for (size_t i = 0; i < layerOrder.size(); ++i) {
            IdString layerId = IdString(layerOrder[i]);
            mLayerOrder.push_back(layerId);
            mLayerNameToIndex[layerId] = static_cast<int>(i);
        }
        mNumLayers = static_cast<int>(mLayerOrder.size());

        mNumNetLayerComb = mNumNets * mNumLayers;

        // 3) Which layer do bumps connect to? default: topmost
        if (bumpLayerIndex < 0) {
            mBumpLayerIndex = mNumLayers > 0 ? (mNumLayers - 1) : 0;
        } else {
            mBumpLayerIndex = clamp(bumpLayerIndex, 0, mNumLayers - 1);
        }
    }

    // Encode (netIndex, layerIndex) into Node.mNet and MetalRes.mNet
    int encodeNetLayer(int netIndex, int layerIndex) const {
        return layerIndex * mNumNets + netIndex;
    }

    // Main entry point
    bool buildCoarsePdnFromDef(const std::string& defPath,
                               CircuitGraph&      outGraph) {
        // First pass: parse units and die area
        if (!parseDefGeometry(defPath)) {
            std::cerr << "ERROR: Failed to parse geometry from DEF: "
                      << defPath << "\n";
            return false;
        }

        // Initialize all per-(net,layer) conductance grids
        initInPlaneGrids();

        // Second pass: parse PDN stripes, vias, and bumps
        if (!parseDefPdnAndBumps(defPath)) {
            std::cerr << "ERROR: Failed to parse PDN & bumps from DEF: "
                      << defPath << "\n";
            return false;
        }

        // Build CircuitGraph from all grids
        buildCircuitGraph(outGraph);
        return true;
    }

    // ---------------------------------------------------------------------
    // TSV extension hook:
    //   You can call this from external TSV parsing code (not from DEF).
    //   It will reuse the same vertical conductance accumulation as vias.
    // ---------------------------------------------------------------------
    void addTsvInstance(const std::string& netName, const std::string& tsvName,
                        double x_um, double y_um) {
        auto netIt = mNetByName.find(IdString::tryLookup(netName));
        if (netIt == mNetByName.end()) return;
        int netIndex = netIt->second.index;

        const TechTsv* tsv = mTechDb.getTsv(IdString::tryLookup(tsvName));
        if (!tsv) return;

        auto blIt = mLayerNameToIndex.find(tsv->bottomLayer);
        auto tlIt = mLayerNameToIndex.find(tsv->topLayer);
        if (blIt == mLayerNameToIndex.end() || tlIt == mLayerNameToIndex.end())
            return;

        int lb = blIt->second;
        int lt = tlIt->second;

        if (mInPlaneGrids.empty() || lb < 0 || lb >= mNumLayers) return;

        const ConductanceGrid2D& gridRef = mInPlaneGrids[netIndex][lb];
        const int                nx      = gridRef.nx;
        const int                ny      = gridRef.ny;

        int ix = clamp(
          static_cast<int>((x_um - gridRef.xMin) / gridRef.dx), 0, nx - 1);
        int iy = clamp(
          static_cast<int>((y_um - gridRef.yMin) / gridRef.dy), 0, ny - 1);

        ViaGrid3D& vg = getOrCreateViaGrid(netIndex, lb, lt);

        if (tsv->resistance <= 0.0) return;

        vg.g(ix, iy) += 1.0 / tsv->resistance;
    }

  private:
    // ---------------------------------------------------------------------
    // DEF parsing: geometry (UNITS, DIEAREA)
    // ---------------------------------------------------------------------
    bool parseDefGeometry(const std::string& defPath) {
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

    // ---------------------------------------------------------------------
    // Initialize in-plane grids for each (net, layer)
    // ---------------------------------------------------------------------
    void initInPlaneGrids() {
        mInPlaneGrids.assign(mNumNets,
                             std::vector<ConductanceGrid2D>(mNumLayers));

        for (int n = 0; n < mNumNets; ++n) {
            for (int l = 0; l < mNumLayers; ++l) {
                mInPlaneGrids[n][l].init(mGridNx,
                                         mGridNy,
                                         mDieXMinUm,
                                         mDieXMaxUm,
                                         mDieYMinUm,
                                         mDieYMaxUm);
            }
        }
    }

    // ---------------------------------------------------------------------
    // Helpers for SPECIALNETS routing
    // ---------------------------------------------------------------------

    // Parse a DEF point: "( x y )"
    // Supports '*' meaning "same as previous value" on that axis.
    static bool parseDefPoint(const std::vector<std::string>& tokens,
                              size_t startIdx, int prevX, int prevY, int& x,
                              int& y, size_t& consumed) {
        consumed       = 0;
        const size_t n = tokens.size();
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
    void addStripeFromSegment(const std::string& netName,
                              const std::string& layerName, int x0, int y0,
                              int x1, int y1, int widthDbu) {
        if (widthDbu <= 0)
            return; // degenerate: used e.g. for via points on met4 0

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

    // ---------------------------------------------------------------------
    // DEF parsing: PDN stripes, vias, and bumps
    // ---------------------------------------------------------------------
    enum class Section { NONE, SPECIALNETS, PINS, VIAS };

    bool parseDefPdnAndBumps(const std::string& defPath) {
        std::ifstream fin(defPath);
        if (!fin) {
            std::cerr << "ERROR: Cannot open DEF file: " << defPath << "\n";
            return false;
        }

        Section section = Section::NONE;

        std::string currentNetName;
        bool        currentNetIsPdn = false;
        std::string currentLayerName; // updated by "ROUTED <layer>"
        int currentRouteWidthDbu = 0; // NEW: track width from "ROUTED/NEW"

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

            // NEW: VIAS section
            if (startsWithIgnoreCase(tline, "VIAS")) {
                section = Section::VIAS;
                continue;
            }
            if (startsWithIgnoreCase(tline, "END VIAS")) {
                section = Section::NONE;
                continue;
            }

            if (startsWithIgnoreCase(tline, "PINS")) {
                section = Section::PINS;
                mPinParseState.reset();
                continue;
            }
            if (startsWithIgnoreCase(tline, "END PINS")) {
                section = Section::NONE;
                mPinParseState.reset();
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
            case Section::PINS: handlePinsLine(tline); break;
            case Section::VIAS: handleViasLine(tline); break;
            default: break;
            }
        }

        return true;
    }

    void handleSpecialNetsLine(const std::string& line,
                               std::string&       currentNetName,
                               bool&              currentNetIsPdn,
                               std::string&       currentLayerName,
                               int&               currentRouteWidthDbu) {
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

        size_t       i = 0;
        const size_t n = tokens.size();

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
                    int    x0 = 0, y0 = 0;
                    size_t consumed = 0;
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
                        if (!parseDefPoint(
                              tokens, i, x0, y0, x1, y1, consumed)) {
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

                    // Geometry comes right after shape type
                    if (i >= n || tokens[i] != "(") {
                        continue;
                    }

                    // First point
                    int    x0 = 0, y0 = 0;
                    size_t consumed = 0;
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
                        if (!parseDefPoint(
                              tokens, i, x0, y0, x1, y1, consumed)) {
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

    void handlePinsLine(const std::string& line) {
        std::vector<std::string> tokens = tokenizeDef(line);
        if (tokens.empty()) return;

        size_t       i = 0;
        const size_t n = tokens.size();

        // Start of a new PIN definition
        if (!mPinParseState.inPin) {
            if (tokens[0] != "-") {
                // Ignore lines until we see "- pinName"
                return;
            }
            if (n < 2) {
                return;
            }
            mPinParseState.reset();
            mPinParseState.inPin   = true;
            mPinParseState.pinName = IdString(stripDefQuotes(tokens[1]));
            i = 2; // Continue parsing remaining tokens on this line
        }

        for (; i < n; ++i) {
            const std::string& tok = tokens[i];

            if (tok == ";") {
                // End of this PIN definition
                if (mPinParseState.isPowerOrGround &&
                    mPinParseState.hasLocation &&
                    mPinParseState.netName.valid()) {
                    auto it = mNetByName.find(mPinParseState.netName);
                    if (it != mNetByName.end()) {
                        Bump b;
                        b.netName = mPinParseState.netName;
                        b.x_um    = static_cast<double>(mPinParseState.xDbu) /
                                 mDbuPerMicron;
                        b.y_um = static_cast<double>(mPinParseState.yDbu) /
                                 mDbuPerMicron;
                        mBumps.push_back(std::move(b));
                    }
                }
                mPinParseState.reset();
                break;
            } else if (tok == "NET") {
                if (i + 1 < n) {
                    mPinParseState.netName =
                      IdString(stripDefQuotes(tokens[++i]));
                    // If the net is in our PDN set, treat as power/ground
                    auto it = mNetByName.find(mPinParseState.netName);
                    if (it != mNetByName.end()) {
                        mPinParseState.isPowerOrGround = true;
                    }
                }
            } else if (tok == "USE") {
                if (i + 1 < n) {
                    const std::string& useType = tokens[++i];
                    if (useType == "POWER" || useType == "GROUND") {
                        mPinParseState.isPowerOrGround = true;
                    }
                }
            } else if (tok == "PLACED" || tok == "FIXED") {
                // Look ahead for "( x y )"
                int    x = 0, y = 0;
                bool   found = false;
                size_t j     = i + 1;
                for (; j + 3 < n; ++j) {
                    if (tokens[j] == "(") {
                        if (parseIntSafe(tokens[j + 1], x) &&
                            parseIntSafe(tokens[j + 2], y)) {
                            found = true;
                        }
                        break;
                    }
                }
                if (found) {
                    mPinParseState.xDbu        = x;
                    mPinParseState.yDbu        = y;
                    mPinParseState.hasLocation = true;
                }
            } else {
                // Ignore other tokens (DIRECTION, LAYER, PORT, '+', etc.)
                continue;
            }
        }
    }

    void handleViasLine(const std::string& line) {
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

        size_t       i = 2;
        const size_t n = tokens.size();

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

    // ---------------------------------------------------------------------
    // Stripe accumulation per (net,layer)
    // ---------------------------------------------------------------------
    void addStripeRectangle(const std::string& netName,
                            const std::string& layerName, int x0Dbu, int y0Dbu,
                            int x1Dbu, int y1Dbu) {
        auto netIt = mNetByName.find(IdString::tryLookup(netName));
        if (netIt == mNetByName.end()) return;
        int netIndex = netIt->second.index;

        auto layerIdxIt =
          mLayerNameToIndex.find(IdString::tryLookup(layerName));
        if (layerIdxIt == mLayerNameToIndex.end()) return;
        int layerIndex = layerIdxIt->second;

        const TechLayer* layer =
          mTechDb.getLayer(IdString::tryLookup(layerName));
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

    void accumulateHorizontalStripe(ConductanceGrid2D& grid,
                                    const TechLayer& layer, double x0,
                                    double y0, double x1, double y1) {
        double stripeWidth = y1 - y0; // µm
        if (stripeWidth <= 0.0) return;

        // Ohm/µm
        double RL =
          layer.resistivity / (layer.thickness * stripeWidth + 1e-30);

        const int    nx = grid.nx;
        const int    ny = grid.ny;
        const double dx = grid.dx;
        const double dy = grid.dy;

        // Rows [jStart..jEnd] where band intersects [y0,y1]
        int jStart = clamp(
          static_cast<int>(std::floor((y0 - grid.yMin) / dy)), 0, ny - 1);
        int jEnd = clamp(
          static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 0, ny - 1);

        // Vertical boundaries [kStart..kEnd] where xMin + k*dx in [x0,x1], 1
        // <= k <= nx-1
        int kStart =
          clamp(static_cast<int>(std::ceil((x0 - grid.xMin) / dx)), 1, nx - 1);
        int kEnd = clamp(
          static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 1, nx - 1);

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

    void accumulateVerticalStripe(ConductanceGrid2D& grid,
                                  const TechLayer& layer, double x0, double y0,
                                  double x1, double y1) {
        double stripeWidth = x1 - x0; // µm
        if (stripeWidth <= 0.0) return;

        double RL = layer.resistivity /
                    (layer.thickness * stripeWidth + 1e-30); // Ohm/µm

        const int    nx = grid.nx;
        const int    ny = grid.ny;
        const double dx = grid.dx;
        const double dy = grid.dy;

        // Columns [iStart..iEnd]
        int iStart = clamp(
          static_cast<int>(std::floor((x0 - grid.xMin) / dx)), 0, nx - 1);
        int iEnd = clamp(
          static_cast<int>(std::floor((x1 - grid.xMin) / dx)), 0, nx - 1);

        // Horizontal boundaries [lStart..lEnd], 1 <= l <= ny-1
        int lStart =
          clamp(static_cast<int>(std::ceil((y0 - grid.yMin) / dy)), 1, ny - 1);
        int lEnd = clamp(
          static_cast<int>(std::floor((y1 - grid.yMin) / dy)), 1, ny - 1);

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

    // ---------------------------------------------------------------------
    // Via accumulation
    // ---------------------------------------------------------------------
    void addViaInstance(const std::string& netName, const std::string& viaName,
                        int xDbu, int yDbu) {
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

        // Use the in-plane grid for the bottom layer to map coordinates
        const ConductanceGrid2D& gridRef = mInPlaneGrids[netIndex][lb];
        const int                nx      = gridRef.nx;
        const int                ny      = gridRef.ny;

        double x_um = static_cast<double>(xDbu) / mDbuPerMicron;
        double y_um = static_cast<double>(yDbu) / mDbuPerMicron;

        int ix = clamp(
          static_cast<int>((x_um - gridRef.xMin) / gridRef.dx), 0, nx - 1);
        int iy = clamp(
          static_cast<int>((y_um - gridRef.yMin) / gridRef.dy), 0, ny - 1);

        ViaGrid3D& vg = getOrCreateViaGrid(netIndex, lb, lt);

        if (via->resistance <= 0.0) {
            // Avoid division by zero; if you want ideal vias, use a very small
            // R
            return;
        }

        vg.g(ix, iy) += 1.0 / via->resistance;
    }

    ViaGrid3D& getOrCreateViaGrid(int netIndex, int lb, int lt) {
        uint64_t key = makeViaKey(netIndex, lb, lt);
        auto     it  = mViaGridLookup.find(key);
        if (it != mViaGridLookup.end()) {
            return mViaGrids[it->second];
        }

        ViaGrid3D vg;
        vg.init(netIndex, lb, lt, mGridNx, mGridNy);

        int idx = static_cast<int>(mViaGrids.size());
        mViaGrids.push_back(std::move(vg));
        mViaGridLookup[key] = idx;
        return mViaGrids.back();
    }

    static uint64_t makeViaKey(int netIndex, int lb, int lt) {
        // Pack (netIndex, lb, lt) into 64 bits (assuming < 65536 of each)
        uint64_t k = 0;
        k |= (static_cast<uint64_t>(netIndex) & 0xFFFFu) << 32;
        k |= (static_cast<uint64_t>(lb) & 0xFFFFu) << 16;
        k |= (static_cast<uint64_t>(lt) & 0xFFFFu);
        return k;
    }

    // ---------------------------------------------------------------------
    // CircuitGraph construction
    // ---------------------------------------------------------------------
    void buildCircuitGraph(CircuitGraph& graph) {
        graph.mCoordinateUnit = CircuitGraph::UM;

        // tileNodeIds[netIndex][layerIndex][iy * nx + ix]
        std::vector<std::vector<std::vector<IdString>>> tileNodeIds;
        tileNodeIds.resize(mNumNets);
        for (int n = 0; n < mNumNets; ++n) {
            tileNodeIds[n].resize(mNumLayers);
            for (int l = 0; l < mNumLayers; ++l) {
                tileNodeIds[n][l].resize(
                  static_cast<size_t>(mGridNx * mGridNy));
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
                        ossName << ni.name.str() << "_" << layerName.str()
                                << "_T_" << ix << "_" << iy;

                        IdString nodeId = IdString(ossName.str());
                        Node     node;
                        node.mName = nodeId;
                        node.mNet  = netLayerCode;
                        node.mX = static_cast<Tick>(std::llround(xCenterUm));
                        node.mY = static_cast<Tick>(std::llround(yCenterUm));

                        graph.mNodes.emplace(nodeId, node);
                        tileNodeIds[n][l][static_cast<size_t>(iy * nx + ix)] =
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
                          tileNodeIds[n][l][static_cast<size_t>(iy * nx + ix)];
                        IdString n2 =
                          tileNodeIds[n][l]
                                     [static_cast<size_t>(iy * nx + (ix + 1))];

                        std::ostringstream ossResName;
                        ossResName << ni.name.str() << "_" << layerName.str()
                                   << "_RH_" << ix << "_" << iy;
                        IdString resId = IdString(ossResName.str());

                        MetalRes mr;
                        mr.mName = resId;
                        mr.mNet  = netLayerCode;
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

                        double   R = 1.0 / G;
                        IdString n1 =
                          tileNodeIds[n][l][static_cast<size_t>(iy * nx + ix)];
                        IdString n2 =
                          tileNodeIds[n][l]
                                     [static_cast<size_t>((iy + 1) * nx + ix)];

                        std::ostringstream ossResName;
                        ossResName << ni.name.str() << "_" << layerName.str()
                                   << "_RV_" << ix << "_" << iy;
                        IdString resId = IdString(ossResName.str());

                        MetalRes mr;
                        mr.mName = resId;
                        mr.mNet  = netLayerCode;
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
            const int nx       = vg.nx;
            const int ny       = vg.ny;

            const NetInfo& ni     = mNetByIndex[netIndex];
            IdString       blName = mLayerOrder[lb];
            IdString       tlName = mLayerOrder[lt];

            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    double G = vg.g(ix, iy);
                    if (G <= 0.0) continue;

                    double R = 1.0 / G;

                    IdString bottomId =
                      tileNodeIds[netIndex][lb]
                                 [static_cast<size_t>(iy * nx + ix)];
                    IdString topId =
                      tileNodeIds[netIndex][lt]
                                 [static_cast<size_t>(iy * nx + ix)];

                    std::ostringstream ossName;
                    ossName << ni.name.str() << "_VIA_" << blName.str()
                            << "_to_" << tlName.str() << "_T_" << ix << "_"
                            << iy << "_" << viaCounter++;
                    IdString viaId = IdString(ossName.str());

                    ViaRes vr;
                    vr.mName = viaId;
                    vr.mN1   = bottomId;
                    vr.mN2   = topId;
                    vr.mR    = R;
                    graph.mViaResistors.push_back(std::move(vr));
                }
            }
        }

        // 3) Create bump nodes and package resistors
        int bumpCounter = 0;
        for (const Bump& b : mBumps) {
            auto netIt = mNetByName.find(b.netName);
            if (netIt == mNetByName.end()) continue;

            const NetInfo& ni       = netIt->second;
            int            netIndex = ni.index;

            ConductanceGrid2D& attachGrid =
              mInPlaneGrids[netIndex][mBumpLayerIndex];
            const int nx = attachGrid.nx;
            const int ny = attachGrid.ny;

            int ix = clamp(
              static_cast<int>((b.x_um - attachGrid.xMin) / attachGrid.dx),
              0,
              nx - 1);
            int iy = clamp(
              static_cast<int>((b.y_um - attachGrid.yMin) / attachGrid.dy),
              0,
              ny - 1);

            IdString tileId = tileNodeIds[netIndex][mBumpLayerIndex]
                                         [static_cast<size_t>(iy * nx + ix)];

            // Bump node
            std::ostringstream ossBumpName;
            ossBumpName << ni.name.str() << "_BUMP_" << bumpCounter;
            IdString bumpNodeId(ossBumpName.str());

            int netLayerCode = encodeNetLayer(netIndex, mBumpLayerIndex);

            Node bumpNode;
            bumpNode.mName = bumpNodeId;
            bumpNode.mNet  = netLayerCode;
            bumpNode.mX    = static_cast<Tick>(std::llround(b.x_um));
            bumpNode.mY    = static_cast<Tick>(std::llround(b.y_um));
            graph.mNodes.emplace(bumpNodeId, bumpNode);

            // Package resistor
            std::ostringstream ossPkgName;
            ossPkgName << ni.name.str() << "_PKG_" << bumpCounter;
            IdString pkgId(ossPkgName.str());

            PkgRes pr;
            pr.mName = pkgId;
            pr.mN1   = bumpNodeId;
            pr.mN2   = tileId;
            pr.mR    = mDefaultPkgR;
            graph.mPkgResistors.push_back(std::move(pr));

            ++bumpCounter;
        }
    }

    // Parse RECT coordinates from token stream.
    // Supports:
    //   RECT x0 y0 x1 y1
    //   RECT ( x0 y0 ) ( x1 y1 )
    // startIdx points to the token immediately after "RECT".
    // On success, 'consumed' is the number of tokens eaten starting at
    // startIdx.
    bool parseRectFromTokens(const std::vector<std::string>& tokens,
                             size_t startIdx, int& x0, int& y0, int& x1,
                             int& y1, size_t& consumed) const {
        consumed       = 0;
        const size_t n = tokens.size();
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

    // Parse VIA placement from token stream.
    // Supports:
    //   VIA viaName x y
    //   VIA viaName ( x y )
    // startIdx points to the token immediately after "VIA".
    bool parseViaFromTokens(const std::vector<std::string>& tokens,
                            size_t startIdx, std::string& viaName, int& x,
                            int& y, size_t& consumed) const {
        consumed       = 0;
        const size_t n = tokens.size();
        if (startIdx >= n) return false;

        viaName    = stripDefQuotes(tokens[startIdx]);
        size_t idx = startIdx + 1;
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

    // ---------------------------------------------------------------------
    // Net registration helper
    // ---------------------------------------------------------------------
    void addNetIfAbsent(const std::string& name, bool isPower, bool isGround) {
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

  private:
    // Inputs
    TechDatabase& mTechDb;
    int           mGridNx      = 0;
    int           mGridNy      = 0;
    double        mDefaultPkgR = 0.0;

    // PDN nets
    std::unordered_map<IdString, NetInfo, IdString::Hash> mNetByName;
    std::vector<NetInfo>                                  mNetByIndex;
    int                                                   mNumNets = 0;

    // PDN layers
    std::vector<IdString>                             mLayerOrder;
    std::unordered_map<IdString, int, IdString::Hash> mLayerNameToIndex;
    int                                               mNumLayers = 0;

    int mNumNetLayerComb = 0;

    // Bump attaches to this layer index (default: topmost)
    int mBumpLayerIndex = 0;

    // DEF geometry
    double mDbuPerMicron = 1.0;
    double mDieXMinUm    = 0.0;
    double mDieYMinUm    = 0.0;
    double mDieXMaxUm    = 0.0;
    double mDieYMaxUm    = 0.0;

    // In-plane conductance grids: [netIndex][layerIndex]
    std::vector<std::vector<ConductanceGrid2D>> mInPlaneGrids;

    // Vertical via/TSV grids
    std::vector<ViaGrid3D>            mViaGrids;
    std::unordered_map<uint64_t, int> mViaGridLookup;

    // Bumps from PINS section
    std::vector<Bump> mBumps;

    // State for parsing multi-line PINS entries.
    struct PinParseState {
        IdString pinName;
        IdString netName;
        bool     isPowerOrGround = false;
        bool     hasLocation     = false;
        int      xDbu            = 0;
        int      yDbu            = 0;
        bool     inPin           = false;

        void reset() {
            pinName         = IdString();
            netName         = IdString();
            isPowerOrGround = false;
            hasLocation     = false;
            xDbu            = 0;
            yDbu            = 0;
            inPin           = false;
        }
    };
    // PINS parsing state (multi-line support)
    PinParseState mPinParseState;
};
} // namespace pdnsol
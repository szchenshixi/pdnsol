#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pdnsol/common.hpp"
#include "pdnsol/io/tech_db.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {

// -----------------------------------------------------------------------------
// Small helper functions for DEF parsing (declared here, implemented in .cpp)
// -----------------------------------------------------------------------------

// Strip optional DEF double quotes from a name token, e.g. "VDD" -> VDD.
std::string stripDefQuotes(const std::string& s);

// Robust integer parser for DEF coordinate tokens.
bool parseIntSafe(const std::string& s, int& out);

// Simple tokenizer for DEF lines.
// Splits on whitespace and also makes '(', ')', ';', '+' separate tokens.
std::vector<std::string> tokenizeDef(const std::string& s);

// -----------------------------------------------------------------------------
// Optional routing context helper (currently unused but kept as utility type)
// -----------------------------------------------------------------------------

// Keeps context while parsing routed SPECIALNETS (layer, width, shape, last
// point)
struct SpecialNetRouteState {
    bool inRoute        = false; // inside a ROUTED/NEW/FIXED/COVER statement
    bool prevPointValid = false; // we have a previous (x,y) to connect from
    int  widthDbu       = 0;     // current routing width in DBU, if specified
    int  prevX          = 0;     // previous routed point (DBU)
    int  prevY          = 0;
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
              double yMax_);

    inline double&       gx(int ix, int iy);
    inline double&       gy(int ix, int iy);
    inline const double& gx(int ix, int iy) const;
    inline const double& gy(int ix, int iy) const;
};

// -----------------------------------------------------------------------------
// Vertical conductance grid for via/TSV aggregation
// -----------------------------------------------------------------------------

struct ViaGrid3D {
    int netIndex       = -1;
    int bottomLayerIdx = -1;
    int topLayerIdx    = -1;

    // Dimensions of the bottom/top layer grids for this via connection.
    int nxB = 0, nyB = 0; // bottom layer grid dims
    int nxT = 0, nyT = 0; // top layer grid dims

    // key = (bottomFlat << 32) | topFlat
    // value = total conductance (Siemens) between those two tiles, before
    // scaling.
    std::unordered_map<std::uint64_t, double> edgeG;

    void init(int netIdx, int lb, int lt, int nxB, int nyB, int nxT, int nyT);

    static std::uint64_t packEdge(std::uint32_t flatB, std::uint32_t flatT) {
        return (static_cast<std::uint64_t>(flatB) << 32) |
               static_cast<std::uint64_t>(flatT);
    }

    static void unpackEdge(std::uint64_t key, std::uint32_t& flatB,
                           std::uint32_t& flatT) {
        flatB = static_cast<std::uint32_t>(key >> 32);
        flatT = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
    }

    void addConductance(int ixB, int iyB, int ixT, int iyT, double dG);

    // G[ixB, iyB, ixT, iyT] for this net:
    // Total conductance between (bottomLayerIdx,ix,iy) and (topLayerIdx,ix,iy)
    const double& g(int ixB, int iyB, int ixT, int iyT) const;
    double&       g(int ixB, int iyB, int ixT, int iyT);
};

// -----------------------------------------------------------------------------
// Net and layer settings
// -----------------------------------------------------------------------------

struct NetInfo {
    int      index = -1; // small integer (0..numNets-1)
    IdString name;
    bool     isPower  = false;
    bool     isGround = false;
};

struct LayerGridResolution {
    int sx = -1; // Tile size in X direction (um)
    int sy = -1; // Tile size in Y direction (um)
    int nx = -1; // Computed tile count in X direction
    int ny = -1; // Computed tile count in Y direction
};

// -----------------------------------------------------------------------------
// CoarsePdnBuilder3D
// -----------------------------------------------------------------------------
// Temporary records for metal stripes and vias
struct StripeRec {
    int netIndex   = -1;
    int layerIndex = -1;
    int x0 = 0, y0 = 0; // normalized DBU
    int x1 = 0, y1 = 0; // x0<=x1, y0<=y1
};

struct ViaRec {
    int      netIndex = -1;
    int      x = 0, y = 0; // DBU
    IdString viaName;

    int lb = -1; // bottom layer idx (resolved from tech via)
    int lt = -1; // top layer idx

    double g = 0.0; // conductance = 1/R
};

struct StripeBins2D {
    int                           nx = 0, ny = 0;
    std::vector<std::vector<int>> bins; // bins[iy*nx+ix] = stripe IDs

    void init(int nx_, int ny_) {
        nx = nx_;
        ny = ny_;
        bins.assign(nx * ny, {});
    }

    inline std::vector<int>& at(int ix, int iy) { return bins[iy * nx + ix]; }
    inline const std::vector<int>& at(int ix, int iy) const {
        return bins[iy * nx + ix];
    }
};

struct StripeDerived {
    StripeRec raw;
    bool      isHorizontal = true;

    // Representative indices on THIS (net,layer) grid:
    // horizontal stripe -> repIy meaningful
    // vertical stripe   -> repIx meaningful
    int repIx = -1;
    int repIy = -1;
};

class CoarsePdnBuilder3D {
  public:
    CoarsePdnBuilder3D(TechDatabase& techDb, LayerGridResolution defaultRes,
                       const IdString::Map<LayerGridResolution>& perLayerRes,
                       const std::vector<std::string>&           powerNetNames,
                       const std::vector<std::string>& groundNetNames,
                       const std::vector<std::string>& layerOrder);

    // Encode (netIndex, layerIndex) into Node.mNet and MetalRes.mNet
    int encodeNetLayer(int netIndex, int layerIndex) const;

    // Main entry point
    bool buildCoarsePdnFromDef(const std::string& defPath,
                               CircuitGraph&      outGraph);

  private:
    // -----------------------------------------------------------------
    // DEF parsing: geometry (UNITS, DIEAREA)
    // -----------------------------------------------------------------
    bool parseDefGeometry(const std::string& defPath);

    // -----------------------------------------------------------------
    // Initialize in-plane grids for each (net,layer)
    // -----------------------------------------------------------------
    void initInPlaneGrids();

    // Clear per-build state so each call to buildCoarsePdnFromDef()
    // starts with a clean slate.
    void resetForNewBuild();

    // -----------------------------------------------------------------
    // Helpers for SPECIALNETS routing
    // -----------------------------------------------------------------

    // Parse a DEF point: "( x y )"
    // Supports '*' meaning "same as previous value" on that axis.
    static bool parseDefPoint(const std::vector<std::string>& tokens,
                              std::size_t startIdx, int prevX, int prevY,
                              int& x, int& y, std::size_t& consumed);

    // Convert a routed segment into a rectangle and feed it into the existing
    // rectangle-based PDN builder. widthDbu is the wire width from
    // "ROUTED/NEW" (in DBU).
    void recordStripeFromSegment(const std::string& netName,
                                 const std::string& layerName, int x0, int y0,
                                 int x1, int y1, int widthDbu);

    // -----------------------------------------------------------------
    // DEF parsing: PDN stripes, vias, and bumps
    // -----------------------------------------------------------------
    enum class Section { NONE, SPECIALNETS, VIAS, COMPONENTS };

    bool parseDefPdnAndBumps(const std::string& defPath);

    void handleSpecialNetsLine(const std::string& line,
                               std::string&       currentNetName,
                               bool&              currentNetIsPdn,
                               std::string&       currentLayerName,
                               int&               currentRouteWidthDbu);
    // void handlePinsLine(const std::string& line);
    void handleViasLine(const std::string& line);
    void handleComponentsLine(const std::string& line,
                              std::string&       currentInstName,
                              std::string& currentMacroName, int& currentX,
                              int& currentY);

    // -----------------------------------------------------------------
    // Stripe accumulation per (net,layer)
    // -----------------------------------------------------------------

    void recordStripeRectangle(const std::string& netName,
                               const std::string& layerName, int x0Dbu,
                               int y0Dbu, int x1Dbu, int y1Dbu);
    // void addStripeRectangle(const std::string& netName,
    //                         const std::string& layerName, int x0Dbu, int
    //                         y0Dbu, int x1Dbu, int y1Dbu);

    void accumulateHorizontalStripe(ConductanceGrid2D& grid,
                                    const TechLayer& layer, double x0,
                                    double y0, double x1, double y1);

    void accumulateVerticalStripe(ConductanceGrid2D& grid,
                                  const TechLayer& layer, double x0, double y0,
                                  double x1, double y1);

    // -----------------------------------------------------------------
    // Via accumulation
    // -----------------------------------------------------------------
    void recordViaInstance(const std::string& netName,
                           const std::string& viaName, int xDbu, int yDbu);
    // void addViaInstance(const std::string& netName, const std::string&
    // viaName,
    //                     int xDbu, int yDbu);
    // void addTsvInstance(const std::string& instName,
    //                     const std::string& macroName, int xDbu, int yDbu);
    void recordTsvInstance(const std::string& instName,
                           const std::string& macroName, int xDbu, int yDbu);

    ViaGrid3D& getOrCreateViaGrid(int netIndex, int lb, int lt);

    static std::uint64_t makeViaKey(int netIndex, int lb, int lt);

    // -----------------------------------------------------------------
    // CircuitGraph construction
    // -----------------------------------------------------------------

    void finalizeRecordedPdnGeometry();
    void buildCircuitGraph(CircuitGraph& graph);

    // Parse RECT coordinates from token stream.
    // Supports:
    //   RECT x0 y0 x1 y1
    //   RECT ( x0 y0 ) ( x1 y1 )
    // startIdx points to the token immediately after "RECT".
    // On success, 'consumed' is the number of tokens eaten starting at
    // startIdx.
    bool parseRectFromTokens(const std::vector<std::string>& tokens,
                             std::size_t startIdx, int& x0, int& y0, int& x1,
                             int& y1, std::size_t& consumed) const;

    // Parse VIA placement from token stream.
    // Supports:
    //   VIA viaName x y
    //   VIA viaName ( x y )
    // startIdx points to the token immediately after "VIA".
    bool parseViaFromTokens(const std::vector<std::string>& tokens,
                            std::size_t startIdx, std::string& viaName, int& x,
                            int& y, std::size_t& consumed) const;

    // -----------------------------------------------------------------
    // Net registration helper
    // -----------------------------------------------------------------
    void addNetIfAbsent(const std::string& name, bool isPower, bool isGround);

  private:
    // Inputs
    TechDatabase&       mTechDb;
    LayerGridResolution mDefaultRes;

    // PDN nets
    IdString::Map<NetInfo> mNetByName;
    std::vector<NetInfo>   mNetByIndex;
    int                    mNumNets = 0;

    // PDN layers
    std::vector<IdString>            mLayerOrder;
    // (layer index -> grid resolution)
    std::vector<LayerGridResolution> mLayerGridRes;
    IdString::Map<int>               mLayerNameToIndex;
    int                              mNumLayers = 0;

    int mNumNetLayerComb = 0;

    // DEF geometry
    double mDbuPerMicron = 1.0;
    double mDieXMinUm    = 0.0;
    double mDieYMinUm    = 0.0;
    double mDieXMaxUm    = 0.0;
    double mDieYMaxUm    = 0.0;

    // In-plane conductance grids: [netIndex][layerIndex]
    std::vector<std::vector<ConductanceGrid2D>> mInPlaneGrids;

    // Vertical via/TSV grids
    std::vector<ViaGrid3D>                 mViaGrids;
    std::unordered_map<std::uint64_t, int> mViaGridLookup;

    std::vector<StripeRec> mRecordedStripes;
    std::vector<ViaRec>    mRecordedVias;
};

// Inline accessors for small structs
inline double& ConductanceGrid2D::gx(int ix, int iy) {
    return Gx[static_cast<std::size_t>(ix + (nx - 1) * iy)];
}

inline double& ConductanceGrid2D::gy(int ix, int iy) {
    return Gy[static_cast<std::size_t>(ix + nx * iy)];
}

inline const double& ConductanceGrid2D::gx(int ix, int iy) const {
    return Gx[static_cast<std::size_t>(ix + (nx - 1) * iy)];
}

inline const double& ConductanceGrid2D::gy(int ix, int iy) const {
    return Gy[static_cast<std::size_t>(ix + nx * iy)];
}

inline double& ViaGrid3D::g(int ixB, int iyB, int ixT, int iyT) {
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

    return edgeG.at(packEdge(flatB, flatT));
}

inline const double& ViaGrid3D::g(int ixB, int iyB, int ixT, int iyT) const {
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

    return edgeG.at(packEdge(flatB, flatT));
}

} // namespace pdnsol
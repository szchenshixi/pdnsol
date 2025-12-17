#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"

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

// TSV geometry extension point (minimal, but allows you to fill later)
// struct TechTsvGeom {
//     IdString name;
//     double   diameter_um = 0.0;
//     double   height_um   = 0.0;
// };

class TechDatabase {
  public:
    // Metal layers
    void addLayer(std::string_view name, double resistivity_ohm_um,
                  double thickness_um);

    const TechLayer* getLayer(IdString name) const;

    // Vias
    void addVia(std::string_view viaName, std::string_view bottomLayer,
                std::string_view topLayer, double resistance_ohm);

    const TechVia* getVia(IdString viaName) const;

    // TSVs
    void addTsv(std::string_view tsvName, std::string_view bottomLayer,
                std::string_view topLayer, double resistance_ohm);

    const TechTsv* getTsv(IdString tsvName) const;

    // Via geometry from DEF "VIAS" section
    void addViaGeometryFromDef(
      std::string_view viaName, std::string_view viaRuleName,
      std::string_view bottomLayer, std::string_view cutLayer,
      std::string_view topLayer, int cutSizeX, int cutSizeY, int cutSpacingX,
      int cutSpacingY, int enclosureBottomX, int enclosureBottomY,
      int enclosureTopX, int enclosureTopY, int rows, int cols);

    const TechViaGeom* getViaGeometry(IdString viaName) const;

    // void addTsvGeometry(std::string_view tsvName, double diameter_um,
    //                     double height_um);

    // const TechTsvGeom* getTsvGeometry(IdString tsvName) const;

  private:
    std::unordered_map<IdString, TechLayer, IdString::Hash> mLayers;
    std::unordered_map<IdString, TechVia, IdString::Hash>   mVias;
    std::unordered_map<IdString, TechTsv, IdString::Hash>   mTsvs;

    std::unordered_map<IdString, TechViaGeom, IdString::Hash> mViaGeometries;
    // std::unordered_map<IdString, TechTsvGeom, IdString::Hash>
    // mTsvGeometries;
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
    int                 netIndex       = -1;
    int                 bottomLayerIdx = -1;
    int                 topLayerIdx    = -1;
    int                 nx             = 0;
    int                 ny             = 0;
    // G[ix,iy]: total conductance between (bottomLayerIdx, ix,iy)
    // and (topLayerIdx, ix,iy) for this net
    std::vector<double> G; // size = nx*ny

    void init(int netIdx, int lb, int lt, int nx_, int ny_);

    inline double&       g(int ix, int iy);
    inline const double& g(int ix, int iy) const;
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
                       const std::string&              bumpLayerName = "");

    // Encode (netIndex, layerIndex) into Node.mNet and MetalRes.mNet
    int encodeNetLayer(int netIndex, int layerIndex) const;

    // Main entry point
    bool buildCoarsePdnFromDef(const std::string& defPath,
                               CircuitGraph&      outGraph);

    //   You can call this from external TSV parsing code (not from DEF).
    //   It will reuse the same vertical conductance accumulation as vias.
    void addTsvInstance(const std::string& netName, const std::string& tsvName,
                        double x_um, double y_um);

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

    // Convert a routed segment into a rectangle and feed it into the
    // existing rectangle-based PDN builder.
    // widthDbu is the wire width from "ROUTED/NEW" (in DBU).
    void addStripeFromSegment(const std::string& netName,
                              const std::string& layerName, int x0, int y0,
                              int x1, int y1, int widthDbu);

    // -----------------------------------------------------------------
    // DEF parsing: PDN stripes, vias, and bumps
    // -----------------------------------------------------------------
    enum class Section { NONE, SPECIALNETS, PINS, VIAS, COMPONENTS };

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
    void addStripeRectangle(const std::string& netName,
                            const std::string& layerName, int x0Dbu, int y0Dbu,
                            int x1Dbu, int y1Dbu);

    void accumulateHorizontalStripe(ConductanceGrid2D& grid,
                                    const TechLayer& layer, double x0,
                                    double y0, double x1, double y1);

    void accumulateVerticalStripe(ConductanceGrid2D& grid,
                                  const TechLayer& layer, double x0, double y0,
                                  double x1, double y1);

    // -----------------------------------------------------------------
    // Via accumulation
    // -----------------------------------------------------------------
    void addViaInstance(const std::string& netName, const std::string& viaName,
                        int xDbu, int yDbu);
    void addTsvInstance(const std::string& instName,
                        const std::string& macroName, int xDbu, int yDbu);

    ViaGrid3D& getOrCreateViaGrid(int netIndex, int lb, int lt);

    static std::uint64_t makeViaKey(int netIndex, int lb, int lt);

    // -----------------------------------------------------------------
    // CircuitGraph construction
    // -----------------------------------------------------------------
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

    // Bumps from PINS section
    // std::vector<Bump> mBumps;

    // State for parsing multi-line PINS entries.
    // struct PinParseState {
    //     IdString pinName;
    //     IdString netName;
    //     bool     isPowerOrGround = false;
    //     bool     hasLocation     = false;
    //     int      xDbu            = 0;
    //     int      yDbu            = 0;
    //     bool     inPin           = false;

    //     void reset() {
    //         pinName         = IdString();
    //         netName         = IdString();
    //         isPowerOrGround = false;
    //         hasLocation     = false;
    //         xDbu            = 0;
    //         yDbu            = 0;
    //         inPin           = false;
    //     }
    // };

    // PINS parsing state (multi-line support)
    // PinParseState mPinParseState;

    // struct ComponentParseState {
    //     std::string instName;  // instance name, e.g. "U_TSV_PG_0"
    //     std::string macroName; // master name, e.g. "TSV_PG_Power1"
    //     bool isTsv = false;    // whether this component is a TSV we care
    //     about

    //     void reset() {
    //         instName.clear();
    //         macroName.clear();
    //         isTsv = false;
    //     }
    // };

    // ComponentParseState mComponentParseState;
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

inline double& ViaGrid3D::g(int ix, int iy) {
    return G[static_cast<std::size_t>(iy * nx + ix)];
}

inline const double& ViaGrid3D::g(int ix, int iy) const {
    return G[static_cast<std::size_t>(iy * nx + ix)];
}

} // namespace pdnsol
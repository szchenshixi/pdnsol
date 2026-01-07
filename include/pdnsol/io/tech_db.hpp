#pragma once

#include <string_view>

#include "pdnsol/common.hpp"

namespace pdnsol {

// -----------------------------------------------------------------------------
// Technology database for metal layers, vias, and TSVs
// -----------------------------------------------------------------------------

struct TechLayer {
    IdString   name;
    ScalarType resistivity; // Ω·µm (Ohm * micron)
    ScalarType thickness;   // µm
};

struct TechVia {
    IdString   name;
    IdString   bottomLayer;
    IdString   topLayer;
    ScalarType resistance; // Ohms per via instance (or per cut)
};

struct TechTsv {
    IdString   name;
    IdString   bottomLayer;
    IdString   topLayer;
    ScalarType resistance; // Ohms per TSV
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
    // Alias to std::unordered_map<IdString, TechLayer, IdString::Hash>
    IdString::Map<TechLayer> mLayers;
    IdString::Map<TechVia>   mVias;
    IdString::Map<TechTsv>   mTsvs;

    IdString::Map<TechViaGeom> mViaGeometries; // Reserved for future use
};
} // namespace pdnsol
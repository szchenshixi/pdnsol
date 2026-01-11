#include "pdnsol/io/tech_db.hpp"

namespace pdnsol {
// -----------------------------------------------------------------------------
// TechDatabase implementation
// -----------------------------------------------------------------------------

// Metal layers
void TechDatabase::addLayer(std::string_view name, double resistivity_ohm_um,
                            double thickness_um) {
    IdString  nameId(name);
    TechLayer layer{nameId, resistivity_ohm_um, thickness_um};
    mLayers[nameId] = layer;
}

const TechLayer* TechDatabase::getLayer(IdString name) const {
    auto it = mLayers.find(name);
    if (it == mLayers.end()) return nullptr;
    return &it->second;
}

// Vias
void TechDatabase::addVia(std::string_view viaName,
                          std::string_view bottomLayer,
                          std::string_view topLayer, double resistance_ohm) {
    IdString viaNameId(viaName);
    IdString bottomLayerId(bottomLayer);
    IdString topLayerId(topLayer);
    TechVia  v{viaNameId, bottomLayerId, topLayerId, resistance_ohm};
    mVias[viaNameId] = v;
}

const TechVia* TechDatabase::getVia(IdString viaName) const {
    auto it = mVias.find(viaName);
    if (it == mVias.end()) return nullptr;
    return &it->second;
}

// TSVs
void TechDatabase::addTsv(std::string_view tsvName,
                          std::string_view bottomLayer,
                          std::string_view topLayer, double resistance_ohm) {
    IdString tsvNameId(tsvName);
    IdString bottomLayerId(bottomLayer);
    IdString topLayerId(topLayer);
    TechTsv  t{tsvNameId, bottomLayerId, topLayerId, resistance_ohm};
    mTsvs[tsvNameId] = t;
}

const TechTsv* TechDatabase::getTsv(IdString tsvName) const {
    auto it = mTsvs.find(tsvName);
    if (it == mTsvs.end()) return nullptr;
    return &it->second;
}

// Via geometry from DEF "VIAS" section
void TechDatabase::addViaGeometryFromDef(
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

    // Make sure basic via connectivity exists as TechVia as well
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

const TechViaGeom* TechDatabase::getViaGeometry(IdString viaName) const {
    auto it = mViaGeometries.find(viaName);
    if (it == mViaGeometries.end()) return nullptr;
    return &it->second;
}

// TSV geometry
// void TechDatabase::addTsvGeometry(std::string_view tsvName, double
// diameter_um,
//                                   double height_um) {
//     IdString    id(tsvName);
//     TechTsvGeom g;
//     g.name             = id;
//     g.diameter_um      = diameter_um;
//     g.height_um        = height_um;
//     mTsvGeometries[id] = g;
// }

// const TechDatabase::TechTsvGeom*
// TechDatabase::getTsvGeometry(IdString tsvName) const {
//     auto it = mTsvGeometries.find(tsvName);
//     if (it == mTsvGeometries.end()) return nullptr;
//     return &it->second;
// }

} // namespace pdnsol
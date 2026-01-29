#pragma once

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/struct/net_filter.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
class CircuitDecorator {
    struct RectRegion {
        Tick xMin;
        Tick xMax;
        Tick yMin;
        Tick yMax;
    };
    struct CurrentRegion {
        RectRegion                rect;
        IdString                  layer;
        // net name -> total current in Amp
        IdString::Map<ScalarType> netCurrents;
    };

    struct VoltageRegion {
        int32_t netId;
        Tick    x;
        Tick    y;
    };

  public:
    CircuitDecorator(CircuitGraph& inGraph, const DieConfig& dieConfig,
                     const NetFilter& netFilter = NetFilter{});
    void build();

  private:
    CircuitGraph&    mIn;
    const DieConfig& mDieConfig;
    NetFilter        mNetFilter;

    // -------------------------------------------------------------------------
    // High-Level Entries
    // -------------------------------------------------------------------------
    void attachCurrents(CircuitGraph& graph, const std::string& jsonFilePath);

    // -------------------------------------------------------------------------
    // Insertions
    // -------------------------------------------------------------------------
    void addCurrentRegionsFromJson(const std::string& jsonFilePath,
                                   CircuitGraph&      graph);
    void addVoltageSourceFromConfig(const std::string& configFilePath,
                                    CircuitGraph&      graph);
    void addIsrcsForRegionNet(const RectRegion& rect, IdString layer,
                              IdString netName, double totalCurrent,
                              CircuitGraph& graph, std::size_t regionIdx);

    // -------------------------------------------------------------------------
    // Parsers
    // -------------------------------------------------------------------------
    std::vector<CurrentRegion>
    parseCurrentRegionsFromJson(const std::string& jsonFilePath);
};

} // namespace pdnsol
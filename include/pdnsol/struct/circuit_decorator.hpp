#pragma once

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {
using Json = nlohmann::json;

struct DecoratorConfig {
    std::string currentConfigPath;
    std::string voltageConfigPath;
};

class CircuitDecorator {
    struct RectRegion {
        Tick xMin;
        Tick xMax;
        Tick yMin;
        Tick yMax;
    };
    struct CurrentRegion {
        RectRegion rect;
        IdString   layer;
        DoubleMap  netCurrents; // net name -> total current in Amp
    };

    struct VoltageRegion {
        int32_t netId;
        Tick    x;
        Tick    y;
    };

  public:
    CircuitDecorator(CircuitGraph& inGraph, const DecoratorConfig& cfg);
    void build();

  private:
    CircuitGraph&   mIn;
    DecoratorConfig mCfg;

    // -------------------------------------------------------------------------
    // High-Level Entries
    // -------------------------------------------------------------------------
    void attachCurrents(CircuitGraph& graph, const std::string& jsonFilePath);

    // -------------------------------------------------------------------------
    // Insertions
    // -------------------------------------------------------------------------
    void addCurrentRegionsFromJson(const std::string& jsonFilePath,
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
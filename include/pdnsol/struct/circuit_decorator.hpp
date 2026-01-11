#pragma once

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/struct/net_filter.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
struct DecoratorConfig {
    struct VSrcProperty {
        ScalarType voltage;  // Volt
        ScalarType packageR; // Ohm
    };
    std::string                                   currentConfigPath;
    std::string                                   voltageConfigPath;
    std::string                                   voltageSourceLandingLayer;
    std::unordered_map<std::string, VSrcProperty> voltageSources;
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
        IdString::Map<ScalarType>
          netCurrents; // net name -> total current in Amp
    };

    struct VoltageRegion {
        int32_t netId;
        Tick    x;
        Tick    y;
    };

  public:
    CircuitDecorator(CircuitGraph& inGraph, const DecoratorConfig& cfg,
                     const NetFilter& netFilter = NetFilter{});
    void build();

  private:
    CircuitGraph&   mIn;
    DecoratorConfig mCfg;
    NetFilter       mNetFilter;

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
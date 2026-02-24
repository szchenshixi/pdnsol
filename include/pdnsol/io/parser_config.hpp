#pragma once

#include <string>
#include <vector>

#include "pdnsol/common.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {

// Forward declarations
struct FilesConfig;
struct PlacementConfig;
struct PowerNetConfig;
struct MetalLayerConfig;
struct ViaConfig;
struct TSVConfig;
struct TechConfig;
struct DieConfig;
struct StackingConfig;
struct GridConfig;
struct NetFilterConfig;
struct SimulationConfig;
struct Config;

// Configuration structs
struct FilesConfig {
    std::string defPath;
    std::string currentSrcPath;
    std::string voltageSrcPath;

    static bool fromJson(const Json& j, FilesConfig& config);
    bool        validate() const;

    // For backward compatibility
    static FilesConfig fromJson(const Json& j) {
        FilesConfig config;
        fromJson(j, config);
        return config;
    }
};

struct PlacementConfig {
    std::string              rotation;
    std::vector<std::string> origin;

    static bool fromJson(const Json& j, PlacementConfig& config);
    bool        validate() const;

    static PlacementConfig fromJson(const Json& j) {
        PlacementConfig config;
        fromJson(j, config);
        return config;
    }
};

struct PowerNetConfig {
    ScalarType voltage;           // volt
    ScalarType packageResistance; // ohm

    static bool fromJson(const Json& j, PowerNetConfig& config);
    bool        validate() const;

    static PowerNetConfig fromJson(const Json& j) {
        PowerNetConfig config;
        fromJson(j, config);
        return config;
    }
};

struct MetalLayerConfig {
    double resistivity; // ohm x um
    double thickness;   // um

    static bool fromJson(const Json& j, MetalLayerConfig& config);
    bool        validate() const;

    static MetalLayerConfig fromJson(const Json& j) {
        MetalLayerConfig config;
        fromJson(j, config);
        return config;
    }
};

struct ViaConfig {
    IdString bottomLayer;
    IdString topLayer;
    ScalarType   resistance; // ohm

    static bool fromJson(const Json& j, ViaConfig& config);
    bool        validate() const;

    static ViaConfig fromJson(const Json& j) {
        ViaConfig config;
        fromJson(j, config);
        return config;
    }
};

struct TSVConfig : public ViaConfig {
    static bool fromJson(const Json& j, TSVConfig& config) {
        return ViaConfig::fromJson(j, config);
    }

    static TSVConfig fromJson(const Json& j) {
        TSVConfig config;
        fromJson(j, config);
        return config;
    }
};

struct TechConfig {
    IdString::Map<PowerNetConfig>   powerNets;
    IdString::Map<PowerNetConfig>   groundNets;
    // std::vector<IdString>           layerOrder;
    IdString::Map<MetalLayerConfig> metalLayers;
    IdString::Map<ViaConfig>        vias;
    IdString::Map<TSVConfig>        tsvs;
    IdString                        bumpLayer;

    static bool fromJson(const Json& j, TechConfig& config);
    bool        validate() const;
    bool        validateLayerReferences(const std::string& context) const;

    static TechConfig fromJson(const Json& j) {
        TechConfig config;
        fromJson(j, config);
        return config;
    }
};

struct DieConfig {
    FilesConfig     files;
    PlacementConfig placement;
    TechConfig      tech;

    static bool fromJson(const Json& j, DieConfig& config);
    bool        validate() const;

    static DieConfig fromJson(const Json& j) {
        DieConfig config;
        fromJson(j, config);
        return config;
    }
};

struct StackingConfig {
    IdString topDie;
    IdString topLayer;
    IdString bottomDie;
    IdString bottomLayer;

    static bool fromJson(const Json& j, StackingConfig& config);
    bool        validate() const;

    static StackingConfig fromJson(const Json& j) {
        StackingConfig config;
        fromJson(j, config);
        return config;
    }
};

struct GridConfig {
    double sx; // um
    double sy; // um

    static bool fromJson(const Json& j, GridConfig& config);
    bool        validate() const;

    static GridConfig fromJson(const Json& j) {
        GridConfig config;
        fromJson(j, config);
        return config;
    }
};

struct NetFilterConfig {
    bool                  includePower;
    bool                  includeGround;
    std::vector<IdString> includeNets;
    std::vector<IdString> excludeNets;

    static bool fromJson(const Json& j, NetFilterConfig& config);
    bool        validate() const;

    static NetFilterConfig fromJson(const Json& j) {
        NetFilterConfig config;
        fromJson(j, config);
        return config;
    }
};

struct SimulationConfig {
    IdString::Map<GridConfig>                defaultGrid;
    // die_name -> layer_name -> 2DGridConfig
    IdString::Map<IdString::Map<GridConfig>> dieGrids;
    NetFilterConfig                          netFilter;

    IdString::Map<GridConfig> getDieGrids(IdString dieName) {
        auto it = dieGrids.find(dieName);
        if (it == dieGrids.end()) return {};
        return it->second;
    }

    static bool fromJson(const Json& j, SimulationConfig& config);
    bool        validate() const;

    static SimulationConfig fromJson(const Json& j) {
        SimulationConfig config;
        fromJson(j, config);
        return config;
    }
};

// Main configuration class
class Config {
  private:
    IdString::Map<DieConfig>    dies;
    std::vector<StackingConfig> stackings;
    SimulationConfig            simulation;
    bool                        isValid;

    // Validation methods
    bool validateRequiredFields();
    bool validateNumericalValues();
    bool validateDataConsistency();

  public:
    Config()
        : isValid(false) {}

    // Factory methods with error reporting
    static bool fromJson(const Json& j, Config& config);
    static bool fromFile(const std::string& filename, Config& config);
    static bool fromString(const std::string& JsonStr, Config& config);

    // Public validation API
    bool validate();

    // Getters for autocompletion support
    const IdString::Map<DieConfig>&    getDies() const { return dies; }
    const std::vector<StackingConfig>& getStackings() const {
        return stackings;
    }
    const SimulationConfig& getSimulation() const { return simulation; }
    bool                    getIsValid() const { return isValid; }

    // Convenience methods
    bool             hasDie(IdString dieName) const;
    const DieConfig& getDie(IdString dieName) const;

    // Utility methods
    std::string toString() const;
    bool        saveToFile(const std::string& filename) const;
};

// JSON serialization support
void to_json(Json& j, const FilesConfig& f);
void to_json(Json& j, const PlacementConfig& p);
void to_json(Json& j, const PowerNetConfig& p);
void to_json(Json& j, const MetalLayerConfig& m);
void to_json(Json& j, const ViaConfig& v);
void to_json(Json& j, const TSVConfig& t);
void to_json(Json& j, const TechConfig& t);
void to_json(Json& j, const DieConfig& d);
void to_json(Json& j, const StackingConfig& s);
void to_json(Json& j, const GridConfig& g);
void to_json(Json& j, const NetFilterConfig& n);
void to_json(Json& j, const SimulationConfig& s);
void to_json(Json& j, const Config& c);
} // namespace pdnsol

template <typename T>
void to_json(Json& j, const IdString::Map<T>& map) {
    for (const auto& [key, val] : map) {
        j[key.c_str()] = val;
    }
}
void to_json(Json& j, const IdString& n);

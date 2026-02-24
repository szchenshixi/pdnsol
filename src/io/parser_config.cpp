#include "pdnsol/io/parser_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "pdnsol/common.hpp"
#include "pdnsol/utils/logging.hpp"

using namespace std;

namespace pdnsol {

// Helper function to check if string is a number
bool isNumber(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c) || c == '.' || c == '-';
    });
}

// Helper function to convert string to number safely
bool safeStod(const std::string& s, double& result) {
    try {
        result = std::stod(s);
        return true;
    } catch (...) {
        return false;
    }
}

// FilesConfig implementation
bool FilesConfig::fromJson(const Json& j, FilesConfig& config) {
    bool success = true;

    if (!j.contains("def_path")) {
        PDN_ERROR("Missing required field: def_path");
        success = false;
    } else {
        config.defPath = j["def_path"].get<string>();
    }

    if (!j.contains("current_src_path")) {
        PDN_ERROR("Missing required field: current_src_path");
        success = false;
    } else {
        config.currentSrcPath = j["current_src_path"].get<string>();
    }

    if (!j.contains("voltage_src_path")) {
        PDN_ERROR("Missing required field: voltage_src_path");
        success = false;
    } else {
        config.voltageSrcPath = j["voltage_src_path"].get<string>();
    }

    return success;
}

bool FilesConfig::validate() const {
    bool valid = true;

    if (defPath.empty()) {
        PDN_ERROR("def_path is required");
        valid = false;
    }

    // Add more file path validations if needed
    return valid;
}

// PlacementConfig implementation
bool PlacementConfig::fromJson(const Json& j, PlacementConfig& config) {
    bool success = true;

    if (!j.contains("rotation")) {
        PDN_ERROR("Missing required field: rotation");
        success = false;
    } else {
        config.rotation = j["rotation"].get<string>();
    }

    if (!j.contains("origin")) {
        PDN_ERROR("Missing required field: origin");
        success = false;
    } else {
        auto originArray = j["origin"];
        config.origin.clear();

        if (!originArray.is_array()) {
            PDN_ERROR("origin must be an array");
            success = false;
        } else {
            for (const auto& item : originArray) {
                if (item.is_string()) {
                    config.origin.push_back(item.get<string>());
                } else if (item.is_number()) {
                    config.origin.push_back(to_string(item.get<double>()));
                } else {
                    PDN_WARN("Origin item is not string or number, converting "
                             "to string");
                    config.origin.push_back(item.dump());
                }
            }
        }
    }

    return success;
}

bool PlacementConfig::validate() const {
    bool valid = true;

    if (rotation.empty()) {
        PDN_ERROR("rotation is required");
        valid = false;
    }

    if (origin.size() != 2) {
        PDN_ERROR("origin must have exactly 2 values, found %zu",
                  origin.size());
        valid = false;
    }

    return valid;
}

// PowerNetConfig implementation
bool PowerNetConfig::fromJson(const Json& j, PowerNetConfig& config) {
    bool success = true;

    if (!j.contains("voltage_volt")) {
        PDN_ERROR("Missing required field: voltage_volt");
        success = false;
    } else {
        config.voltage = j["voltage_volt"].get<double>();
    }

    if (!j.contains("package_resistance_ohm")) {
        PDN_ERROR("Missing required field: package_resistance_ohm");
        success = false;
    } else {
        config.packageResistance =
          j["package_resistance_ohm"].get<double>();
    }

    return success;
}

bool PowerNetConfig::validate() const {
    bool valid = true;

    if (voltage < 0) {
        PDN_ERROR("voltage_volt must be non-negative, found: %f", voltage);
        valid = false;
    }

    if (packageResistance < 0) {
        PDN_ERROR("package_resistance_ohm must be non-negative, found: %f",
                  packageResistance);
        valid = false;
    }

    return valid;
}

// MetalLayerConfig implementation
bool MetalLayerConfig::fromJson(const Json& j, MetalLayerConfig& config) {
    bool success = true;

    if (!j.contains("resistivity_ohm_x_um")) {
        PDN_ERROR("Missing required field: resistivity_ohm_x_um");
        success = false;
    } else {
        config.resistivity = j["resistivity_ohm_x_um"].get<double>();
    }

    if (!j.contains("thickness_um")) {
        PDN_ERROR("Missing required field: thickness_um");
        success = false;
    } else {
        config.thickness = j["thickness_um"].get<double>();
    }

    return success;
}

bool MetalLayerConfig::validate() const {
    bool valid = true;

    if (resistivity < 0) {
        PDN_ERROR("resistivity_ohm_x_um must be non-negative, found: %f",
                  resistivity);
        valid = false;
    }

    if (thickness < 0) {
        PDN_ERROR("thickness_um must be non-negative, found: %f", thickness);
        valid = false;
    }

    return valid;
}

// ViaConfig implementation
bool ViaConfig::fromJson(const Json& j, ViaConfig& config) {
    bool success = true;

    if (!j.contains("bottom_layer")) {
        PDN_ERROR("Missing required field: bottom_layer");
        success = false;
    } else {
        config.bottomLayer = IdString(j["bottom_layer"].get<string>());
    }

    if (!j.contains("top_layer")) {
        PDN_ERROR("Missing required field: top_layer");
        success = false;
    } else {
        config.topLayer = IdString(j["top_layer"].get<string>());
    }

    if (!j.contains("resistance_ohm")) {
        PDN_ERROR("Missing required field: resistance_ohm");
        success = false;
    } else {
        config.resistance = j["resistance_ohm"].get<double>();
    }

    return success;
}

bool ViaConfig::validate() const {
    bool valid = true;

    if (!bottomLayer.valid()) {
        PDN_ERROR("bottom_layer is required");
        valid = false;
    }

    if (!topLayer.valid()) {
        PDN_ERROR("top_layer is required");
        valid = false;
    }

    if (resistance < 0) {
        PDN_ERROR("resistance_ohm must be non-negative, found: %f",
                  resistance);
        valid = false;
    }

    return valid;
}

// TechConfig implementation
bool TechConfig::fromJson(const Json& j, TechConfig& config) {
    bool success = true;

    // Parse power nets
    if (j.contains("power_nets")) {
        for (auto& [key, value] : j["power_nets"].items()) {
            PowerNetConfig net;
            if (PowerNetConfig::fromJson(value, net)) {
                config.powerNets[IdString(key)] = net;
            } else {
                PDN_WARN("Failed to parse power net: %s", key.c_str());
                success = false;
            }
        }
    } else {
        PDN_WARN("Missing power_nets section");
    }

    // Parse ground nets
    if (j.contains("ground_nets")) {
        for (auto& [key, value] : j["ground_nets"].items()) {
            PowerNetConfig net;
            if (PowerNetConfig::fromJson(value, net)) {
                config.groundNets[IdString(key)] = net;
            } else {
                PDN_WARN("Failed to parse ground net: %s", key.c_str());
                success = false;
            }
        }
    } else {
        PDN_WARN("Missing ground_nets section");
    }

    // Parse metal layers
    if (j.contains("metal_layers")) {
        for (auto& [key, value] : j["metal_layers"].items()) {
            MetalLayerConfig layer;
            if (MetalLayerConfig::fromJson(value, layer)) {
                config.metalLayers[IdString(key)] = layer;
            } else {
                PDN_WARN("Failed to parse metal layer: %s", key.c_str());
                success = false;
            }
        }
    } else {
        PDN_ERROR("Missing required section: metal_layers");
        success = false;
    }

    // Parse vias
    if (j.contains("vias")) {
        for (auto& [key, value] : j["vias"].items()) {
            ViaConfig via;
            if (ViaConfig::fromJson(value, via)) {
                config.vias[IdString(key)] = via;
            } else {
                PDN_WARN("Failed to parse via: %s", key.c_str());
                success = false;
            }
        }
    }

    // Parse TSVs
    if (j.contains("tsvs")) {
        for (auto& [key, value] : j["tsvs"].items()) {
            TSVConfig tsv;
            if (TSVConfig::fromJson(value, tsv)) {
                config.tsvs[IdString(key)] = tsv;
            } else {
                PDN_WARN("Failed to parse TSV: %s", key.c_str());
                success = false;
            }
        }
    }

    // Parse bump layer
    if (j.contains("bump_layer")) {
        config.bumpLayer = IdString(j["bump_layer"].get<string>());
    }

    return success;
}

bool TechConfig::validate() const {
    bool valid = true;

    // Validate individual components
    for (const auto& [name, net] : powerNets) {
        if (!net.validate()) {
            PDN_ERROR("Power net '%s' validation failed", name.c_str());
            valid = false;
        }
    }

    for (const auto& [name, net] : groundNets) {
        if (!net.validate()) {
            PDN_ERROR("Ground net '%s' validation failed", name.c_str());
            valid = false;
        }
    }

    for (const auto& [name, layer] : metalLayers) {
        if (!layer.validate()) {
            PDN_ERROR("Metal layer '%s' validation failed", name.c_str());
            valid = false;
        }
    }

    for (const auto& [name, via] : vias) {
        if (!via.validate()) {
            PDN_ERROR("Via '%s' validation failed", name.c_str());
            valid = false;
        }
    }

    for (const auto& [name, tsv] : tsvs) {
        if (!tsv.validate()) {
            PDN_ERROR("TSV '%s' validation failed", name.c_str());
            valid = false;
        }
    }

    // Validate layer references
    if (!validateLayerReferences("TechConfig")) {
        valid = false;
    }

    return valid;
}

bool TechConfig::validateLayerReferences(const std::string& context) const {
    bool valid = true;

    // Check vias reference existing layers
    for (const auto& [viaName, via] : vias) {
        if (metalLayers.find(via.bottomLayer) == metalLayers.end()) {
            PDN_ERROR("%s: Via '%s' references non-existent bottom_layer: %s",
                      context.c_str(),
                      viaName.c_str(),
                      via.bottomLayer.c_str());
            valid = false;
        }
        if (metalLayers.find(via.topLayer) == metalLayers.end()) {
            PDN_ERROR("%s: Via '%s' references non-existent top_layer: %s",
                      context.c_str(),
                      viaName.c_str(),
                      via.topLayer.c_str());
            valid = false;
        }
    }

    // Check TSVs reference existing layers
    for (const auto& [tsvName, tsv] : tsvs) {
        if (metalLayers.find(tsv.bottomLayer) == metalLayers.end()) {
            PDN_ERROR("%s: TSV '%s' references non-existent bottom_layer: %s",
                      context.c_str(),
                      tsvName.c_str(),
                      tsv.bottomLayer.c_str());
            valid = false;
        }
        if (metalLayers.find(tsv.topLayer) == metalLayers.end()) {
            PDN_ERROR("%s: TSV '%s' references non-existent top_layer: %s",
                      context.c_str(),
                      tsvName.c_str(),
                      tsv.topLayer.c_str());
            valid = false;
        }
    }

    // Check bump layer exists
    if (bumpLayer.valid() &&
        metalLayers.find(bumpLayer) == metalLayers.end()) {
        PDN_ERROR("%s: Bump layer '%s' not found in metal_layers",
                  context.c_str(),
                  bumpLayer.c_str());
        valid = false;
    }

    return valid;
}

// DieConfig implementation
bool DieConfig::fromJson(const Json& j, DieConfig& config) {
    bool success = true;

    if (!j.contains("files")) {
        PDN_ERROR("Missing required section: files");
        success = false;
    } else {
        if (!FilesConfig::fromJson(j["files"], config.files)) {
            PDN_WARN("Failed to parse files section");
            success = false;
        }
    }

    if (!j.contains("placement")) {
        PDN_ERROR("Missing required section: placement");
        success = false;
    } else {
        if (!PlacementConfig::fromJson(j["placement"], config.placement)) {
            PDN_WARN("Failed to parse placement section");
            success = false;
        }
    }

    if (!j.contains("tech")) {
        PDN_ERROR("Missing required section: tech");
        success = false;
    } else {
        if (!TechConfig::fromJson(j["tech"], config.tech)) {
            PDN_WARN("Failed to parse tech section");
            success = false;
        }
    }

    return success;
}

bool DieConfig::validate() const {
    bool valid = true;

    if (!files.validate()) {
        PDN_ERROR("Files validation failed");
        valid = false;
    }

    if (!placement.validate()) {
        PDN_ERROR("Placement validation failed");
        valid = false;
    }

    if (!tech.validate()) {
        PDN_ERROR("Tech validation failed");
        valid = false;
    }

    return valid;
}

// StackingConfig implementation
bool StackingConfig::fromJson(const Json& j, StackingConfig& config) {
    bool success = true;

    if (!j.contains("top_die")) {
        PDN_ERROR("Missing required field: top_die");
        success = false;
    } else {
        config.topDie = IdString(j["top_die"].get<string>());
    }

    if (!j.contains("top_layer")) {
        PDN_ERROR("Missing required field: top_layer");
        success = false;
    } else {
        config.topLayer = IdString(j["top_layer"].get<string>());
    }

    if (!j.contains("bottom_die")) {
        PDN_ERROR("Missing required field: bottom_die");
        success = false;
    } else {
        config.bottomDie = IdString(j["bottom_die"].get<string>());
    }

    if (!j.contains("bottom_layer")) {
        PDN_ERROR("Missing required field: bottom_layer");
        success = false;
    } else {
        config.bottomLayer = IdString(j["bottom_layer"].get<string>());
    }

    return success;
}

bool StackingConfig::validate() const {
    bool valid = true;

    if (!topDie.valid()) {
        PDN_ERROR("top_die is required");
        valid = false;
    }

    if (!topLayer.valid()) {
        PDN_ERROR("top_layer is required");
        valid = false;
    }

    if (!bottomDie.valid()) {
        PDN_ERROR("bottom_die is required");
        valid = false;
    }

    if (!bottomLayer.valid()) {
        PDN_ERROR("bottom_layer is required");
        valid = false;
    }

    return valid;
}

// GridConfig implementation
bool GridConfig::fromJson(const Json& j, GridConfig& config) {
    bool success = true;

    if (!j.contains("sx")) {
        PDN_ERROR("Missing required field: sx");
        success = false;
    } else {
        config.sx = j["sx"].get<int>();
    }

    if (!j.contains("sy")) {
        PDN_ERROR("Missing required field: sy");
        success = false;
    } else {
        config.sy = j["sy"].get<int>();
    }

    return success;
}

bool GridConfig::validate() const {
    bool valid = true;

    if (sx <= 0) {
        PDN_ERROR("grid sx must be positive, found: %d", sx);
        valid = false;
    }

    if (sy <= 0) {
        PDN_ERROR("grid sy must be positive, found: %d", sy);
        valid = false;
    }

    return valid;
}

// NetFilterConfig implementation
bool NetFilterConfig::fromJson(const Json& j, NetFilterConfig& config) {
    bool success = true;

    if (!j.contains("include_power")) {
        PDN_ERROR("Missing required field: include_power");
        success = false;
    } else {
        config.includePower = j["include_power"].get<bool>();
    }

    if (!j.contains("include_ground")) {
        PDN_ERROR("Missing required field: include_ground");
        success = false;
    } else {
        config.includeGround = j["include_ground"].get<bool>();
    }

    if (!j.contains("include_nets")) {
        PDN_ERROR("Missing required field: include_nets");
        success = false;
    } else {
        try {
            vector<string> includeNets =
              j["include_nets"].get<vector<string>>();
            config.includeNets.reserve(includeNets.size());
            for (const string& n : includeNets) {
                config.includeNets.push_back(IdString(n));
            }
        } catch (...) {
            PDN_ERROR("Failed to parse include_nets");
            success = false;
        }
    }

    if (!j.contains("exclude_nets")) {
        PDN_ERROR("Missing required field: exclude_nets");
        success = false;
    } else {
        try {
            vector<string> excludeNets =
              j["exclude_nets"].get<vector<string>>();
            config.excludeNets.reserve(excludeNets.size());
            for (const string& n : excludeNets) {
                config.excludeNets.push_back(IdString(n));
            }
        } catch (...) {
            PDN_ERROR("Failed to parse exclude_nets");
            success = false;
        }
    }

    return success;
}

bool NetFilterConfig::validate() const {
    bool valid = true;

    // Check for conflicts between include_nets and exclude_nets
    for (const auto& included : includeNets) {
        if (std::find(excludeNets.begin(), excludeNets.end(), included) !=
            excludeNets.end()) {
            PDN_ERROR("Net '%s' cannot be both included and excluded",
                      included.c_str());
            valid = false;
        }
    }

    return valid;
}

// SimulationConfig implementation
bool SimulationConfig::fromJson(const Json& j, SimulationConfig& config) {
    bool success = true;

    if (!j.contains("grid")) {
        PDN_ERROR("Missing required section: grid");
        success = false;
    } else {
        const auto& grid = j["grid"];

        // Parse default grid
        if (grid.contains("default")) {
            GridConfig defaultGrid;
            if (GridConfig::fromJson(grid["default"], defaultGrid)) {
                config.defaultGrid[IdString("default")] = defaultGrid;
            } else {
                PDN_WARN("Failed to parse default grid");
                success = false;
            }
        }

        // Parse die-specific grids
        for (const auto& [dieName, dieGrid] : grid.items()) {
            if (dieName != "default") {
                IdString::Map<GridConfig> dieGridMap;
                for (const auto& [layerName, layerGrid] : dieGrid.items()) {
                    GridConfig gridConfig;
                    if (GridConfig::fromJson(layerGrid, gridConfig)) {
                        dieGridMap[IdString(layerName)] = gridConfig;
                    } else {
                        PDN_WARN("Failed to parse grid for die %s region %s",
                                 dieName.c_str(),
                                 layerName.c_str());
                        success = false;
                    }
                }
                config.dieGrids[IdString(dieName)] = dieGridMap;
            }
        }
    }

    if (!j.contains("net_filter")) {
        PDN_ERROR("Missing required section: net_filter");
        success = false;
    } else {
        if (!NetFilterConfig::fromJson(j["net_filter"], config.netFilter)) {
            PDN_WARN("Failed to parse net_filter section");
            success = false;
        }
    }

    return success;
}

bool SimulationConfig::validate() const {
    bool valid = true;

    // Validate grid configurations
    for (const auto& [name, grid] : defaultGrid) {
        if (!grid.validate()) {
            PDN_ERROR("Default grid validation failed");
            valid = false;
        }
    }

    for (const auto& [dieName, dieGrids] : dieGrids) {
        for (const auto& [regionName, grid] : dieGrids) {
            if (!grid.validate()) {
                PDN_ERROR("Grid validation failed for die %s region %s",
                          dieName.c_str(),
                          regionName.c_str());
                valid = false;
            }
        }
    }

    // Validate net filter
    if (!netFilter.validate()) {
        PDN_ERROR("Net filter validation failed");
        valid = false;
    }

    return valid;
}

// Config class implementation
bool Config::fromJson(const Json& j, Config& config) {
    bool success = true;

    // Clear previous state
    config.isValid = false;

    // Parse dies
    if (!j.contains("dies")) {
        PDN_ERROR("Missing required section: dies");
        success = false;
    } else {
        for (auto& [dieName, dieData] : j["dies"].items()) {
            DieConfig die;
            if (DieConfig::fromJson(dieData, die)) {
                config.dies[IdString(dieName)] = die;
            } else {
                PDN_ERROR("Failed to parse die: %s", dieName.c_str());
                success = false;
            }
        }
    }

    // Parse stackings
    if (!j.contains("stackings")) {
        PDN_ERROR("Missing required section: stackings");
        success = false;
    } else {
        if (!j["stackings"].is_array()) {
            PDN_ERROR("stackings must be an array");
            success = false;
        } else {
            for (const auto& stackingData : j["stackings"]) {
                StackingConfig stacking;
                if (StackingConfig::fromJson(stackingData, stacking)) {
                    config.stackings.push_back(stacking);
                } else {
                    PDN_ERROR("Failed to parse stacking configuration");
                    success = false;
                }
            }
        }
    }

    // Parse simulation
    if (!j.contains("simulation")) {
        PDN_ERROR("Missing required section: simulation");
        success = false;
    } else {
        if (!SimulationConfig::fromJson(j["simulation"], config.simulation)) {
            PDN_ERROR("Failed to parse simulation section");
            success = false;
        }
    }

    // If parsing was successful, validate
    if (success) {
        config.isValid = config.validate();
    }

    return success && config.isValid;
}

bool Config::fromFile(const std::string& filename, Config& config) {
    ifstream file(filename);
    if (!file.is_open()) {
        PDN_ERROR("Cannot open file: %s", filename.c_str());
        return false;
    }

    try {
        Json j;
        file >> j;
        return fromJson(j, config);
    } catch (const Json::exception& e) {
        PDN_ERROR("JSON parsing error: %s", string(e.what()).c_str());
        return false;
    } catch (const exception& e) {
        PDN_ERROR("Error reading file: %s", string(e.what()).c_str());
        return false;
    }
}

bool Config::fromString(const std::string& JsonStr, Config& config) {
    try {
        Json j = Json::parse(JsonStr);
        return fromJson(j, config);
    } catch (const Json::exception& e) {
        PDN_ERROR("JSON parsing error: %s", string(e.what()).c_str());
        return false;
    }
}

bool Config::validate() {
    isValid = true;

    if (!validateRequiredFields()) {
        isValid = false;
    }

    if (!validateNumericalValues()) {
        isValid = false;
    }

    if (!validateDataConsistency()) {
        isValid = false;
    }

    return isValid;
}

bool Config::validateRequiredFields() {
    bool valid = true;

    // Check required top-level sections
    if (dies.empty()) {
        PDN_ERROR("At least one die is required");
        valid = false;
    }

    if (stackings.empty()) {
        PDN_ERROR("At least one stacking configuration is required");
        valid = false;
    }

    // Validate each die
    for (const auto& [dieName, die] : dies) {
        if (!die.validate()) {
            PDN_ERROR("Die validation failed for: %s", dieName.c_str());
            valid = false;
        }
    }

    // Validate stackings
    for (const auto& stacking : stackings) {
        if (!stacking.validate()) {
            PDN_ERROR("Stacking validation failed");
            valid = false;
        }
    }

    // Validate simulation
    if (!simulation.validate()) {
        PDN_ERROR("Simulation validation failed");
        valid = false;
    }

    return valid;
}

bool Config::validateNumericalValues() {
    // Numerical validation is already done in individual validate() methods
    // This method serves as a central point for additional numerical
    // validations
    bool valid = true;

    // Check for any negative numerical values in the entire configuration
    for (const auto& [dieName, die] : dies) {
        // Add any die-level numerical validations here if needed
        (void)dieName; // Suppress unused parameter warning
        (void)die;
    }

    return valid;
}

bool Config::validateDataConsistency() {
    bool valid = true;

    // Check that stacking references exist
    for (const auto& stacking : stackings) {
        if (dies.find(stacking.topDie) == dies.end()) {
            PDN_ERROR("Stacking references non-existent top_die: %s",
                      stacking.topDie.c_str());
            valid = false;
        }

        if (dies.find(stacking.bottomDie) == dies.end()) {
            PDN_ERROR("Stacking references non-existent bottom_die: %s",
                      stacking.bottomDie.c_str());
            valid = false;
        }

        // Check that layers referenced in stacking exist in respective dies
        if (dies.find(stacking.topDie) != dies.end()) {
            const auto& topDie = dies.at(stacking.topDie);
            if (topDie.tech.metalLayers.find(stacking.topLayer) ==
                topDie.tech.metalLayers.end()) {
                PDN_ERROR("Stacking references non-existent top_layer '%s' in "
                          "die '%s'",
                          stacking.topLayer.c_str(),
                          stacking.topDie.c_str());
                valid = false;
            }
        }

        if (dies.find(stacking.bottomDie) != dies.end()) {
            const auto& bottomDie = dies.at(stacking.bottomDie);
            if (bottomDie.tech.metalLayers.find(stacking.bottomLayer) ==
                bottomDie.tech.metalLayers.end()) {
                PDN_ERROR("Stacking references non-existent bottom_layer '%s' "
                          "in die '%s'",
                          stacking.bottomLayer.c_str(),
                          stacking.bottomDie.c_str());
                valid = false;
            }
        }
    }

    // Check that die-specific grid configurations reference existing dies
    for (const auto& [dieName, _] : simulation.dieGrids) {
        if (dies.find(dieName) == dies.end()) {
            PDN_ERROR("Grid configuration for non-existent die: %s",
                      dieName.c_str());
            valid = false;
        }
    }

    return valid;
}

bool Config::hasDie(IdString dieName) const {
    return dies.find(dieName) != dies.end();
}

const DieConfig& Config::getDie(IdString dieName) const {
    auto     it       = dies.find(dieName);
    if (it == dies.end()) {
        PDN_FATAL("Die not found: %s", dieName.c_str());
    }
    return it->second;
}

std::string Config::toString() const {
    Json j = *this;
    return j.dump(2); // Pretty print with 2-space indentation
}

bool Config::saveToFile(const std::string& filename) const {
    Json     j = *this;
    ofstream file(filename);
    if (!file.is_open()) {
        PDN_ERROR("Cannot open file for writing: %s", filename.c_str());
        return false;
    }
    file << j.dump(2);
    return true;
}

// JSON serialization implementations
void to_json(Json& j, const FilesConfig& f) {
    j = Json{{"def_path", f.defPath},
             {"current_src_path", f.currentSrcPath},
             {"voltage_src_path", f.voltageSrcPath}};
}

void to_json(Json& j, const PlacementConfig& p) {
    j = Json{{"rotation", p.rotation}, {"origin", p.origin}};
}

void to_json(Json& j, const PowerNetConfig& p) {
    j = Json{{"voltage_volt", p.voltage},
             {"package_resistance_ohm", p.packageResistance}};
}

void to_json(Json& j, const MetalLayerConfig& m) {
    j = Json{{"resistivity_ohm_x_um", m.resistivity},
             {"thickness_um", m.thickness}};
}

void to_json(Json& j, const ViaConfig& v) {
    j = Json{{"bottom_layer", v.bottomLayer},
             {"top_layer", v.topLayer},
             {"resistance_ohm", v.resistance}};
}

void to_json(Json& j, const TSVConfig& t) {
    to_json(j, static_cast<const ViaConfig&>(t));
}

void to_json(Json& j, const TechConfig& t) {
    j = Json{{"power_nets", t.powerNets},
             {"ground_nets", t.groundNets},
             {"metal_layers", t.metalLayers},
             {"vias", t.vias},
             {"tsvs", t.tsvs},
             {"bump_layer", t.bumpLayer}};
}

void to_json(Json& j, const DieConfig& d) {
    j = Json{{"files", d.files}, {"placement", d.placement}, {"tech", d.tech}};
}

void to_json(Json& j, const StackingConfig& s) {
    j = Json{{"top_die", s.topDie},
             {"top_layer", s.topLayer},
             {"bottom_die", s.bottomDie},
             {"bottom_layer", s.bottomLayer}};
}

void to_json(Json& j, const GridConfig& g) {
    j = Json{{"sx", g.sx}, {"sy", g.sy}};
}

void to_json(Json& j, const NetFilterConfig& n) {
    j = Json{{"include_power", n.includePower},
             {"include_ground", n.includeGround},
             {"include_nets", n.includeNets},
             {"exclude_nets", n.excludeNets}};
}

void to_json(Json& j, const SimulationConfig& s) {
    j = Json{{"grid", {{"default", s.defaultGrid}}},
             {"net_filter", s.netFilter}};

    // Add die-specific grids
    for (const auto& [dieName, dieGrid] : s.dieGrids) {
        j["grid"][dieName.c_str()] = dieGrid;
    }
}

void to_json(Json& j, const Config& c) {
    j = Json{{"dies", c.getDies()},
             {"stackings", c.getStackings()},
             {"simulation", c.getSimulation()}};
}

} // namespace pdnsol

void to_json(Json& j, const IdString& n) { j = n.str(); }
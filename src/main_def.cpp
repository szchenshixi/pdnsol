#include <filesystem>
#include <iostream>
#include <unordered_set>

#include "pdnsol/io/parser_def.hpp"
#include "pdnsol/struct/circuit_decorator.hpp"
#include "pdnsol/utils/logging.hpp"

using namespace pdnsol;

// Alias for filesystem if you want
namespace fs = std::filesystem;

// Assuming Json is something like nlohmann::json
bool integrityCheck(const Json& configJ, const std::string& filePath) {
    bool result = true;

    // -------------------------------------------------------------------------
    // Helper lambdas
    // -------------------------------------------------------------------------

    // Require that an object field exists and is itself a JSON object.
    auto requireObject = [&](const Json&        parent,
                             const char*        key,
                             const std::string& context) -> const Json* {
        if (!parent.contains(key)) {
            PDN_ERROR("Cannot find '%s' section in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        const Json& child = parent.at(key);
        if (!child.is_object()) {
            PDN_ERROR("'%s' in %s must be a JSON object (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        return &child;
    };

    // Require that a field exists and is an array (optionally non-empty).
    auto requireArray = [&](const Json&        parent,
                            const char*        key,
                            const std::string& context,
                            bool mustBeNonEmpty = true) -> const Json* {
        if (!parent.contains(key)) {
            PDN_ERROR("Cannot find '%s' array in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        const Json& arr = parent.at(key);
        if (!arr.is_array()) {
            PDN_ERROR("Field '%s' in %s must be an array (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        if (mustBeNonEmpty && arr.empty()) {
            PDN_ERROR("Array '%s' in %s must not be empty (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            // We still return it so caller can iterate if desired.
        }
        return &arr;
    };

    // Require that a field exists, is a string, and is not empty.
    auto requireNonEmptyString =
      [&](const Json&        obj,
          const char*        key,
          const std::string& context) -> std::string {
        if (!obj.contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return std::string{};
        }

        const Json& jv = obj.at(key);
        if (!jv.is_string()) {
            PDN_ERROR("Field '%s' in %s must be a string (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return std::string{};
        }

        std::string value = jv.get<std::string>();
        if (value.empty()) {
            PDN_ERROR(
              "Field '%s' in %s must not be an empty string (context: %s)",
              key,
              filePath.c_str(),
              context.c_str());
            result = false;
        }
        return value;
    };

    // Require that a field exists, is numeric, and >= 0.0.
    // If it's exactly 0.0, only emit a warning.
    auto requireNonNegativeNumber = [&](const Json&        obj,
                                        const char*        key,
                                        const std::string& context) -> double {
        if (!obj.contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return 0.0;
        }

        const Json& jv = obj.at(key);
        if (!jv.is_number()) {
            PDN_ERROR("Field '%s' in %s must be a number (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return 0.0;
        }

        double value = jv.get<double>();
        if (value < 0.0) {
            PDN_ERROR("Field '%s' in %s must be >= 0.0 (got %g) (context: %s)",
                      key,
                      filePath.c_str(),
                      value,
                      context.c_str());
            result = false;
        } else if (value == 0.0) {
            PDN_WARN("Field '%s' in %s is 0.0 (context: %s)",
                     key,
                     filePath.c_str(),
                     context.c_str());
        }
        return value;
    };

    // Require that a field exists, is a string, is not empty, and points to an
    // existing file on disk.
    auto requireExistingFile =
      [&](const Json& obj, const char* key, const std::string& context) {
          std::string path = requireNonEmptyString(obj, key, context);
          if (!path.empty()) {
              if (!fs::exists(path)) {
                  PDN_ERROR(
                    "File '%s' referenced by '%s' does not exist (config: %s)",
                    path.c_str(),
                    context.c_str(),
                    filePath.c_str());
                  result = false;
              }
          }
      };

    // -------------------------------------------------------------------------
    // Top-level sections
    // -------------------------------------------------------------------------

    const Json* fileJ = requireObject(configJ, "file", "top-level");
    const Json* simJ  = requireObject(configJ, "simulation", "top-level");
    const Json* techJ = requireObject(configJ, "tech", "top-level");

    // -------------------------------------------------------------------------
    // File Section
    // -------------------------------------------------------------------------
    if (fileJ) {
        const std::string context = "file section";

        // "def_path": must exist, non-empty string, and file must exist
        requireExistingFile(*fileJ, "def_path", "file.def_path");

        // "current_src_path": same
        requireExistingFile(
          *fileJ, "current_src_path", "file.current_src_path");

        // "voltage_src_path": same
        requireExistingFile(
          *fileJ, "voltage_src_path", "file.voltage_src_path");
    }

    // -------------------------------------------------------------------------
    // Technology Section
    // -------------------------------------------------------------------------
    std::unordered_set<std::string> metalLayerNames;

    if (techJ) {
        // ------------------------- metal_layers -----------------------------
        const Json* metalsArr =
          requireArray(*techJ, "metal_layers", "tech.metal_layers");
        if (metalsArr) {
            for (std::size_t i = 0; i < metalsArr->size(); ++i) {
                const Json& m = (*metalsArr)[i];
                std::string context =
                  "tech.metal_layers[" + std::to_string(i) + "]";

                if (!m.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.metal_layers' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string name =
                  requireNonEmptyString(m, "name", context + ".name");
                if (!name.empty()) {
                    metalLayerNames.insert(name);
                }

                requireNonNegativeNumber(m,
                                         "resistivity_ohm_x_um",
                                         context + ".resistivity_ohm_x_um");
                requireNonNegativeNumber(
                  m, "thickness_um", context + ".thickness_um");
            }
        }

        // ------------------------- vias -------------------------------------
        const Json* viasArr = requireArray(*techJ, "vias", "tech.vias");
        if (viasArr) {
            for (std::size_t i = 0; i < viasArr->size(); ++i) {
                const Json& v       = (*viasArr)[i];
                std::string context = "tech.vias[" + std::to_string(i) + "]";

                if (!v.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.vias' must be an object "
                              "(config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string viaName =
                  requireNonEmptyString(v, "name", context + ".name");
                std::string bottom = requireNonEmptyString(
                  v, "bottom_layer", context + ".bottom_layer");
                std::string top = requireNonEmptyString(
                  v, "top_layer", context + ".top_layer");

                requireNonNegativeNumber(
                  v, "resistance_ohm", context + ".resistance_ohm");

                // Cross-check that via layers exist in metal_layers
                if (!bottom.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(bottom) == metalLayerNames.end()) {
                    PDN_ERROR("Via '%s' references unknown bottom_layer '%s' "
                              "(config: %s)",
                              viaName.c_str(),
                              bottom.c_str(),
                              filePath.c_str());
                    result = false;
                }
                if (!top.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(top) == metalLayerNames.end()) {
                    PDN_ERROR("Via '%s' references unknown top_layer '%s' "
                              "(config: %s)",
                              viaName.c_str(),
                              top.c_str(),
                              filePath.c_str());
                    result = false;
                }
            }
        }

        // -------------------- powerNets -------------------------------------
        const Json* powerArr =
          requireArray(*techJ, "power_nets", "tech.power_nets");
        if (powerArr) {
            for (std::size_t i = 0; i < powerArr->size(); ++i) {
                const Json& pn = (*powerArr)[i];
                std::string context =
                  "tech.power_nets[" + std::to_string(i) + "]";

                if (!pn.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.power_nets' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                requireNonEmptyString(pn, "name", context + ".name");
                requireNonNegativeNumber(
                  pn, "voltage_volt", context + ".voltage_volt");
                requireNonNegativeNumber(pn,
                                         "package_resistance_ohm",
                                         context + ".package_resistance_ohm");
            }
        }

        // ------------------- groundNets -------------------------------------
        const Json* groundArr =
          requireArray(*techJ, "ground_nets", "tech.ground_nets");
        if (groundArr) {
            for (std::size_t i = 0; i < groundArr->size(); ++i) {
                const Json& gn = (*groundArr)[i];
                std::string context =
                  "tech.ground_nets[" + std::to_string(i) + "]";

                if (!gn.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.ground_nets' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                requireNonEmptyString(gn, "name", context + ".name");
                requireNonNegativeNumber(
                  gn, "voltage_volt", context + ".voltage_volt");
                requireNonNegativeNumber(gn,
                                         "package_resistance_ohm",
                                         context + ".package_resistance_ohm");
            }
        }

        // ------------------- layer_order ------------------------------------
        const Json* orderArr =
          requireArray(*techJ, "layer_order", "tech.layer_order");
        if (orderArr) {
            for (std::size_t i = 0; i < orderArr->size(); ++i) {
                const Json& lv = (*orderArr)[i];
                std::string context =
                  "tech.layer_order[" + std::to_string(i) + "]";

                if (!lv.is_string()) {
                    PDN_ERROR("Element %zu of 'tech.layer_order' must be a "
                              "string (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string layer = lv.get<std::string>();
                if (layer.empty()) {
                    PDN_ERROR("Element %zu of 'tech.layer_order' must not be "
                              "empty (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                if (!metalLayerNames.empty() &&
                    metalLayerNames.find(layer) == metalLayerNames.end()) {
                    PDN_ERROR("Layer '%s' in 'tech.layer_order' is not "
                              "defined in 'tech.metal_layers' (config: %s)",
                              layer.c_str(),
                              filePath.c_str());
                    result = false;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Simulation Section
    // -------------------------------------------------------------------------
    if (simJ) {
        // Integers in JSON are also numbers, so requireNonNegativeNumber is
        // OK.

        // grid_Nx >= 0.0, warn if 0
        requireNonNegativeNumber(*simJ, "grid_Nx", "simulation.grid_Nx");

        // grid_Ny >= 0.0, warn if 0
        requireNonNegativeNumber(*simJ, "grid_Ny", "simulation.grid_Ny");

        // default_pkg_R >= 0.0, warn if 0
        requireNonNegativeNumber(
          *simJ, "default_pkg_R", "simulation.default_pkg_R");

        // Enforce it to by within the defined metal layers
        const char* key = "bump_layer";
        if (!simJ->contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      "simulation.bump_layer");
            result = false;
        } else {
            const Json& jv = simJ->at(key);
            if (!jv.is_string()) {
                PDN_ERROR("Field '%s' in %s must be a string (context: %s)",
                          key,
                          filePath.c_str(),
                          "simulation.bump_layer");
                result = false;
            } else {
                std::string bumpLayer = jv.get<std::string>();
                if (bumpLayer.empty()) {
                    PDN_ERROR("Field '%s' in %s must not be an empty string "
                              "(context: %s)",
                              key,
                              filePath.c_str(),
                              "simulation.bump_layer");
                    result = false;
                } else if (!metalLayerNames.empty() &&
                           metalLayerNames.find(bumpLayer) ==
                             metalLayerNames.end()) {
                    PDN_ERROR(
                      "'bump_layer' references unknown metal layer '%s' "
                      "(config: %s)",
                      bumpLayer.c_str(),
                      filePath.c_str());
                    result = false;
                }
            }
        }
    }

    return result;
}

void init() {
    // Initialize the logging facility
    Logger::instance().init(Logger::Level::Debug, // Log level
                            "app.log",            // Log file path
                            5 * 1024 * 1024,      // File size
                            3,                    // Number of files
                            true);                // Print to console
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: coarse_pdn_3d <simulation.json>\n";
        return 1;
    }
    init();
    std::string   simulationConfigPath = argv[1];
    std::ifstream inFile(simulationConfigPath);
    if (!inFile.is_open()) {
        PDN_FATAL("Cannot open file %s", simulationConfigPath.c_str());
    }
    const Json configJ = Json::parse(inFile);
    if (!integrityCheck(configJ, simulationConfigPath)) {
        PDN_FATAL("Cannot proceed due to the above error. Abort.");
    }

    // ============================================================
    // 1) Technology Database Setup
    // ============================================================
    TechDatabase techDb;
    const Json&  techConfigJ = configJ["tech"];

    // Metal Layers - Add ALL layers from DEF
    // Format: addLayer(layer_name, resistivity_Ω·µm, thickness_µm)
    // Example: addLayer("met1", 0.0300, 0.2000)
    for (const auto& j : techConfigJ["metal_layers"]) {
        const std::string& layerName   = j["name"];
        const double       resistivity = j["resistivity_ohm_x_um"];
        const double       thickness   = j["thickness_um"];
        techDb.addLayer(layerName, resistivity, thickness);
    }

    // Vias - Add ALL vias from DEF VIAS section
    // Format: addVia(via_name, bottom_layer, top_layer, resistance_Ω)
    // Example: addVia("via_1600x480", "met1", "met2", 0.000100)
    for (const auto& j : techConfigJ["vias"]) {
        const std::string& name        = j["name"];
        const std::string& bottomLayer = j["bottom_layer"];
        const std::string& topLayer    = j["top_layer"];
        const double       resistance  = j["resistance_ohm"];
        techDb.addVia(name, bottomLayer, topLayer, resistance);
    }

    // Additional via types (if not in DEF)
    // techDb.addVia("custom_via", "met1", "met2", 0.001);

    // ============================================================
    // 2) PDN Configuration
    // ============================================================

    // Power nets from SPECIALNETS section
    std::vector<std::string> powerNets = {"VDD"};

    // Ground nets from SPECIALNETS section
    std::vector<std::string> groundNets = {"VSS"};

    // Metal layer order (bottom to top)
    // Example: layerOrder = {"met1", "met2", "met3", "met4", "met5"}
    std::vector<std::string> layerOrder;
    layerOrder.reserve(techConfigJ["layer_order"].size());
    for (const auto& j : techConfigJ["layer_order"]) {
        std::string layerName = j.get<std::string>();
        layerOrder.push_back(std::move(layerName));
    }

    // ============================================================
    // 3) Simulation Configuration
    // ============================================================

    const Json&        simConfigJ  = configJ["simulation"];
    const int          gridNx      = simConfigJ["grid_Nx"];
    const int          gridNy      = simConfigJ["grid_Ny"];
    const double       defaultPkgR = simConfigJ["default_pkg_R"];
    // -1 => use topmost layer in layerOrder
    const std::string& bumpLayer   = simConfigJ["bump_layer"];

    CoarsePdnBuilder3D builder(techDb,
                               gridNx,
                               gridNy,
                               powerNets,
                               groundNets,
                               layerOrder,
                               defaultPkgR,
                               bumpLayer);

    // ============================================================
    // 4) Circuit Construction
    // ============================================================

    const Json&        fileConfigJ         = configJ["file"];
    const std::string& defPath             = fileConfigJ["def_path"];
    const std::string& currentSrcPath      = fileConfigJ["current_src_path"];
    const std::string& voltageSrcPath      = fileConfigJ["voltage_src_path"];
    const std::string& voltageLandingLayer = simConfigJ["bump_layer"];

    CircuitGraph graph;
    if (!builder.buildCoarsePdnFromDef(defPath, graph)) {
        std::cerr << "Failed to build 3D coarse PDN graph.\n";
        return 1;
    }

    std::cout << "3D Coarse PDN graph built.\n";
    std::cout << "Nodes:      " << graph.mNodes.size() << "\n";
    std::cout << "MetalRes:   " << graph.mMetalResistors.size() << "\n";
    std::cout << "ViaRes:     " << graph.mViaResistors.size() << "\n";
    std::cout << "PkgRes:     " << graph.mPkgResistors.size() << "\n";

    DecoratorConfig decoratorConfig;
    decoratorConfig.currentConfigPath         = currentSrcPath;
    decoratorConfig.voltageConfigPath         = voltageSrcPath;
    decoratorConfig.voltageSourceLandingLayer = voltageLandingLayer;
    decoratorConfig.defaultPkgR               = defaultPkgR;
    CircuitDecorator decorator(graph, decoratorConfig);
    decorator.build();

    std::cout << "Current/Voltage sources embedded\n";
    std::cout << "Current sources:      " << graph.mIsrcs.size() << "\n";
    std::cout << "Voltage sources:      " << graph.mVsrcs.size() << "\n";

    // Next steps:
    //   - Build sparse conductance matrix G from mMetalResistors +
    //   mViaResistors.
    //   - Treat bump nodes as Dirichlet BCs (VDD/VSS).
    //   - Aggregate load currents per (net,layer,tile) node into I vector.
    //   - Solve G V = I (e.g., with CG/PCG) and extract coarse IR-drop
    //   heatmap.
    return 0;
}

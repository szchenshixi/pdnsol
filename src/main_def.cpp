#include <filesystem>
#include <iostream>
#include <unordered_set>

#include "pdnsol/io/exporter_viz.hpp"
#include "pdnsol/io/parser_def.hpp"
#include "pdnsol/sanitizer/sanitizer_circuit.hpp"
#include "pdnsol/sanitizer/sanitizer_config.hpp"
#include "pdnsol/solver/mna.hpp"
#include "pdnsol/struct/circuit_decorator.hpp"
#include "pdnsol/utils/logging.hpp"

using namespace pdnsol;

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

    for (const auto& j : techConfigJ["tsvs"]) {
        const std::string& name        = j["name"];
        const std::string& bottomLayer = j["bottom_layer"];
        const std::string& topLayer    = j["top_layer"];
        const double       resistance  = j["resistance_ohm"];
        techDb.addTsv(name, bottomLayer, topLayer, resistance);
    }

    // ============================================================
    // 2) PDN Configuration
    // ============================================================

    std::unordered_map<std::string, DecoratorConfig::VSrcProperty> vsrcs;
    // Power nets from SPECIALNETS section
    std::vector<std::string>                                       powerNets;
    for (const auto& j : techConfigJ["power_nets"]) {
        std::string name       = j["name"];
        ScalarType  voltage    = j["voltage_volt"];
        ScalarType  resistance = j["package_resistance_ohm"];
        vsrcs.insert({name, {voltage, resistance}});
        powerNets.push_back(std::move(name));
    }

    // Ground nets from SPECIALNETS section
    std::vector<std::string> groundNets = {"VSS"};
    for (const auto& j : techConfigJ["ground_nets"]) {
        std::string name       = j["name"];
        ScalarType  voltage    = j["voltage_volt"];
        ScalarType  resistance = j["package_resistance_ohm"];
        vsrcs.insert({name, {voltage, resistance}});
        groundNets.push_back(std::move(name));
    }

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

    const Json& simConfigJ    = configJ["simulation"];
    const Json& gridConfigJ   = simConfigJ["grid"];
    const int   defaultGridNx = gridConfigJ["default"]["nx"];
    const int   defaultGridNy = gridConfigJ["default"]["ny"];
    IdString::Map<LayerGridResolution> perLayerGridRes;
    for (const auto& [l, j] : gridConfigJ.items()) {
        IdString layerName         = IdString(l);
        perLayerGridRes[layerName] = LayerGridResolution{j["nx"], j["ny"]};
    }

    CoarsePdnBuilder3D builder(techDb,
                               defaultGridNx,
                               defaultGridNy,
                               perLayerGridRes,
                               powerNets,
                               groundNets,
                               layerOrder);

    // ============================================================
    // 4) Circuit Construction
    // ============================================================

    const Json&        fileConfigJ    = configJ["file"];
    const std::string& defPath        = fileConfigJ["def_path"];
    const std::string& currentSrcPath = fileConfigJ["current_src_path"];
    const std::string& voltageSrcPath = fileConfigJ["voltage_src_path"];
    const std::string& bumpLayer      = simConfigJ["bump_layer"];

    CircuitGraph circ;
    if (!builder.buildCoarsePdnFromDef(defPath, circ)) {
        std::cerr << "Failed to build 3D coarse PDN graph.\n";
        return 1;
    }
    circ.purgeParallelElements();
    circ.purgeIsolatedNodes();

    DecoratorConfig decoratorConfig;
    decoratorConfig.currentConfigPath         = currentSrcPath;
    decoratorConfig.voltageConfigPath         = voltageSrcPath;
    decoratorConfig.voltageSourceLandingLayer = bumpLayer;
    decoratorConfig.voltageSources            = std::move(vsrcs);
    CircuitDecorator decorator(circ, decoratorConfig);
    decorator.build();

    std::cout << "3D Coarse PDN graph built.\n";
    std::cout << "Nodes:      " << circ.mNodes.size() << "\n";
    std::cout << "MetalRes:   " << circ.mMetalResistors.size() << "\n";
    std::cout << "ViaRes:     " << circ.mViaResistors.size() << "\n";
    std::cout << "PkgRes:     " << circ.mPkgResistors.size() << "\n";

    std::cout << "Current/Voltage sources embedded\n";
    std::cout << "Current sources:      " << circ.mIsrcs.size() << "\n";
    std::cout << "Voltage sources:      " << circ.mVsrcs.size() << "\n";

    // Next steps:
    //   - Build sparse conductance matrix G from mMetalResistors +
    //   mViaResistors.
    //   - Treat bump nodes as Dirichlet BCs (VDD/VSS).
    //   - Aggregate load currents per (net,layer,tile) node into I vector.
    //   - Solve G V = I (e.g., with CG/PCG) and extract coarse IR-drop
    //   heatmap.

    // Create checker instance
    CircuitConnectivityChecker checker;

    // Option 1: Get comprehensive diagnostic
    auto diagnostic = checker.checkIsolation(circ);

    // Option 2: Get just isolated nodes
    auto isolatedNodes = checker.findIsolatedNodes(circ);

    // Option 3: Generate full report
    std::string report = checker.generateReport(circ);
    std::cout << report;

    exportCircuitGraphForVizJson(circ, "viz_output/viz.json");

    return 0;
}

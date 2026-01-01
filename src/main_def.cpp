#include <iostream>

#include "pdnsol/io/exporter_viz.hpp"
#include "pdnsol/io/parser_def.hpp"
#include "pdnsol/sanitizer/sanitizer_circuit.hpp"
#include "pdnsol/sanitizer/sanitizer_config.hpp"
#include "pdnsol/solver/mna.hpp"
#include "pdnsol/solver/solver_basic.hpp"
#include "pdnsol/struct/circuit_decorator.hpp"
#include "pdnsol/utils/logging.hpp"
#include "pdnsol/utils/perf_stats.hpp"
#include "pdnsol/viz/heatmap.hpp"

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

    const Json&               simConfigJ     = configJ["simulation"];
    const Json&               gridConfigJ    = simConfigJ["grid"];
    const int                 defaultStrideX = gridConfigJ["default"]["sx"];
    const int                 defaultStrideY = gridConfigJ["default"]["sy"];
    const LayerGridResolution defaultGridRes = {defaultStrideX,
                                                defaultStrideY};

    IdString::Map<LayerGridResolution> perLayerGridRes;
    // Example: stride_X/Y feature is under construction
    // "M10": { "nx": 64, "ny": 64, "sx": -1, "sy": -1 }
    for (const auto& [l, j] : gridConfigJ.items()) {
        IdString  layerName        = IdString(l);
        const int strideX          = j["sx"];
        const int strideY          = j["sy"];
        perLayerGridRes[layerName] = LayerGridResolution{strideX, strideY};
    }

    CoarsePdnBuilder3D builder(techDb,
                               defaultGridRes,
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
        PDN_FATAL("Failed to build 3D coarse PDN graph.");
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

    PDN_INFO("3D Coarse PDN graph built.");
    PDN_INFO("Nodes:      %d", circ.mNodes.size());
    PDN_INFO("MetalRes:   %d", circ.mMetalResistors.size());
    PDN_INFO("ViaRes:     %d", circ.mViaResistors.size());
    PDN_INFO("PkgRes:     %d", circ.mPkgResistors.size());

    PDN_INFO("Current/Voltage sources embedded");
    PDN_INFO("Current sources:      %d", circ.mIsrcs.size());
    PDN_INFO("Voltage sources:      %d", circ.mVsrcs.size());

    // Create checker instance
    CircuitConnectivityChecker checker;
    // Option 1: Get comprehensive diagnostic
    auto                       diagnostic    = checker.checkIsolation(circ);
    // Option 2: Get just isolated nodes
    auto                       isolatedNodes = checker.findIsolatedNodes(circ);
    // Option 3: Generate full report
    std::string                report        = checker.generateReport(circ);
    std::cout << report;

    // Next steps:
    //   - Build sparse conductance matrix G from mMetalResistors +
    //   mViaResistors.
    //   - Treat bump nodes as Dirichlet BCs (VDD/VSS).
    //   - Aggregate load currents per (net,layer,tile) node into I vector.
    //   - Solve G V = I (e.g., with CG/PCG) and extract coarse IR-drop
    //   heatmap.

    {
        PERF_STATS("Solving coarse circuit");
        PDN_INFO("Starting solving coarse circuit with %'d nodes",
                 circ.mNodes.size());
        // 1. Stamp the MNA system
        MNASystem           mna = assembleMNA(circ);
        // 2. Solve the MNA system
        MNASolution         sol = solveMNA(mna);
        // 3. Build heatmap for the chosen net(s)
        IRDropHeatmapConfig heatmapCfg;
        heatmapCfg.vddNominal = 0.8;
        heatmapCfg.vssNominal = 0.0;
        heatmapCfg.width      = 32;
        heatmapCfg.height     = 32;
        HeatmapByNet hm = buildIRDropHeatmapsMultiNet(circ, sol, heatmapCfg);
        // 4. Export
        writeAllHeatmapsToPng(hm, "work_coarse", /*useMaxValue=*/true);
        // 5. (Optional) Export a model layout plot
        exportCircuitGraphForVizJson(circ, "viz_output/viz.json");
    }

    return 0;
}

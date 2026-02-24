#include <iostream>

#include "pdnsol/io/exporter_viz.hpp"
#include "pdnsol/io/parser_config.hpp"
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
    std::string simulationConfigPath = argv[1];

    Config config;
    if (!Config::fromFile(simulationConfigPath, config)) {
        PDN_FATAL("Cannot open file '%s'", simulationConfigPath.c_str());
    }

    // ============================================================
    // 1) Circuit Construction
    // ============================================================

    SimulationConfig simConfig = config.getSimulation();
    NetFilter        f(simConfig.netFilter);

    CircuitGraph circ;
    {
        IdString                  dieName("die0");
        DieConfig                 dieConfig   = config.getDie(dieName);
        IdString::Map<GridConfig> gridConfigs = simConfig.getDieGrids(dieName);

        CircuitGraph       circ0;
        CoarsePdnBuilder3D builder(dieConfig, gridConfigs);

        if (!builder.buildCoarsePdnFromDef(
              dieConfig.files.defPath, circ0, f)) {
            PDN_FATAL("Failed to build 3D coarse PDN graph");
        }
        circ.purgeParallelElements();
        circ.purgeIsolatedNodes();

        // ============================================================
        // 2) Aggregate current/voltage sources into the circuits
        // ============================================================
        CircuitDecorator decorator(circ, dieConfig, f);
        decorator.build();
    }

    PDN_INFO("3D Coarse PDN graph built");
    PDN_INFO("Nodes:      %zu", circ.mNodes.size());
    PDN_INFO("MetalRes:   %zu", circ.mMetalResistors.size());
    PDN_INFO("ViaRes:     %zu", circ.mViaResistors.size());
    PDN_INFO("TsvRes:     %zu", circ.mTsvResistors.size());
    PDN_INFO("PkgRes:     %zu", circ.mPkgResistors.size());

    PDN_INFO("Current/Voltage sources embedded");
    PDN_INFO("Current sources:      %zu", circ.mIsrcs.size());
    PDN_INFO("Voltage sources:      %zu", circ.mVsrcs.size());

    // Create checker instance
    CircuitConnectivityChecker checker;
    // Option 1: Get comprehensive diagnostic
    auto                       diagnostic    = checker.checkIsolation(circ);
    // Option 2: Get just isolated nodes
    auto                       isolatedNodes = checker.findIsolatedNodes(circ);
    // Option 3: Generate full report
    std::string                report        = checker.generateReport(circ);
    std::cout << report;

    // ============================================================
    // 6) Construct the modal nodal analysis (MNA) linear system
    // ============================================================
    //   - Build sparse conductance matrix G from mMetalResistors +
    //   mViaResistors + mTsvResistors + mPkgResistors
    //   - Treat bump nodes as Dirichlet BCs (VDD/VSS)
    //   - Aggregate load currents per (net,layer,tile) node into I vector
    //   - Solve G V = I (e.g., with CG/PCG) and extract coarse IR-drop
    //   heatmap

    {
        PERF_STATS("Solving coarse circuit");
        PDN_INFO("Starting solving coarse circuit with %'d nodes",
                 circ.mNodes.size());
        // 0. Configure the heatmap visualization
        IRDropHeatmapConfig heatmapCfg;
        heatmapCfg.vddNominal = 0.8;
        heatmapCfg.vssNominal = 0.0;
        heatmapCfg.width      = 32;
        heatmapCfg.height     = 32;
        // 1. Stamp the MNA system
        MNASystem    mna      = assembleMNA(circ);
        // 2. Solve the MNA system
        MNASolution  sol      = solveMNA(mna);
        // 3. Build heatmap for the chosen net(s)
        HeatmapByNet hm = buildIRDropHeatmapsMultiNet(circ, sol, heatmapCfg);
        // 4. Export
        writeAllHeatmapsToPng(hm, "work", /*useMaxValue=*/true);
        // 5. (Optional) Export a model layout
        exportCircuitGraphForVizJson(circ, "viz_output/viz.json");
    }

    return 0;
}

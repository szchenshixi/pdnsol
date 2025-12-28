#include "pdnsol/io/exporter_viz.hpp"
#include "pdnsol/io/parser_spef.hpp"
#include "pdnsol/io/parser_spice.hpp"
#include "pdnsol/solver/solver_basic.hpp"
#include "pdnsol/struct/circuit_coarsener.hpp"
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

int main() {
    PERF_STATS("main");
    init();

    // 0. Configure heatmap
    IRDropHeatmapConfig heatmapCfg;
    heatmapCfg.width  = 32;
    heatmapCfg.height = 32;

    // CircuitGraph circ =
    //   parseSpefFile("./test/data/datc-rdf-calibrations-master/calibration/"
    //                 "sky130hd/aes_cipher_top/aes_cipher_top_1.spef");
    // 1. Prepare the circuits
    CircuitGraph fcirc =
      parseSpiceFile("/home/szchenshixi/git_repository/pdnsol_cpp/test/data/"
                     "ibmpg/6/ibmpg6.spice");
    // CircuitGraph
    CoarseModelConfig cfg;
    cfg.tileSizeUm = 200000.0;

    // Suppose your MetalRes::mName for layers are something like:
    // "M1_VDD", "M2_VDD", ..., "M9_VDD", and similarly for VSS.

    // Mark bottom rails accurate:
    // cfg.perLayerMode[IdString("M1_VDD")] = LayerMode::Accurate;
    // cfg.perLayerMode[IdString("M1_VSS")] = LayerMode::Accurate;

    // Mark top metals accurate:
    // cfg.perLayerMode[IdString("M9_VDD")] = LayerMode::Accurate;
    // cfg.perLayerMode[IdString("M9_VSS")] = LayerMode::Accurate;

    // All other layers (M2..M8 for both rails) default to Approximate.

    CircuitCoarsener coarsener(fcirc, cfg);
    CircuitGraph     ccirc = coarsener.build();
    ccirc.purgeParallelElements();

    if (false) {
        PERF_STATS("Solving full circuit");
        PDN_INFO("Starting solving full circuit with %'d nodes",
                 fcirc.mNodes.size());
        // 2. Solve the MNA system
        MNASystem    fmna = assembleMNA(fcirc);
        MNASolution  fsol = solveMNA(fmna);
        // 3. Build heatmap for the chosen net(s)
        HeatmapByNet fhm =
          buildIRDropHeatmapsMultiNet(fcirc, fsol, heatmapCfg);
        // 4. Export
        writeAllHeatmapsToPng(fhm, "work_full", /*useMaxValue=*/true);
    }

    {
        PERF_STATS("Solving coarse circuit");
        PDN_INFO("Starting solving coarse circuit with %'d nodes",
                 ccirc.mNodes.size());
        // 2. Solve the MNA system
        MNASystem    cmna = assembleMNA(ccirc);
        MNASolution  csol = solveMNA(cmna);
        // 3. Build heatmap for the chosen net(s)
        HeatmapByNet chm =
          buildIRDropHeatmapsMultiNet(ccirc, csol, heatmapCfg);
        // 4. Export
        writeAllHeatmapsToPng(chm, "work_coarse", /*useMaxValue=*/true);
        exportCircuitGraphForVizJson(ccirc, "viz_output/viz.json");
    }

    return 0;
}

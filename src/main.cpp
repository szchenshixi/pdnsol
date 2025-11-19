#include "pdnsol/io/parser_spef.hpp"
#include "pdnsol/io/parser_spice.hpp"
#include "pdnsol/solver/solver_basic.hpp"
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
    // CircuitGraph circ =
    //   parseSpefFile("./test/data/datc-rdf-calibrations-master/calibration/"
    //                 "sky130hd/aes_cipher_top/aes_cipher_top_1.spef");
    CircuitGraph circ =
      parseSpiceFile("/home/szchenshixi/git_repository/pdnsol_cpp/test/data/"
                     "ibmpg/6/ibmpg6.spice");
    MNASystem mna = assembleMNA(circ);
    MNASolution sol = solveMNA(mna);

    // 2. Configure heatmap
    IRDropHeatmapConfig cfg;
    cfg.width = 32;
    cfg.height = 32;

    // 3. Build heatmap for the chosen net(s)
    HeatmapByNet hm = buildIRDropHeatmapsMultiNet(circ, sol, cfg);

    // 4. Export
    writeAllHeatmapsToPng(hm, "work", /*useMaxValue=*/true);
    // IdString i("i");
    // IdString ii("ii");
    // PDN_INFO("%s", i.c_str());
    // PDN_INFO("%s", ii.c_str());
    return 0;
}

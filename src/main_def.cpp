#include <iostream>

#include "pdnsol/io/parser_def.hpp"

using namespace pdnsol;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: coarse_pdn_3d <design.def>\n";
        return 1;
    }
    std::string defPath = argv[1];

    // 1) Technology setup
    TechDatabase techDb;

    // NOTE: Use realistic values from your process.
    // Resistivity in Ω·µm, thickness in µm.
    techDb.addLayer("M3", 0.02, 0.4);
    techDb.addLayer("M4", 0.02, 0.4);
    techDb.addLayer("M5", 0.02, 0.6);

    // Example via types (resistances are dummy values)
    techDb.addVia("VIA34", "M3", "M4", 0.001); // 1 mΩ
    techDb.addVia("VIA45", "M4", "M5", 0.001);

    // Example TSV type (extension point)
    techDb.addTsv("TSV_TOP", "M5", "M5", 0.01); // e.g., to backside

    // 2) PDN nets and layers
    std::vector<std::string> powerNets  = {"VDD"};
    std::vector<std::string> groundNets = {"VSS"};

    // Order of metal layers from bottom to top in PDN modeling
    std::vector<std::string> layerOrder = {"M3", "M4", "M5"};

    int    gridNx      = 64;
    int    gridNy      = 64;
    double defaultPkgR = 0.0; // ideal bump → Dirichlet boundary at bump node
    int    bumpLayer   = -1;  // -1 => use topmost layer in layerOrder

    CoarsePdnBuilder3D builder(techDb,
                               gridNx,
                               gridNy,
                               powerNets,
                               groundNets,
                               layerOrder,
                               defaultPkgR,
                               bumpLayer);

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

    // Next steps:
    //   - Build sparse conductance matrix G from mMetalResistors +
    //   mViaResistors.
    //   - Treat bump nodes as Dirichlet BCs (VDD/VSS).
    //   - Aggregate load currents per (net,layer,tile) node into I vector.
    //   - Solve G V = I (e.g., with CG/PCG) and extract coarse IR-drop
    //   heatmap.

    return 0;
}

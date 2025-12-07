#include <filesystem>
#include <iostream>

#include "pdnsol/io/parser_def.hpp"
#include "pdnsol/struct/circuit_decorator.hpp"
#include "pdnsol/utils/logging.hpp"

using namespace pdnsol;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: coarse_pdn_3d <design.def>\n";
        return 1;
    }
    std::string defPath = argv[1];

    // ============================================================
    // 1) Technology Database Setup
    // ============================================================
    TechDatabase techDb;

    // Metal Layers - Add ALL layers from DEF
    // Format: addLayer(layer_name, resistivity_Ω·µm, thickness_µm)
    techDb.addLayer("met1", 0.0300, 0.2000);
    techDb.addLayer("met2", 0.0300, 0.2000);
    techDb.addLayer("met3", 0.0300, 0.2000);
    techDb.addLayer("met4", 0.0200, 0.4000);
    techDb.addLayer("met5", 0.0200, 0.4000);

    // Vias - Add ALL vias from DEF VIAS section
    // Format: addVia(via_name, bottom_layer, top_layer, resistance_Ω)
    techDb.addVia("via_1600x480", "met1", "met2", 0.000100);   // 150x150
    techDb.addVia("via2_1600x480", "met2", "met3", 0.000100);  // 200x200
    techDb.addVia("via3_1600x480", "met3", "met4", 0.000100);  // 200x200
    techDb.addVia("via4_1600x1600", "met4", "met5", 0.000100); // 800x800

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
    // Includes ALL metal layers from via definitions
    std::vector<std::string> layerOrder = {
      "met1", "met2", "met3", "met4", "met5"};

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

    if (std::filesystem::exists("../test/data/current.json")) {
        DecoratorConfig decoratorConfig;
        decoratorConfig.currentConfigPath = "../test/data/current.json";
        CircuitDecorator decorator(graph, decoratorConfig);
        decorator.build();
    } else if (std::filesystem::exists("./test/data/current.json")) {
        DecoratorConfig decoratorConfig;
        decoratorConfig.currentConfigPath = "./test/data/current.json";
        CircuitDecorator decorator(graph, decoratorConfig);
        decorator.build();
    } else {
        PDN_INFO("Cannot find current source definition. Skip.");
    }

    std::cout << "Current sources embedded\n";
    std::cout << "Current sources:      " << graph.mIsrcs.size() << "\n";
    return 0;
}

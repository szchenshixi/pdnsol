#include "pdnsol/io/parser_spef.hpp"
#include "pdnsol/solver/solver_basic.hpp"
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

int main() {
    init();
    CircuitGraph circuit =
      parseSpefFile("./test/data/datc-rdf-calibrations-master/calibration/"
                    "sky130hd/aes_cipher_top/aes_cipher_top_1.spef");
    MNASystem mna = assembleMNA(circuit);
    MNASolution sol = solveMNA(mna);

    // IdString i("i");
    // IdString ii("ii");
    // PDN_INFO("%s", i.c_str());
    // PDN_INFO("%s", ii.c_str());
    return 0;
}

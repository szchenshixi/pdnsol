#include <iostream>

#include "pdnsol/utils/id_string.hpp"
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
    IdString i("aa");
    IdString ii("bb");
    PDN_INFO("%s", i.c_str());
    PDN_INFO("%s", ii.c_str());
    return 0;
}

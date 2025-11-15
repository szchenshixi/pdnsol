#include <gtest/gtest.h>

#include "pdnsol/utils/logging.hpp"

using namespace pdnsol;

void init() {
    // Initialize the logging facility
    Logger::instance().init(Logger::Level::Debug, // Log level
                            "test.log",           // Log file path
                            5 * 1024 * 1024,      // File size
                            3,                    // Number of files
                            true);                // Print to console
}

int main(int argc, char** argv) {
    init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

# project(PdnSolTest LANGUAGE CXX) find_package(GTest REQUIRED)

add_executable(PdnSol-test src/main_test.cpp src/id_string_test.cpp)
target_link_libraries(PdnSol-test PRIVATE PdnSol_lib GTest::gtest GTest::gmock)
target_compile_features(PdnSol-test PRIVATE cxx_std_20)

# [GTest] Discover all test cases from a target gtest_discover_tests(
# PdnSol-test EXTRA_ARGS --colour-mode ansi WORKING_DIRECTORY
# ${CMAKE_SOURCE_DIR})

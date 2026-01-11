cmake_minimum_required(VERSION 3.22)

# In-source build guard
include(cmake/prelude.cmake)

project(
  PdnSol
  VERSION 0.1.0
  DESCRIPTION "Test repository for project utilities"
  HOMEPAGE_URL ""
  LANGUAGES C CXX)

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)
include(cmake/options.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link to the address sanitizer when build in a Debug mode
# add_compile_options($<$<CONFIG:Debug>:-fsanitize=address>)
# add_link_options($<$<CONFIG:Debug>:-fsanitize=address>)

# ---- Find external projects  ----

include(cmake/find-3rdparty.cmake)

# ---- Declare library ----
# I. util_lib

add_library(
  util_lib STATIC src/utils/logging.cpp src/utils/id_string.cpp
                  src/utils/perf_stats.cpp src/utils/fixed_point_number.cpp)
target_link_libraries(util_lib PRIVATE plog::plog)
target_include_directories(util_lib PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(util_lib PRIVATE cxx_std_20)

# II. pdnsol_lib

add_library(
  pdnsol_lib STATIC
  src/io/tech_db.cpp
  src/io/exporter_viz.cpp
  src/io/parser_def.cpp
  src/io/parser_config.cpp
  src/io/parser_utils.cpp
  src/io/parser_spice.cpp
  src/io/parser_spef.cpp
  src/viz/heatmap.cpp
  src/struct/circuit.cpp
  src/struct/circuit_coarsener.cpp
  src/struct/circuit_decorator.cpp
  src/sanitizer/sanitizer_config.cpp
  src/sanitizer/sanitizer_circuit.cpp
  src/solver/mna.cpp
  src/solver/solver_basic.cpp)

target_include_directories(pdnsol_lib PUBLIC ${PROJECT_SOURCE_DIR}/include
                                             ${PROJECT_BINARY_DIR})
target_link_libraries(pdnsol_lib PUBLIC fmt::fmt Eigen3::Eigen
                                        nlohmann_json::nlohmann_json util_lib)
target_compile_features(pdnsol_lib PUBLIC cxx_std_20)

# III. ohmify_lib

# add_library(ohmify_lib STATIC src/io/parser_def.cpp)
# target_include_directories(ohmify_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
# target_link_libraries(ohmify_lib PUBLIC util_lib)
# target_compile_features(ohmify_lib PUBLIC cxx_std_20)

# ---- Declare executable ----

# Debug: Parse def
add_executable(pdnsol_def src/main_def.cpp)
target_link_libraries(pdnsol_def PRIVATE pdnsol_lib)

add_executable(pdnsol_exe src/main.cpp)
add_executable(pdnsol::exe ALIAS pdnsol_exe)

set_target_properties(
  pdnsol_exe
  PROPERTIES LINK_FLAGS -no-pie
             OUTPUT_NAME pdnsol
             EXPORT_NAME exe)

target_compile_features(pdnsol_exe PRIVATE cxx_std_20)
target_link_libraries(pdnsol_exe PRIVATE pdnsol_lib CLI11::CLI11)
# target_include_directories(pdnsol_exe PRIVATE ${PROJECT_SOURCE_DIR}/src)

# ---- Install rules ----

# This project is not intended to be installed or run out-from-the-source-tree
# if(NOT CMAKE_SKIP_INSTALL_RULES) include(cmake/install-rules.cmake) endif()

# ---- Unit Tests ----
if(PDN_SOL_BUILD_TESTING)
  message("Entering test...")
  add_subdirectory(test)
endif()

# ---- Developer mode ----
if(NOT PDN_SOL_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(AUTHOR_WARNING "Developer mode is intended for developers of PdnSol")
endif()

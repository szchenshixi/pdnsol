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

add_library(
  PdnSol_lib OBJECT
  src/utils/logging.cpp
  src/utils/id_string.cpp
  src/io/parser_def.cpp
  src/io/parser_utils.cpp
  src/io/parser_spice.cpp
  src/io/parser_spef.cpp
  src/struct/circuit.cpp
  src/solver/mna.cpp)

target_include_directories(PdnSol_lib PUBLIC ${PROJECT_SOURCE_DIR}/include
                                             ${PROJECT_BINARY_DIR})

target_link_libraries(PdnSol_lib PUBLIC fmt::fmt plog::plog Eigen3::Eigen)
target_compile_features(PdnSol_lib PUBLIC cxx_std_20)

# ---- Declare executable ----

add_executable(PdnSol_exe src/main.cpp)
add_executable(PdnSol::exe ALIAS PdnSol_exe)

set_target_properties(
  PdnSol_exe
  PROPERTIES LINK_FLAGS -no-pie
             OUTPUT_NAME PdnSol
             EXPORT_NAME exe)

target_compile_features(PdnSol_exe PRIVATE cxx_std_20)
target_link_libraries(PdnSol_exe PRIVATE PdnSol_lib CLI11::CLI11)
# target_include_directories(PdnSol_exe PRIVATE ${PROJECT_SOURCE_DIR}/src)

# ---- Install rules ----

# This project is not intended to be installed or run out-from-the-source-tree
# if(NOT CMAKE_SKIP_INSTALL_RULES) include(cmake/install-rules.cmake) endif()

# ---- Unit Tests ----
if(PDN_SOL_BUILD_TESTING)
  message("Entering test...")
  add_subdirectory(test)
endif()

# ---- Developer mode ----
if(NOT PdnSol_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(AUTHOR_WARNING "Developer mode is intended for developers of PdnSol")
endif()

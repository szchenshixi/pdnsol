# [Ref] A pretty version of all Boost modules
# https://stackoverflow.com/questions/29989512/where-can-i-find-the-list-of-boost-component-that-i-can-use-in-cmake
# find_package(Boost REQUIRED COMPONENTS ALL)
# find_package(Boost REQUIRED)

# Needs CMake 3.14 or above
include(FetchContent)
# -------------------------------------------------------------------
# fmt
# message(NOTICE "Fetching fmt...")
set(FMT_TEST OFF CACHE BOOL "")
set(FMT_INSTALL OFF CACHE BOOL "")
set(FMT_HEADERS "")
set(FMT_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/fmt)
if(EXISTS ${FMT_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded fmt: ${FMT_ROOT}")
    FetchContent_Declare(
        fmt
        SYSTEM
        SOURCE_DIR ${FMT_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading fmt...")
    FetchContent_Declare(
        fmt
        SYSTEM
        URL https://github.com/fmtlib/fmt/archive/refs/tags/9.1.0.tar.gz
        DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/3rdparty/downloads
        DOWNLOAD_NAME fmt-v9.1.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_DIR ${FMT_ROOT}
        URL_HASH MD5=21fac48cae8f3b4a5783ae06b443973a
        QUIET)
endif()
# -------------------------------------------------------------------
# CLI11
# message(NOTICE "Fetching CLI11...")
set(CLI11_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/CLI11)
if(EXISTS ${CLI11_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded CLI11: ${CLI11_ROOT}")
    FetchContent_Declare(
        CLI11
        SYSTEM
        SOURCE_DIR ${CLI11_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading CLI11...")
    FetchContent_Declare(
        CLI11
        SYSTEM
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11
        SOURCE_DIR ${CLI11_ROOT}
        # GIT_TAG v2.3.2
        GIT_TAG main
        GIT_SHALLOW ON)
endif()
# -------------------------------------------------------------------
# plog
# message(NOTICE "Fetching plog...")
set(PLOG_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/plog)
if(EXISTS ${PLOG_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded Plog: ${PLOG_ROOT}")
    FetchContent_Declare(
        plog
        SYSTEM
        SOURCE_DIR ${PLOG_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading plog...")
    FetchContent_Declare(
        plog
        SYSTEM
        URL https://github.com/SergiusTheBest/plog/archive/refs/tags/1.1.11.tar.gz
        DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/3rdparty/downloads
        DOWNLOAD_NAME plog-1.1.11.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_DIR ${PLOG_ROOT}
        URL_HASH MD5=755990d33e8d26cd24c179e57acde5e3
        QUIET)
endif()
# -------------------------------------------------------------------
# nlohmann::json
# message(NOTICE "Fetching nlohmann::json...")
set(NLOHMANN_JSON_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/json)
if(EXISTS ${NLOHMANN_JSON_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded nlohmann_json: ${NLOHMANN_JSON_ROOT}")
    FetchContent_Declare(
        nlohmann_json
        SYSTEM
        SOURCE_DIR ${NLOHMANN_JSON_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading nlohmann_json...")
    FetchContent_Declare(
        nlohmann_json
        SYSTEM
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/3rdparty/downloads
        DOWNLOAD_NAME json-3.11.3.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_DIR ${NLOHMANN_JSON_ROOT}
        URL_HASH MD5=c23a33f04786d85c29fda8d16b5f0efd
        QUIET)
endif()
# -------------------------------------------------------------------
# GoogleTest
# message(NOTICE "Fetching gtest...")
set(GTEST_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/googletest)
if(EXISTS ${GTEST_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded gtest: ${GTEST_ROOT}")
    FetchContent_Declare(
        gtest
        SYSTEM
        SOURCE_DIR ${GTEST_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading gtest...")
    FetchContent_Declare(
        gtest
        SYSTEM
        URL https://github.com/google/googletest/releases/download/v1.16.0/googletest-1.16.0.tar.gz
        DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/3rdparty/downloads
        DOWNLOAD_NAME googletest-1.16.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_DIR ${GTEST_ROOT}
        URL_HASH MD5=9a75eb2ac97300cdb8b65b1a5833f411
        QUIET)
endif()
# -------------------------------------------------------------------
# Eigen5
# message(NOTICE "Fetching Eigen5...")
set(EIGEN5_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/Eigen5)
if(EXISTS ${EIGEN5_ROOT}/CMakeLists.txt)
    message(NOTICE "Using pre-downloaded Eigen5: ${EIGEN5_ROOT}")
    FetchContent_Declare(
        eigen5
        SYSTEM
        SOURCE_DIR ${EIGEN5_ROOT}
        DOWNLOAD_COMMAND "")
else()
    message(NOTICE "Downloading gtest...")
    FetchContent_Declare(
        eigen5
        SYSTEM
        URL https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.tar.gz
        DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/3rdparty/downloads
        DOWNLOAD_NAME eigen-5.0.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_DIR ${EIGEN5_ROOT}
        URL_HASH MD5=27cabe8e2f67230a5da6504775725ed7
        QUIET)
endif()



fetchcontent_makeavailable(fmt CLI11 plog nlohmann_json gtest eigen5)


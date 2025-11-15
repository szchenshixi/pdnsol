# set(CMAKE_GENERATOR
#     "Unix Makefiles"
#     CACHE INTERNAL "")
# Set a default build type if none was specified
set(default_build_type "Release")
if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    set(default_build_type "Debug")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(
        STATUS
            "Setting build type to '${default_build_type}' as none was specified."
    )
    set(CMAKE_BUILD_TYPE
        "${default_build_type}"
        CACHE STRING "Choose the type of build" FORCE)
    # set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
    # "MinSizeRel" "RelWithDebInfo")
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS
    ON
    CACHE BOOL "" FORCE)
set(CMAKE_CXX_STANDARD 17)
set(PDN_SOL_BUILD_MAIN
    ON
    CACHE BOOL "Build PdnSol executable")
set(PDN_SOL_BUILD_TESTING
    OFF
    CACHE BOOL "Build PdnSol unit tests")

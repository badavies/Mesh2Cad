cmake_minimum_required(VERSION 3.25)

# vcpkg is used only when asked for (the presets set MESH2CAD_USE_VCPKG=ON) and only when VCPKG_ROOT
# points at an installation. Otherwise cmake/Mesh2CadDependencies.cmake downloads the dependencies.
# This must run before project().
option(MESH2CAD_USE_VCPKG "Get dependencies from vcpkg (needs the VCPKG_ROOT environment variable)" OFF)
if(MESH2CAD_USE_VCPKG AND NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg toolchain file")
    else()
        message(WARNING
            "MESH2CAD_USE_VCPKG is ON but VCPKG_ROOT does not point at a vcpkg installation. "
            "Falling back to downloading the dependencies from GitHub. Set VCPKG_ROOT and reset "
            "the CMake cache to use vcpkg.")
    endif()
endif()

project(Mesh2Cad
    VERSION 1.0.0
    DESCRIPTION "Scan-to-CAD reverse engineering: meshes in, editable CAD solids out"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(MESH2CAD_BUILD_TESTS "Build the test suite" ON)

include(Mesh2CadBuild)
include(Mesh2CadDependencies)

add_subdirectory(src)

if(MESH2CAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Mesh2CadPackaging)

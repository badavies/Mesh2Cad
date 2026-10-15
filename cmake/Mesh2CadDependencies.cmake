# Third-party libraries (see THIRD_PARTY.md).
#
# With a vcpkg preset (CMakePresets.json) the libraries come from vcpkg and find_package() finds
# them. Without vcpkg - for example CLion's default "Debug" profile - FetchContent downloads the
# same pinned versions on the first configure, so the project builds with no setup (ADR 0020).
# FIND_PACKAGE_ARGS makes FetchContent try find_package() first, so vcpkg always wins.
#
# Header-only libraries that are downloaded (Eigen, CGAL, Boost) are not added as CMake
# subprojects: SOURCE_SUBDIR points at a folder that does not exist, and we define the targets
# ourselves. Their own CMake files configure test suites and installers we do not need.

include(FetchContent)

set(m2c_no_cmake SOURCE_SUBDIR m2c-header-only)

# ---- nlohmann/json ------------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 3.11 CONFIG)

# ---- Eigen (ADR 0022) ---------------------------------------------------------------------------
FetchContent_Declare(Eigen3
    URL      https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
    ${m2c_no_cmake}
    FIND_PACKAGE_ARGS 3.3 CONFIG NAMES Eigen3)

# ---- miniz (ADR 0024) ---------------------------------------------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "miniz: build examples")
set(BUILD_FUZZERS OFF CACHE BOOL "miniz: build fuzzers")
set(BUILD_TESTS OFF CACHE BOOL "miniz: build tests")
set(INSTALL_PROJECT OFF CACHE BOOL "miniz: install")
# miniz 3.0.2 asks for CMake 3.4 compatibility, which CMake 4 refuses without this. Older CMake
# versions ignore the variable.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS CONFIG)

set(m2c_dependencies nlohmann_json Eigen3 miniz)

if(MESH2CAD_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.1
        GIT_SHALLOW    TRUE
        FIND_PACKAGE_ARGS 3 CONFIG)
    list(APPEND m2c_dependencies Catch2)
endif()

FetchContent_MakeAvailable(${m2c_dependencies})
unset(CMAKE_POLICY_VERSION_MINIMUM)

if(NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${eigen3_SOURCE_DIR}")
endif()
if(NOT TARGET miniz::miniz)
    add_library(miniz::miniz ALIAS miniz)
endif()

if(MESH2CAD_BUILD_TESTS)
    # A fetched Catch2 keeps Catch.cmake (catch_discover_tests) in its extras folder;
    # an installed one adds it to the module path itself.
    if(catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()

foreach(dep IN LISTS m2c_dependencies)
    if(${dep}_FOUND)
        message(STATUS "Mesh2Cad: ${dep} found by find_package (vcpkg or system)")
    else()
        message(STATUS "Mesh2Cad: ${dep} downloaded")
    endif()
endforeach()

# ---- CGAL and Boost (ADR 0022) ------------------------------------------------------------------
# m2c_cgal_deps carries everything the CGAL adapter needs. An installed CGAL (vcpkg or system)
# brings Boost and GMP with it. Otherwise CGAL and the Boost headers are downloaded (about 50 MB,
# once) and CGAL is told to use Boost.Multiprecision instead of GMP.
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE TRUE)
find_package(CGAL 5.6 CONFIG QUIET)
add_library(m2c_cgal_deps INTERFACE)
if(CGAL_FOUND)
    message(STATUS "Mesh2Cad: CGAL ${CGAL_VERSION} found by find_package (vcpkg or system)")
    target_link_libraries(m2c_cgal_deps INTERFACE CGAL::CGAL Eigen3::Eigen)
else()
    FetchContent_Declare(cgal
        URL      https://github.com/CGAL/cgal/releases/download/v6.0.1/CGAL-6.0.1-library.tar.xz
        URL_HASH SHA256=c752737f91d1af71fa96038f0e37945ce82a5f1fffb6200172cfcdd77755a356
        ${m2c_no_cmake})
    FetchContent_Declare(boost_headers
        URL      https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.xz
        URL_HASH SHA256=a4d99d032ab74c9c5e76eddcecc4489134282245fffa7e079c5804b92b45f51d
        ${m2c_no_cmake})
    FetchContent_MakeAvailable(cgal boost_headers)
    message(STATUS "Mesh2Cad: CGAL 6.0.1 and Boost 1.86 headers downloaded")

    find_package(Threads REQUIRED)
    target_include_directories(m2c_cgal_deps SYSTEM INTERFACE
        "${cgal_SOURCE_DIR}/include" "${boost_headers_SOURCE_DIR}")
    target_compile_definitions(m2c_cgal_deps INTERFACE CGAL_DISABLE_GMP=1)
    target_link_libraries(m2c_cgal_deps INTERFACE Threads::Threads Eigen3::Eigen)
    # The flags CGAL's own CMake package would add (CGAL_SetupCGALDependencies.cmake).
    if(MSVC)
        target_compile_definitions(m2c_cgal_deps INTERFACE
            _SCL_SECURE_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS)
        target_compile_options(m2c_cgal_deps INTERFACE /fp:strict /fp:except- /bigobj)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(m2c_cgal_deps INTERFACE -frounding-math)
    endif()
endif()
target_compile_definitions(m2c_cgal_deps INTERFACE CGAL_EIGEN3_ENABLED)
if(MINGW)
    # CGAL translation units exceed the default COFF section limit.
    target_compile_options(m2c_cgal_deps INTERFACE -Wa,-mbig-obj)
endif()

# ---- Qt 6 (ADR 0023): optional, the desktop app is built only when Qt is found -----------------
set(MESH2CAD_BUILD_APP "AUTO" CACHE STRING "Build the Qt desktop app: AUTO, ON or OFF")
set_property(CACHE MESH2CAD_BUILD_APP PROPERTY STRINGS AUTO ON OFF)
set(M2C_HAVE_QT OFF)
if(NOT MESH2CAD_BUILD_APP STREQUAL "OFF")
    if(MESH2CAD_BUILD_APP STREQUAL "ON")
        find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Network)
    else()
        find_package(Qt6 6.4 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets Network)
    endif()
    if(Qt6_FOUND)
        set(M2C_HAVE_QT ON)
        message(STATUS "Mesh2Cad: Qt ${Qt6_VERSION} found; building the desktop app")
    else()
        message(STATUS "Mesh2Cad: Qt 6 not found; skipping the desktop app (see README, "
                       "'Building the desktop app')")
    endif()
endif()

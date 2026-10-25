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
# miniz 3.0.2 asks for CMake 3.4 compatibility, which CMake 4 refuses without this, and CMake 3.31
# and later warn about anything below 3.10. Older CMake versions ignore the variable.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS CONFIG)

# ---- oneTBB (ADR 0036) --------------------------------------------------------------------------
# Built as a subproject when no installed copy is found: a shared library, as every installed copy
# is, since oneTBB does not support static builds. miniz's BUILD_SHARED_LIBS option (OFF) would
# make it static, so the variable is ON while oneTBB configures, below. m2c_copy_runtime puts the
# DLL next to each Windows executable, and Mesh2CadPackaging.cmake installs it; oneTBB's own
# install rules (headers, CMake files) are off, and so is tbbmalloc, which nothing uses.
# v2022.2.0 is the first to configure with MinGW on Windows (it probed the assembler through
# /dev/null). With MinGW, oneTBB's profiling hooks (on in Debug) do not compile, as they pass
# char names where Windows builds take wchar_t; nothing here uses them. Its headers are system
# headers, as an installed copy's are: their Windows-only code fails core's warning flags.
# onetbb-cmp0148.cmake moves it off a policy setting that CMake 4 warns about. The signature check
# is for DLLs oneTBB loads by itself at run time (tbbbind, which needs hwloc and is not built) and
# wants them Authenticode-signed; it is off by default, and saying so stops oneTBB warning.
set(TBB_TEST OFF CACHE BOOL "oneTBB: build tests")
set(TBB_STRICT OFF CACHE BOOL "oneTBB: treat warnings as errors")
set(TBB_VERIFY_DEPENDENCY_SIGNATURE OFF CACHE BOOL "oneTBB: verify signatures of loaded DLLs")
if(MINGW)
    add_compile_definitions(TBB_USE_PROFILING_TOOLS=0)
endif()
FetchContent_Declare(TBB
    GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
    GIT_TAG        v2022.3.0
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/onetbb-cmp0148.cmake"
    SYSTEM
    FIND_PACKAGE_ARGS 2021.5 CONFIG)

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

# Normal variables, which win over the cache entries an older configure (or miniz) left behind,
# and only while oneTBB configures.
set(BUILD_SHARED_LIBS ON)
set(TBB_INSTALL OFF)
set(TBBMALLOC_BUILD OFF)
FetchContent_MakeAvailable(TBB)
unset(BUILD_SHARED_LIBS)
unset(TBB_INSTALL)
unset(TBBMALLOC_BUILD)
list(APPEND m2c_dependencies TBB)
set(m2c_tbb_fetched OFF)
if(NOT TBB_FOUND)
    set(m2c_tbb_fetched ON)
endif()

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
    # Even as big objects (Mesh2CadBuild.cmake), unoptimised Debug CGAL objects are too big for
    # binutils 2.39 (the MinGW 13.1 that ships with Qt); -Og shrinks them and keeps them debuggable.
    target_compile_options(m2c_cgal_deps INTERFACE $<$<CONFIG:Debug>:-Og>)
endif()

# ---- Building a dependency from source at configure time (ADR 0026) ----------------------------
# m2c_build_from_source(<name> SOURCE_DIR <dir> CONFIG <config> ARGS <cmake arguments...>)
#
# Configures, builds and installs a CMake project into <build>/_deps/<name>-install with this
# project's generator and compilers, while this project configures. It runs once per build folder:
# later configures find the stamp file and skip it. Sets <name>_INSTALL_DIR in the caller.
function(m2c_build_from_source name)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;CONFIG" "ARGS" ${ARGN})
    set(prefix "${CMAKE_BINARY_DIR}/_deps/${name}-install")
    set(build "${CMAKE_BINARY_DIR}/_deps/${name}-build")
    set(${name}_INSTALL_DIR "${prefix}" PARENT_SCOPE)
    string(SHA1 settings "${ARG_CONFIG};${ARG_ARGS};${CMAKE_CXX_COMPILER}")
    set(stamp "${prefix}/m2c-${settings}.stamp")
    if(EXISTS "${stamp}")
        return()
    endif()
    message(STATUS "Mesh2Cad: building ${name} from source into ${prefix}. This happens once "
                   "per build folder and can take 15 to 40 minutes.")

    set(generator -G "${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND generator -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND generator -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    set(tools "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    # This project enables C++ only, so find the matching C compiler next to the C++ one.
    set(c_compiler "${CMAKE_C_COMPILER}")
    if(NOT c_compiler AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        get_filename_component(dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(file "${CMAKE_CXX_COMPILER}" NAME)
        string(REPLACE "g++" "gcc" file "${file}")
        set(c_compiler "${dir}/${file}")
    elseif(NOT c_compiler AND MSVC)
        set(c_compiler "${CMAKE_CXX_COMPILER}")
    endif()
    if(c_compiler)
        list(APPEND tools "-DCMAKE_C_COMPILER=${c_compiler}")
    endif()
    foreach(var CMAKE_MAKE_PROGRAM CMAKE_TOOLCHAIN_FILE)
        if(${var})
            list(APPEND tools "-D${var}=${${var}}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${ARG_SOURCE_DIR}" -B "${build}" ${generator} ${tools}
                "-DCMAKE_BUILD_TYPE=${ARG_CONFIG}" "-DCMAKE_INSTALL_PREFIX=${prefix}" ${ARG_ARGS}
        RESULT_VARIABLE failed)
    if(NOT failed)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${ARG_CONFIG}" --parallel
            RESULT_VARIABLE failed)
    endif()
    if(NOT failed)
        execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build}" --config "${ARG_CONFIG}"
                        RESULT_VARIABLE failed)
    endif()
    if(failed)
        message(FATAL_ERROR "Building ${name} from source failed (see the output above; the build "
                            "folder is ${build}). Install ${name} and point CMAKE_PREFIX_PATH at it, "
                            "or use a vcpkg preset.")
    endif()
    file(WRITE "${stamp}" "")
endfunction()

# MSVC cannot mix Debug and Release runtimes, so a source build matches this build's
# configuration there; GCC and MinGW always get an optimised build.
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(m2c_source_build_config Debug)
else()
    set(m2c_source_build_config Release)
endif()

# ---- Open CASCADE (ADR 0026) --------------------------------------------------------------------
# vcpkg, a system package or CMAKE_PREFIX_PATH first; otherwise the pinned release is built from
# source with only the toolkits the OCCT adapter needs (no visualisation drivers, no Draw).
find_package(OpenCASCADE CONFIG QUIET)
if(OpenCASCADE_FOUND)
    message(STATUS "Mesh2Cad: Open CASCADE ${OpenCASCADE_VERSION} found by find_package "
                   "(vcpkg or system)")
else()
    FetchContent_Declare(occt
        GIT_REPOSITORY https://github.com/Open-Cascade-SAS/OCCT.git
        GIT_TAG        V8_0_1
        GIT_SHALLOW    TRUE
        ${m2c_no_cmake})
    FetchContent_MakeAvailable(occt)
    set(occt_args
        -DBUILD_LIBRARY_TYPE=Shared
        -DBUILD_MODULE_Draw=OFF -DBUILD_MODULE_Visualization=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF
        "-DBUILD_ADDITIONAL_TOOLKITS=TKDESTEP"
        -DUSE_FREETYPE=OFF -DUSE_OPENGL=OFF -DUSE_GLES2=OFF -DUSE_XLIB=OFF -DUSE_TK=OFF
        -DUSE_FREEIMAGE=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF -DINSTALL_TEST_CASES=OFF -DINSTALL_DIR_LAYOUT=Unix)
    if(MINGW)
        list(APPEND occt_args "-DCMAKE_CXX_FLAGS=-Wa,-mbig-obj")
    endif()
    if(NOT WIN32)
        list(APPEND occt_args "-DCMAKE_INSTALL_RPATH=\$ORIGIN")
    endif()
    m2c_build_from_source(occt SOURCE_DIR "${occt_SOURCE_DIR}"
                          CONFIG ${m2c_source_build_config} ARGS ${occt_args})
    find_package(OpenCASCADE CONFIG REQUIRED PATHS "${occt_INSTALL_DIR}" NO_DEFAULT_PATH
                 NO_CMAKE_FIND_ROOT_PATH)
    message(STATUS "Mesh2Cad: Open CASCADE ${OpenCASCADE_VERSION} built from source")
endif()
# The toolkits the OCCT adapter uses. OCCT 7.8 merged the STEP toolkits into TKDESTEP.
set(m2c_occt_toolkits
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKShHealing
    TKMesh TKXSBase TKFillet)
if(TARGET TKDESTEP)
    list(APPEND m2c_occt_toolkits TKDESTEP TKDE)
else()
    list(APPEND m2c_occt_toolkits TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209)
endif()
add_library(m2c_occt_deps INTERFACE)
target_link_libraries(m2c_occt_deps INTERFACE ${m2c_occt_toolkits})
if(WIN32)
    # OCCT's headers use M_PI, which MinGW's <cmath> hides in strict ISO mode.
    target_compile_definitions(m2c_occt_deps INTERFACE _USE_MATH_DEFINES)
endif()
if(OpenCASCADE_INCLUDE_DIR)
    target_include_directories(m2c_occt_deps SYSTEM INTERFACE "${OpenCASCADE_INCLUDE_DIR}")
endif()

# ---- OpenSubdiv (ADR 0026) ----------------------------------------------------------------------
# Only the CPU library (Far) is used. vcpkg's package config is used as is. Elsewhere the library
# is looked up directly, because Ubuntu's config names a static library the package does not ship;
# when it is not found, the pinned release is built from source as a static library.
add_library(m2c_osd_deps INTERFACE)
if(VCPKG_TOOLCHAIN)
    find_package(OpenSubdiv 3.5 CONFIG REQUIRED)
    message(STATUS "Mesh2Cad: OpenSubdiv ${OpenSubdiv_VERSION} from vcpkg")
else()
    find_path(M2C_OSD_INCLUDE_DIR opensubdiv/version.h)
    find_library(M2C_OSD_CPU_LIBRARY NAMES osdCPU)
    if(M2C_OSD_INCLUDE_DIR AND M2C_OSD_CPU_LIBRARY)
        target_include_directories(m2c_osd_deps SYSTEM INTERFACE "${M2C_OSD_INCLUDE_DIR}")
        target_link_libraries(m2c_osd_deps INTERFACE "${M2C_OSD_CPU_LIBRARY}")
        message(STATUS "Mesh2Cad: OpenSubdiv found at ${M2C_OSD_CPU_LIBRARY}")
    else()
        FetchContent_Declare(opensubdiv
            GIT_REPOSITORY https://github.com/PixarAnimationStudios/OpenSubdiv.git
            GIT_TAG        v3_5_0
            GIT_SHALLOW    TRUE
            ${m2c_no_cmake})
        FetchContent_MakeAvailable(opensubdiv)
        set(osd_args
            -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DMSVC_STATIC_CRT=OFF
            -DNO_EXAMPLES=ON -DNO_TUTORIALS=ON -DNO_REGRESSION=ON -DNO_DOC=ON -DNO_TESTS=ON
            -DNO_GLTESTS=ON -DNO_PTEX=ON -DNO_OMP=ON -DNO_TBB=ON -DNO_CUDA=ON -DNO_OPENCL=ON
            -DNO_CLEW=ON -DNO_OPENGL=ON -DNO_METAL=ON -DNO_DX=ON -DNO_GLEW=ON -DNO_GLFW=ON
            -DNO_GLFW_X11=ON)
        m2c_build_from_source(opensubdiv SOURCE_DIR "${opensubdiv_SOURCE_DIR}"
                              CONFIG ${m2c_source_build_config} ARGS ${osd_args})
        find_package(OpenSubdiv CONFIG REQUIRED PATHS "${opensubdiv_INSTALL_DIR}" NO_DEFAULT_PATH
                     NO_CMAKE_FIND_ROOT_PATH)
        message(STATUS "Mesh2Cad: OpenSubdiv ${OpenSubdiv_VERSION} built from source")
    endif()
endif()
if(TARGET OpenSubdiv::osdCPU_static)
    target_link_libraries(m2c_osd_deps INTERFACE OpenSubdiv::osdCPU_static)
elseif(TARGET OpenSubdiv::osdCPU)
    target_link_libraries(m2c_osd_deps INTERFACE OpenSubdiv::osdCPU)
endif()
# OpenSubdiv's exported targets carry no include directory; its config sets this variable.
if(OpenSubdiv_INCLUDE_DIR)
    target_include_directories(m2c_osd_deps SYSTEM INTERFACE "${OpenSubdiv_INCLUDE_DIR}")
endif()
if(WIN32)
    # OpenSubdiv's limit masks (primvarRefiner.h) use M_PI, which MSVC's <cmath> defines only on
    # request.
    target_compile_definitions(m2c_osd_deps INTERFACE _USE_MATH_DEFINES)
endif()

# ---- Instant Meshes (ADR 0009) ------------------------------------------------------------------
# Not packaged anywhere: its field and extraction sources are compiled into the quad adapter
# (src/adapters/quad). Submodules are not fetched; the two small headers it needs come separately.
FetchContent_Declare(instant_meshes
    GIT_REPOSITORY https://github.com/wjakob/instant-meshes.git
    GIT_TAG        7b3160864a2e1025af498c84cfed91cbfb613698
    GIT_SUBMODULES ""
    ${m2c_no_cmake})
FetchContent_Declare(pcg32
    GIT_REPOSITORY https://github.com/wjakob/pcg32.git
    GIT_TAG        0ef13e68ca0be5506e1cfc0db76831e6f916e9e9
    ${m2c_no_cmake})
FetchContent_Declare(dset
    GIT_REPOSITORY https://github.com/wjakob/dset.git
    GIT_TAG        7967ef0e6041cd9d73b9c7f614ab8ae92e9e587a
    ${m2c_no_cmake})
FetchContent_MakeAvailable(instant_meshes pcg32 dset)

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

# ---- 3DxWare SDK (ADR 0039): optional SpaceMouse navigation in the app, Windows and MSVC only ----
# The SDK is not redistributable and its licence is not GPL-compatible, so it is never fetched and
# is off by default: a build with it is for your own machine and must not be distributed.
option(MESH2CAD_SPACEMOUSE
    "SpaceMouse navigation in the app with the 3DxWare SDK (Windows, MSVC; set TDxWare_SDK_DIR)" OFF)
set(M2C_HAVE_SPACEMOUSE OFF)
if(MESH2CAD_SPACEMOUSE)
    if(NOT M2C_HAVE_QT)
        message(FATAL_ERROR "MESH2CAD_SPACEMOUSE needs the desktop app (Qt 6)")
    elseif(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "MESH2CAD_SPACEMOUSE needs Windows and MSVC (the 3DxWare SDK library "
                            "is built for MSVC)")
    endif()
    find_package(TDxWare_SDK 4.0 CONFIG REQUIRED HINTS "$ENV{TDxWare_SDK_DIR}")
    set(M2C_HAVE_SPACEMOUSE ON)
    message(STATUS "Mesh2Cad: SpaceMouse navigation on, with the 3DxWare SDK in "
                   "${TDxWare_SDK_DIR}. Do not distribute this build (ADR 0039).")
endif()

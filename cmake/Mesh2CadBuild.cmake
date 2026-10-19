# Build policy shared by every target (ARCHITECTURE.md section 9).

# ---- Minimum compilers (C++23: std::expected) ---------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(FATAL_ERROR "GCC 13 or newer is required (found ${CMAKE_CXX_COMPILER_VERSION}).")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    message(FATAL_ERROR "Clang 17 or newer is required (found ${CMAKE_CXX_COMPILER_VERSION}).")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC 19.40 (Visual Studio 2022 17.10) or newer is required.")
endif()

# The compiler version is not enough: Clang 18 with libstdc++ 13 (the Ubuntu 24.04 default) has no
# std::expected, because libstdc++ 13 gates <expected> on concepts support Clang only reports from
# version 20. Fail early with an explanation instead of with template errors in Error.h.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <expected>
    int main() { std::expected<int, int> e{1}; return *e - 1; }"
    M2C_HAS_STD_EXPECTED)
if(NOT M2C_HAS_STD_EXPECTED)
    message(FATAL_ERROR
        "This compiler/standard-library combination has no std::expected (C++23). Use GCC 13+, "
        "MSVC 17.10+, or Clang with libc++ 16+. Clang with libstdc++ 13 is not supported.")
endif()

# ---- Floating point: deterministic on one platform, no fast-math, no FMA contraction -------------
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()

# ---- MinGW: big COFF objects ----------------------------------------------------------------------
# Template-heavy translation units (Eigen, CGAL) need more than the 32767 sections a default COFF
# object can hold; an unoptimised Debug build of src/pipeline/Align.cpp has about 44000. MSVC gets
# /bigobj where it is needed (Mesh2CadDependencies.cmake).
if(MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif()

# ---- Warnings ----------------------------------------------------------------------------------
# OFF by default so a newer compiler's new warnings cannot block a local build; the presets and
# CI turn it ON.
option(MESH2CAD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(m2c_warnings INTERFACE)
if(MSVC)
    target_compile_options(m2c_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    # The MSVC CRT deprecates standard functions such as std::getenv in favour of its own _s
    # variants (C4996). We use the standard ones, as on the other platforms.
    target_compile_definitions(m2c_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(MESH2CAD_WARNINGS_AS_ERRORS)
        target_compile_options(m2c_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(m2c_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
        -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough)
    if(MESH2CAD_WARNINGS_AS_ERRORS)
        target_compile_options(m2c_warnings INTERFACE -Werror)
    endif()
endif()

# ---- Sanitizers (GCC/Clang, used by the linux-*-sanitize presets and CI) --------------------------
option(MESH2CAD_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(MESH2CAD_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "MESH2CAD_SANITIZE is only supported with GCC or Clang.")
    endif()
    # Alignment reports are recoverable so that cmake/ubsan-suppressions.txt can drop the ones
    # inside Open CASCADE (ADR-028); the sanitize test preset sets halt_on_error=1, so every other
    # report still stops the test.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined -fsanitize-recover=alignment)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- One module = one target --------------------------------------------------------------------
# m2c_add_library(<name> SOURCES <files...> [DEPS <targets...>])
#
# Creates the static library m2c_<name> with the alias Mesh2Cad::<name>. Public headers live in
# <module>/include/m2c/<name>/. DEPS are the only modules this one may include; they encode the
# layering rules of ARCHITECTURE.md section 4, and scripts/check_layers.py checks the includes.
function(m2c_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_library(m2c_${name} STATIC ${ARG_SOURCES})
    add_library(Mesh2Cad::${name} ALIAS m2c_${name})
    target_include_directories(m2c_${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(m2c_${name} PUBLIC ${ARG_DEPS} PRIVATE m2c_warnings)
endfunction()

# ---- Runtime DLLs next to each executable ------------------------------------------------------
# m2c_copy_runtime(<target>)
#
# Windows executables need the DLLs they link next to them, or they exit with 0xc0000135 (DLL not
# found), which also breaks Catch2's test discovery after the build. Two kinds:
# - MinGW's libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll. CLion puts the MinGW bin
#   folder on the PATH, but Explorer, a plain terminal and Claude Desktop do not. (Static linking
#   fails with CLion's MinGW; see ADR-020.)
# - Shared libraries from imported targets, such as Open CASCADE built from source (ADR-026).
#   vcpkg copies its own; copying again is harmless.
# Call it before catch_discover_tests(), so the copy runs before discovery.
function(m2c_copy_runtime target)
    if(NOT WIN32)
        return()
    endif()
    set(dll_list "$<TARGET_RUNTIME_DLLS:${target}>")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:${dll_list}>,copy_if_different,true>
                ${dll_list} "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
    if(NOT MINGW)
        return()
    endif()
    get_filename_component(bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(dlls)
    foreach(dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${bin}/${dll}")
            list(APPEND dlls "${bin}/${dll}")
        endif()
    endforeach()
    if(dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dlls} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# m2c_deploy_qt(<target>)
#
# Windows executables linked against Qt need the Qt DLLs and the platform plugin next to them;
# without them they exit with 0xc0000135 (DLL not found), which also breaks test discovery for the
# app tests. windeployqt ships with Qt and copies them.
function(m2c_deploy_qt target)
    if(NOT WIN32)
        return()
    endif()
    find_program(M2C_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(M2C_WINDEPLOYQT)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${M2C_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                    "$<TARGET_FILE:${target}>"
            COMMENT "Copying Qt libraries next to ${target}")
    endif()
endfunction()

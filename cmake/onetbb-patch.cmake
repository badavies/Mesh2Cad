# PATCH_COMMAND for the downloaded oneTBB (Mesh2CadDependencies.cmake), run in its source folder.
# Each replacement is skipped once applied, so running it again changes nothing.

# m2c_patch(<file> <old> <new>): replaces <old> with <new> in <file>, which must contain one of them.
function(m2c_patch file old new)
    file(READ "${file}" text)
    string(FIND "${text}" "${new}" done)
    if(done GREATER -1)
        return()
    endif()
    string(FIND "${text}" "${old}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "onetbb-patch.cmake: ${file} has changed; update the patch")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${file}" "${text}")
endfunction()

# v2022.3.0 sets policy CMP0148 to OLD for its Python bindings, which are not built here, and
# CMake 4 warns about OLD policies. oneTBB's master branch sets it to NEW, as this does.
m2c_patch(CMakeLists.txt "cmake_policy(SET CMP0148 OLD)" "cmake_policy(SET CMP0148 NEW)")

# The shared library loads its helper DLLs with LOAD_LIBRARY_SAFE_CURRENT_DIRS, which the
# mingw-w64 headers of MinGW 13.1 (the one that ships with Qt) do not define. The value is the
# Windows SDK's, which newer mingw-w64 headers define too. A static oneTBB never compiled this.
m2c_patch(src/tbb/dynamic_link.cpp
    "#if _WIN32\n    DWORD loading_flags(int) {"
    "#if _WIN32\n#ifndef LOAD_LIBRARY_SAFE_CURRENT_DIRS\n#define LOAD_LIBRARY_SAFE_CURRENT_DIRS 0x00002000\n#endif\n    DWORD loading_flags(int) {")

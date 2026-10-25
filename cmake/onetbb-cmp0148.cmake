# PATCH_COMMAND for the downloaded oneTBB (Mesh2CadDependencies.cmake), run in its source folder.
# v2022.3.0 sets policy CMP0148 to OLD for its Python bindings, which are not built here, and
# CMake 4 warns about OLD policies. oneTBB's master branch sets it to NEW, as this does.
file(READ CMakeLists.txt text)
string(REPLACE "cmake_policy(SET CMP0148 OLD)" "cmake_policy(SET CMP0148 NEW)" text "${text}")
file(WRITE CMakeLists.txt "${text}")

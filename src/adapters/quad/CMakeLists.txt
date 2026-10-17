# Instant Meshes' field optimisation and extraction (ADR-009), built as its own library with its
# own language level and no warnings: it is third-party code. serial_tbb/ comes first on its
# include path, so its TBB and parallel-sort includes get the serial stand-ins.
set(im_src "${instant_meshes_SOURCE_DIR}/src")
add_library(m2c_instant_meshes STATIC
    ${im_src}/adjacency.cpp
    ${im_src}/bvh.cpp
    ${im_src}/cleanup.cpp
    ${im_src}/dedge.cpp
    ${im_src}/extract.cpp
    ${im_src}/field.cpp
    ${im_src}/hierarchy.cpp
    ${im_src}/meshstats.cpp
    ${im_src}/normal.cpp
    ${im_src}/reorder.cpp
    ${im_src}/subdivide.cpp)
target_include_directories(m2c_instant_meshes SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/serial_tbb ${im_src} ${dset_SOURCE_DIR} ${pcg32_SOURCE_DIR})
target_link_libraries(m2c_instant_meshes PUBLIC Eigen3::Eigen)
set_target_properties(m2c_instant_meshes PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(m2c_instant_meshes PRIVATE /W0 /bigobj)
else()
    target_compile_options(m2c_instant_meshes PRIVATE -w)
endif()

# The adapter: core types in its public header, Instant Meshes inside.
m2c_add_library(quad
    SOURCES
        InstantMeshes.cpp
    DEPS
        Mesh2Cad::core)
target_link_libraries(m2c_quad PRIVATE m2c_instant_meshes)

# GCC's optimiser reports false positives inside inlined Eigen code (fixed-size SSE loads of a
# Vector3f), as in the CGAL adapter. Our own code is still checked in Debug builds.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(m2c_quad PRIVATE
        -Wno-maybe-uninitialized -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread)
endif()

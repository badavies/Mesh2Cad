# The only target that sees CGAL types (ARCHITECTURE.md section 4). Its public header uses core
# types only, so nothing above it needs CGAL's include paths or compile flags.
m2c_add_library(cgal
    SOURCES
        CgalMesh.cpp
        HoleFilling.cpp
        Remesh.cpp
        Repair.cpp
        RegionGrowing.cpp
        Simplify.cpp
        Slice.cpp
        Wrap.cpp
    DEPS
        Mesh2Cad::core)
target_link_libraries(m2c_cgal PRIVATE m2c_cgal_deps)
target_include_directories(m2c_cgal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# GCC's optimiser reports false positives deep inside inlined CGAL, Boost and Eigen templates in
# Release builds (seen with GCC 13 and CGAL 6.0.1). Our own code is still checked in Debug builds.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(m2c_cgal PRIVATE
        -Wno-maybe-uninitialized -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread)
endif()
# MSVC's optimiser does the same in Release builds (seen with MSVC 19.44 and CGAL 6.2.1:
# potentially uninitialized local, unreachable code). /external:W0 does not hide them because
# they come from code generation, not parsing. Our own code is still checked in Debug builds.
if(MSVC)
    target_compile_options(m2c_cgal PRIVATE /wd4701 /wd4702 /wd4703)
endif()

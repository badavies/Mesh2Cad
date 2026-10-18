# The only target that sees OpenSubdiv types. Its public header uses core types only.
m2c_add_library(osd
    SOURCES
        CageFit.cpp
        Limit.cpp
        Refine.cpp
        Subdivision.cpp
        Tessellation.cpp
    DEPS
        Mesh2Cad::core)
target_link_libraries(m2c_osd PRIVATE m2c_osd_deps Eigen3::Eigen)
target_include_directories(m2c_osd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# As in the other adapters: GCC's optimiser reports false positives inside inlined Eigen code.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(m2c_osd PRIVATE
        -Wno-maybe-uninitialized -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread)
endif()

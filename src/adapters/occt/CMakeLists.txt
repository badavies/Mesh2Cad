# The only target that sees Open CASCADE types. Its public header uses core types only; the
# kernel shape is the opaque SolidShape, defined in Shape.h.
m2c_add_library(occt
    SOURCES
        Boolean.cpp
        Brep.cpp
        Profile.cpp
        Surface.cpp
        Trim.cpp
    DEPS
        Mesh2Cad::core)
target_link_libraries(m2c_occt PRIVATE m2c_occt_deps)
target_include_directories(m2c_occt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

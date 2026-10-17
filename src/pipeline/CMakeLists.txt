m2c_add_library(pipeline
    SOURCES
        Align.cpp
        Deviation.cpp
        Measure.cpp
        MeshTools.cpp
        QuadRemesh.cpp
        Select.cpp
        Shells.cpp
        Smooth.cpp
        Surface.cpp
    DEPS
        Mesh2Cad::core
        Mesh2Cad::cgal
        Mesh2Cad::quad
        Mesh2Cad::osd
        Mesh2Cad::occt)
target_link_libraries(m2c_pipeline PRIVATE Eigen3::Eigen)

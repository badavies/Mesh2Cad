m2c_add_library(core
    SOURCES
        MeshSelection.cpp
        DeviationField.cpp
        DisplayProxy.cpp
        MeshStats.cpp
        MeshTopology.cpp
        Parallel.cpp
        Primitive.cpp
        QuadLayout.cpp
        Raster.cpp
        Sketch.cpp
        Solid.cpp
        SpatialIndex.cpp
        Transform.cpp
        TriMesh.cpp)
target_link_libraries(m2c_core PRIVATE TBB::tbb)

add_executable(m2c_stage_tests
    AlignStageTests.cpp
    BooleanStageTests.cpp
    MeshToolStageTests.cpp
    PatchMergeStageTests.cpp
    PrimitiveFitStageTests.cpp
    ProfileStageTests.cpp
    QuadRemeshStageTests.cpp
    RegionFitStageTests.cpp
    SectionStageTests.cpp
    SegmentStageTests.cpp
    SelectMeasureStageTests.cpp
    SurfaceStageTests.cpp
    TrimStageTests.cpp)
target_link_libraries(m2c_stage_tests PRIVATE Mesh2Cad::pipeline Mesh2Cad::core m2c_test_support)
m2c_copy_runtime(m2c_stage_tests)
catch_discover_tests(m2c_stage_tests PROPERTIES LABELS "stages")

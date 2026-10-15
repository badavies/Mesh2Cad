add_executable(m2c_stage_tests
    AlignStageTests.cpp
    MeshToolStageTests.cpp
    SelectMeasureStageTests.cpp)
target_link_libraries(m2c_stage_tests PRIVATE Mesh2Cad::pipeline Mesh2Cad::core m2c_test_support)
catch_discover_tests(m2c_stage_tests PROPERTIES LABELS "stages")

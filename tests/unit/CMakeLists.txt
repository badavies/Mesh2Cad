add_executable(m2c_unit_tests
    DocumentTests.cpp
    GeometryTests.cpp
    MeshStatsTests.cpp
    ObjPlyTests.cpp
    ProjectTests.cpp
    StlTests.cpp
    TriMeshTests.cpp)
target_link_libraries(m2c_unit_tests PRIVATE
    Mesh2Cad::core Mesh2Cad::document Mesh2Cad::io m2c_test_support)
m2c_copy_runtime(m2c_unit_tests)
catch_discover_tests(m2c_unit_tests PROPERTIES LABELS "unit")

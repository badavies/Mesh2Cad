add_executable(m2c_golden_tests GoldenTests.cpp)
target_link_libraries(m2c_golden_tests PRIVATE
    Mesh2Cad::commands Mesh2Cad::document Mesh2Cad::core Mesh2Cad::rpc m2c_mcp m2c_test_support)
target_compile_definitions(m2c_golden_tests PRIVATE
    M2C_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# Real scans take minutes; run `ctest -L golden` on a machine with the scans before each milestone
# gate. Without scans (as in CI, nightly included) the test reports "skipped" rather than passing.
catch_discover_tests(m2c_golden_tests PROPERTIES LABELS "golden")

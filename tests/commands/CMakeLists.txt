add_executable(m2c_command_tests
    CadCommandTests.cpp
    CommandTests.cpp
    ExampleTests.cpp
    FrontEndTests.cpp
    ToolCommandTests.cpp)
target_link_libraries(m2c_command_tests PRIVATE
    Mesh2Cad::commands Mesh2Cad::document Mesh2Cad::io Mesh2Cad::core Mesh2Cad::rpc m2c_mcp
    m2c_test_support)
target_compile_definitions(m2c_command_tests PRIVATE
    M2C_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
m2c_copy_runtime(m2c_command_tests)
catch_discover_tests(m2c_command_tests PROPERTIES LABELS "commands")

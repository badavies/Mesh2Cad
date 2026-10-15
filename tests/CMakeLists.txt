include(Catch)

add_library(m2c_test_support INTERFACE)
target_include_directories(m2c_test_support INTERFACE support)
target_link_libraries(m2c_test_support INTERFACE Catch2::Catch2WithMain m2c_warnings)

add_subdirectory(unit)
add_subdirectory(stages)
add_subdirectory(commands)

# Every Catch2 test case is registered with CTest individually (catch_discover_tests) and labelled
# "unit" or "commands", so `ctest -L unit` and the IDE test runner can select a group.
# Discovery runs as a post-build step (the default). Do not switch to DISCOVERY_MODE PRE_TEST: CLion
# starts two ctest processes at once, both rewrite the test list and every test is registered twice.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME layering
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_layers.py)
    set_tests_properties(layering PROPERTIES LABELS "layering")
endif()

# `cmake --build <dir> --target check` builds everything and runs the whole suite.
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS m2c_unit_tests m2c_stage_tests m2c_command_tests mesh2cad
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running all tests")

if(TARGET m2c_app)
    add_subdirectory(app)
    add_dependencies(check m2c_app_tests mesh2cad-app)
endif()

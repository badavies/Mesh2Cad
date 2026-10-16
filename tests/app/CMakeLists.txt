# Drives the real desktop window: needs a display (xvfb-run on Linux CI). Labelled "app" so
# `ctest -LE app` skips it on headless machines.
add_executable(m2c_app_tests AppTests.cpp)
target_link_libraries(m2c_app_tests PRIVATE m2c_app m2c_test_support)
# Before catch_discover_tests: listing the tests runs the exe, which needs the DLLs next to it.
m2c_copy_runtime(m2c_app_tests)
m2c_deploy_qt(m2c_app_tests)
catch_discover_tests(m2c_app_tests PROPERTIES LABELS "app")

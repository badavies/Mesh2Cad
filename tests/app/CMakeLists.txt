# Drives the real desktop window: needs a display (xvfb-run on Linux CI). Labelled "app" so
# `ctest -LE app` skips it on headless machines.
add_executable(m2c_app_tests AppTests.cpp)
target_link_libraries(m2c_app_tests PRIVATE m2c_app m2c_test_support)
# Before catch_discover_tests: listing the tests runs the exe, which needs the DLLs next to it.
m2c_copy_runtime(m2c_app_tests)
m2c_deploy_qt(m2c_app_tests)
catch_discover_tests(m2c_app_tests PROPERTIES LABELS "app")

# The 10M-triangle frame-rate test (Catch2 tag [perf], hidden) builds a 250 MB file and needs a
# Release build and a real GPU, so it is only registered when asked for: configure with
# -DMESH2CAD_PERF_TESTS=ON, then `ctest -L perf`. See tests/README.md.
option(MESH2CAD_PERF_TESTS "Register the slow performance tests (CTest label perf)" OFF)
if(MESH2CAD_PERF_TESTS)
    add_test(NAME app.perf.orbit_10m COMMAND m2c_app_tests "[perf]")
    set_tests_properties(app.perf.orbit_10m PROPERTIES LABELS "perf" TIMEOUT 1800)
endif()


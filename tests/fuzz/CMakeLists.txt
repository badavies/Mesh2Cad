add_executable(m2c_fuzz_mesh_parsers
    FuzzDriver.cpp
    MeshParserFuzz.cpp)
target_link_libraries(m2c_fuzz_mesh_parsers PRIVATE Mesh2Cad::io m2c_warnings)

# A short deterministic run on every build; nightly runs pass a larger --iterations. Under the
# linux-gcc-sanitize preset this runs with AddressSanitizer and UndefinedBehaviorSanitizer.
add_test(NAME fuzz.mesh_parsers COMMAND m2c_fuzz_mesh_parsers --iterations 20000)
set_tests_properties(fuzz.mesh_parsers PROPERTIES LABELS "fuzz" TIMEOUT 300)

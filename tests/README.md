# Tests

Catch2 v3 tests, registered with CTest one test case at a time.

| Folder | Executable | CTest label | Contents |
| --- | --- | --- | --- |
| `unit/` | `m2c_unit_tests` | `unit` | Core types, spatial index, display proxy, STL/OBJ/PLY and project I/O, document and undo |
| `stages/` | `m2c_stage_tests` | `stages` | Pipeline stages and CGAL tools on synthetic meshes |
| `commands/` | `m2c_command_tests` | `commands` | Commands through the registry, JSON-RPC and MCP (same results), the files in `examples/` |
| `app/` | `m2c_app_tests` | `app` | The real desktop window driven by mouse events and by an agent on the local socket (built with Qt; needs a display) |
| `fuzz/` | `m2c_fuzz_mesh_parsers` | `fuzz` | STL, OBJ and PLY readers on mutated input (libFuzzer entry point plus a fixed-seed driver for GCC and MSVC) |
| `golden/` | `m2c_golden_tests` | `golden` | Real scans replayed through the CLI path, JSON-RPC and MCP against stored metrics; skipped without scans (see `golden/README.md`) |
| (script) | — | `layering` | `scripts/check_layers.py` module dependency check |
| `support/` | — | — | Shared helpers: `makeCube()`, `makeBox()`, `makeSphere()`, `addNoise()`, `TempDir` |

CI runs nightly as well (and from *Run workflow* on the Actions page): the full suite on every preset, plus the fuzzer under the `linux-gcc-sanitize` preset for 1,000,000 inputs with a new seed each night (`m2c_fuzz_mesh_parsers --iterations N --seed S`; `--save-last FILE` keeps the input being tried, so a crash can be reproduced, and CI uploads it). CI has no golden scans, so run `ctest -L golden` on a machine with them before each milestone gate. On a machine without a display run `ctest -LE app`, or wrap ctest in `xvfb-run` as CI does. Set `M2C_TEST_SCREENSHOT_DIR` to save what the app tests drew.

## Running

In CLion: pick **All Tests**, **Unit Tests**, **Stage Tests**, **Command Tests** or **App Tests** in the run configuration list and press Run or Debug. The green gutter icon next to any `TEST_CASE` runs or debugs just that case.

From a terminal, after configuring with a preset (here `linux-gcc-debug`):

```
cmake --build --preset linux-gcc-debug --target check     # build and run everything
ctest --preset linux-gcc-debug                            # run everything already built
ctest --preset linux-gcc-debug -L unit                    # one label
ctest --preset linux-gcc-debug -R "STL"                   # by test name
build/linux-gcc-debug/tests/unit/m2c_unit_tests "[stl]"   # Catch2 directly, filtered by tag
build/linux-gcc-debug/tests/unit/m2c_unit_tests --list-tests
```

Use the `linux-gcc-sanitize` preset to run the suite under AddressSanitizer and UndefinedBehaviorSanitizer.

## Writing a test

- Put the test in the folder matching the module under test and add the `.cpp` to that folder's `CMakeLists.txt`.
- Tag every `TEST_CASE` with the module and topic, for example `"[core][stats]"`.
- Check measured outcomes (counts, tolerances, error codes), not byte-exact output.
- Never change an expected value or tolerance to make a test pass (`docs/AI_GUIDELINES.md` rule 10).
- Every new command needs a test in `commands/` that runs it through `CommandRegistry::execute`.

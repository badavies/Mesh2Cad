# Tests

Catch2 v3 tests, registered with CTest one test case at a time.

| Folder | Executable | CTest label | Contents |
| --- | --- | --- | --- |
| `unit/` | `m2c_unit_tests` | `unit` | Core types, spatial index, display proxy, STL/OBJ/PLY and project I/O, document and undo |
| `stages/` | `m2c_stage_tests` | `stages` | Pipeline stages and CGAL tools on synthetic meshes |
| `commands/` | `m2c_command_tests` | `commands` | Commands through the registry, JSON-RPC and MCP (same results), the files in `examples/` |
| `app/` | `m2c_app_tests` | `app` | The real desktop window driven by mouse events and by an agent on the local socket (built with Qt; needs a display) |
| (script) | — | `layering` | `scripts/check_layers.py` module dependency check |
| `support/` | — | — | Shared helpers: `makeCube()`, `makeBox()`, `makeSphere()`, `addNoise()`, `TempDir` |

Later milestones add `golden/` and `fuzz/` (see `docs/ARCHITECTURE.md` section 10). On a machine without a display run `ctest -LE app`, or wrap ctest in `xvfb-run` as CI does. Set `M2C_TEST_SCREENSHOT_DIR` to save what the app tests drew.

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

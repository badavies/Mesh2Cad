# Tests

Catch2 v3 tests, registered with CTest one test case at a time.

| Folder | Executable | CTest label | Contents |
| --- | --- | --- | --- |
| `unit/` | `m2c_unit_tests` | `unit` | Core types, spatial index, display proxy, STL/OBJ/PLY and project I/O, document and undo |
| `stages/` | `m2c_stage_tests` | `stages` | Pipeline stages and CGAL tools on synthetic meshes |
| `commands/` | `m2c_command_tests` | `commands` | Commands through the registry, JSON-RPC and MCP (same results), the files in `examples/` |
| `app/` | `m2c_app_tests` | `app` | The real desktop window driven by mouse events and by an agent on the local socket (built with Qt; needs a display) |
| `app/` | `m2c_app_tests "[perf]"` | `perf` | The M0 frame-rate gate: a 10M-triangle scan loaded and orbited in the real window (hidden; see below) |
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

## Frame-rate test (M0 gate)

`A 10M-triangle scan loads and orbits at 30 fps` builds a 10.2M-triangle organic test surface, opens it in the real window, then draws 30 still frames at full resolution and 120 frames of orbiting (which draws the coarse proxy, once it is built). It prints one line with the load time and the frame rates and fails if orbiting is below 30 fps. It is hidden from normal runs, needs a Release build of the app and a real GPU (not a virtual display), and takes a minute or two.

On the Windows machine, in CLion: copy the Qt profile you build the app with, set *Build type* to `Release`, build `m2c_app_tests` with it, then run from a terminal in the build folder:

```
tests\app\m2c_app_tests.exe "[perf]"
```

Or configure with `-DMESH2CAD_PERF_TESTS=ON` and run `ctest -L perf --output-on-failure`. Record the printed line in the M0 gate issue.

## Writing a test

- Put the test in the folder matching the module under test and add the `.cpp` to that folder's `CMakeLists.txt`.
- Tag every `TEST_CASE` with the module and topic, for example `"[core][stats]"`.
- Check measured outcomes (counts, tolerances, error codes), not byte-exact output.
- Never change an expected value or tolerance to make a test pass (`docs/AI_GUIDELINES.md` rule 10).
- Every new command needs a test in `commands/` that runs it through `CommandRegistry::execute`.

# CLion run configurations

The assistant cannot write into `.idea/`, so the shared run/debug configurations are kept here. Copy them over the ones in `.idea/runConfigurations/` (overwrite when asked), ideally with CLion closed:

```
xcopy /E /I /Y docs\ide\runConfigurations .idea\runConfigurations      (Windows cmd)
cp -r docs/ide/runConfigurations/. .idea/runConfigurations/              (Linux/macOS)
```

| Configuration | Target | Arguments |
| --- | --- | --- |
| `mesh2cad list` | `mesh2cad` | `list` |
| `mesh2cad batch (example session)` | `mesh2cad` | `batch session.jsonl`, working directory `examples/` |
| `Mesh2Cad app` | `mesh2cad-app` | none (needs Qt; see the main README) |
| `mesh2cad-mcp (headless)` | `mesh2cad-mcp` | `--headless` |
| `Unit Tests` | `m2c_unit_tests` | none |
| `Stage Tests` | `m2c_stage_tests` | none |
| `Command Tests` | `m2c_command_tests` | none |
| `App Tests` | `m2c_app_tests` | none (opens a window; needs Qt) |
| `All Tests` | compound of Unit, Stage and Command Tests | |

Each configuration builds its target before it starts (the *Before launch: Build* step). They do not name a CMake profile, so they use whichever profile is selected.

For a test tree with one entry per test case, use CLion's own **All CTest** configuration or the green run icons next to each `TEST_CASE`.

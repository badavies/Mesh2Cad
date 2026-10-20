# Rules for AI-assisted development

AI coding tools drift in predictable ways: they widen scope, add dependencies and abstractions, duplicate existing code, and change tests until they pass. These rules counter each of those. They come from section 11 of [`ARCHITECTURE.md`](ARCHITECTURE.md); if the two ever disagree, fix whichever is out of date in the same change.

## Scope

1. Every AI task starts from a roadmap issue with written acceptance criteria. The AI does not choose scope.
2. A change touches only the modules named in its issue, plus their tests. Anything else found along the way becomes a new issue, not part of this change.
3. Keep changes small: about 400 lines of non-test code at most. Bigger needs a decision record or a split.
4. Nothing from a later milestone is built early, including "while I'm here" helpers and hooks for future features.

## Structure

5. No new dependency without a decision record (`docs/adr/`) and an entry in [`THIRD_PARTY.md`](../THIRD_PARTY.md).
6. No new interface, factory, plugin point, configuration system or generic framework unless two implementations exist or are scheduled.
7. Before writing an algorithm, check whether CGAL, OCCT, OpenSubdiv or our own `core` already has it. The pull request states what was checked.
8. Follow the module's existing patterns for naming, errors, logging and parameters. New patterns need a decision record. New capabilities are added as registered commands with schemas and tests, never as UI-only or front-end-only code paths.

## Tests and review

9. Every change includes tests. Stage changes include a synthetic-shape stage test.
10. AI must not change expected values, thresholds or golden metrics to make a test pass. If a test seems wrong, it says so and stops.
11. A human reads every line of geometry code before merging. Tolerances, orientation, degenerate cases and units are where plausible-but-wrong code hides.
12. Review checklist for each pull request: scope matches the issue; no new dependency; no kernel types above the adapters; tests added; nothing deleted from tests.

## Housekeeping at each milestone gate

13. Delete dead code, unused helpers and abandoned scaffolding.
14. Check the dependency graph, module sizes and the TODO count against the previous gate.
15. Update `ARCHITECTURE.md` and the module READMEs so the context given to AI tools stays accurate. Out-of-date context is a major cause of drift.

## Conventions that already exist (follow them)

| Topic | Convention | Example |
| --- | --- | --- |
| Errors | Return `Result<T>` (`std::expected<T, Error>`) across module boundaries; use `fail(ErrorCode::..., "message")`. Exceptions never cross a module boundary. | `src/core/include/m2c/core/Error.h` |
| Modules | One folder under `src/` = one CMake target created with `m2c_add_library`. Public headers live in `include/m2c/<module>/`. `DEPS` lists the only modules it may include. | `src/io/CMakeLists.txt` |
| Layering | `scripts/check_layers.py` enforces who may include whom and keeps kernel headers inside `src/adapters/`. It runs as a ctest. | `scripts/check_layers.py` |
| Meshes | `TriMesh` is immutable and shares its buffers. A tool returns a new `TriMesh`; it never mutates one. | `src/core/include/m2c/core/TriMesh.h` |
| Units and names | Millimetres. Put units in names: `surface_area_mm2`, `tolerance_mm`. JSON keys are snake_case; C++ uses `camelCase` functions and members with a trailing `_`. | `src/commands/JsonConvert.cpp` |
| Commands | Dotted lower-case names (`mesh.import`). Each has a summary, a JSON Schema, a `mutatesDocument` flag and a test in `tests/commands`. Object ids carry `x-object` (ADR-029); a `path` parameter carries `x-file-types`, and a command that writes that file sets `writesFiles` (ADR-031). | `src/commands/*Commands.cpp` (one file per group; `registerBuiltinCommands` is in `DocumentCommands.cpp`) |
| Floating point | `-ffp-contract=off` / `/fp:precise` are set globally; never add fast-math. Serial loops in index order unless a deterministic parallel reduction is required. | `cmake/Mesh2CadBuild.cmake` |
| Tests | Catch2. Check measured outcomes (counts, validity, tolerances), not byte-exact output. Shared helpers in `tests/support/`. | `tests/unit/MeshStatsTests.cpp` |
| Licence header | First line of every source file: `// SPDX-License-Identifier: GPL-3.0-or-later` | any `.cpp` |

## Adding a command (checklist)

1. Implement the logic in the right layer (`pipeline`, `io`, `core`), not in the command handler.
2. Add the handler to `src/commands/` with a summary, a parameter schema and `mutatesDocument`. Give every object id `x-object` and every `path` `x-file-types`; set `writesFiles` if the command writes the file at `path`.
3. Register it in `registerBuiltinCommands` (or the registration function of its module).
4. Add a test in `tests/commands/` that runs it through the registry, plus unit or stage tests for the logic.
5. Do not touch `app/`, `cli/`, `rpc/` or `mcp/`: they pick up the command from the registry, including its object pickers, file dialogs, log lines and MCP annotations.
6. Regenerate `docs/COMMANDS.md` with `python3 scripts/gen_commands.py <path to mesh2cad>`.

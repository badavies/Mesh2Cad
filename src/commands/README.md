# commands (`Mesh2Cad::commands`)

The command engine. Every user-visible action is a `Command` in the `CommandRegistry`, and every
front end (CLI, JSON-RPC, MCP, desktop app) runs commands through `CommandRegistry::execute`.
Handlers read parameters, call `io` and `pipeline` functions, and commit the results to the
`Document` as one undo step with provenance.

## Public API

- `m2c/commands/Command.h`:
  - `Command`: name, summary, `paramsSchema` (JSON Schema), `mutatesDocument`, `opensUndoStep` and `handler`.
  - `CommandRegistry`: `add`, `find`, `list` and `execute`.
  - `CommandContext`: the `Document`, the `source` ("ui", "cli", "json", "mcp") and the optional `sessionLog` callback.
  - `registerBuiltinCommands`: registers every built-in command.
  - `Json`: alias for `nlohmann::json`.
- `m2c/commands/JsonConvert.h`: `toJson` for `Error`, `Aabb`, `MeshStats`, `Vec3d`, `Transform3d`, `PickedPoint` and `Measurement`.

Private: `detail/Params.h` (`makeCommand`, parameter readers such as `requireId` and
`optionalNumber`, `requireMesh`, `requireSelection`, `commitMesh`, the `M2C_TRY` macro) and
`detail/Register.h` (one `add...Commands` function per source file).

## Dependencies

May use: `core`, `document`, `io`, `pipeline`; nlohmann_json (public, through `Json`).
Must not depend on: `adapters`, `app`, `cli`, `rpc`, `mcp`, Qt, and kernel library headers
(CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- To add a command: put the logic in `pipeline`, `io` or `core`; add the handler with
  `makeCommand` (name, summary, schema, `mutates`) in the matching `*Commands.cpp`; make sure it
  is registered through `registerBuiltinCommands`; add a test in `tests/commands` that runs it
  through `CommandRegistry::execute`; regenerate `docs/COMMANDS.md` with
  `scripts/gen_commands.py`. Front ends pick it up from the registry with no change.
- Names are dotted and lower-case (`mesh.repair`). JSON keys are snake_case, lengths in mm.
- The schema is published by `commands.list` but not enforced by the dispatcher. Handlers
  validate their own parameters with the `detail/Params.h` helpers and return errors; they do
  not throw on bad input.
- A failed mutating command leaves the document unchanged (the undo step is aborted).

# mcp (`m2c_mcp`, executable `mesh2cad-mcp`)

A Model Context Protocol server over stdio for AI agents. It exposes every registered command
as an MCP tool and keeps no engine logic: each tool call becomes one JSON-RPC request to an
engine, either in-process (headless) or over the running desktop app's local socket.

## Public API

Headers live in this folder (no `m2c/mcp/` prefix).

- `McpServer.h`: `McpServer` (`handle` for one MCP message, `toolName` to turn `mesh.import`
  into `mesh_import`) and its `EngineCall` callback type. It answers `initialize`, `ping`,
  `tools/list` (from `commands.list`) and `tools/call`.
  A result that names a picture it wrote (`{"image": {"path", "mime_type"}}`, as
  `view.render`'s does) also carries the picture as an image content block.
  A tool call whose request carries `_meta.progressToken` gets a `notifications/progress` every
  two seconds until it returns (through the optional `Notify` callback): the progress is the
  seconds spent, and the message names the latest stage the command reported, which the engine
  sends as `rpc.progress` and `engineEvent` takes in ("surface.build: making the faces, 12 s").
  Tools whose command writes a file are not marked read-only.

The executable (`main.cpp`) chooses the engine:

- `mesh2cad-mcp`: connects to the running app if its session file is found, otherwise uses a private headless document.
- `--headless`: always uses a private headless document.
- `--connect`: only connects to the running app; fails if it is not open.
- `--session <file>`: uses a different session file.

## Dependencies

May use: `core`, `document`, `commands`, `rpc`. The `m2c_mcp` library links `commands` only; the
executable adds `rpc` for `RpcServer`, `LocalClient` and the session file.
Must not depend on: `adapters`, `io`, `pipeline`, `app`, `cli`, Qt, and kernel library headers
(CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- Do not add tools here. A new command in `src/commands` becomes a tool automatically.
- The headless engine and the app connection both mark their changes with source `mcp`, so
  undo history and the session log show what an agent did.

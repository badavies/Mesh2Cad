# rpc (`Mesh2Cad::rpc`, executable `mesh2cad-rpc`)

The JSON-RPC 2.0 channel over the command registry. `RpcServer` is transport-free: a transport
feeds it one message per line and writes back the reply. `mesh2cad-rpc` is a headless engine on
stdin and stdout; the desktop app serves the same protocol on its local socket.

## Public API

- `m2c/rpc/RpcServer.h`: `RpcServer` (`handle`, `handleLine`, `authenticated` and the JSON-RPC error codes) and `rpcError`, which maps an `Error` to a JSON-RPC error object.
  - Every registered command is a method of the same name.
  - `rpc.hello` reports the server name, version and whether a token is needed; `rpc.auth` checks the token and may name the client `source`.
  - A `doc.changed` notification with the new revision follows each request that changed the document.
  - While a request runs, each stage its command reports (`CommandContext::progress`) is sent as an `rpc.progress` notification `{"id", "command", "stage", "fraction"?}` before the reply.
  - A request's id is its job id: `rpc.cancel` `{"id"}` asks the running request with that id to stop (`cancel`, `cancelAll`), and returns `{"cancelled"}`, whether one was running. The command stops at its next checkpoint and fails with -32800, `kCancelled`. `rpc.cancel` is safe to handle on another thread while a request runs (`isCancel`), so a transport answers it at once instead of queueing it behind the request.
- `m2c/rpc/Session.h`: `SessionInfo`, `defaultSessionFile`, `writeSessionFile`, `readSessionFile`, `makeSessionToken` and `LocalClient`, a line-based client for the app's local socket (Unix domain socket, or a named pipe on Windows).

## Dependencies

May use: `core`, `document`, `commands`; nlohmann_json through `commands`; the platform socket
API.
Must not depend on: `adapters`, `io`, `pipeline`, `app`, `cli`, `mcp`, Qt, and kernel library
headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- There is no network listener. Local connections need the per-session token from the session
  file, which only the current user can read.
- The server side of the local socket lives in `app` (`Engine`, with Qt's `QLocalServer`); this
  module has the client and the protocol.
- `mesh2cad-rpc --log <session.jsonl>` appends each successful mutating command to a session log.
- `mesh2cad-rpc` reads on one thread and runs requests in order on another, so `rpc.cancel`
  reaches a long request while it runs.

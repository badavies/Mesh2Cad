# 0023. Qt 6 desktop app, optional in the build, with one engine thread

- Status: Accepted (accepts ADR-008 and ADR-011)
- Date: 2026-10-15

## Context
M0 needs the viewport and UI, and M1 needs agents to drive the running app (section 8). Qt is too large to fetch like the other dependencies, and the command-line tools must still build where Qt is not installed.

## Decision
- `src/app` builds `mesh2cad-app` when Qt 6.4+ (Widgets, OpenGL, OpenGLWidgets, Network) is found. `MESH2CAD_BUILD_APP` is `AUTO` (default), `ON` (fail without Qt) or `OFF`.
- The `Engine` owns the document and registry and runs every command, from the UI or from the local socket, on one worker thread in arrival order. The UI receives an immutable snapshot of the object table after each change and never touches the document. Commands from different sources therefore never interleave inside an undo step, and the UI stays responsive during long tools.
- The local socket (`QLocalServer`: a named pipe on Windows, a user-only Unix socket elsewhere) speaks JSON-RPC per line. Its name and a random token are written to the per-user session file, which `mesh2cad-mcp` reads. `rpc.auth` may name the client (`"source": "mcp"`), and that name is recorded on undo steps and in the session log.
- Tool forms are generated from each command's JSON schema, so the UI offers exactly what agents can call.
- Viewport: OpenGL 3.3 core, float buffers relative to each mesh's centre, camera maths in double, flat shading from screen-space derivatives, selection as a per-face byte texture buffer indexed by `gl_PrimitiveID`, and a vertex-clustered proxy for meshes over two million faces while the camera moves.

## Alternatives considered
- Run commands on the UI thread: simpler, but a ten-second repair freezes the window and agent requests would block on the UI.
- A mutex around the document shared by UI and engine: every paint would contend with long commands.
- Qt Quick: less mature for dense desktop tool panels; no gain for a custom GL viewport.

## Consequences
- Long commands cannot be cancelled yet (jobs and `job.cancel` are still planned); the UI shows a busy state and queues further commands.
- On Windows the Qt build must match the compiler: use the MinGW kit from the Qt installer with that kit's own MinGW, or vcpkg's `qtbase` built by the same toolchain as the project. A Qt built by another MinGW version may fail to link.
- The 10M-triangle, 30 fps gate has not been measured on real hardware yet.

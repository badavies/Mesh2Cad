# app (`m2c_app`, executable `mesh2cad-app`)

The Qt 6 desktop app: an OpenGL viewport, object tree, tool forms, history and log. It adds no
capability of its own. Every action becomes a command run by the `Engine`, which also accepts
JSON-RPC from scripts and AI agents on a local socket, so a person and an agent work on the same
document. Built only when Qt 6 is found (`MESH2CAD_BUILD_APP`, default `AUTO`).

## Public API

Headers live in this folder (no `m2c/app/` prefix); everything is in namespace `m2c::app`.
`m2c_app` holds everything but `main()`, so `tests/app` can drive the real window.

- `Engine.h`: `Engine` (owns the `Document` and `CommandRegistry`, runs commands in order on one worker thread, serves the local socket and writes the session file) and `DocSnapshot`.
- `MainWindow.h`: `MainWindow`, the window and menus; `openFiles` imports meshes or opens a project.
- `Viewport.h`: `Viewport` (OpenGL 3.3 view of a `DocSnapshot`), interaction `Mode` and `Hit`.
- `ParamForm.h`: `ParamForm`, a form generated from a command's JSON schema: drop-downs for objects, comma-separated numbers for points and id lists, and JSON (with an example) for lists of objects.
- `Camera.h`: `Camera` (orbit camera, +Z up) and `Mat4d` (double-precision 4x4 matrix).
- `DeviationColors.h`: `deviationColor` and `deviationRange`, the deviation colour scale.

## Dependencies

May use: `core`, `document`, `commands`, `rpc`; Qt 6 Widgets, OpenGL, OpenGLWidgets and Network.
Must not depend on: `adapters`, `io`, `pipeline`, `cli`, `mcp`, and kernel library headers
(CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- The UI never touches the `Document`. It reads `DocSnapshot` copies published by the engine
  thread and sends commands with `Engine::run`.
- The viewport never changes the document either: tools emit signals that `MainWindow` turns
  into commands. Never add a UI-only code path; add a command instead.
- Tool forms show each object parameter (`x-object` in its schema, ADR-029) as a drop-down of
  the document's objects of that kind. Choosing one selects it in the Objects list and
  highlights it in the viewport. The other way round, rows selected in the Objects list (Ctrl or
  Shift for several) fill the open form: id lists get the selected ids of their kinds in the
  order they were picked, single object fields one each (`ParamForm::setSelectedObjects`).
  Showing, hiding or deleting one of several selected rows acts on all of them, as one step.
- A path field's browse button filters on the parameter's `x-file-types` and opens a Save dialog
  when the command writes files, an Open dialog otherwise (ADR-031).
- The log panel shows every command that changes the document or writes a file (with the path),
  from any source. An agent's command still running after two seconds gets a "running" line,
  and its finishing line gives the time taken.
- Sketches are listed under the mesh they were cut from and drawn as cyan lines over everything.
- Solids draw from their display mesh, quad layouts as orange edges. A mesh with a visible
  deviation object is coloured by its newest one (green within tolerance, warm outward, cold
  inward; **View > Colour-blind-safe deviation colours** switches to grey, orange and purple),
  with a legend in the top right corner.
- `--screenshot out.png FILE` renders and quits (CI smoke test). `--no-agent-socket` turns the
  local socket off.

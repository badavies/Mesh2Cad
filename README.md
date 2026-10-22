# Mesh2Cad
The app turns 3D scans of any shape, from a plain cube or machined bracket to organic, mostly non-flat objects such as a console game controller, into watertight solids that Fusion 360 opens as editable bodies, plus clean meshes for Blender

Status: **M0 Foundations and M1 Mesh preparation are implemented**; their milestone gates (a 10M-triangle scan orbiting at 30 fps, golden scans, CI on both platforms) have not been signed off yet. What works today:

- **Desktop app** (`mesh2cad-app`, Qt 6): OpenGL viewport with orbit, pan, zoom and standard views; object list; brush, lasso and crease selection; point-to-point measurement; a form for every command; workflow panel; history with the source of each change; undo and redo; drag and drop.
- **Import and export**: STL (binary and ASCII), OBJ, PLY (ASCII and binary); project files (`.m2c`) that keep meshes, selections, measurements, alignment and provenance.
- **Mesh preparation**: repair (soup repair, orientation, stitching, degenerate and non-manifold fixes, optional self-intersection removal), hole filling with fairing, shell detection, splitting and cleanup, Taubin smoothing, simplification, isotropic remeshing, shrink-wrap, face deletion and flipping.
- **Alignment and measuring**: principal-axes alignment, 3-2-1 datum alignment from selected faces, explicit transforms, reset to the scanned pose, plane fitting, snapped distance measurement.
- **Automation**: every action is a command available from the CLI (`mesh2cad`), JSON-RPC (`mesh2cad-rpc`, or the running app's local socket) and MCP (`mesh2cad-mcp`) for AI agents, with identical results. See [`docs/COMMANDS.md`](docs/COMMANDS.md).

The CAD surfacing pipeline (M2 onwards) comes next; see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) section 12.

Licence: GPL-3.0-or-later (see [`LICENSE`](LICENSE)).

## Installing

Installers are built by the [Packages workflow](.github/workflows/package.yml) and kept as its artifacts (and attached to each release):

- **Windows 10 and 11**: run `mesh2cad-<version>-windows-x64.exe`. It installs the app, the command-line tools and the MCP server under `C:\Program Files\Mesh2Cad`, adds a Start menu entry, and can put `bin` on the `PATH`. The `.zip` holds the same files for use without installing.
- **Ubuntu 24.04**: `sudo apt install ./mesh2cad_<version>_amd64.deb` installs the programs in `/usr/bin` and pulls in Qt, Open CASCADE, OpenSubdiv and oneTBB from Ubuntu. Examples and the command reference go to `/usr/share/mesh2cad`. `sudo apt remove mesh2cad` uninstalls it.

To make them yourself from a Release build: `cpack` in the build folder (NSIS must be installed on Windows).

## Building in CLion

Requirements: CMake 3.25+, Ninja, git, and a C++23 compiler: CLion's bundled **MinGW** (GCC 13+) or **MSVC 17.10+** (Visual Studio 2022) on Windows, **GCC 13+** on Linux. Clang is only supported with libc++ (see [ADR 0019](docs/adr/0019-linux-compilers.md)).

1. Open the project folder in CLion and let CMake load. Without vcpkg, the first load downloads the libraries (nlohmann/json, Catch2, Eigen, miniz, and CGAL 6.0.1 with the Boost headers, about 60 MB; [ADR 0022](docs/adr/0022-cgal-eigen-boost-fallback.md)). This takes a few minutes once; later loads use the cache.
2. Optional: use vcpkg instead of downloads. Install it once, set `VCPKG_ROOT` (for example `setx VCPKG_ROOT C:\vcpkg`), restart CLion, and pick one of the presets (`windows-mingw-debug`, `windows-msvc-debug`, `linux-gcc-debug`, ...) under *Settings | Build, Execution, Deployment | CMake*.
3. Build and run the targets `mesh2cad-app` (if Qt is installed, see below), `mesh2cad`, `m2c_unit_tests`, `m2c_stage_tests` and `m2c_command_tests`, or the `check` target for everything.

From a terminal:

```
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

### Building the desktop app

The app is built when CMake finds Qt 6.4 or newer; otherwise it is skipped with a message and the command-line tools still build ([ADR 0023](docs/adr/0023-desktop-app-and-engine-thread.md)). Qt is never downloaded automatically.

On Windows the Qt build must match the compiler:

- **Visual Studio toolchain** (most reliable): install Qt with the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss), choosing *Qt 6.8 > MSVC 2022 64-bit*. Use the `windows-msvc-debug` preset or a CLion profile with the Visual Studio toolchain.
- **MinGW toolchain**: in the installer choose *Qt 6.8 > MinGW 64-bit* **and** *Build Tools > MinGW* of the same version, then add a CLion toolchain that uses that MinGW (not the bundled one), because Qt's MinGW DLLs must come from the same compiler version.

Then tell CMake where Qt is, in the profile's *CMake options*:

```
-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64        (or .../mingw_64)
```

With vcpkg, `vcpkg install` with the `app` feature builds Qt from source instead (slow). On Linux, `sudo apt install qt6-base-dev libgl1-mesa-dev` is enough. `-DMESH2CAD_BUILD_APP=ON` makes a missing Qt an error, `OFF` skips the app.

### Running outside CLion

Executables are in the build folder, for example `cmake-build-debug\src\app\mesh2cad-app.exe`, `src\cli\mesh2cad.exe`, `src\rpc\mesh2cad-rpc.exe` and `src\mcp\mesh2cad-mcp.exe`. The build copies the MinGW runtime DLLs and (via `windeployqt`) the Qt DLLs next to them, so they start from Explorer, a terminal or Claude Desktop.

## Using the desktop app

`mesh2cad-app [scan.stl | project.m2c ...]`, or drop files on the window.

| Input | Action |
| --- | --- |
| Left drag (Navigate mode), right drag (any mode) | Orbit |
| Middle drag, Shift + left drag | Pan |
| Wheel | Zoom |
| Double-click | Orbit around the point under the cursor |
| F, Ctrl+1/3/7/0 | Fit all; front, right, top, isometric view |
| 1-5 | Navigate, Brush, Lasso, Crease, Measure |
| Brush: drag | Paint faces into the current selection; Ctrl removes; `[` and `]` change the size |
| Lasso: drag | Select visible faces inside the outline; Shift adds, Ctrl removes |
| Crease: click | Select the region around a face up to edges sharper than the crease angle |
| Measure: two clicks | Save a distance measurement |
| Delete | Delete the faces of the current selection |

The **Workflow** panel lists the usual order (import, repair, fill holes, remove small shells, smooth, simplify, align, save); each step opens its form in the **Tool** panel. Every other command is in the **Tools** menu. Pick an object in the **Objects** list to make it the current mesh or selection; tool forms fill in its id.

## Working with an AI agent

While the app is open, scripts and AI agents can work on the same document: the app listens on a local socket (a named pipe on Windows) and writes its address and a per-session token to a session file that only your user can read. Changes made by an agent show in the viewport immediately and appear in **History** with the source `mcp`, and Edit > Undo reverts them.

For Claude Desktop, add the MCP server to `%APPDATA%\Claude\claude_desktop_config.json` (macOS/Linux: `~/.config/Claude/claude_desktop_config.json`) and restart Claude Desktop:

```json
{
  "mcpServers": {
    "mesh2cad": {
      "command": "C:\\Users\\you\\ClionProjects\\Mesh2Cad\\cmake-build-debug\\src\\mcp\\mesh2cad-mcp.exe"
    }
  }
}
```

`mesh2cad-mcp` connects to the running app if there is one and otherwise works on a private document (`--connect` requires the app, `--headless` never uses it). For Claude Code: `claude mcp add mesh2cad -- <path to mesh2cad-mcp>`.

Scripts can also speak JSON-RPC 2.0 directly, one message per line: start `mesh2cad-rpc` and write to its stdin, or connect to the app's socket and call `rpc.auth` with the token first. Each command is a method of the same name, for example `{"jsonrpc":"2.0","id":1,"method":"mesh.repair","params":{"id":1}}`. After each change the app sends a `doc.changed` notification.

## Using the command line

Every action is a registered command; the CLI is just one front end.

```
mesh2cad list                                        # all commands and their parameter schemas
mesh2cad run mesh.import '{"path":"scan.stl"}'       # one command on an empty document
mesh2cad batch session.jsonl                         # many commands on one shared document
```

A batch file has one JSON object per line, for example:

```
{"command":"mesh.import","params":{"path":"scan.stl"}}
{"command":"mesh.info","params":{"id":1}}
{"command":"mesh.export","params":{"id":1,"path":"copy.stl"}}
```

Each command prints one line, `{"ok":true,"result":...}` or `{"ok":false,"error":{"code":...,"message":...}}`. The exit status is 0 on success, 1 if a command failed and 2 for a usage error.

## Repository guide

| Path | Contents |
| --- | --- |
| `src/core` | Mesh, selection, transform, spatial index, statistics, errors (no third-party dependencies) |
| `src/adapters/cgal` | The only code that sees CGAL: repair, holes, simplify, remesh, wrap |
| `src/io` | STL, OBJ, PLY and project file reading and writing |
| `src/pipeline` | Shells, smoothing, alignment, selection and measuring tools |
| `src/document` | Objects, provenance and undo history (depends on `core` only) |
| `src/commands` | Command registry and every built-in command |
| `src/cli`, `src/rpc`, `src/mcp`, `src/app` | Front ends: `mesh2cad`, `mesh2cad-rpc`, `mesh2cad-mcp`, `mesh2cad-app` |
| `tests/` | Catch2 unit, stage, command and app tests |
| `scripts/check_layers.py` | Enforces the module dependency rules (runs as a ctest) |
| `docs/` | Architecture and roadmap, AI development rules, decision records |

AI coding tools: read [`AGENTS.md`](AGENTS.md) first.

## Run, debug and test in CLion

Shared run configurations live in `docs/ide/runConfigurations/`. Copy them into `.idea/runConfigurations/` once (see [`docs/ide/README.md`](docs/ide/README.md)); they then appear in the run drop-down:

| Configuration | What it does |
| --- | --- |
| `mesh2cad list` | Prints every registered command |
| `mesh2cad batch (example session)` | Runs `examples/session.jsonl` (import a cube, inspect it, undo) |
| `Mesh2Cad app` | Starts the desktop app (needs Qt) |
| `mesh2cad-mcp (headless)` | Runs the MCP server on stdin/stdout, for trying requests by hand |
| `Unit Tests`, `Stage Tests`, `Command Tests`, `App Tests` | Run one Catch2 executable |
| `All Tests` | Runs all of them |

Each can be started with **Run** or **Debug** (set breakpoints first); CLion builds the needed target before it starts. They use whichever CMake profile is selected, so the same files work on Windows and Linux.

Choosing a CMake profile:

- **Default `Debug` profile (no setup).** If vcpkg is not configured, the first CMake load downloads nlohmann/json and Catch2 from GitHub (git must be on your PATH). This takes a minute; later loads are cached.
- **Presets (`windows-mingw-debug`, `windows-msvc-debug`, ...).** Enable them under *Settings | Build, Execution, Deployment | CMake*. Each preset names the CLion toolchain it needs (`MinGW` or `Visual Studio`), so that toolchain must exist under *Settings | Build, Execution, Deployment | Toolchains*. They use vcpkg when `VCPKG_ROOT` is set and otherwise download the libraries like the default profile (with a warning). This is what CI uses.

If a run configuration says its executable cannot be found, the CMake load failed: open the **CMake** tool window, read the error, fix it and use *Reset Cache and Reload Project*.

See [`tests/README.md`](tests/README.md) for running tests from the terminal, filtering by label or tag, and writing new tests.

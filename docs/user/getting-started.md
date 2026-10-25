# Getting started

## Installing

- **Windows 10 and 11**: run `mesh2cad-<version>-windows-x64.exe`. It installs into `C:\Program Files\Mesh2Cad` and adds **Mesh2Cad** to the Start menu. During installation you can also add the command-line tools to your `PATH`. The `.zip` contains the same files if you'd rather not install.
- **Ubuntu 24.04**: `sudo apt install ./mesh2cad_<version>_amd64.deb`. This also installs the libraries Mesh2Cad needs from Ubuntu. Start it from the applications menu or with `mesh2cad-app`.

You get four programs:

| Program | What it is |
| --- | --- |
| `mesh2cad-app` | The desktop app |
| `mesh2cad` | The command-line tool: runs one command, or a session file of commands |
| `mesh2cad-rpc` | A headless engine for scripts, using JSON-RPC over its input and output |
| `mesh2cad-mcp` | The MCP server for AI agents such as Claude |

## The window

- **Viewport**, in the middle. Orbit with a left drag (in Navigate mode, or off the mesh in Brush, Crease, Measure and Shell) or a right drag (in any mode). Pan with a middle drag or Shift + left drag, and zoom with the wheel. Double-click a point to orbit around it. F fits everything in view, and Ctrl+1, Ctrl+3, Ctrl+7 and Ctrl+0 give the front, right, top and isometric views.
- **Objects**, on the left, lists meshes, selections, measurements, quad layouts, solids, primitives, sketches and deviation maps, each with its id. Click an object to make it current: tool forms then fill in its id. Right-click an object for the tools that work on its kind (for a quad layout, Build surface; for a sketch, Extrude and Revolve), each opening its form with the object filled in; **Change and run ... again** opens the command that made the object with the values it was made with, to adjust and run again (it adds a new object). The menu also fits the view to the object, renames it, shows its details or deletes it.
- **Workflow**, on the left, gives the steps in order as four tracks you can fold away: prepare and align (import, repair, fill holes, shells, smooth, simplify, remesh, align), path A for organic surfacing (quad remesh, build surface), path B for prismatic parts (segment, fit primitives, trim, section sketch, extrude, revolve, boolean, fillet), and verify and export (deviation, STEP, save). At its top, the stage gate tells you whether the current object is ready for the next stage, and what to do if not: a mesh must be closed, manifold and one shell before surfacing, a solid one valid closed solid before export, and a deviation's 95th percentile within its tolerance.
- **Tool**, on the right, shows the form for the command you have chosen. Every command is in the **Tools** menu, grouped by task (mesh cleanup, selection, alignment, primitives, sketches and solids, surfacing, inspection, files), with the command's name beside its title. The objects it works on and the values it needs come first; settings with a default you seldom change are folded under **Advanced**, and **Reset to defaults** puts them back. A setting left at its default is not sent, so the command's own default applies.
- **History**, on the right, lists each change with where it came from (the app, a script or an agent). **Edit > Undo** and **Edit > Redo** step through it, and a double-click on a step undoes or redoes to just after it ((start) undoes everything).
- **Inspector**, beside History, shows an object's **Info** and the result of each read-only tool (mesh info, shells, solid info, deviation) as rows of names and values, with units, and problems such as open edges in amber. **Copy as JSON** copies the whole result. Click a shell or a deviation hotspot to look at it. For shells, check some and press **Select checked**, or **Keep main** or **Split others off**; for a deviation, **Select hotspots** measures again with the same settings and makes each hotspot a selection. Picking a deviation in Objects shows it here.
- **Log**, at the bottom, shows each command as it runs and any errors. While a long command runs (quad layout, surface build, deviation), the status bar shows its stage and a **Cancel** button.

**Help > User guide** (F1) opens this guide in its own window.

Keys 1 to 6 switch the mouse mode: Navigate, Brush (paint faces into a selection), Lasso (select the visible faces inside an outline), Crease (select a smooth region bounded by sharp edges), Measure (click two points) and Shell (select the whole connected part you click).

## Opening a scan and saving a project

Use **File > Import mesh** (Ctrl+I), or drop STL, OBJ or PLY files onto the window. **File > Save project** (Ctrl+S) writes a `.m2c` project file. It keeps everything: meshes, selections, measurements, alignments, quad layouts, solids, primitives, sketches and deviation maps.

The same steps as a session:

```jsonl
{"command": "mesh.import", "params": {"path": "bracket.stl"}}
{"command": "mesh.info", "params": {"id": 1}}
{"command": "project.save", "params": {"path": "bracket.m2c"}}
```

`mesh.info` reports the mesh's triangle count, size, area and volume. It also reports any problems: holes (boundary edges), non-manifold edges and degenerate faces. A scan that is closed with no problems says `"is_watertight": true`.

## Which route to take

1. Prepare the mesh ([Mesh preparation](mesh-preparation.md)) until it is watertight and sits square to the origin.
2. Rebuild it as a solid:
   - For a smooth, organic shape such as a controller, a handle or a figure, use [Organic parts](organic-parts.md). This route lays a quad cage over the scan and fits B-spline surfaces to it.
   - For a machined shape made of planes, cylinders and other simple surfaces, such as a bracket or a housing, use [Prismatic and hybrid parts](prismatic-parts.md). This route fits exact surfaces and trims them against each other, so edges come out sharp. A part that is mostly organic with a few machined features is surfaced first, then its holes and faces are cut in with booleans.
3. Check it against the scan with a deviation map, then [export](export.md) it.

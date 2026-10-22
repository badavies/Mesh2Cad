# Mesh2Cad user guide

Mesh2Cad turns 3D scans into CAD. You import a scanned mesh (STL, OBJ or PLY), clean it up, and rebuild it as a watertight solid. Organic shapes get smooth B-spline surfaces, and machined shapes get exact planes, cylinders and sharp edges. You then export the result: STEP for Fusion 360 and other CAD programs, or GLB, OBJ and STL meshes for Blender and 3D printing.

| Page | What it covers |
| --- | --- |
| [Getting started](getting-started.md) | Installing, the window, opening a scan, saving a project |
| [Mesh preparation](mesh-preparation.md) | Repair, holes, loose parts, smoothing, simplifying, wrapping, aligning, measuring |
| [Organic parts](organic-parts.md) | Quad layout, surface build, refinement, deviation map and report |
| [Prismatic and hybrid parts](prismatic-parts.md) | Segmentation, primitive and box fits, trimming, fillets, sketches, extrude, revolve, booleans |
| [Export](export.md) | STEP for CAD, and GLB, glTF, OBJ and STL meshes with their settings |
| [Automation and AI agents](automation.md) | The command line, JSON-RPC and MCP |
| [Troubleshooting](troubleshooting.md) | Common failure messages and what to do about them |

Every command is listed with all of its parameters in the [command reference](../COMMANDS.md). The desktop app, the command line, scripts and AI agents all use the same commands, so a step in this guide works the same way from any of them.

## Following the examples

Each page has sessions you can run as they stand. They use the test meshes that come with Mesh2Cad, in `examples/test-meshes`. In an installed copy, that folder is `C:\Program Files\Mesh2Cad\doc\examples\test-meshes` on Windows and `/usr/share/mesh2cad/examples/test-meshes` on Linux. Copy the folder somewhere you can write to, because the sessions write their output files next to the meshes. Then go into it and run a session with the command-line tool:

```
cd test-meshes
mesh2cad batch session.jsonl
```

Each line of a session is one command and its parameters. In the app, you get the same result by choosing the command from the **Tools** menu and filling in its form with the same values. Objects are numbered in the order they are made, starting at 1, so the ids in a session (such as `"id": 1`) refer to objects made earlier in that same session.

All lengths are in millimetres.

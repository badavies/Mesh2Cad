# Golden scans

Real scans that gate each milestone (`docs/ARCHITECTURE.md` sections 10 and 12). The session and the limits for each scan are in the repository; the scan itself is too large for Git, so each folder keeps a local, gitignored copy of it named `scan.<ext>`.

One folder per scan:

```
tests/golden/dji_controller_3/
  scan.obj         local copy of the raw scan (gitignored; see "Getting the scans")
  session.jsonl    the commands to run, one JSON object per line
  metrics.json     which scan file, and the limits checked after the session
```

| Folder | Source scan | What it exercises |
| --- | --- | --- |
| `dji_controller_3` | `D:\Revopoint\DJI Controller 3.obj` | Closed organic body: shell clean-up, repair, principal alignment |
| `dewalt_multitool` | `D:\Revopoint\Dewalt Multitool.obj` | 145 holes, some in folded pockets the CGAL triangulation refuses |
| `siemens_coffee_lid` | `D:\Revopoint\Siemens Coffe Machine Lid.obj` | A single-sided sheet: closed with the shrink-wrap fallback, not hole filling |
| `tyranid_v2` | `D:\Revopoint\tyranid v2.obj` | Quad faces, non-manifold edges, inconsistent orientation |
| `dji_controller_3_surface` | the `dji_controller_3` copy | The M2 gate: the M1 session, then quad layout, surface build, deviation, STEP export and the deviation report |

## Getting the scans

Copy each source scan into its folder as `scan.obj` (the name in `metrics.json` under `scan.file`; `dji_controller_3_surface` reads the copy in `dji_controller_3`, so it needs none of its own). The original stays where it is. `metrics.json` records the size and SHA-256 of the exact file the limits were measured on, and the test fails if the copy differs; check a copy with `certutil -hashfile scan.obj SHA256` (Windows) or `sha256sum scan.obj`. A folder without its scan is skipped with a warning, so CI, which has no scans, reports the test as skipped.

To keep the whole golden set somewhere else, point `M2C_GOLDEN_DIR` at a folder laid out the same way.

## File formats

`session.jsonl` uses the format of `mesh2cad batch` and of the app's session log, so a session recorded in the app can be copied in. Relative paths, in the session and in the checks, are resolved against the scan folder. Files a session writes should be named `scan.*` (for example `scan.step`), which keeps them out of Git; the STEP file is the one to take through `docs/FUSION_CHECKLIST.md`.

```
{"command":"mesh.import","params":{"path":"scan.obj"}}
{"command":"mesh.repair","params":{"id":1}}
{"command":"holes.fill","params":{"id":1}}
{"command":"align.principal","params":{"id":1,"origin":"bounds_bottom_center"}}
```

`metrics.json` names the scan and lists query commands to run on the final document and what their results must contain. Keys are JSON pointers into the result; a plain value must match exactly, `{"min": a, "max": b}` is an inclusive range.

```json
{
  "scan": {"file": "scan.obj", "source": "D:\\Revopoint\\DJI Controller 3.obj", "bytes": 49034218, "sha256": "a3d2..."},
  "session": "session.jsonl",
  "max_seconds": 30,
  "checks": [
    {
      "command": "mesh.info",
      "params": {"id": 1},
      "expect": {
        "/stats/is_watertight": true,
        "/stats/non_manifold_edge_count": 0,
        "/stats/face_count": {"min": 612575, "max": 677056},
        "/stats/bounds/min/2": {"min": -0.001, "max": 0.001}
      }
    }
  ]
}
```

The gate limits are fixed: watertight, no non-manifold or boundary edges, positive volume (outward faces), and the part sitting on Z = 0 after alignment. The other limits were set from the first run: face count within 5%, volume within 2% and the aligned bounding box within 1%, so a change in repair or a turned alignment shows up. `max_seconds` is about four times the first optimised (Release) run and is not checked in debug builds.

`dji_controller_3_surface` (M2) requires one valid, closed solid with one shell, no free edges and a STEP file that reads back as the same solid; its face count is within 5% and its volume within 2% of the first run. Its deviation limits are the first run's values with some room (RMS 3.62 mm, 95th percentile 0.78 mm, 70% of points within 0.1 mm, largest 26.2 mm) and are there for M3 to tighten towards the M3 gate of 0.1 mm at the 95th percentile.

The test replays the session through the registry (CLI path), the JSON channel and the MCP server, requires identical documents from all three, then checks the metrics. Run it on a Release build with `ctest -L golden`; it takes a few minutes.

CI, including the nightly run, has no scans and reports the golden test as skipped. Running it is a manual step on a machine with the scans, before each milestone gate.

Changing a stored limit is a separate, human-reviewed commit that says why (`docs/AI_GUIDELINES.md` rule 10).

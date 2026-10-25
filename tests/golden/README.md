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

`session.jsonl` uses the format of `mesh2cad batch` and of the app's session log, so a session recorded in the app can be copied in. The sessions fill holes with `"max_edges":0`, so every hole is filled whatever the default, as when the limits were measured (`dewalt_multitool` has six gaps of more than 100 edges). Relative paths, in the session and in the checks, are resolved against the scan folder. Files a session writes should be named `scan.*` (for example `scan.step`), which keeps them out of Git; the STEP file is the one to take through `docs/FUSION_CHECKLIST.md`, and a GLB written with `solid.export.mesh` the one for `docs/BLENDER_CHECKLIST.md`.

```
{"command":"mesh.import","params":{"path":"scan.obj"}}
{"command":"mesh.repair","params":{"id":1}}
{"command":"holes.fill","params":{"id":1,"max_edges":0}}
{"command":"align.principal","params":{"id":1,"origin":"bounds_bottom_center"}}
```

`metrics.json` names the scan and lists query commands to run on the final document and what their results must contain. A check with `"step": n` instead of a command reads the result of the session's n-th line (counting from 1), for figures only that command reports, such as `surface.build`'s seam angles. Keys are JSON pointers into the result; a plain value must match exactly, `{"min": a, "max": b}` is an inclusive range.

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

`dji_controller_3_surface` (M2) requires one valid, closed solid with one shell, no free edges and a STEP file that reads back as the same solid; its face count is within 5% and its volume within 2% of the first run. Since M3 its session uses an 8,000-quad cage and a tighter fit. The 95th percentile limit is the M3 gate, 0.1 mm. The other deviation limits are the measured values with some room: 95% of points within 0.1 mm, RMS 0.1 mm and largest 2 mm, now that the thumbstick posts are joined to the body (#25). Two step checks cover the session's own figures. On `quad.remesh`, one part is joined and at most 10 faces are left out. On `surface.build`, the fit needs one try, seams next to caps stay under 4 degrees, and seams between regular pieces stay under 0.001 degrees (#26).

The test replays the session through the registry (CLI path), the JSON channel and the MCP server, requires identical documents from all three, then checks the metrics. Run it on a Release build with `ctest -L golden`; it takes a few minutes.

CI, including the nightly run, has no scans and reports the golden test as skipped. Running it is a manual step on a machine with the scans, before each milestone gate.

Changing a stored limit is a separate, human-reviewed commit that says why (`docs/AI_GUIDELINES.md` rule 10).

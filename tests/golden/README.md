# Golden scans

Real scans that gate each milestone (`docs/ARCHITECTURE.md` sections 10 and 12). The scans are stored in Git LFS (see `.gitattributes`); keep very large ones outside the repository and point `M2C_GOLDEN_DIR` at their parent folder.

One folder per scan:

```
tests/golden/controller/
  scan.stl         the raw scan (STL, OBJ or PLY; Git LFS)
  session.jsonl    the commands to run, one JSON object per line
  metrics.json     limits checked after the session
```

`session.jsonl` uses the format of `mesh2cad batch` and of the app's session log, so a session recorded in the app can be copied in. Relative paths are resolved against the scan folder.

```
{"command":"mesh.import","params":{"path":"scan.stl"}}
{"command":"mesh.repair","params":{"id":1}}
{"command":"holes.fill","params":{"id":1}}
{"command":"align.principal","params":{"id":1,"origin":"bounds_bottom_center"}}
```

`metrics.json` lists query commands to run on the final document and what their results must contain. Keys are JSON pointers into the result; a plain value must match exactly, `{"min": a, "max": b}` is an inclusive range.

```json
{
  "session": "session.jsonl",
  "max_seconds": 120,
  "checks": [
    {
      "command": "mesh.info",
      "params": {"id": 1},
      "expect": {
        "/stats/is_watertight": true,
        "/stats/non_manifold_edge_count": 0,
        "/stats/face_count": {"min": 200000, "max": 2000000},
        "/stats/bounds/min/2": {"min": -0.01, "max": 0.01}
      }
    }
  ]
}
```

The test replays the session through the registry (CLI path), the JSON channel and the MCP server, requires identical documents from all three, then checks the metrics. Run it with `ctest -L golden`; without scans it is reported as skipped.

Changing a stored limit is a separate, human-reviewed commit that says why (`docs/AI_GUIDELINES.md` rule 10).

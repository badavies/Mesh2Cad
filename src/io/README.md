# io (`Mesh2Cad::io`)

Reads and writes mesh files, STEP files, reports and the project file. Mesh readers weld duplicate positions, drop
unused vertices and validate the result through `TriMesh::create`. The module works on core
types only; it cannot see the document, so the command engine converts between project data and
document objects.

## Public API

- `m2c/io/MeshIO.h`: `readMesh` and `writeMesh`, which pick STL, OBJ or PLY from the file extension.
- `m2c/io/Stl.h`: `readStl` (binary or ASCII) and `writeStl` (binary, float coordinates).
- `m2c/io/Obj.h`: `readObj` (positions and faces only) and `writeObj` (round-trips exactly).
- `m2c/io/Ply.h`: `readPly` (ASCII or binary, either byte order), `readPlyUnwelded` (the project file's mesh reader, which keeps vertices as stored) and `writePly` (binary little-endian, double precision).
- `m2c/io/Step.h`: `writeStep` (AP242, millimetres; one solid, or several `StepBody`s of solids and sketches as named products), `readStep` returning a `SolidSummary` of the whole file and `readStepBodies` one per body, from the Open CASCADE adapter.
- `m2c/io/Png.h`: `encodePng` and `writePng`, an `Image` as an 8-bit RGB PNG (miniz).
- `m2c/io/Report.h`: `writeDeviationReport`, a deviation's statistics and histogram as CSV.
- `m2c/io/Project.h`: `ProjectData`, `ProjectObject`, `ProjectMesh`, `ProjectSelection`, `ProjectDeviation`, `kProjectFormatVersion`, `writeProject` and `readProject`.

Private: `detail/Parse.h` (`LineReader`, `weldAndCompact`, `appendFan` and number parsing),
shared by the readers.

## Dependencies

May use: `core`; `adapters` for B-rep files (STEP now, B-rep in the project file); miniz and
nlohmann_json (linked privately, for the project zip and manifest).
Must not depend on: `document`, `pipeline`, `commands`, `app`, `cli`, `rpc`, `mcp`,
and kernel library headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by
`scripts/check_layers.py`.

## Notes

- Open input files in binary mode. STL detection relies on the exact binary size rule.
- The project file (format 6) is a zip with `manifest.json`, meshes as binary PLY, selections as
  bitsets, quad layouts and deviations as length-prefixed arrays, solids as Open CASCADE
  binary BREP (bit-exact) plus their B-spline faces, and fitted primitives and sketches in the manifest; a
  solid's display mesh is rebuilt from its faces. A solid trimmed from primitives has no
  B-spline faces: it is the BREP alone, and its display is the kernel's triangulation again.
  Format 3, 4 and 5 files hold a subset and open unchanged. Format 2 files (BREP text, one face per cage quad) are migrated on reading; format 1
  files hold a subset and open unchanged. The file is written next to the target and renamed
  into place. A newer format version is refused with
  `ErrorCode::Unsupported`; a format change needs a migration and a test.
- The readers are fuzzed (`tests/fuzz`). Keep reserve sizes bounded with `safeReserve`.

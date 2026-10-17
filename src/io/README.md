# io (`Mesh2Cad::io`)

Reads and writes mesh files, STEP files and the project file. Mesh readers weld duplicate positions, drop
unused vertices and validate the result through `TriMesh::create`. The module works on core
types only; it cannot see the document, so the command engine converts between project data and
document objects.

## Public API

- `m2c/io/MeshIO.h`: `readMesh` and `writeMesh`, which pick STL, OBJ or PLY from the file extension.
- `m2c/io/Stl.h`: `readStl` (binary or ASCII) and `writeStl` (binary, float coordinates).
- `m2c/io/Obj.h`: `readObj` (positions and faces only) and `writeObj` (round-trips exactly).
- `m2c/io/Ply.h`: `readPly` (ASCII or binary, either byte order) and `writePly` (binary little-endian, double precision).
- `m2c/io/Step.h`: `writeStep` (AP242, millimetres) and `readStep` returning a `SolidSummary` of the whole file, from the Open CASCADE adapter.
- `m2c/io/Project.h`: `ProjectData`, `ProjectObject`, `ProjectMesh`, `ProjectSelection`, `kProjectFormatVersion`, `writeProject` and `readProject`.

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
- The project file is a zip with `manifest.json`, meshes as binary PLY and selections as bitsets.
  It is written next to the target and renamed into place. A newer format version is refused with
  `ErrorCode::Unsupported`; a format change needs a migration and a test.
- The readers are fuzzed (`tests/fuzz`). Keep reserve sizes bounded with `safeReserve`.

# 0037. Our own glTF writer; Open CASCADE only triangulates

- Status: Accepted
- Date: 2026-10-22

## Context
Issue #120 (M5): solids must leave as GLB or glTF for Blender, and as OBJ and STL. ARCHITECTURE.md section 7 planned "OCCT tessellation, then OCCT glTF writer" (`RWGltf_CafWriter`) and OCCT's OBJ writer.

`RWGltf_CafWriter` writes JSON through RapidJSON, which is an optional Open CASCADE feature: without it the writer fails at run time. Our vcpkg manifest takes `opencascade` with no default features, and the source fallback (ADR-026) builds only the toolkits the adapter needs, so neither has it; Ubuntu's package may or may not. The writer also takes an XCAF document (`TDocStd_Document` with shape and colour tools), which pulls in TKXCAF, TKLCAF, TKCAF and TKVCAF, the last of which depends on the visualisation toolkit TKService. Using it would add a dependency (RapidJSON), four to six toolkits, a rebuild of the cached Open CASCADE in CI on every platform, and a writer that can be missing at run time.

The glTF 2.0 we need is small: one node, one mesh, positions, normals and indices in one buffer, one material. The project already writes STL, OBJ and PLY itself (ADR-021) and has nlohmann_json for the project manifest.

## Decision
- Open CASCADE triangulates the solid (`BRepMesh_IncrementalMesh` at the export's chord and angle, on a copy of the topology so the solid's display mesh is unchanged), and the adapter welds the faces into one closed `TriMesh` (`occt::exportTriangulation`).
- `src/io` writes glTF itself (`writeGltf`): GLB, or `.gltf` with the buffer embedded as a base64 data URI so it is one file. Positions are in metres and Y up, as the glTF spec requires (Blender's importer turns them back to millimetre-sized, Z-up objects). Normals are per corner, split where neighbouring triangles turn more than a crease angle, so curved faces shade smoothly and sharp edges stay sharp.
- OBJ and STL use the existing writers (`writeMesh`): the same triangulation, Z up and millimetres as before.

## Alternatives considered
- OCCT's `RWGltf_CafWriter` and `RWObj_CafWriter`: as above, a new dependency and toolkits for a few hundred lines of file format, and the glTF writer silently unavailable on builds without RapidJSON.
- tinygltf or cgltf: small and permissive, but a new dependency (and a decision record) for writing one buffer and a JSON header nlohmann_json already handles.
- Normals from the B-rep surface at each node: exact, but they need the faces' parameters carried through welding and a new core type; crease-split mesh normals at the export chord look the same in Blender.

## Consequences
- The writer is tested by parsing what it writes (`tests/unit/GltfTests.cpp`), and the M5 gate opens the files in Blender itself (issue #121), which is the real check.
- Only what we write is supported: no textures, colours per face, multiple bodies or scene hierarchy. A later need for those is a new decision (tinygltf or OCCT's writer then).
- ARCHITECTURE.md section 7's export table names this writer instead of OCCT's.

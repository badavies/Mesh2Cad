# 0038. Scan spikes and tunnels found by our own search, as a selection

- Status: Accepted
- Date: 2026-10-24

## Context
Scanner software that meshes a point cloud leaves small defects where stray points were: thin spikes standing out of the surface, slivers folded back on themselves, and small tunnels (a strap of surface bridging over the scan, or a tube through it). Ben asked for these to be found below a face count and selected, so they can be deleted and the holes filled (Geomagic Wrap's Mesh Doctor and Design X's Healing Wizard do the same). His Revopoint scan of a bust (1.53 million faces) has an Euler characteristic of -3595 with 682 boundary edges: about 1500 tunnels, each a real handle of the surface, plus several thousand sharp tips and folded edges.

ARCHITECTURE.md section 7 listed spike removal under clean-up (`mesh.repair`) as not implemented. The change is over the 400-line guideline (about 540 lines of code in `src/pipeline/Defects.cpp`), hence this record.

## Decision
- One command, `select.defects`, returns a selection (`findScanDefects` in `src/pipeline/Defects.cpp`). Deleting and filling stay the existing `faces.delete` and `holes.fill`, so the user sees what goes first.
- Spikes: seeds are tips (a closed vertex whose face corners add up to less than `tip_angle_deg`, 150) and folds (an edge whose faces' normals differ by more than `fold_angle_deg`, 150). Seed faces are grouped into connected clusters; a cluster of at most `max_faces` grows over neighbouring faces steeper than 60 degrees against the area-weighted normal of the surrounding surface, so the whole spike goes, and falls back to its seeds if that passes `max_faces` (a real wall).
- Tunnels are found from topology, not shape. A patch grown in breadth-first rings around a face keeps its Euler characteristic up to date face by face (counting a vertex once per fan of patch faces around it); only where that drops to -1 or below are its boundary loops walked and the genus, (2 - characteristic - loops) / 2, worked out, then confirmed with an exact count. Every vertex is covered by a patch of 4 `max_faces` faces grown near it: a patch without a handle clears the vertices of its inner third of rings, whose own `max_faces` patches it contains. A patch with a handle is moved towards the centre that holds the handle in the fewest faces (to the middle of the path between the faces that close it from either side, then to neighbouring faces while that helps), selected if at most `max_faces`, and treated as deleted while the search goes on, so each handle is found once and the search ends.
- Parts whose genus is 0 are skipped. The search is serial, so the result does not depend on threads.

## Alternatives considered
- CGAL Polygon Mesh Processing: it has self-intersection removal, sharp-edge detection and needle and cap removal (badly shaped triangles, not spikes), but nothing that finds handles. Alpha wrapping removes tunnels by rebuilding the whole mesh, losing the scan's detail.
- Every vertex as a seed with a `max_faces` patch, run in parallel: exact, but 7 seconds at 200 faces and 55 seconds at 1000 on the bust. The covering search takes 4 and 11 seconds and finds about as many tunnels (1499 against 1502 at 200 faces) in fewer faces.
- Short non-separating cycles (Guskov and Wood's topological noise removal) or Reeb graphs (Wood et al. 2004): cut a handle along its shortest loop, which on a scan strap leaves two dangling flaps; selecting a patch that holds the whole handle leaves one hole that fills cleanly.
- A size limit in millimetres as well as faces: not asked for. It would matter on coarse meshes (below).

## Consequences
- On the bust, one pass selects 2108 spikes and 1499 tunnels (about 51,000 faces, 3% of the mesh). After delete and fill, a second pass finds 7 tunnels and a third none. Spikes fall more slowly (1002, then 414): many of those left are folds where the scan overlaps itself, which `mesh.repair` reports as self-intersections. The 3.8 million face vape scan has 27 spikes and 23 tunnels and takes 6 seconds. The test meshes in `examples/test-meshes` (noisy, scanned and damaged stand-ins) give none, apart from the coarse bracket below.
- `max_faces` counts faces, so it depends on the mesh's resolution: on the 284-face bracket a 10 mm through-hole is a 75-face tunnel. The user guide says so.
- The search expects a manifold, consistently oriented mesh (`mesh.repair` first); on other meshes folds and genus can be wrong.
- Tunnels between `max_faces` and 4 `max_faces` are reported as `too_large`; bigger ones are not seen at all.

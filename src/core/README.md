# core (`Mesh2Cad::core`)

App-owned geometry types that every other module shares. Meshes are immutable and passed by value
or `std::shared_ptr<const T>`, so undo is cheap and worker threads are safe. Lengths are in
millimetres. Errors cross module boundaries as `Result<T>` (`std::expected<T, Error>`).

## Public API

- `m2c/core/Vec3.h`: `Vec3d` (a point or vector in mm) with `dot`, `cross`, `length` and arithmetic operators.
- `m2c/core/Error.h`: `ErrorCode`, `Error`, `Result<T>`, `toString(ErrorCode)` and the `fail(code, message)` shorthand.
- `m2c/core/Parallel.h`: `parallelChunks`, a parallel loop over fixed chunks with oneTBB, the same for any thread count (ADR-036), and `ThreadLimit`, which caps the threads while it lives.
- `m2c/core/Progress.h`: `Progress` (a callback told the stage a long operation is at, and how far through it, with a `CancelToken` a front end can cancel from another thread), `report`, which calls it when set, and `checkCancelled` and `checkpoint`, which fail with `ErrorCode::Cancelled` once the token is cancelled.
- `m2c/core/TriMesh.h`: `TriMesh` (immutable indexed triangle mesh, built with `TriMesh::create`) and `Aabb`.
- `m2c/core/MeshStats.h`: `MeshStats` and `computeStats` (counts, boundary and non-manifold edges, area, volume, bounds).
- `m2c/core/MeshTopology.h`: `FaceAdjacency`, `buildFaceAdjacency`, `faceComponents`, `faceNormal`, `faceArea`, `extractFaces`.
- `m2c/core/QuadLayout.h`: `QuadLayout` (immutable polygon control cage, mostly quads, with `withPositions`), `CageStats` with `cageStats`, `signedVolume`, `cageEdges` (each edge once, for drawing) and `sharpCageEdges` (where a closed cage turns sharply).
- `m2c/core/SurfacePatch.h`: `BicubicPatch` and `SurfacePatch` (one B-spline face of the surface build: spans x spans Bezier pieces), `bernstein`, `bernsteinDerivative`, `evaluate` and `normal`.
- `m2c/core/Solid.h`: `Solid` (the kernel shape as the opaque `SolidShape`, its faces and a display mesh), `SolidSummary`, `tessellate` for faces, and `measureSeams` returning a `SeamReport` (gaps and normal angles where faces meet, sharp edges left out of the angles) and `measureFairness` returning a `FairnessReport` (how fast the mean curvature changes across each face).
- `m2c/core/DeviationField.h`: `DeviationField` (signed distance per mesh vertex, positive outside), `DeviationStats` and `histogram` returning `DeviationHistogram`.
- `m2c/core/SpatialIndex.h`: `SpatialIndex` (BVH with `raycast`, `closestPoint`, `facesWithin`), `RayHit`, `ClosestPoint`, `closestPointOnTriangle`.
- `m2c/core/MeshSelection.h`: `MeshSelection`, a bitset of face indices on one mesh.
- `m2c/core/Transform.h`: `Transform3d` (rigid rotation plus translation) and `transformMesh`.
- `m2c/core/Raster.h`: `Raster`, a software rasteriser (orthographic, flat shaded like the app's viewport, depth tested) drawing triangles and lines into an `Image` of `Rgb` pixels, for `view.render`; it also draws an axis triad and upper-case labels.
- `m2c/core/SelectionColors.h`: `selectionColor(id)`, the colour of a non-current selection in the viewport and in `view.render`.
- `m2c/core/Base64.h`: `encodeBase64`, for pictures returned inside JSON results.
- `m2c/core/DeviationColors.h`: `deviationColor` (the deviation colour map, standard or colour-blind safe, as `Rgb8`) and `deviationRange`.
- `m2c/core/Sketch.h`: `Sketch` (a `SketchPlane` and closed `SketchLoop`s of `SketchCurve` lines, arcs and splines), `Vec2d`, `planeThrough`, `curveKindName`, `distanceTo` for a curve, and `sketchLines` (the curves as world-space segments, for drawing).
- `m2c/core/Primitive.h`: `Primitive` (an unbounded plane, cylinder, sphere or cone) and `FittedPrimitive`, `OrientedBox` (a cuboid at any angle), `distanceTo` and `normalAt`, and `primitiveDisplay` for drawing.
- `m2c/core/Measurement.h`: `Measurement`, `PickedPoint` and `SnapKind` for point-to-point distances.
- `m2c/core/DisplayProxy.h`: `buildDisplayProxy`, a decimated stand-in mesh for drawing while the camera moves.

## Dependencies

May use: the C++ standard library only. The target links no third-party library.
Must not depend on: `adapters`, `io`, `document`, `pipeline`, `commands`, `app`, `cli`, `rpc`,
`mcp`, and kernel library headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by
`scripts/check_layers.py`.

## Notes

- `TriMesh` never changes after `create`. Its position and index buffers are shared
  individually, so a tool that only moves vertices can reuse the index buffer.
- `TriMesh::spatialIndex()` and `TriMesh::faceAdjacency()` are built lazily on first use
  (thread-safe) and shared by every copy of the mesh. They never go stale because the mesh is
  immutable.
- `computeStats` runs serially in index order and `faceComponents` numbers components by lowest
  face index, so results are reproducible.
- Do not add a type here until a stage needs it (no `StageReport` yet). The roadmap's `SolidRef`
  is `Solid`.

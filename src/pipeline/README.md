# pipeline (`Mesh2Cad::pipeline`)

Processing stages and tools that turn core types into new core types: mesh tools, selection,
alignment, shells, smoothing, measurement, the quad layout, the surface build and deviation
analysis. Each function is pure: it takes immutable inputs
and returns a new result or an `Error`, and never touches the document. Algorithms that need
CGAL come from the CGAL adapter, the quad remesher from the quad adapter, subdivision from the
OpenSubdiv adapter and solids from the Open CASCADE adapter.

## Public API

- `m2c/pipeline/MeshTools.h`: `deleteFaces`, `keepFaces`, `flipFaces`, plus `simplifyMesh`, `remeshMesh` and re-exports of the adapter's `repair`, `fillHoles` and `shrinkWrap` with their parameter types.
- `m2c/pipeline/Smooth.h`: `smoothTaubin` with `SmoothParams` (whole mesh or a selection; shares the face buffer).
- `m2c/pipeline/Select.h`: `brushSelect`, `creaseFloodSelect`, `lassoSelect` with `LassoParams`, `growSelection` and `allFaces`.
- `m2c/pipeline/Shells.h`: `detectShells` returning `ShellReport` and `ShellInfo`, plus `shellFaces` and `smallShellFaces`.
- `m2c/pipeline/Align.h`: `fitPlane` returning `PlaneFit`, `principalAxesAlignment` with `AlignOrigin`, `symmetryAlignment` returning `SymmetryAlignment`, and `datumAlignment` with `DatumAlignParams` and `DatumAlignResult`.
- `m2c/pipeline/Measure.h`: `snapToMesh` and `pickMesh`.
- `m2c/pipeline/QuadRemesh.h`: `quadRemesh` with `QuadRemeshParams` and `CageCloseParams` returning `QuadRemeshResult` (closed cage, `CageStats`, distances), and `closeCage` with its `CageCleanup` report (loose closed parts, such as a post whose stem the remesher missed, are joined with a tube of quads).
- `m2c/pipeline/Deviation.h`: `deviationToSolid` (exact, projected onto the faces by the Open CASCADE adapter) and `deviationToMesh` (through the reference's spatial index).
- `m2c/pipeline/PatchMerge.h`: `partitionQuads` returning `QuadPartition` (rectangles of cage quads between lines traced from extraordinary vertices) and `mergeFaces` returning `MergeResult` (one multi-span face per rectangle, same surface).
- `m2c/pipeline/Surface.h`: `buildSurface` with `SurfaceBuildParams` returning `SurfaceBuildResult` (the solid, the fitted cage, fit, cap, seam and merge figures; a fit that folds the surface is retried with more smoothing; a surface that crosses itself is refused, checked on the kernel's triangulation; cage edges sharper than `creaseAngleDeg` stay sharp edges; `FlatArea`s, a selection of the scan and a plane, are held flat while fitting) and `heldPlanes` (the cage vertices each flat area holds), plus re-exports of the OpenSubdiv adapter's `fitCage`, `extractPatches` and `tessellateLimit` with their parameter types.
- `m2c/pipeline/Boolean.h`: re-exports the Open CASCADE adapter's `booleanSolids` with `BooleanOp` (fuse, cut or common of two solids into one solid) `primitiveSolid` (a fitted cylinder or sphere as a solid) and `boxSolid` (a box as a solid).
- `m2c/pipeline/FilletFit.h`: `fitFillet` with `FilletFitParams` returning `FilletFitResult` (edges rounded at the radius that brings the solid closest to a scanned fillet band: a growing bracket, then a golden-section search; ADR-033), plus a re-export of the Open CASCADE adapter's `filletEdges`.
- `m2c/pipeline/BoxFit.h`: `fitBox` with `BoxFitParams` returning `BoxFit` (an exact box fitted to a region: axes from the faces' normals, then Gauss-Newton on the distance to the nearest face).
- `m2c/pipeline/Profile.h`: re-exports the Open CASCADE adapter's `extrudeSketch` and `revolveSketch` (a sketch's profile as a solid).
- `m2c/pipeline/Section.h`: `sectionMesh` with `SectionParams` returning `SectionResult` (a sketch of the closed pieces where a plane cuts a mesh), and `fitLoop` returning `LoopFit` (lines, arcs and splines fitted to one closed polyline).
- `m2c/pipeline/PrimitiveFit.h`: `fitRegion` with `RegionFitParams` and `fitPrimitive` returning `PrimitiveFit` (an exact plane, cylinder, sphere or cone fitted to a region), `fitWithDirection` (the same with the normal or axis held) and `alignedDirection` (a direction parallel, perpendicular or at an angle to a reference), plus `regionPoints` and `turnAngle`.
- `m2c/pipeline/Trim.h`: re-exports the Open CASCADE adapter's `trimPrimitives` with `TrimParams` and `TrimResult` (fitted primitives split against each other, the faces on the scan closed into a solid with sharp edges).

## Dependencies

May use: `core`, `adapters` (through their public headers only); Eigen (linked privately).
Must not depend on: `io`, `document`, `commands`, `app`, `cli`, `rpc`, `mcp`, and kernel
library headers (CGAL, Open CASCADE, OpenSubdiv). Enforced by `scripts/check_layers.py`.

## Notes

- One file pair per stage or tool. Parameters are plain structs with documented defaults and
  units in their names (`radiusMm`, `sharpAngleDeg`).
- Selections come in and go out as `MeshSelection`. Resolving object ids is the command
  engine's job.
- Keep results deterministic: no unordered iteration or non-deterministic parallel reductions.

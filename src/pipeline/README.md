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
- `m2c/pipeline/QuadRemesh.h`: `quadRemesh` with `QuadRemeshParams` returning `QuadRemeshResult` (closed cage, `CageStats`, distances), and `closeCage` with its `CageCleanup` report.
- `m2c/pipeline/Deviation.h`: `deviationToSolid` (exact, projected onto the faces by the Open CASCADE adapter) and `deviationToMesh` (through the reference's spatial index).
- `m2c/pipeline/PatchMerge.h`: `partitionQuads` returning `QuadPartition` (rectangles of cage quads between lines traced from extraordinary vertices) and `mergeFaces` returning `MergeResult` (one multi-span face per rectangle, same surface).
- `m2c/pipeline/Surface.h`: `buildSurface` with `SurfaceBuildParams` returning `SurfaceBuildResult` (the solid, the fitted cage, fit, cap, seam and merge figures; a fit that folds the surface is retried with more smoothing), plus re-exports of the OpenSubdiv adapter's `fitCage`, `extractPatches` and `tessellateLimit` with their parameter types.

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

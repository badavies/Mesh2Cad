# adapters (`Mesh2Cad::cgal`, `Mesh2Cad::quad`, `Mesh2Cad::osd`, `Mesh2Cad::occt`)

The only code that sees kernel library types. Each adapter converts between core types and one
third-party library and wraps its algorithms behind functions that take and return core types.
`adapters/cgal` wraps CGAL Polygon Mesh Processing; `adapters/quad` wraps Instant Meshes
(ADR-009); `adapters/osd` wraps OpenSubdiv (ADR-004); `adapters/occt` wraps Open CASCADE
(ADR-003, ADR-005). There is no `adapters` target of its own: each adapter folder is a separate CMake
target.

## Public API

- `m2c/adapters/cgal/MeshTools.h` (namespace `m2c::cgal`):
  - `repair` with `RepairParams` and `RepairResult`: merge duplicates, remove degenerate faces and fins, make manifold, orient, optionally remove self-intersections.
  - `fillHoles` with `HoleFillParams` and `HoleFillResult`: fill holes up to a perimeter or edge limit, optionally refined and faired.
  - `simplify` with `SimplifyParams`: Garland-Heckbert edge collapse to a target face count.
  - `remesh` with `RemeshParams`: isotropic remeshing that keeps sharp edges.
  - `shrinkWrap` with `WrapParams`: 3D alpha wrapping to a watertight, manifold mesh.

- `m2c/adapters/quad/QuadRemesh.h` (namespace `m2c::quad`):
  - `remesh` with `QuadRemeshParams`: Instant Meshes' field optimisation and pure-quad extraction
    with fixed settings (4-RoSy, 4-PoSy, extrinsic, deterministic). The raw `QuadLayout` can have
    open loops, fragments and non-manifold spots; `pipeline` closes it.

- `m2c/adapters/osd/Subdivision.h` (namespace `m2c::osd`):
  - `fitCage` with `CageFitParams` and `CageFitResult`: moves the cage vertices so the
    Catmull-Clark limit surface follows a mesh (point-to-plane least squares through the limit
    stencils, with smoothing; mesh points beyond a distance gate are left out).
  - `extractPatches` with `PatchParams` and `PatchResult`: the limit surface as one
    `SurfacePatch` per ptex face, exact bicubics where the cage is regular and caps fitted to
    OpenSubdiv's Gregory patches (positions and tangent planes along their edges) elsewhere.
  - `tessellateLimit`: the limit surface as triangles, for tests and checks.
- `m2c/adapters/occt/Brep.h` (namespace `m2c::occt`):
  - `makeSolid` with `SewParams`: B-spline faces (knots where pieces join with C2 continuity
    removed), sewing, an outward solid (`ShapeFix_Solid`) and the kernel's checker; fails unless
    the result is one valid closed solid.
  - `writeStep` (AP242, millimetres, checks the solid first) and `readStep`, which summarises
    a STEP file as a `SolidSummary`.

Private to the adapters: `osd/Limit.h` (the OpenSubdiv refiner, patch table and stencils),
`osd/Tessellation.h` (limit surface sampling shared by the fit and the display mesh) and
`occt/Shape.h` (the definition of the opaque `SolidShape`). Private to the adapter: `CgalMesh.h` (`toSurfaceMesh`, `fromSurfaceMesh`, `guarded`). Do not
include it from outside `src/adapters/cgal`.

## Dependencies

May use: `core`; CGAL, Boost and Eigen through the private `m2c_cgal_deps` target; Instant
Meshes through the private `m2c_instant_meshes` target, which is compiled single-threaded against
the serial stand-ins in `quad/serial_tbb` instead of TBB; OpenSubdiv and Eigen through the
private `m2c_osd_deps` target; Open CASCADE through the private `m2c_occt_deps` target.
Must not depend on: `io`, `document`, `pipeline`, `commands`, `app`, `cli`, `rpc`, `mcp`, and
other adapters. Enforced by `scripts/check_layers.py` (the script treats all of `src/adapters`
as one module, so it does not catch one adapter including another).

## Notes

- The public header uses core types only, so nothing above this layer needs CGAL's include
  paths or compile flags. Keep it that way.
- No exception escapes an adapter. Wrap CGAL calls in `guarded`, which turns exceptions into
  an `Error` with `ErrorCode::Internal`.
- Instant Meshes prints progress to standard output; `remesh` discards it while it runs, because
  the CLI and the JSON channel use standard output for results.
- Results are new `TriMesh` objects; the input mesh is never changed.
- Open CASCADE prints progress through its default messenger; the OCCT adapter removes its
  printers once, for the same reason. The adapter builds with OCCT 7.6 to 8.0: use
  `NCollection_*` types rather than the `TColgp`/`TopTools` typedefs, and `opencascade::handle`.
- `SolidShape` is declared in core and defined only in `occt/Shape.h`, so code above this layer
  can hold a solid but never sees an OCCT type.

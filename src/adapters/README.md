# adapters (`Mesh2Cad::cgal`, `Mesh2Cad::quad`)

The only code that sees kernel library types. Each adapter converts between core types and one
third-party library and wraps its algorithms behind functions that take and return core types.
`adapters/cgal` wraps CGAL Polygon Mesh Processing; `adapters/quad` wraps Instant Meshes
(ADR-009). There is no `adapters` target of its own: each adapter folder is a separate CMake
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

Private to the adapter: `CgalMesh.h` (`toSurfaceMesh`, `fromSurfaceMesh`, `guarded`). Do not
include it from outside `src/adapters/cgal`.

## Dependencies

May use: `core`; CGAL, Boost and Eigen through the private `m2c_cgal_deps` target; Instant
Meshes through the private `m2c_instant_meshes` target, which is compiled single-threaded against
the serial stand-ins in `quad/serial_tbb` instead of TBB.
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

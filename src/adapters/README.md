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
  - `slice`: the polylines where a plane cuts a mesh (`Polygon_mesh_slicer`), closed or open.
- `m2c/adapters/cgal/RegionGrowing.h` (namespace `m2c::cgal`):
  - `growRegions` with `GrowParams` and `GrownRegion`: Shape_detection region growing of planes,
    cylinders or spheres over the free faces of a mesh, for segmentation.
  - `estimatePrimitive`: CGAL's least-squares cylinder or sphere through a set of faces, the
    start for a primitive fit.

- `m2c/adapters/quad/QuadRemesh.h` (namespace `m2c::quad`):
  - `remesh` with `QuadRemeshParams`: Instant Meshes' field optimisation and pure-quad extraction
    with fixed settings (4-RoSy, 4-PoSy, extrinsic, deterministic). The raw `QuadLayout` can have
    open loops, fragments and non-manifold spots; `pipeline` closes it.

- `m2c/adapters/osd/Subdivision.h` (namespace `m2c::osd`):
  - `fitCage` with `CageFitParams` and `CageFitResult`: moves the cage vertices so the
    Catmull-Clark limit surface follows a mesh (point-to-plane least squares through the limit
    stencils, with smoothing; mesh points beyond a distance gate are left out; `HeldPlane`s keep
    chosen vertices on a plane, so the limit surface is flat where all of a patch's vertices are
    held).
  - `extractPatches` with `PatchParams` and `PatchResult`: the limit surface as one
    `SurfacePatch` per ptex face, exact bicubics where the cage is regular and caps fitted to
    OpenSubdiv's Gregory patches (positions and tangent planes along their edges) elsewhere.
  - `tessellateLimit`: the limit surface as triangles, for tests and checks.
- `m2c/adapters/occt/Profile.h` (namespace `m2c::occt`): `extrudeSketch` and `revolveSketch`, a
  sketch's profile (largest loop less the holes in it) as a planar face, extruded along the normal
  (`BRepPrimAPI_MakePrism`) or revolved about an axis in its plane (`BRepPrimAPI_MakeRevol`).
- `m2c/adapters/occt/Boolean.h` (namespace `m2c::occt`):
  - `booleanSolids` with `BooleanOp`: fuse, cut or common of two solids with fuzzy booleans
    (`BRepAlgoAPI_Fuse`, `BRepAlgoAPI_Cut`, `BRepAlgoAPI_Common`), faces of one surface merged and
    the shape fixed (`ShapeFix_Shape`); fails unless the result is one valid closed solid.
  - `primitiveSolid`: a fitted cylinder (bounded along its axis) or sphere as a solid
    (`BRepPrimAPI_MakeCylinder`, `BRepPrimAPI_MakeSphere`).
  - `boxSolid`: an `OrientedBox` as a solid of six exact planes (`BRepPrimAPI_MakeBox`).
- `m2c/adapters/occt/Fillet.h` (namespace `m2c::occt`): `filletEdges`, the edges of a solid
  nearest some points rounded by a constant-radius rolling-ball fillet
  (`BRepFilletAPI_MakeFillet`, ADR-033).
- `m2c/adapters/occt/Brep.h` (namespace `m2c::occt`):
  - `makeSolid` with `SewParams`: B-spline faces (knots where pieces join with C2 continuity
    removed), sewing, an outward solid (`ShapeFix_Solid`) and the kernel's checker; fails unless
    the result is one valid closed solid.
  - `writeStep` (AP242, millimetres, checks the solid first) and `readStep`, which summarises
    a STEP file as a `SolidSummary`. `writeStep` also takes several `StepBody`s (solids and
    sketches, a sketch as the planar faces of its loops) and names each product after its
    body; `readStepBodies` summarises each body of a file on its own (ADR-034).
- `m2c/adapters/occt/Surface.h` (namespace `m2c::occt`):
  - `surfaceDistances`: distances from points to a fitted primitive made as an unbounded Open
    CASCADE surface (`Geom_Plane`, `Geom_CylindricalSurface`, `Geom_SphericalSurface`,
    `Geom_ConicalSurface`).
- `m2c/adapters/occt/Trim.h` (namespace `m2c::occt`):
  - `trimPrimitives` with `TrimParams`: fitted primitives bounded by the scan's box and split
    against each other (General Fuse, `BRepAlgoAPI_BuilderAlgo`); the split faces on the scan are
    sewn into an outward solid and faces of one surface merged (`ShapeUpgrade_UnifySameDomain`).
    The solid's display mesh is the kernel's triangulation (`BRepMesh_IncrementalMesh`).

Private to the adapters: `osd/Limit.h` (the OpenSubdiv refiner, patch table and stencils),
`osd/Tessellation.h` (limit surface sampling shared by the fit and the display mesh) and
`occt/Shape.h` (the definition of the opaque `SolidShape`, `summarise`, `triangulate` and `kernelSolid`), `occt/PrimitiveSurface.h`
(`toGeomSurface`, a primitive as a `Geom_Surface`) and `occt/Guard.h` (kernel exceptions as
errors). Private to the adapter: `CgalMesh.h` (`toSurfaceMesh`, `fromSurfaceMesh`, `guarded`). Do not
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

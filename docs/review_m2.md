# Mesh2Cad Codebase Review — Milestone 2 (Automatic Quad-NURBS Surfacing)

**Scope:** All M2 commits (`6cb7bcb` through `50cdd17`, 88 files changed) across `src/core`, `src/adapters/quad`, `src/adapters/osd`, `src/adapters/occt`, `src/pipeline`, `src/io`, `src/document`, `src/commands`, `src/app`, `scripts`, and `tests`, evaluated alongside the untackled M0/M1 findings in [codebase_review_m0_m1.md](file:///C:/Users/badav/.gemini/antigravity/brain/a7fb43ef-ddfd-4022-b476-f1341fe13430/codebase_review_m0_m1.md) and [performance_review_m0_m1.md](file:///C:/Users/badav/.gemini/antigravity/brain/a7fb43ef-ddfd-4022-b476-f1341fe13430/performance_review_m0_m1.md).

---

## Part 1: Bugs & Correctness Issues in M2

### 1.1 Critical & High Severity

#### 1. [CRITICAL] Heap Buffer Over-Read & `uint32_t` Overflow in `QuadLayout::create`
- **Location:** [QuadLayout.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/QuadLayout.cpp#L58-L70)
- **Issue:** `QuadLayout::create` validates `faceStarts.front() == 0` and `faceStarts.back() == faceVertices.size()`, and inside the per-face loop checks `if (faceStarts[f + 1] < faceStarts[f] + 3)`. This has two holes:
  1. It never checks `faceStarts[f + 1] <= faceVertices.size()` for intermediate entries (`f + 1 < faceCount`). For example, `faceStarts = {0, 1000, 6}` with `faceVertices.size() == 6` passes the header check and then reads `faceVertices[0..999]` out of bounds on `f = 0` before reaching `f = 1`.
  2. If `faceStarts[f] >= UINT32_MAX - 2`, `faceStarts[f] + 3` wraps around in unsigned 32-bit arithmetic, bypassing the `< faceStarts[f] + 3` check.
- **Fix:**
  ```cpp
  if (faceStarts[f + 1] < faceStarts[f] ||
      faceStarts[f + 1] > faceVertices.size() ||
      faceStarts[f + 1] - faceStarts[f] < 3) {
      return Error::invalid("QuadLayout face must have at least 3 vertices");
  }
  ```

---

#### 2. [CRITICAL] Infinite Edge-Split Loop / OOM on Zero-Area or Degenerate Meshes in `InstantMeshes.cpp`
- **Location:** [InstantMeshes.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/InstantMeshes.cpp#L95-L113), [InstantMeshes.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/InstantMeshes.cpp#L205-L218)
- **Contract:** [`QuadRemesh.h`](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/include/m2c/adapters/quad/QuadRemesh.h#L26) documents: *"Fails with InvalidArgument for an empty or degenerate mesh."*
- **Issue:** `remesh()` only checks `mesh.faceCount() < 4` and `params.targetFaces < 8`. If a mesh has $\ge 4$ faces that are collinear/zero-area (`stats.mSurfaceArea <= 0.0` or `meanEdge <= 0.0`), `scale` becomes `0.0f`:
  ```cpp
  float scale = params.edgeLengthMm > 0.0
      ? static_cast<float>(params.edgeLengthMm)
      : std::sqrt(stats.mSurfaceArea / static_cast<float>(params.targetFaces));
  ```
  When `maxEdge > 0.0f` and `scale == 0.0f`, `maxEdge * 2.0f > scale` is `true`, and `subdivide(E, V, ..., std::min(scale / 2.0f, meanEdge * 2.0f))` is called with `max_size == 0.0f`. In `dedge.cpp`, `subdivide` splits every edge with `length > max_size` in an infinite `while ( true )` loop until memory exhaustion.
- **Fix:** Validate `if (!(stats.mSurfaceArea > 0.0f) || !(meanEdge > 0.0f) || !(scale > 0.0f) || !std::isfinite(scale))` and return `Error::invalid("Mesh has zero surface area or degenerate edges")`.

---

#### 3. [HIGH] `fitCage` Selects a Diverged Cage When `used == 0` or Samples Drop Outside `gate`
- **Location:** [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L309-L347)
- **Issue:** At each Gauss-Newton iteration, `fitCage` filters samples by `if (near.distance <= gate)` and computes:
  ```cpp
  const double rms = std::sqrt(sum2 / double(std::max<std::size_t>(used, 1)));
  if (rms < bestRms) {
      bestRms = rms;
      best = positions;
  }
  ```
  1. If an iteration moves the cage far enough that `used == 0` (all samples exceed `gate`), `sum2` is `0.0`, so `rms == 0.0 < bestRms`! `fitCage` records the completely diverged cage as `best` and sets `bestRms = 0.0`, locking it in for all remaining iterations.
  2. Even when `used > 0`, a round that pushes outlier regions across the `gate` threshold excludes those hard samples from `sum2`, artificially lowering `rms` compared to a better-fitting round that kept more samples inside `gate`.
- **Fix:**
  - Require `used > 0` before updating `best`, and either score `best` by untruncated (or Huber/clamped-at-`gate`) RMS across all `targets.size()` samples so dropping samples outside `gate` is penalized by `gate^2` rather than rewarded with `0`.

---

#### 4. [HIGH] `measureSeams` Reports False-Positive Seam Gaps & Compares Non-Corresponding Samples
- **Location:** [Solid.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/Solid.cpp#L44-L120)
- **Issue:**
  1. **Maximum-of-neighborhood instead of closest corresponding point:** For each boundary sample `a`, `measureSeams` iterates over all samples `b` on other faces within `toleranceMm` (`1e-2 mm` in `Surface.cpp`) and takes the **maximum** distance and **maximum** normal angle:
     ```cpp
     const double gap = length(a.p - b.p);
     if (gap > toleranceMm) continue;
     matched = true;
     out.maxGapMm = std::max(out.maxGapMm, gap);
     if (lengthSq(a.n) > 0.0 && lengthSq(b.n) > 0.0) {
         out.maxNormalAngleDeg = std::max(out.maxNormalAngleDeg, angleDeg(a.n, b.n));
     }
     ```
     Whenever the parameter-step spacing along an edge (`edgeLength / (spans * 8)`) is smaller than `toleranceMm` (i.e. any patch edge shorter than `0.08 mm * spans`), adjacent samples `b_{i-1}`, `b_{i+1}` along the *same* watertight seam fall inside the `toleranceMm` sphere around `a_i`. Even on an **exact $G^1$ watertight seam** (`gap == 0` at `b_i`), `measureSeams` measures the chord distance to `b_{i±1}` (`gap ≈ toleranceMm`) and the normal change along the edge, reporting a false seam gap and false $G^1$ normal error! Similarly, at a 4-patch corner, all 4 patches' near-corner samples cross-compare within `toleranceMm`.
  2. **Duplicate corner samples:** In `samplePatchBorders` ([Solid.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/Solid.cpp#L58-L64)), `for (std::size_t i = 0; i <= k; ++i)` pushes all 4 boundaries including both endpoints `i = 0` and `i = k`, duplicating all 4 corners `(0,0), (1,0), (0,1), (1,1)` per patch.
  3. **Potential `std::llround` UB:** `cellOf` ([Solid.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/Solid.cpp#L82-L88)) calls `std::llround(p.x / toleranceMm)`, which has undefined behavior if `p.x / toleranceMm` exceeds `LLONG_MAX`.
- **Fix:** For each sample `a` and each candidate neighbor face `f_b`, find the **minimum-distance** sample `b*` on `f_b` (or project onto the neighbor patch's boundary curve), and update `out.maxGapMm` and `out.maxNormalAngleDeg` only from that closest corresponding point `b*` when `dist(a, b*) <= toleranceMm`.

---

#### 5. [HIGH] `occt::makeSolid` Can Silently Drop Unsewn Faces & Produce a Corrupt `.m2c` Archive
- **Location:** [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L168-L202), [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L244-L249)
- **Issue:** After `BRepBuilderAPI_Sewing::Perform()`, `makeSolid` iterates only over `TopExp_Explorer(sewn, TopAbs_SHELL)`. If `SewedShape()` is a `TopoDS_Compound` containing one closed `TopoDS_Shell` plus one or more standalone `TopoDS_Face`s that failed to sew into any shell, `shellCount` is still `1` and `onlyShell` is that partial shell!
  - `makeSolid` succeeds, building a `Solid` whose `summary().faceCount` is less than `faces().size()`.
  - When the user saves the project (`project.save`) and reopens it (`project.open`), `occt::readBrep` checks `if (summary.faceCount != faces.size())` ([Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L246)) and rejects the saved file as `"Saved B-rep face count does not match patch table"`!
- **Fix:** In `makeSolid`, verify `summary.faceCount == faces.size()` (or check `TopExp_Explorer(sewn, TopAbs_FACE, TopAbs_SHELL)` is empty) before constructing the `TopoDS_Solid`.

---

#### 6. [HIGH] `occt::signedDistances` Sign Flips Near Seams/Creases & Single-Face Boundary Clamping
- **Location:** [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L287-L319)
- **Issue:**
  1. **Single-face projection with `[0, 1]` UV clamping:** `signedDistances` finds the closest triangle on the coarse ($4 \times 4$ per span) display mesh, maps that triangle to a single B-spline face `f = near.face / (2 * n * n)`, projects `p` onto `surfaces[f]`, and clamps `(u, v)` to `[0, 1]`. Because the coarse display mesh chord differs from the curved B-spline surface near a patch boundary, the true closest point on the CAD solid frequently lies just across the seam on an adjacent face `f'`. Clamping `(u, v)` to `[0, 1]` on `f` leaves `q` stuck on the seam curve instead of the interior of `f'`, inflating the distance and making `offset = p - q` non-orthogonal to `f`'s surface normal.
  2. **Crease/seam normal-cone sign flip:** Even when `q` legitimately lies on a seam or crease edge where two faces meet at a dihedral angle, `offset = p - q` lies in the normal cone spanned by the two faces' normals (`n_1`, `n_2`), which need not have a positive dot product with the single face `f`'s normal `du.Crossed(dv)` alone. As a result, points directly outside a convex edge or inside a concave fillet can receive the **wrong sign**.
  3. **$C^0$ interior knot multiplicity 3 in `toSurface`:** [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L103-L110) sets interior knot multiplicity to `3` on multi-span patches, where `Geom_BSplineSurface::D1` is only $C^0$ continuous and `ShapeAnalysis_Surface::NextValueOfUV` Newton steps can stall at internal knot lines (`1/spans, 2/spans, ...`).
- **Fix:** When the projected `(u, v)` on face `f` hits the boundary (`u <= eps || u >= 1 - eps || v <= eps || v >= 1 - eps`), also evaluate candidate faces from the other triangles in the spatial neighborhood (or adjacent patches) and pick the projection with minimum `SquareDistance`, averaging the outward normals of incident faces when `q` lies on a shared edge/corner.

---

#### 7. [HIGH] `InstantMeshes.cpp` Unsynchronized Global `std::cout`/`std::cerr` Hijack & Global `nprocs`
- **Location:** [InstantMeshes.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/InstantMeshes.cpp#L29-L54)
- **Issue:**
  1. `SilenceConsole` swaps `std::cout.rdbuf()` and `std::cerr.rdbuf()` process-wide without mutex synchronization. `std::ios::rdbuf` is not thread-safe. In `mesh2cad --stdio` (where RPC/MCP responses are written to `std::cout` and logs to `std::cerr`) or if any other thread logs during `quad.remesh`, swapping stream buffers causes a data race or silently swallows stdio output.
  2. `int nprocs = 1;` ([InstantMeshes.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/InstantMeshes.cpp#L29)) is a non-atomic mutable global variable in the global namespace.
- **Fix:** Patch the bundled Instant Meshes headers (`hierarchy.cpp`, `field.cpp`, `dedge.cpp`, `subdivide.cpp`, `smooth.cpp`) or define a no-op stream/macro in `common.h` instead of mutating global `std::cout`/`std::cerr` buffers at runtime.

---

#### 8. [HIGH] `serial_tbb/tbb/tbb.h` Nested `spawn_root_and_wait` Use-After-Free & Exception Leak
- **Location:** [tbb.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/quad/serial_tbb/tbb/tbb.h#L107-L153)
- **Issue:** `allocate_root()` and `allocate_child()` push all allocated tasks into a single `thread_local std::vector<task*> tasks` (`owned()`), and `spawn_root_and_wait` deletes every pointer in `owned()` upon completion:
  ```cpp
  static void spawn_root_and_wait(task& root) {
      run(&root);
      for (task* t : owned()) {
          delete t;
      }
      owned().clear();
  }
  ```
  If any `task::execute()` ever calls `spawn_root_and_wait` recursively (nested root task), the inner `spawn_root_and_wait` deletes the outer `root` and sibling tasks while the outer `run(&root)` is still executing on the call stack (use-after-free). Furthermore, if `run(&root)` throws an exception, `owned()` is neither freed nor cleared, leaking memory and leaving dangling/stale task pointers for the next call on that thread.
- **Fix:** Scope task ownership per `spawn_root_and_wait` invocation using an RAII guard (saving/restoring the previous `owned()` vector or start index in a `struct ScopeGuard`).

---

### 1.2 Medium & Low Severity Bugs

#### 9. [MEDIUM] `closeCage` Edge-Case Failures in `dissolveValenceTwo` & Component Metric Mismatch
- **Location:** [QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L70-L106), [QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L234-L267), [QuadLayout.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/QuadLayout.cpp#L182-L184)
- **Issue:**
  1. In `dissolveValenceTwo` ([QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L234-L267)): when a valence-2 vertex `v` is shared by a triangle `f1 = {a, v, b}` (`dropFace[f1] = true`) and a quad `f2 = {b, v, a, c}` (which becomes `{b, a, c}` after `std::erase(faces[f2], v)`), if the neighbor across edge `(a, b)` of `f1` is already the triangle `f3 = {a, b, c}`, then `f2` and `f3` become duplicate triangles `{b, a, c}` and `{a, b, c}` sharing all three edges. No subsequent `dropNonManifoldEdgeFaces` pass runs after `dissolveValenceTwo`, leaving non-manifold edges when `fillLoops` runs.
  2. Also in `dissolveValenceTwo`, if a quad is pinched at `v` (`{a, v, a, c}` after an earlier vertex weld/collapse), erasing `v` leaves `{a, a, c}` with a repeated vertex `a`, which causes `assemble` -> `QuadLayout::create` to fail with `"QuadLayout face repeats a vertex"`!
  3. **Component count mismatch:** `keepLargestComponent` ([QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L70-L106)) groups faces by **edge adjacency**, whereas `cageStats` ([QuadLayout.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/QuadLayout.cpp#L135)) groups faces by **vertex adjacency** (`vertexFaces[v]`).

---

#### 10. [MEDIUM] `SurfacePatch` Unsigned Underflow When `spans == 0` & Unchecked NaN/Inf Poles in `decodeFaces`
- **Location:** [SurfacePatch.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/include/m2c/core/SurfacePatch.h#L85-L94), [Solid.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/Solid.cpp#L24-L28), [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L84), [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L124-L149)
- **Issue:**
  1. `SurfacePatch` is a plain aggregate struct. If `patch.spans == 0` or `patch.poles.size() != patch.poleGridSize() * patch.poleGridSize()`, `evaluate(const SurfacePatch&, ...)` computes `patch.spans - 1` which underflows `std::size_t` to `SIZE_MAX`, and `patch.piece(su, sv)` reads out of bounds.
  2. `decodeFaces` in [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L138-L143) checks `spans == 0 || spans > 64`, `degree != 3`, and `poleCount != expected`, but never validates `std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)` on the deserialized NURBS control points (unlike `decodeMesh`, `decodeCage`, and `decodeDeviation`, which all check `std::isfinite`).
  3. `ByteReader::done()` ([Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L84)) is defined but never called in `decodeMesh`, `decodeCage`, `decodeFaces`, or `decodeDeviation`, so trailing bytes in binary archive streams are silently ignored.

---

#### 11. [MEDIUM] `Document::checkData` Allows `DeviationData::referenceId` to Reference Any Object Kind or `meshId` Itself
- **Location:** [Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L99-L102)
- **Issue:**
  ```cpp
  if (const auto* d = std::get_if<DeviationData>(&data)) {
      return isMeshObject(objects_, d->meshId) && objects_.contains(d->referenceId) && d->deviation != nullptr;
  }
  ```
  `checkData` only verifies `objects_.contains(d->referenceId)`—it allows `referenceId` to point to a `Selection`, `Measurement`, `QuadLayout`, `Deviation`, or even `d->meshId` itself (which `ProjectTests.cpp:79` actually does: `ProjectDeviation{1, 1, dev}`).
- **Fix:** Check `d->referenceId != d->meshId` and verify the target is a `MeshData` or `SolidData`.

---

#### 12. [MEDIUM] `surface.build` Discards the Fitted `QuadLayout` (Pre-Fit Cage Shown in Viewport)
- **Location:** [SurfaceCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SurfaceCommands.cpp#L97-L104), [Surface.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/include/m2c/pipeline/Surface.h#L28-L36)
- **Issue:** `pipeline::buildSurface` runs `osd::fitCage`, producing the fitted control cage `built->cage` (and storing a duplicate copy in `built->fit.cage`). However, `surface.build` only adds `built->solid` to the document and discards `built->cage`. As a result:
  - The `quad_layout` object in the document remains at its pre-fit vertex positions, so the orange cage overlay in the 3D viewport does not match the B-rep solid's control cage.
  - Re-running `surface.build` or inspecting the `quad_layout` loses the fitted control positions.

---

#### 13. [MEDIUM] Desktop UI Regressions & M2 Integration Bugs (`MainWindow.cpp`, `Viewport.cpp`)
- **Locations:** [MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L151-L183), [MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L541-L553), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L120-L132), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L190-L195), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L360-L366), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L696-L708), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L764-L781)
- **Issues:**
  1. **`MainWindow::formDefaults` populates wrong `"id"` for M2 commands ([MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L545-L548)):**
     `formDefaults` sets `values["id"] = currentMesh()` for every command whose name does not start with `"object."`. When a user opens `surface.build` (expects `"id"` = `quad_layout` ID and `"mesh_id"` = mesh ID), `solid.info` (expects `"id"` = `solid` ID), `solid.export` (expects `"id"` = `solid` ID), or `deviation.report` (expects `"id"` = `deviation` ID) from the **Tools** menu, `"id"` is pre-filled with the mesh ID (`1`), and `"mesh_id"` in `surface.build` is left at `0`, causing immediate `InvalidArgument` failures on **Run**.
  2. **Context menu "Info" fails on Solids ([MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L151-L154)):**
     Right-clicking a `solid` in the Objects tree and clicking **Info** always dispatches `mesh.info`, which fails with `"Object is not a mesh"`. It should dispatch `solid.info` when the selected item is a `solid` (and `deviation.report` for a `deviation`).
  3. **Workflow dock missing M2 steps ([MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L165-L183)):**
     The left Workflow dock still only lists M0/M1 steps `1. Import` through `8. Export`; none of the M2 stages (`quad.remesh`, `surface.build`, `deviation.compute`, `solid.export`) are exposed there.
  4. **`Viewport::legendDeviation` vs `Viewport::syncColors` mismatch ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L360-L366) vs [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L696-L708)):**
     In `syncColors`, `for (const auto& o : snapshot_.objects)` overwrites `devByMesh[d->meshId] = d->deviation.get()`, so the mesh is colored by the **last (newest)** visible deviation. In `legendDeviation()`, `for (const auto& o : snapshot_.objects)` returns immediately on the **first (oldest)** visible deviation for `currentMesh_`. When two deviations exist for the same mesh, the 3D heatmap colors and the 2D color legend display **different deviations**!
  5. **`Viewport::cageEdges` drops boundary edges with `a > b` ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L120-L132)):**
     `cageEdges` emits an edge only when `if (a < b)`. On an open cage with boundary edges, any boundary edge whose single incident face traverses `a -> b` with `a > b` is never drawn.
  6. **Empty-scene hint & auto-fit ignore Solids and QuadLayouts ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L190-L195), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L591)):**
     `setSnapshot` and `drawOverlay` check `std::any_of(..., isMesh)` instead of `isDrawn`. If the user hides the scan mesh to inspect the generated `Solid` or `QuadLayout`, `"Import a mesh or drop a file to begin"` is drawn right on top of the CAD solid!
  7. **Viewport picking & `measure.distance` ignore `Solid` objects ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L764-L781), [AlignMeasureCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/AlignMeasureCommands.cpp#L80)):**
     Double-clicking a `Solid` to focus the camera or using the Measure tool on a `Solid` fails because `Viewport::pick` only raycasts `isMesh(o)` and `measure.distance` calls `requireMesh` instead of accepting `Solid::displayMesh()`.

---

#### 14. [LOW] `scripts/check_layers.py` & `GoldenTests.cpp` Coverage Gaps for M2
- **Locations:** [check_layers.py](file:///c:/Users/badav/ClionProjects/Mesh2Cad/scripts/check_layers.py#L20-L36), [GoldenTests.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/tests/golden/GoldenTests.cpp#L255-L266)
- **Issues:**
  1. `ARCHITECTURE.md` §4 requires: *"Subdirectories of `adapters/` (`cgal/`, `occt/`, `osd/`, `quad/`) never include each other."* `scripts/check_layers.py` treats all of `src/adapters/*` as a single `"adapters"` layer and does not forbid cross-adapter includes (e.g. `occt` including `osd`), nor does `KERNEL_INCLUDE` match Instant Meshes headers.
  2. `GoldenTests.cpp` (`verifyAllCommandSurfacesMatch`) only compares the final `mesh` object (`findMesh`) across CLI, JSON-RPC, and MCP—it does not verify bit-identical output for `quad_layout`, `solid`, or `deviation` objects produced in M2.

---

## Part 2: Compounding Interactions with Untackled M0/M1 Issues

Because the earlier M0/M1 issues have not yet been tackled, several of them now directly impact M2 workflows:

1. **Stale `Deviation` & `QuadLayout` after in-place mesh edits (extends M0/M1 Issue #3):**
   - In M0/M1, mutating a mesh in-place (`mesh.simplify`, `mesh.remesh`, `faces.delete`, `shells.clean`, `align.*`, `mesh.flip`) left `MeshSelection` with a mismatched `faceCount` and `Measurement` at stale coordinates.
   - In M2, `DeviationData` stores per-vertex `signedDistancesMm` indexed by the scan mesh's vertices (`d->deviation->pointCount() == mesh.vertexCount()`). If the user runs `mesh.simplify`, `faces.delete`, or `mesh.remesh` after `deviation.compute`, `DeviationData` is **not** invalidated or removed—`Viewport::syncColors` silently falls back to uniform color because `dev->signedDistancesMm().size() != m->mesh->vertexCount()`, while `deviation.report` still reports the stale deviation as if it applied to the modified mesh. Similarly, running `align.*` on the mesh moves the mesh out of alignment with any already-created `QuadLayout` and `Solid`.
2. **`TriMesh::spatialIndex()` data race & recursive BVH stack overflow (extends M0/M1 Issues #1 & #2):**
   - M2 heavily relies on `TriMesh::spatialIndex()` inside `osd::fitCage` (called on every Gauss-Newton iteration for a fresh `TriMesh` limit surface) and inside `occt::signedDistances` (called on `solid.displayMesh()`). If `signedDistances` or `fitCage` is parallelized across threads, calling `spatialIndex()` without `std::call_once` will immediately race.
3. **`ObjectId` reuse after `object.delete` (extends M0/M1 Issue #8):**
   - `Document::fromParts` resets `nextId_` to `maxSeenId + 1` on `project.open`. Combined with `Provenance::inputs` storing `ObjectId`s for `quad_layout`, `solid`, and `deviation`, deleting the top object and saving/reloading can alias historical IDs in provenance records.

---

## Part 3: Performance & Efficiency Bottlenecks in M2

### 3.1 High-Impact Bottlenecks

#### P1. [HIGH] Duplicate `Limit::create` (Full OpenSubdiv Refinement & Stencil Build) in `buildSurface`
- **Location:** [Surface.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Surface.cpp#L20-L24), [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L252), [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L394)
- **Impact:** **2× OpenSubdiv topology refinement and patch/stencil table construction** on every `surface.build`.
- **Analysis:** `Limit::create(cage)` depends **only on the cage's topology** (not its vertex coordinates), as documented in [`Limit.h:35`](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/include/m2c/adapters/osd/Limit.h#L35). In `pipeline::buildSurface`:
  1. `osd::fitCage(cage, target, params.fit)` calls `Limit::create(cage)` (which runs `cageStats`, constructs `Far::TopologyRefiner`, runs `RefineAdaptive`, builds `PatchTable`, and builds `StencilTableReal<double>`) and only updates vertex coordinates in `fit->cage`.
  2. Immediately afterward, `osd::extractPatches(fit->cage, params.patch)` calls `Limit::create(cage)` **a second time** on the exact same topology!
- **Fix:** Either reuse the `Limit` instance created in `fitCage` by passing it into `extractPatches`, or build `Limit` once in `buildSurface`.

---

#### P2. [HIGH] Redundant Final Evaluation, Tessellation, BVH Build & 60,000 Closest-Point Queries in `fitCage`
- **Location:** [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L309-L348) vs [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L375-L389)
- **Impact:** **25% wasted work in `fitCage`** (with default `iterations = 3`, runs 5 full passes of limit-surface tessellation + BVH construction + 60,000 `closestPoint` queries when only 4 are needed).
- **Analysis:**
  - The fitting loop runs `for (unsigned it = 0; it <= params.iterations; ++it)`. On the last iteration (`it == params.iterations`), lines 310–348 already evaluate `limit->patchPoints(positions)`, tessellate the limit surface (`tessellateLimit(*limit, points, kSearchGrid)`), build the `SpatialIndex` BVH, and run up to 60,000 `locate` queries to compute `rms`, `maxD`, `used`, and `ignored`, and then `break` on line 347 before solving the linear system.
  - Immediately after the loop, lines 375–389 run `limit->patchPoints(best)`, `tessellateLimit(*limit, finalPoints, kSearchGrid)`, build the BVH, and run all 60,000 `locate` queries **a second time** on `best` (which in normal convergence is the exact `positions` vector just evaluated at `it == params.iterations`!).
- **Fix:** Store `(bestSamples, bestIgnored, bestRms, bestMaxD)` whenever `best = positions` is updated inside the loop, and delete lines 375–389 completely.

---

#### P3. [HIGH] Unreserved Sparse Triplets & 3× Redundant Block-Diagonal Matrix Work in `fitCage`
- **Location:** [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L106-L127), [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L280-L365)
- **Impact:** High memory churn (~138 MB of reallocating `Eigen::Triplet<double>` vectors per iteration) and 3× redundant sparse matrix assembly/multiplication.
- **Analysis:**
  1. **Unreserved `planeRows` and `pointRows`:** With 60,000 samples and 16 bicubic control points per patch, `planeRows` pushes up to $60{,}000 \times 16 \times 3 = 2.88\text{M}$ `Eigen::Triplet<double>` (~69 MB) and `pointRows` pushes another $2.88\text{M}$ triplets (~69 MB) per iteration with **no `.reserve()`**.
  2. **3× identical block-diagonal `point` matrix:** In `pointRows`, the x, y, and z coordinates share the exact same scalar weight matrix $W \in \mathbb{R}^{m \times n}$ (`wP[k]`), replicated 3 times along the diagonal into a $3m \times 3n$ matrix `point`. Assembling $W$ as an $m \times n$ sparse matrix once per round uses **3× fewer triplets**, and computing $W^T W$ ($n \times n$) is **3× faster** than `point.transpose() * point` ($3n \times 3n$).
  3. **Loop-invariant matrices recomputed every iteration:** `mu * smooth`, `mu * (smooth * start)`, and `identity` (`3n × 3n`) do not change across iterations, yet are reconstructed and multiplied inside the `for (unsigned it = 0; ...)` loop.
  4. **`laplacian(cage)` uses `std::vector<std::set<uint32_t>>` ([Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L107)):** Allocates a red-black tree node on the heap for every half-edge instead of a flat adjacency vector or small sorted vector.

---

#### P4. [HIGH] Single-Threaded `occt::signedDistances` & Repeated `ShapeAnalysis_Surface` Construction
- **Location:** [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L260-L322)
- **Impact:** Multi-second CPU bottleneck on 1M–10M vertex scans during `deviation.compute`.
- **Analysis:**
  1. For every vertex of the scan mesh (`mesh.vertexCount()`, up to 10M vertices), `signedDistances` runs sequentially on a single thread, executing a BVH `closestPoint` query followed by OCCT's `ShapeAnalysis_Surface::NextValueOfUV` and `Geom_BSplineSurface::D1`.
  2. Every call to `occt::signedDistances` rebuilds `Handle(Geom_BSplineSurface)` and `Handle(ShapeAnalysis_Surface)` for all ~2,000 faces from scratch (`toSurface(faces[f])`) instead of caching them on `Solid::SolidShape`.
  3. Notice that `ShapeAnalysis_Surface::NextValueOfUV` mutates internal state (`myGap`, UV cache) inside `ShapeAnalysis_Surface`, so when parallelizing `signedDistances`, either call stateless `Extrema_ExtPS` / analytical Newton step on `Geom_BSplineSurface::D1`/`D2` directly, or keep per-thread `ShapeAnalysis_Surface` instances.

---

### 3.2 Medium & Low-Impact Bottlenecks

#### P5. [MEDIUM] Expensive `summarise` (`BRepBndLib::AddOptimal`, `BRepCheck_Analyzer`, `BRepGProp`) Re-Run on `readBrep` and `solid.export`
- **Location:** [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L113-L153), [Brep.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/occt/Brep.cpp#L242), [SurfaceCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SurfaceCommands.cpp#L140-L152)
- **Analysis:**
  - `summarise()` runs `BRepCheck_Analyzer(shape, true)`, `BRepBndLib::AddOptimal(shape, box, false, false)` (which performs numerical optimization over every B-spline surface patch), and `BRepGProp::SurfaceProperties` + `BRepGProp::VolumeProperties` (Gaussian quadrature across all ~2,000 NURBS faces).
  - When opening a `.m2c` project (`project.open`), `readBrep` calls `summarise(shape)` from scratch even though the `SolidSummary` could be serialized in the `.m2c` metadata (or bounding boxes taken from the patch control polygons / display mesh).
  - In `solid.export` ([SurfaceCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SurfaceCommands.cpp#L145)), when `check_roundtrip` is `true` (the default!), exporting a STEP file immediately re-parses the entire STEP file via `occt::readStep` and runs `summarise` (including `BRepCheck_Analyzer`, `AddOptimal`, and `BRepGProp`) a second time.

---

#### P6. [MEDIUM] Finite-Difference Basis Evaluations in `fitCapInterior` & Lagrange Re-Interpolation in `extractPatches`
- **Location:** [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L200-L234), [Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L440-L445)
- **Analysis:**
  1. In `fitCapInterior`, for each extraordinary cap and each boundary sample, `at(f, u, v)` calls `Limit::evaluate` 4 times with finite-difference steps `h = 1e-6` (`at(f, s0, t0)`, `at(f, s1, t1)`, `at(f, 0.0, 0.0)`, `at(f, uCross, vCross)`) to approximate first derivatives. Note that `at(f, 0.0, 0.0)` is evaluated **3 times per cap** at the exact same point `(0, 0)`! Calling `patches_->EvaluateBasis(handle, s, t, wP, wDs, wDt)` once returns the exact analytical position and partial derivative weights in a single call.
  2. In `extractPatches` ([Subdivision.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/osd/Subdivision.cpp#L440-L445)), when splitting a single cubic Bézier patch into `sub * sub` sub-patches (`sub > 1`), it evaluates the cubic Bézier on a $4 \times 4$ grid and solves a $4 \times 4$ Vandermonde system (`interpolate`) per sub-patch instead of directly applying de Casteljau subdivision.

---

#### P7. [MEDIUM] Duplicate `cageStats` & Repeated Unreserved `unordered_map` Builds in `QuadRemesh.cpp`
- **Location:** [QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L29-L39), [QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L317), [QuadRemesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/QuadRemesh.cpp#L344)
- **Analysis:**
  1. `closeCage` calls `cageStats(*cage)` on line 317 to check `stats.watertight && stats.manifold`, and then `quadRemesh` immediately calls `cageStats(*cage)` **again** on line 344 on the exact same `cage`! Returning `std::pair<QuadLayout, CageStats>` from `closeCage` avoids the second pass.
  2. `edgeFaces(faces)` constructs an unreserved `std::unordered_map<uint64_t, std::vector<uint32_t>>` (allocating both hash nodes and a separate heap `std::vector<uint32_t>` for every single edge in the cage) 3 separate times per cleanup pass (`dropNonManifoldEdgeFaces`, `keepLargestComponent`, `orientConsistently`). Since manifold edges have at most 2 incident faces, a flat sorted edge array or a fixed inline pair `struct EdgeEntry { uint32_t f0, f1, count; }` avoids thousands of small heap allocations.

---

#### P8. [LOW] Viewport CPU Overhead on QuadLayout Bounds, Deviation Legend & Vertex Coloring
- **Location:** [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L116), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L380-L388), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L435), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L734-L738)
- **Analysis:**
  1. `drawnBounds` ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L116)) and `syncGpu` ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L435)) call `boundsOf(q->cage->positions())`, iterating over all cage vertices even though `q->cage->bounds()` is already precomputed and cached on `QuadLayout`!
  2. `drawLegend` ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L734-L738)) runs a 200-iteration loop calling `deviationColor`, `painter.setPen`, and `painter.drawLine` on every `paintGL` frame instead of using a single `QLinearGradient` with 5 color stops.
  3. `syncColors` ([Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L380-L388)) computes `deviationColor(d, range)` which quantizes each float channel to an 8-bit `int` inside `QColor(r, g, b)` and then converts back to `float` via `c.redF(), c.greenF(), c.blueF()` for every vertex in the mesh.

---

## Summary Table

| ID | Category | Severity / Impact | Location | Summary |
| :--- | :--- | :--- | :--- | :--- |
| **1** | Bug | **CRITICAL** | `src/core/QuadLayout.cpp:58` | Heap buffer over-read & `uint32_t` wrap in `QuadLayout::create` on intermediate `faceStarts` |
| **2** | Bug | **CRITICAL** | `src/adapters/quad/InstantMeshes.cpp:95, 205` | Infinite edge-split loop / OOM on zero-area or degenerate meshes (`scale == 0`) |
| **3** | Bug | **HIGH** | `src/adapters/osd/Subdivision.cpp:342` | `fitCage` selects diverged cage when `used == 0` (`rms == 0`) or when samples drop outside `gate` |
| **4** | Bug | **HIGH** | `src/core/Solid.cpp:58, 96` | `measureSeams` takes max over neighborhood within `toleranceMm` (false seam gaps) & duplicates corners |
| **5** | Bug | **HIGH** | `src/adapters/occt/Brep.cpp:175, 246` | `makeSolid` silently drops unsewn standalone faces if 1 closed shell exists, breaking `readBrep` on reload |
| **6** | Bug | **HIGH** | `src/adapters/occt/Brep.cpp:287` | `signedDistances` single-face `[0,1]` clamping & crease normal-cone sign flips |
| **7** | Bug | **HIGH** | `src/adapters/quad/InstantMeshes.cpp:29, 38` | Unsynchronized process-wide `std::cout`/`std::cerr` `rdbuf` hijack & global `nprocs` |
| **8** | Bug | **HIGH** | `src/adapters/quad/serial_tbb/tbb/tbb.h:107` | Nested `spawn_root_and_wait` use-after-free & exception leak in `owned()` |
| **9** | Bug | **MEDIUM** | `src/pipeline/QuadRemesh.cpp:234` | `dissolveValenceTwo` can leave duplicate triangles or repeated face vertices; component count mismatch |
| **10** | Bug | **MEDIUM** | `src/core/include/m2c/core/SurfacePatch.h:85`, `src/io/Project.cpp:124` | `SurfacePatch` unsigned underflow if `spans == 0`; `decodeFaces` skips `std::isfinite`; `ByteReader::done()` unused |
| **11** | Bug | **MEDIUM** | `src/document/Document.cpp:99` | `Document::checkData` allows `DeviationData::referenceId` to point to non-geometry or `meshId` itself |
| **12** | Bug | **MEDIUM** | `src/commands/SurfaceCommands.cpp:97` | `surface.build` discards fitted `QuadLayout`, leaving pre-fit cage in document/viewport |
| **13** | Bug | **MEDIUM** | `src/app/MainWindow.cpp:151, 545`, `src/app/Viewport.cpp` | Wrong default IDs for M2 command forms, Solid Info menu error, legend vs heatmap mismatch, missing Solid pick |
| **14** | Bug | **LOW** | `scripts/check_layers.py`, `tests/golden/GoldenTests.cpp` | Missing cross-adapter include check and M2 object parity checks in golden tests |
| **P1** | Perf | **HIGH** | `src/pipeline/Surface.cpp:20` | Duplicate `Limit::create` (full OpenSubdiv refinement & stencil build) in `fitCage` + `extractPatches` |
| **P2** | Perf | **HIGH** | `src/adapters/osd/Subdivision.cpp:375` | Redundant final limit-surface tessellation, BVH build & 60k closest-point queries in `fitCage` |
| **P3** | Perf | **HIGH** | `src/adapters/osd/Subdivision.cpp:280` | Unreserved 2.88M triplets & 3× redundant block-diagonal sparse matrix assembly/multiply in `fitCage` |
| **P4** | Perf | **HIGH** | `src/adapters/occt/Brep.cpp:260` | Single-threaded `occt::signedDistances` & repeated `ShapeAnalysis_Surface` construction |
| **P5** | Perf | **MEDIUM** | `src/adapters/occt/Brep.cpp:113, 242` | Expensive `summarise` (`AddOptimal`, `BRepGProp`) re-run on `readBrep` and `solid.export` |
| **P6** | Perf | **MEDIUM** | `src/adapters/osd/Subdivision.cpp:200, 440` | Finite-difference basis calls in `fitCapInterior` (4× per sample) & Lagrange re-interpolation in `extractPatches` |
| **P7** | Perf | **MEDIUM** | `src/pipeline/QuadRemesh.cpp:29, 317, 344` | Duplicate `cageStats` call and repeated unreserved `unordered_map<uint64_t, vector>` builds in `closeCage` |
| **P8** | Perf | **LOW** | `src/app/Viewport.cpp:116, 380, 435, 734` | Recomputing cached `QuadLayout::bounds()`, 200-line legend draw per frame, `QColor` round-trip in `syncColors` |

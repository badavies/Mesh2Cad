# Mesh2Cad M0 & M1 Full Codebase Review

This review covers all 10 source modules (`core`, `document`, `adapters/cgal`, `io`, `pipeline`, `commands`, `cli`, `rpc`, `mcp`, `app`), the build system (`cmake/`), tooling (`scripts/`), CI (`.github/workflows/ci.yml`), and the test suite (`tests/`), evaluated against [ARCHITECTURE.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/ARCHITECTURE.md) and [AI_GUIDELINES.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/AI_GUIDELINES.md).

> [!IMPORTANT]
> Per [AI_GUIDELINES.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/AI_GUIDELINES.md#L5-L10), each fix should be tracked as a scoped issue touching only the named modules and their tests, kept under ~400 lines of non-test code.

---

## Executive Overview by Severity

| Severity | Count | Summary |
| --- | --- | --- |
| **Critical** | 6 | Uncaught `nlohmann::json::type_error` crashes (`std::terminate`) in `Engine`, `RpcServer`, and `McpServer`; UB (`float-cast-overflow`) and infinite loop (DoS) in `readPly`; out-of-bounds read + CGAL precondition crash in `Repair.cpp::crosses`; 1D mesh collapse in `datumAlignment`. |
| **High** | 13 | State loss on `.m2c` save/open (`weldAndCompact` re-merges split non-manifold vertices); stale/dangling `Measurement` indices after in-place mesh edits; no-op `holes.fill`/`mesh.repair`/`faces.delete` wiping `MeshSelection`s; `smoothTaubin` moving selection-border vertices; `SpatialIndex::rayBox` `-0.0` miss bug; `closestPointOnTriangle` degenerate bug; `toSurfaceMesh` dropping non-manifold faces; `fanFill` 2-edge/pinch-vertex corruption; `Engine::onReadyRead` stuck `busyChanged`; detached thread shutdown race in `Viewport::syncGpu`; UAF in object tree context menu; missing `snap_mm` in viewport Measure tool; `m2c_add_library` `PUBLIC` dependency leakage. |
| **Medium** | 19 | Protocol, validation, UI synchronization, geometric edge cases, and build/layer-check gaps. |
| **Low** | 8 | Minor numerical precision, CLI/script polish, and test hygiene issues. |

---

## 1. Critical Bugs (Crashes, Memory Safety, Data Destruction)

### 1.1. Engine Worker Thread Crashes (`std::terminate`) on JSON-RPC Notifications
- **Location:** [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L239-L260), [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L108-L121)
- **Explanation:**
  When a JSON-RPC notification (a request without an `"id"` field, e.g. `{"jsonrpc":"2.0","method":"doc.undo"}`) arrives on the local socket for any command where `command->mutatesDocument == true`:
  1. `c->rpc->handleLine(line)` returns `""` because notifications produce no response ([RpcServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/RpcServer.cpp#L77-L84)).
  2. `Json::parse(reply, nullptr, false)` returns a `discarded` JSON value (`response.is_discarded() == true`).
  3. `failed` is `false`, and because `command->mutatesDocument` is `true`, line 257 executes `response.value("result", Json())` on the `discarded` value.
  4. `nlohmann::json::value()` on a non-object throws `nlohmann::json::type_error.306`. Because `Engine::workerLoop()` has no `try/catch`, `std::terminate()` immediately kills the desktop app—even if the client is **unauthenticated**.

### 1.2. Uncaught `nlohmann::json::type_error` Crashes in `RpcServer`, `McpServer`, and `readSessionFile`
- **Locations:**
  - [RpcServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/RpcServer.cpp#L70-L96)
  - [McpServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/mcp/McpServer.cpp#L58-L93)
  - [Session.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/Session.cpp#L66-L70)
- **Explanation:**
  In `nlohmann::json`, `obj.value(key, default_val)` only returns `default_val` when `key` is **absent**; if `key` is present with a non-convertible type (such as `null`, number, array, or object), it throws `nlohmann::json::type_error`.
  - In [RpcServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/RpcServer.cpp#L70): `request.value("jsonrpc", "")` throws if `"jsonrpc"` is `2.0` or `null`; line 96 `params.value("token", "")` throws if `"token"` is `123` or `null`.
  - In [McpServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/mcp/McpServer.cpp#L90): `message["method"].get<std::string>()` is called without checking `is_string()`; lines 59 and 63 call `params.value("name", "")` and `params.value("arguments", ...)` without checking `params.is_object()` or `params["name"].is_string()`.
  - In [Session.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/Session.cpp#L66-L70): `readSessionFile` does not check `j.is_object()` or field types before calling `j.value(...)`, allowing exceptions to escape across the `rpc` module boundary.

### 1.3. Undefined Behavior (`float-cast-overflow`) in ASCII PLY List Count Parsing
- **Location:** [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L235-L248), [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L305-L311)
- **Explanation:**
  `AsciiReader::read(Type, double& value)` ignores `Type` and calls `parseDouble` (`std::from_chars`), which accepts `"nan"`, `"inf"`, and `"1e308"`. In `readBody`:
  ```cpp
  if (!reader.read(p.countType, countValue) || countValue < 0.0) { ... }
  const auto count = static_cast<std::uint64_t>(countValue);
  ```
  If `countValue` is `NaN`, `+Inf`, or $\ge 2^{64}$, `countValue < 0.0` evaluates to `false`, and `static_cast<std::uint64_t>(countValue)` triggers C++ Undefined Behavior (`[conv.fpint]`), crashing under UBSan.

### 1.4. Infinite Loop / Hang (DoS) in `readPly` on Zero-Property Elements with Large Counts
- **Location:** [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L121-L129), [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L299-L346)
- **Explanation:**
  A PLY header with a non-vertex/non-face element that declares 0 properties and a huge count (e.g. `element dummy 999999999999`) is accepted by `readHeader`. In `readBody`, the outer loop `for (std::uint64_t row = 0; row < element.count; ++row)` runs `999999999999` times while the inner property loop is empty and never reads from the stream, hanging the process indefinitely.

### 1.5. Out-of-Bounds Read (`b[3]`) and CGAL Precondition Crash in `Repair.cpp::crosses`
- **Location:** [Repair.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/Repair.cpp#L92-L118)
- **Explanation:**
  1. `crosses(const Triangle& a, const Triangle& b)` compares `a[i] == b[j]` by 3D point equality. If triangle `a` has a collapsed edge (`a[0] == a[1]`, e.g. created by `PMP::experimental::remove_self_intersections` at line 229, which runs *after* `PMP::remove_degenerate_faces` at line 223) and touches `b[0]`, `shared` contains `{0, 0}` and `{1, 0}` (`shared.size() == 2`). Line 113 then evaluates `b[3 - shared[0].second - shared[1].second]` = `b[3]` on a 3-element `std::array<Point, 3>` (out-of-bounds read / UB).
  2. Line 115 calls `CGAL::coplanar_orientation(p, q, ra, rb)` without checking `!CGAL::collinear(p, q, ra)`. If `p, q, ra` are collinear, `CGAL::coplanar(p, q, ra, rb)` is `true` and `CGAL::coplanar_orientation` throws `CGAL::Precondition_exception`, failing `mesh.repair` with `ErrorCode::Internal`.

### 1.6. `datumAlignment` Without a Secondary Plane Collapses the Mesh to 1D When Primary Normal Is Along Global X
- **Location:** [Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L307-L318)
- **Explanation:**
  When `params.secondary` is omitted:
  ```cpp
  Eigen::Vector3d px{r[0], r[1], r[2]};
  px -= px.dot(z) * z;
  if (px.norm() < 1e-6) px = Eigen::Vector3d::UnitX() - z.x() * z;
  y = z.cross(px.normalized());
  ```
  If the mesh's longest principal axis `px` is `(±1, 0, 0)` and the selected primary datum plane also has normal `(±1, 0, 0)` (so `z = (±1, 0, 0)`), then `px - px.dot(z)*z` is `(0, 0, 0)`, and the fallback `Eigen::Vector3d::UnitX() - z.x() * z` is **also `(0, 0, 0)`**! `y` and `x` both become `(0, 0, 0)`, producing a rank-1 transform that flattens the entire 3D mesh onto the Z axis (`x = 0, y = 0`).

---

## 2. High-Severity Bugs

### 2.1. Project Save/Open (`readProject`) Re-Welds Split Non-Manifold Vertices and Invalidates Vertex Indices
- **Location:** [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L207-L212), [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L351)
- **Explanation:**
  `mesh.repair` splits non-manifold vertices/edges into duplicate vertices with identical `(x, y, z)` coordinates. When saving a `.m2c` project (`writePly`) and reopening it (`readProject` -> `readPly`), `readPly` calls `weldAndCompact`, which merges all coincident vertices back together—turning a repaired manifold mesh back into a **non-manifold mesh** and shifting vertex indices referenced by `Measurement` objects (`SnapKind::Vertex`).

### 2.2. Integer Overflow, Uncaught `std::bad_alloc`, and Zip Bomb Risk in `readProject` / `MeshSelection::fromWords`
- **Locations:** [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L75-L84), [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L219-L228), [MeshSelection.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshSelection.cpp#L33-L40)
- **Explanation:**
  1. In `MeshSelection::fromWords`, `(faceCount + 63) / 64` overflows `std::size_t` to `0` if `faceCount >= SIZE_MAX - 62`, leaving `words_` empty while `size_ = faceCount` (subsequent `test`/`set` accesses out of bounds).
  2. `readProject` constructs `MeshSelection::fromWords(std::move(words), faceCount)` *before* `Document::load` validates `faceCount` against the target mesh, so a large `face_count` in `manifest.json` throws uncaught `std::bad_alloc`.
  3. `readEntry` extracts zip entries via `mz_zip_reader_extract_file_to_heap` with no uncompressed size cap (`m_uncomp_size`) and copies the buffer 3 times in memory.

### 2.3. `Measurement` Objects Become Stale or Out-of-Bounds After In-Place Mesh Edits
- **Locations:** [Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L113-L122), [Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L151-L160)
- **Explanation:**
  `Document::replaceMesh` calls `removeDependents(id, /*onlySelections=*/true)`. When a mesh is modified in place by `mesh.repair`, `holes.fill`, `mesh.simplify`, `mesh.remesh`, `faces.delete`, `shells.clean`, `shells.keep`, `shells.split`, `mesh.flip`, or `mesh.smooth`, existing `Measurement` objects referencing `objectId == id` are retained unchanged. Their `PickedPoint::element` (vertex/face index) can now be out of bounds, their `position` no longer lies on the modified surface, or the shell they were attached to may have been deleted or split into a different object ID.

### 2.4. No-Op Runs of `holes.fill`, `mesh.repair`, and `faces.delete` Wipe All `MeshSelection`s on the Mesh
- **Locations:** [MeshToolCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/MeshToolCommands.cpp#L50-L56), [MeshToolCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/MeshToolCommands.cpp#L82-L88), [MeshToolCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/MeshToolCommands.cpp#L239-L242)
- **Explanation:**
  When `holes.fill` fills 0 holes (`filled->filled == 0`), `faces.delete` deletes 0 faces (`removed == 0`), or `mesh.repair` makes no changes, the stage still returns a new `TriMesh` with a freshly allocated `IndexBuffer`. `commitMesh` calls `Document::replaceMesh`, which sees `data.mesh->sharedIndices() != mesh->sharedIndices()` and deletes every `MeshSelection` on the mesh while pushing a no-op undo step. (Similarly, `mesh.flip` preserves face numbering `0..faceCount()-1` but drops all selections because it allocates a new `IndexBuffer`.)

### 2.5. `smoothTaubin` Moves Selection-Border Vertices, Deforming Unselected Faces
- **Location:** [Smooth.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Smooth.cpp#L59-L65)
- **Explanation:**
  Despite [Smooth.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/include/m2c/pipeline/Smooth.h#L18) documenting that *"Open-boundary vertices, vertices outside the selection and, if asked, vertices on sharp edges stay fixed"*, `smoothTaubin` marks all 3 vertices of every selected face as `movable[v] = true` without fixing vertices that also belong to unselected faces. Smoothing a selected region therefore deforms the unselected triangles bordering the selection.

### 2.6. `SpatialIndex::rayBox` Falsely Misses Boxes When Ray Direction Has `-0.0` and Origin Lies on a Box Face
- **Location:** [SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L34-L48)
- **Explanation:**
  When `dir[k]` is `-0.0` (`inv[k] = -inf`) and `o[k] == lo[k]` (or `o[k] == hi[k]`), one slab distance is `0.0 * (-inf) = NaN` and the other is `-inf` (or `+inf`). Because `if (a > b) std::swap(a, b);` evaluates `false` when either operand is `NaN`, `b = -inf` (or `a = +inf`) is not swapped into the `NaN` slot and updates `t1 = -inf` (or `t0 = +inf`), causing `rayBox` to return `+inf` (a miss).

### 2.7. `closestPointOnTriangle` Returns Wrong Point on Degenerate Triangles (`a == b`)
- **Location:** [SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L52-L96)
- **Explanation:**
  If `a == b` and `c != a`, then `ab = {0, 0, 0}`, `d1 = 0`, `d3 = 0`, and `vc = 0`. For any query point `p` with `d2 = dot(ac, ap) > 0` (including `p == c`), the `ab`-edge Voronoi check `if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)` at line 69 triggers with `denom = 0` and returns `a`, completely ignoring vertex `c` and segment `ac`.

### 2.8. `toSurfaceMesh` Silently Drops Non-Manifold or Inconsistently Oriented Faces
- **Location:** [CgalMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/CgalMesh.cpp#L10-L27)
- **Explanation:**
  Contrary to the comment at line 24, `CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh` does **not** duplicate non-manifold vertices or orient flipped faces (`orient_polygon_soup` does that). If `fillHoles`, `simplify`, or `remesh` is called on a mesh before `mesh.repair`, any non-manifold or opposite-winding face is silently dropped by `toSurfaceMesh`, creating unexpected holes.

### 2.9. `HoleFilling.cpp::fanFill` Corrupts `SurfaceMesh` on 2-Edge Slit Holes or Pinch-Vertex Holes
- **Location:** [HoleFilling.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/HoleFilling.cpp#L71-L95), [HoleFilling.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/HoleFilling.cpp#L151-L154)
- **Explanation:**
  When `PMP::triangulate_hole` fails on a 2-edge degenerate hole (`count == 2`) or a figure-8 hole whose boundary cycle visits a non-manifold pinch vertex twice, `fanFill` calls `CGAL::Euler::fill_hole` + `CGAL::Euler::add_center_vertex`, which creates parallel halfedges and corrupts the `SurfaceMesh` halfedge structure.

### 2.10. `Engine::onReadyRead` Never Emits `busyChanged`, Permanently Locking the UI in `"Working…"` State
- **Location:** [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L158-L184), [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L238-L272)
- **Explanation:**
  `Engine::onReadyRead` increments `++pending_` and decrements `--pending_` around socket RPC jobs without emitting `busyChanged`. If a UI command finishes while a socket RPC job is still queued (`pending_` goes `2 -> 1`), `Engine::run` emits `busyChanged(true, ...)`. When the socket job finishes (`pending_` goes `1 -> 0`), `busyChanged(false, ...)` is never emitted, permanently disabling the `Run` button and locking the busy cursor.

### 2.11. Detached Thread in `Viewport::syncGpu` Races with Application Shutdown
- **Location:** [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L322-L342)
- **Explanation:**
  `Viewport::syncGpu()` spawns and `.detach()`es a raw `std::thread` on every new mesh to build `spatialIndex()`, `faceAdjacency()`, and `buildDisplayProxy()`, then calls `QMetaObject::invokeMethod(qApp, ...)`. Closing the app (or finishing a test/screenshot run) while the detached thread is running races with `~QApplication()` (`qApp` dangling/null) and static destruction.

### 2.12. Use-After-Free in `MainWindow` Object Tree Context Menu
- **Location:** [MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L140-L155)
- **Explanation:**
  The context menu lambda for `"Rename"` captures the raw `QTreeWidgetItem* item` across `menu.exec(...)`. If an external agent or queued engine job updates the document while the popup menu is open, `refreshTree()` calls `tree_->clear()`, freeing `item` before the user clicks `"Rename"`.

### 2.13. Viewport Measure Tool Never Snaps to Vertices or Edges
- **Location:** [MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L638-L648), [Measure.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Measure.cpp#L8-L15)
- **Explanation:**
  `measure.distance` defaults `snap_mm` to `0.0`, which causes `snapToMesh` to return `SnapKind::Surface` immediately. `MainWindow::onMeasured` omits `"snap_mm"`, so interactive viewport measurements never snap to vertices or edges as required by [ARCHITECTURE.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/ARCHITECTURE.md#L222).

### 2.14. `m2c_add_library` Links All `DEPS` as `PUBLIC`, Breaking CMake Compile-Time Include Isolation
- **Location:** [Mesh2CadBuild.cmake](file:///c:/Users/badav/ClionProjects/Mesh2Cad/cmake/Mesh2CadBuild.cmake#L81-L88)
- **Explanation:**
  `target_link_libraries(m2c_${name} PUBLIC ${ARG_DEPS} ...)` propagates every lower module's `include/` directory transitively to all higher targets (for example, `m2c_app` gets `src/io/include`, `src/pipeline/include`, and `src/adapters/cgal/include` on its include path, and [AppTests.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/tests/app/AppTests.cpp#L18) includes `"m2c/io/MeshIO.h"` directly via `m2c_app`).

---

## 3. Medium-Severity Bugs & Guideline/Architecture Discrepancies

### 3.1. `McpServer::callTool` Forwards Unmapped Tool Names Verbatim and `align.transform` Accepts `NaN`/`Inf`
- **[McpServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/mcp/McpServer.cpp#L58-L65):** If `name` is not in `commandForTool_`, `callTool` passes `name` straight to `call_`, allowing MCP clients to call internal RPC methods like `rpc.hello` or `rpc.auth`.
- **[AlignMeasureCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/AlignMeasureCommands.cpp#L199-L218):** In `align.transform`, if `rotation` contains `NaN` or `Inf`, `error` and `det` become `NaN`, and `if (error > 1e-6 || det < 0.0)` evaluates to `false`, accepting a `NaN` matrix!

### 3.2. `Document::load` and `checkData` Gaps (`nextId == 0`, Unvalidated `MeasurementData`, Open Step State)
- **[Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L206-L231):** Calling `Document::load({}, 0)` sets `nextId_ = 0`, so the next added object receives the reserved sentinel `ObjectId{0}`.
- **[Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L68-L86):** `checkData` never validates `MeasurementData` (allowing references to missing or non-mesh object IDs, out-of-range element indices, or non-finite coordinates).
- **[Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L125-L137):** `replaceSelection` does not update `Provenance`.
- **[Document.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/document/Document.cpp#L199-L231):** `clear()` and `load()` do not reset `depth_`, `dirty_`, or `open_` if called inside `beginStep()`.

### 3.3. Quadratic $O(k^2)$ Memory Blowup and 32-Bit Offset Overflow in `buildFaceAdjacency`
- **[MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L32-L54):** An edge shared by $k$ faces pushes $k(k-1)$ pairs, and `adj.offsets` is `std::vector<std::uint32_t>`, overflowing at $k \ge 65,537$.
- **[MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L20), [Smooth.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Smooth.cpp#L70-L95), [Shells.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Shells.cpp#L10-L43):** Degenerate faces (`v[0] == v[1]`) have 0 neighbours in `FaceAdjacency`, which causes `smoothTaubin` to freeze their vertices as "open boundary" vertices and `detectShells` / `shells.split` to treat each degenerate face as a separate 1-face shell.

### 3.4. Repair & Simplification Stage Reporting Issues
- **[Repair.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/Repair.cpp#L215-L259):** `PMP::orient_polygon_soup` returns `true` even when it flips faces (it only returns `false` if vertices had to be duplicated), so `result.orientationFixed` is falsely `false` when flipped faces are fixed without vertex duplication. Also `removedFaces` and `splitVertices` miscount when `stitch_borders` or `removeSelfIntersections` runs, and `uniteCrossingShells` lacks a per-group `try/catch` around `corefine_and_compute_union`.
- **[Simplify.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/Simplify.cpp#L36):** `SimplifyStop` checks `number_of_faces() < targetFaces` instead of `<= targetFaces`, collapsing one extra edge past `targetFaces`.

### 3.5. Pipeline & Selection Edge Cases (`lassoSelect`, `growSelection`, `snapToMesh`, `symmetryAlignment`, `shells.split`)
- **[Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L95-L115):** `lassoSelect` checks `cw > 0.0` but never checks `cz / cw` against `[-1, 1]` (failing to clip points behind the camera in orthographic projections where `cw == 1.0`) and uses a distance-scaled occlusion dead-zone `dist * (1.0 - 1e-6)`.
- **[Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L136-L146):** `growSelection` shrink mode (`rings < 0`) checks `degree < 3` (unique adjacent faces) rather than open edges, mishandling non-manifold edges and eroding `select.all` selections around mesh holes.
- **[Measure.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Measure.cpp#L8-L45):** `snapToMesh` only checks vertices/edges of `nearest.face`, missing closer vertices on adjacent triangles across an edge.
- **[Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L74-L83), [Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L154-L200):** `symmetryAlignment` divides by `totalWeight == 0.0` (`NaN`) if strided samples hit only zero-area faces; `skewSign` uses unweighted vertices instead of area-weighted moments; `datumAlignment` tertiary plane can place the part in $X \le 0$.
- **[ShellCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/ShellCommands.cpp#L157-L167):** `shells.split` drops the parent mesh's accumulated `alignment` on newly split shell objects.

### 3.6. File Parser Edge Cases (`Obj.cpp`, `Stl.cpp`, `Parse.h`)
- **[Parse.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/detail/Parse.h#L51-L67), [Stl.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Stl.cpp#L86-L93):** `parseDouble`/`parseInt`/`parseFloat` strip a leading `'+'` before `std::from_chars`, accepting `"+-1.0"` as `-1.0`; `parseDouble` also accepts `"nan"` and `"inf"`, allowing unused `NaN`/`Inf` vertices in OBJ/PLY to pass undetected.
- **[Stl.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Stl.cpp#L133-L165):** `readAscii` only reads the word `"solid"` and does not skip the rest of the header line, so a solid name containing `"vertex"` (e.g. `solid vertex_mesh`) fails to parse; uppercase `"VERTEX"` is also ignored.
- **[Obj.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Obj.cpp#L27-L69):** Inline `#` comments on `f` lines are not stripped and cause a parse error.

### 3.7. Command Schema vs. Handler Mismatches & CLI/RPC Parity
- **[main.cpp (cli)](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/cli/main.cpp#L111-L112):** CLI never sets `context.source = "cli"`, so `doc.history` records `"api"` instead of `"cli"`.
- **[ShellCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/ShellCommands.cpp#L66-L79), [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L155-L162), [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L223-L236):** `shells.select`, `select.lasso`, `select.grow`, and `select.shrink` use raw `params.at(...)` / `params.value(...)` instead of `detail/Params.h`, silently truncating floats to ints, clamping out-of-range `rings`, or failing to check `polygon.is_array()`.
- **[DocumentCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/DocumentCommands.cpp#L124-L128):** `object.show` schema marks `"visible"` as required, but handler uses `optionalBool(params, "visible", true)`.
- **[RpcServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/RpcServer.cpp#L118-L129):** `mesh2cad-rpc` writes `doc.changed` notifications to `stdout` *before* the command response, and its `doc.changed` payload includes `"command"` whereas [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L139-L141) does not.

### 3.8. Desktop UI & Viewport Issues
- **[MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L629-L636):** `onFaceClicked` passes `add || !subtract` to `selectionTarget`, which is always `true` when `subtract == false`, making unmodified clicks in `Mode::Crease` always add instead of replace.
- **[MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L137-L155):** Context menu `"Info"` and `"Fit view"` fail or do nothing on Selection and Measurement tree items.
- **[MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L508-L512):** Selecting a parent Mesh in the object tree makes `currentSelection()` return `0`, causing subsequent brush/lasso strokes to create duplicate `Selection` objects and breaking selection shortcuts.
- **[MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L383-L427), [MainWindow.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/MainWindow.cpp#L700-L724):** `ParamForm` default IDs do not update when tree selection changes; `saveProject()` captures `snapshot_->revision` before queuing `project.save`.
- **[Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L138-L147), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L233-L296):** `setSnapshot` does not clear stale `measureStart_`; `upload()` uses a single monolithic buffer instead of chunked buffers ([ARCHITECTURE.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/ARCHITECTURE.md#L268)); `gpu.selectionKey` compares raw `const MeshSelection*` pointers (ABA hazard).
- **[DisplayProxy.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/DisplayProxy.cpp#L74-L82):** If a later iteration of the coarsening loop collapses all faces (`cluster` returns `std::nullopt`), it discards the valid proxy from the previous round and returns the full 10M-triangle mesh.

### 3.9. Build, Layering Script & Test Suite Gaps
- **[check_layers.py](file:///c:/Users/badav/ClionProjects/Mesh2Cad/scripts/check_layers.py#L20-L36):** Does not catch `<m2c/...>` angle-bracket includes, relative `../` includes, Qt/Eigen includes in forbidden modules, or future adapter-to-adapter includes (`adapters/occt` including `adapters/cgal`).
- **[Mesh2CadBuild.cmake](file:///c:/Users/badav/ClionProjects/Mesh2Cad/cmake/Mesh2CadBuild.cmake#L27-L31), [Mesh2CadDependencies.cmake](file:///c:/Users/badav/ClionProjects/Mesh2Cad/cmake/Mesh2CadDependencies.cmake#L114):** Global `/fp:precise` vs `/fp:strict` on `m2c_cgal` triggers MSVC warning `D9025`.
- **[tests/unit/CMakeLists.txt](file:///c:/Users/badav/ClionProjects/Mesh2Cad/tests/unit/CMakeLists.txt#L1-L11):** Missing `TriMesh` <-> CGAL `Surface_mesh` round-trip unit test required by [ARCHITECTURE.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/ARCHITECTURE.md#L347).

---

## 4. Low-Severity Issues

1. **[MeshStats.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/include/m2c/core/MeshStats.h#L27-L29):** `isWatertight()` returns `true` for a mesh whose faces are all degenerate (`edgeCount == 0`).
2. **[HoleFilling.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/HoleFilling.cpp#L41-L49), [MeshStats.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshStats.cpp#L45):** Newell hole normal and signed volume use `ORIGIN` instead of a local reference point, risking catastrophic cancellation far from the origin.
3. **[Transform.h](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/include/m2c/core/Transform.h#L19-L24):** Doc comment on `Transform3d::fromAxes` says "images of the x, y and z axes (the rows of R)" instead of pre-images (or columns).
4. **[Session.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/Session.cpp#L47-L56):** `writeSessionFile` on POSIX creates `session.json` with default umask before `chmod(0600)`, and `LocalClient::readLine` has no line-size cap.
5. **[gen_commands.py](file:///c:/Users/badav/ClionProjects/Mesh2Cad/scripts/gen_commands.py#L52-L53):** `write_text` without `newline="\n"` writes CRLF on Windows.
6. **[FuzzDriver.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/tests/fuzz/FuzzDriver.cpp#L158-L163):** `--save-last` reopens and truncates the output file on every iteration (1,000,000 times on nightly failure).
7. **[AppTests.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/tests/app/AppTests.cpp#L132-L142):** Does not call `QStandardPaths::setTestModeEnabled(true)`, leaving session `.jsonl` logs in the user's real `AppLocalDataLocation/sessions` directory.
8. **[main.cpp (app)](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/main.cpp#L56-L72):** `--screenshot` mode can block on `QMessageBox::warning` or exit `0` when file import fails.

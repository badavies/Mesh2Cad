# Mesh2Cad Performance & Efficiency Review (M0 & M1)

This report analyzes algorithmic, memory, cache, I/O, and GPU inefficiencies across the codebase, quantified against the v1.0 target workload in [ARCHITECTURE.md](file:///c:/Users/badav/ClionProjects/Mesh2Cad/docs/ARCHITECTURE.md#L11-L30) (**1M–10M+ triangles**, ~0.5M–5M+ vertices).

---

## Summary of High-Impact Bottlenecks (10M-Triangle Scale)

| Rank | Bottleneck | Location | Cost on 10M-Face Mesh |
| --- | --- | --- | --- |
| **1** | Uncached `computeStats()` runs after **every** mesh command | [Params.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/detail/Params.cpp#L194-L201), [MeshStats.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshStats.cpp#L20-L69) | **240 MB** allocation + `std::sort` of **30M** `uint64_t` edges on every edit/align/import/info call |
| **2** | `FaceAdjacency` cache discarded on vertex-only edits (`align.*`, `mesh.smooth`) + double global sort | [TriMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/TriMesh.cpp#L23-L38), [MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L10-L55) | **~1.2 GB** peak memory + two `std::sort` passes (**30M** edge uses and **60M** face pairs) |
| **3** | `smoothTaubin` does full-mesh $O(F \log F + \text{iter}\cdot V)$ work even for a tiny selection | [Smooth.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Smooth.cpp#L23-L119) | **480 MB** allocation + `std::sort` of **60M** directed edges on every `mesh.smooth` call |
| **4** | `shells.split` is $O(S \cdot (F + V))$ where $S$ is the number of shells | [ShellCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/ShellCommands.cpp#L147-L168), [MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L98-L123) | With 500 stray shells on 10M faces: **$2.25 \times 10^{10}$ loop iterations** |
| **5** | `Repair.cpp` runs full-mesh self-intersection scans up to **3 times** | [Repair.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/Repair.cpp#L150-L236) | 2–3 full AABB-tree self-intersection passes in a single `mesh.repair` call |
| **6** | `select.brush` stroke allocates 2 full bitsets and scans all bitset words per dab | [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L36-L42), [Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L20-L46) | $O(D \cdot F / 64)$ for $D$ stroke points instead of $O(\text{touched faces})$ |
| **7** | `growSelection` scans all $F$ faces on every ring step | [Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L119-L150) | $O(\text{rings} \cdot F)$ instead of $O(\text{frontier})$ |
| **8** | `readProject` copies `.ply` data 5 times in RAM and re-welds already-indexed vertices | [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L75-L84), [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L166-L212) | **>1.2 GB** RAM and **5M** `std::unordered_map` node allocations on project load |
| **9** | `SpatialIndex` allocates 720 MB of temp arrays and `raycast` does not order children by distance | [SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L98-L209) | **720 MB** temporary build memory; unpruned BVH node visits during raycasting |
| **10** | Synchronous UI-thread GPU upload of full mesh + EBO re-upload on vertex-only edits | [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L233-L347) | **180 MB** CPU/GPU buffer upload on the Qt GUI thread on first paint and after every edit |

---

## 1. Core & Command Engine Bottlenecks

### 1.1. Uncached `computeStats()` Sorts 30M Edges After Every Mesh Command
- **Locations:** [Params.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/detail/Params.cpp#L194-L201), [MeshStats.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshStats.cpp#L20-L69), [MeshToolCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/MeshToolCommands.cpp#L22-L26), [AlignMeasureCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/AlignMeasureCommands.cpp#L52)
- **What happens:**
  15 commands (`mesh.import`, `mesh.info`, `mesh.repair`, `holes.fill`, `mesh.smooth`, `mesh.simplify`, `mesh.remesh`, `mesh.wrap`, `faces.delete`, `mesh.flip`, and all 5 `align.*` commands) end by calling `describeMesh(context.document, id)`, which calls `computeStats(*object->mesh()->mesh)`.
  On a 10M-triangle mesh, **every single call** to `computeStats`:
  1. Allocates `std::vector<std::uint64_t> edges` with `30,000,000` elements (**240 MB**).
  2. Computes 10M cross products, `sqrt`s, and signed tetrahedron volumes.
  3. Runs `std::sort(edges.begin(), edges.end())` on 30,000,000 64-bit integers.
- **Why it hurts:**
  - `MeshStats` is not cached in `MeshCaches` on `TriMesh`, so calling `mesh.info` (or `describeMesh`) recomputes it every time.
  - Rigid transforms (`align.principal`, `align.symmetry`, `align.datum`, `align.transform`, `align.reset`) preserve `edgeCount`, `boundaryEdgeCount`, `nonManifoldEdgeCount`, `degenerateFaceCount`, `surfaceArea`, and `signedVolume` (up to floating-point rounding), yet still sort 30M edges from scratch.
  - `buildFaceAdjacency` ([MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L16-L30)) already sorts the exact same 30M undirected edge keys!

### 1.2. `FaceAdjacency` Cache Is Discarded When Only Vertex Positions Change, and `TriMesh::create` Re-Validates Unchanged Indices
- **Locations:** [TriMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/TriMesh.cpp#L23-L38), [TriMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/TriMesh.cpp#L70-L77), [Transform.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/Transform.cpp#L9-L15), [Smooth.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Smooth.cpp#L121-L125)
- **What happens:**
  When `transformMesh` (`align.*`) or `smoothTaubin` (`mesh.smooth`) creates a new `TriMesh`, it passes `mesh.sharedIndices()` to `TriMesh::create`:
  1. `TriMesh::create` scans all 30M indices again (`lines 70-77`) even when `vertexCount` and `indices` are unchanged.
  2. `TriMesh` allocates a fresh `MeshCaches` struct (`line 25`), throwing away the already-built `FaceAdjacency` even though `FaceAdjacency` depends **only** on `indices_` (not vertex coordinates).

### 1.3. `buildFaceAdjacency` Performs Two 30M–60M Sorts and Allocates ~1.2 GB on 10M Faces
- **Location:** [MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L10-L55)
- **What happens:**
  1. Allocates `uses` (`3 * F` = 30M `EdgeUse` structs, **480 MB**) and runs `std::sort` on 30M elements.
  2. Allocates an unreserved `std::vector<std::pair<uint32_t, uint32_t>> pairs` (which grows via geometric reallocations up to 60M elements, peaking at **~536 MB**), pushes `(f, g)` and `(g, f)`, and runs a **second global `std::sort`** on all 60M pairs (`line 45`).
  3. Allocates `adj.offsets` (40 MB) and `adj.neighbours` (240 MB) and copies `pairs` into CSR.
- **Remedy:**
  After sorting `uses`, count per-face degrees directly from `uses`, prefix-sum `adj.offsets`, and scatter neighbors directly into `adj.neighbours`. Deduplicating each face's tiny neighbor slice (`adj.offsets[f] .. adj.offsets[f+1]`, typically 3 elements) eliminates the entire 536 MB `pairs` vector and the 60M-element global `std::sort`.

---

## 2. Pipeline & Tool Bottlenecks (`src/pipeline`, `src/adapters/cgal`)

### 2.1. `smoothTaubin` Does $O(F \log F + \text{iterations} \cdot V)$ Whole-Mesh Work Even for a Small Selection
- **Location:** [Smooth.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Smooth.cpp#L23-L119)
- **What happens:**
  1. `buildVertexRing(mesh)` (`lines 23-45`) is not cached on `TriMesh`: on every `mesh.smooth` call, it allocates `6 * F` = 60M `std::pair<uint32_t, uint32_t>` (**480 MB** for 10M faces) and runs a global `std::sort` on 60M elements, instead of counting vertex degrees in $O(F + V)$, filling CSR directly, and deduplicating each vertex's ~6 neighbors locally.
  2. When smoothing a small selection (e.g., 500 brushed faces on a 10M-face scan):
     - Lines 70–95 still loop over **all 10M faces** and compute `faceNormal(mesh, f)` (`cross` + `sqrt`), even for faces whose vertices are all `movable == false`.
     - `buildVertexRing` still builds the 1-ring for **all 5M vertices**.
     - Each of the `2 * iterations` Taubin half-steps (`lines 105-118`) loops over **all 5M vertices** (`for (std::size_t v = 0; v < n; ++v) next[v] = current[v];`) instead of iterating only over the movable vertices.

### 2.2. `shells.split` Is $O(S \cdot (F + V))$ Instead of $O(F + V)$
- **Locations:** [ShellCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/ShellCommands.cpp#L147-L168), [Shells.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Shells.cpp#L45-L55), [MeshTopology.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/MeshTopology.cpp#L98-L123)
- **What happens:**
  For every non-main shell `s` in `report.shells`:
  - `shellFaces(report, {s.index})` scans all $F$ faces (`report.faceShell`).
  - `keepFaces` allocates `std::vector<bool> keep(F)` and scans all $F$ faces.
  - `extractFaces` allocates `used(V)` and `remap(V)` and scans all $F$ faces twice and all $V$ vertices once.
  On a 10M-face scan with 500 loose scan fragments, `shells.split` scans the 10M-face and 5M-vertex arrays **thousands of times** ($>2 \times 10^{10}$ iterations), plus `Document::addMesh` copies `objects_` on the first mutation. Partitioning faces by `report.faceShell[f]` in a single pass reduces this from $O(S(F + V))$ to $O(F + V)$.
- **Related (`shells.keep`):** [ShellCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/ShellCommands.cpp#L132-L136) calls `keepFaces(*object->mesh, shellFaces(report, main))` *before* checking `if (deleted > 0)`, cloning and validating the entire 10M-triangle mesh even when there is only 1 shell.

### 2.3. `select.brush` Stroke and `storeSelection` Do Redundant Full-Bitset Passes
- **Locations:** [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L36-L42), [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L132-L136), [Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L20-L46)
- **What happens:**
  1. For a brush stroke with $D$ points (`centers`, e.g. $D = 100$), [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L133-L136) calls `brushSelect` $D$ times:
     - Each call allocates two full `MeshSelection(F)` bitsets (2.5 MB at 10M faces).
     - Each call runs **both** `index.facesWithin(center, radiusMm)` and `index.closestPoint(center)` (two BVH traversals when `facesWithin` could track the closest face during the same traversal).
     - Then `dab.indices()` scans all $F/64$ words twice (`count()` + bit extraction) to copy a handful of bits into `faces`.
  2. In `storeSelection` ([SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L38-L42)), combining `faces` with `old` via `"add"` or `"subtract"` calls `faces.indices()` (allocating a `std::vector<uint32_t>` up to 40 MB) and loops `combined.set(f, ...)` bit-by-bit instead of word-level `words_[w] |= other.words_[w]` or `words_[w] &= ~other.words_[w]`.

### 2.4. `growSelection` Scans All $F$ Faces on Every Ring Step ($O(\text{rings} \cdot F)$)
- **Location:** [Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L119-L150)
- **What happens:**
  Each ring iteration `r = 0 .. steps - 1` clones the full `MeshSelection` and loops `for (std::size_t f = 0; f < mesh.faceCount(); ++f)` across all 10M faces. Growing a 50-face selection by 20 rings scans 200,000,000 faces instead of advancing a frontier queue of ~50–500 boundary faces.

### 2.5. `select.shell` Computes Areas and Bounds for Every Shell in the Mesh
- **Location:** [SelectionCommands.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/commands/SelectionCommands.cpp#L211)
- **What happens:**
  `select.shell` calls `detectShells(*mesh->mesh)` (which labels every component in the mesh and computes areas with `cross`/`sqrt` and bounding boxes for all 10M faces) when it only needs a BFS from the single seed `face` over `mesh.faceAdjacency()`.

### 2.6. `lassoSelect` Lacks 2D Polygon AABB Rejection and Normalizes Normals Unnecessarily
- **Location:** [Select.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Select.cpp#L84-L115)
- **What happens:**
  1. `insidePolygon(cx / cw, cy / cw)` runs the $O(|P|)$ ray-crossing loop over every vertex of the lasso polygon for **all 10M triangles** without first checking whether `(ndcX, ndcY)` lies inside the lasso polygon's 2D `[minX, maxX] × [minY, maxY]` bounding box (or `[-1, 1]^2`). For a 200-point freehand lasso on 10M faces, a 2D AABB pre-check avoids up to $2 \times 10^9$ floating-point divisions.
  2. Line 106 calls `dot(faceNormal(mesh, f), toFace) >= 0.0`, where `faceNormal` computes `length(n)` (`std::sqrt`) and `n * (1.0 / len)` even though only the sign of `dot(cross(p[b] - p[a], p[c] - p[a]), toFace)` is needed.

### 2.7. `datumAlignment` Computes Full-Mesh `surfaceMoments` Twice, and `fitPlane` Scans All $F$ Faces
- **Locations:** [Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L103-L130), [Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L308-L321)
- **What happens:**
  1. When `datumAlignment` runs without a secondary plane, line 309 calls `principalAxesAlignment(mesh)` (which computes `surfaceMoments` over all 10M faces and `skewSign` over all 5M vertices twice), and line 321 immediately calls `surfaceMoments(mesh, [](std::size_t) { return true; })` a second time over all 10M faces just to get `all.centroid`.
  2. In `fitPlane` ([Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L107)), `surfaceMoments` iterates `f = 0 .. F - 1` calling `faces.test(f)` instead of iterating only over the set words/bits of `faces`, and line 117 allocates `std::vector<bool> seen(mesh.vertexCount(), false)` plus `faces.indices()`.
  3. In `symmetryAlignment` ([Align.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/pipeline/Align.cpp#L216)), each of the $3 \times 50 = 150$ Gauss-Newton iterations runs a full `std::sort` on 20,000 elements to find the weighted median, and `match()` recomputes `faceNormal(mesh, c.face)` (`sqrt`) 20,000 times per iteration.

### 2.8. `Repair.cpp` Runs Self-Intersection Detection up to 3 Times and `HoleFilling.cpp` Clones Meshes When 0 Holes Are Filled
- **Locations:** [Repair.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/Repair.cpp#L150-L236), [HoleFilling.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/HoleFilling.cpp#L101-L165), [CgalMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/CgalMesh.cpp#L10-L27)
- **What happens:**
  1. In `repair()` with `removeSelfIntersections = true`:
     - `uniteCrossingShells` calls `scanIntersections(mesh)` at line 150 (running `PMP::connected_components` + `PMP::self_intersections`).
     - At line 171, it calls `!PMP::does_self_intersect(shells[s])` on each shell, even though `scanIntersections` already saw all self-intersections and could have recorded whether any pair had `a == b`.
     - At line 236, `repair()` calls `scanIntersections(mesh)` **again**—even when `uniteCrossingShells` returned early at line 151 because `scan.shellPairs.empty()`!
  2. In `fillHoles()` ([HoleFilling.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/HoleFilling.cpp#L101-L165)):
     - Even when the input mesh has 0 boundary edges (or all holes are skipped so `filled == 0`), it converts the entire mesh to `CGAL::Surface_mesh` (`toSurfaceMesh`) and back (`fromSurfaceMesh`), allocating ratusan MB and invalidating `FaceAdjacency`, `SpatialIndex`, and all `MeshSelection`s.
  3. In `toSurfaceMesh()` ([CgalMesh.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/adapters/cgal/CgalMesh.cpp#L15-L25)):
     - Allocates intermediate `std::vector<Point> points(V)` (120 MB) and `std::vector<std::array<std::size_t, 3>> faces(F)` (240 MB) before calling `polygon_soup_to_polygon_mesh`.

---

## 3. Spatial Index & Display Proxy Bottlenecks (`src/core`)

### 3.1. `SpatialIndex` Build Allocates 720 MB of Temporary Vectors and Suffers Random-Access Cache Misses
- **Location:** [SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L98-L164)
- **What happens:**
  1. Constructor allocates `centroid(F)`, `lo(F)`, and `hi(F)` as `std::vector<Vec3d>` ($3 \times 10\text{M} \times 24\text{ B} = \mathbf{720\text{ MB}}$) alongside `faces_` (40 MB) and `nodes_` (~140 MB). Storing `lo`/`hi`/`centroid` in `float` (or computing leaf bounds from `positions_`) cuts temporary build memory by half.
  2. At each of the ~21 BVH tree levels, the loop at lines 130–136 and `std::nth_element` at lines 150–155 index into `lo[faces_[i]]`, `hi[faces_[i]]`, and `centroid[faces_[i]]` indirectly via `faces_[i]`, causing 3 random cache misses per face per level across 720 MB of memory.
  3. `SpatialIndex::facesWithin` ([SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L250)) heap-allocates `std::vector<std::uint32_t> stack{0}` on every query instead of using `std::array<std::uint32_t, 128>` like `raycast` and `closestPoint`.

### 3.2. `SpatialIndex::raycast` Does Not Order Child Nodes by Entry Distance
- **Location:** [SpatialIndex.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/SpatialIndex.cpp#L178-L185)
- **What happens:**
  ```cpp
  if (rayBox(origin, inv, node.min, node.max, bestT) > bestT) continue;
  if (node.count == 0) {
      stack[top++] = node.first;
      stack[top++] = node.first + 1;
      continue;
  }
  ```
  Unlike `closestPoint` (lines 223–228), `raycast` never compares the entry distances `tLeft = rayBox(..., l.min, l.max, bestT)` and `tRight = rayBox(..., r.min, r.max, bestT)` before pushing children. It always visits `node.first + 1` first, so rays traveling from `node.first` toward `node.first + 1` traverse the back subtree before finding the front hit that would have pruned the back subtree.

### 3.3. `DisplayProxy::cluster` Uses an Unreserved `std::unordered_map` and Re-Clusters Up to 9 Times
- **Location:** [DisplayProxy.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/core/DisplayProxy.cpp#L15-L82)
- **What happens:**
  1. `cellIndex` (`std::unordered_map<uint64_t, uint32_t>`), `sum`, and `count` are not reserved (e.g. `targetFaces`), causing repeated rehashes and ~125k–500k heap node allocations per round across 5M vertex lookups.
  2. When the initial cell size leaves `proxy->faceCount() > targetFaces`, the loop at lines 76–80 runs `cluster(mesh, cell)` again from scratch over all 5M original vertices and 10M original faces (including full `TriMesh::create` validation each round), instead of clustering the already-reduced proxy or sorting/hashing once.

---

## 4. File I/O & Project Archive Bottlenecks (`src/io`)

### 4.1. `readProject` Copies Mesh Data 5 Times in Memory and Re-Runs `weldAndCompact`
- **Locations:** [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L75-L84), [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L169-L212), [Ply.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Ply.cpp#L351)
- **What happens on `project.open`:**
  1. [Project.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Project.cpp#L169-L170) reads the entire `.m2c` zip file using single-character `std::istreambuf_iterator<char>(file)` into an unreserved `std::string bytes` (instead of seeking to file size and calling `file.read(...)` once, or using `mz_zip_reader_init_file` / file callback).
  2. `readEntry` extracts `meshes/1.ply` to a heap buffer (`mz_zip_reader_extract_file_to_heap`), then copies it into `std::string out` (`line 81`).
  3. Line 209 constructs `std::istringstream in(*text)`, which **copies the entire `.ply` string a third time**!
  4. `readPly(in)` parses into `positions` and `indices`, then calls `weldAndCompact` (`Ply.cpp:351`), which builds a 5M-entry `std::unordered_map` and copies into `outPositions` and `outIndices` even though `.m2c` PLY files were written from already-welded `TriMesh`es.

### 4.2. `Stl.cpp` Binary Reader Never Reserves `Welder::lookup_` or `positions_`
- **Location:** [Stl.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Stl.cpp#L71), [Stl.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Stl.cpp#L106-L107)
- **What happens:**
  `readBinary` knows the exact `triangleCount` from the STL header and calls `welder.reserveTriangles(triangleCount)`, which reserves `indices_.reserve(n * 3)`, but **never reserves `lookup_` or `positions_`** (`~n / 2` unique vertices for a closed manifold mesh). On a 10M-triangle binary STL, `lookup_` rehashes ~23 times and `positions_` reallocates ~23 times.

### 4.3. `writeObj` Allocates 8 Temporary `std::string`s Per Face (80M Allocations for 10M Faces)
- **Location:** [Obj.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/Obj.cpp#L101-L106)
- **What happens:**
  ```cpp
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
      const auto [a, b, c] = mesh.face(f);
      buffer += "f " + std::to_string(a + 1) + ' ' + std::to_string(b + 1) + ' ' +
                std::to_string(c + 1) + '\n';
      if (buffer.size() > 60000) flush();
  }
  ```
  Whereas the vertex loop (`lines 85-100`) uses `std::to_chars` into a stack buffer `std::array<char, 32>`, the face loop calls `std::to_string` 3 times and chains 5 `operator+` concatenations per face, creating 8 temporary `std::string` objects per triangle.

### 4.4. `LineReader::next()` Zero-Fills 64 KiB on Every Chunk Read
- **Location:** [Parse.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/io/detail/Parse.cpp#L29-L36)
- **What happens:**
  `buffer_.resize(old + chunk_)` zero-initializes 64 KiB of memory immediately before `in_.read(buffer_.data() + old, chunk_)` overwrites it, and `buffer_.erase(0, pos_)` shifts remaining bytes before resizing.

---

## 5. Desktop App, Viewport & IPC Bottlenecks (`src/app`, `src/rpc`)

### 5.1. Synchronous Full-Mesh VBO + EBO Upload on the Qt GUI Thread and Redundant EBO Re-Uploads
- **Location:** [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L233-L262), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L310-L347)
- **What happens:**
  1. `Viewport::syncGpu()` runs inside `paintGL()` on the UI thread. When a 10M-triangle mesh is added or modified, `upload()` allocates a 60 MB `std::vector<float> xyz`, converts 5M `Vec3d`s on the CPU, and uploads 60 MB (`vbo`) + 120 MB (`ebo`) = **180 MB** synchronously on the UI thread.
  2. When a vertex-only operation (`mesh.smooth`, `align.*`) runs, `mesh->sharedIndices()` is unchanged, yet `syncGpu()` calls `release(gpu)` and re-uploads the 120 MB `ebo` alongside the `vbo`.
  3. Hidden meshes (`o.visible == false`) are still uploaded to GPU memory and still spawn background proxy/index threads in `syncGpu()`.

### 5.2. Hover Picking on the UI Thread Can Block on `std::call_once` While 10M-Face BVH Builds
- **Location:** [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L547-L564), [Viewport.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Viewport.cpp#L653-L667)
- **What happens:**
  In `Mode::Brush`, `Mode::Crease`, and `Mode::Measure`, every `mouseMoveEvent` calls `pick(pos)` on the UI thread, which calls `o.mesh()->mesh->spatialIndex().raycast(...)`. If the mesh was just imported or modified and the background thread spawned by `syncGpu()` is currently building the 10M-face `SpatialIndex`, `std::call_once(caches_->indexOnce, ...)` blocks the Qt UI thread for seconds until the BVH build finishes.

### 5.3. Triple JSON Parsing and $O(N^2)$ Buffer Shifting in `Engine::onReadyRead`
- **Location:** [Engine.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/app/Engine.cpp#L234-L260), [RpcServer.cpp](file:///c:/Users/badav/ClionProjects/Mesh2Cad/src/rpc/RpcServer.cpp#L60)
- **What happens:**
  1. `c->buffer.remove(0, nl + 1)` shifts the remaining `QByteArray` buffer on every newline (`O(N^2)` when multiple commands arrive in one read).
  2. Every RPC line is parsed **twice** (`Json::parse(line)` in `Engine.cpp:240` and `Json::parse(line)` in `RpcServer.cpp:60`), and the serialized reply string is re-parsed a **third** time (`Json::parse(reply)` in `Engine.cpp:250`) just to log the result.

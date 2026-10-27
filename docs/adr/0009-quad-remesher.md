# 0009. Quad remesher: Instant Meshes for M2

- Status: Accepted
- Date: 2026-10-17

## Context
The surface build fits a Catmull-Clark control cage to the scan (ADR-015), so it needs a quad mesh of the scan with few extraordinary vertices: every extraordinary vertex becomes a cap patch, and caps are where gaps and kinks come from (section 13). The architecture named QuadWild (bi-MDF variant) as the front-runner and Instant Meshes as the baseline, to be settled by a spike on three golden scans (issue #14).

Spike set-up: the DJI controller, Dewalt multitool and Tyranid scans, each run through its golden session (shell clean-up, repair, hole filling, principal alignment) and simplified to 100,000 triangles. Distances are from 200,000 points sampled on the scan to the nearest point on the quad mesh. Run on Linux, one thread, deterministic mode.

## Results

### Instant Meshes (commit `7b31608`, pure-quad output)

| Scan (genus) | Target faces | Quads out | Extraordinary vertices | Open loops | Components | Non-manifold edges | Genus after closing the loops | Scan to quads, 95th pct / max (mm) | Time |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Controller (1) | 1,000 | 3,935 | 155 (3.9%) | 1 | 1 | 0 | 0 | 0.54 / 4.40 | 1.6 s |
| Controller (1) | 4,000 | 16,016 | 384 (2.4%) | 0 | 2 | 0 | not a valid surface | 0.19 / 2.87 | 2.2 s |
| Multitool (105) | 1,000 | 3,652 | 357 (9.8%) | 3 | 1 | 1 | 0 | 1.86 / 9.57 | 1.4 s |
| Multitool (105) | 4,000 | 15,466 | 883 (5.7%) | 6 | 1 | 0 | not a valid surface | 0.51 / 3.72 | 1.9 s |
| Tyranid (17) | 1,000 | 3,577 | 595 (16.6%) | 17 | 9 | 0 | not a valid surface | 2.39 / 9.40 | 1.5 s |
| Tyranid (17) | 4,000 | 15,588 | 1,361 (8.7%) | 15 | 3 | 0 | 14 | 0.29 / 4.46 | 1.9 s |

"Not a valid surface" means the Euler characteristic gives a fractional genus: the output has a non-manifold vertex or inconsistently oriented faces.

Pure-quad output has about four times the target face count, because Instant Meshes extracts a quad-dominant mesh at the target size and splits it once. Quads are well shaped (5th percentile corner angle 55 to 82 degrees). The output is not a usable cage as it comes: it leaves small holes where it refuses to close a polygon of degree 9 or more, small loose fragments, a few valence-2 vertices and, sometimes, non-manifold or flipped spots; and it closes handles smaller than the target edge length (the multitool's 105 handles collapse to a few). So the adapter must clean the result into a closed manifold cage (keep the main component, fill each open loop (with one polygon when it is small, otherwise with pieces of at most five sides: OpenSubdiv's patch setup grows with about the cube of a face's sides, so one polygon across a 1,800-edge hole took hours), dissolve valence-2 vertices, repair or reject non-manifold spots) and the face target must be fine enough for the features that matter.

### QuadWild bi-MDF (commit `e722c7e`)
Not run. Its build pulls twelve git submodules (VCGlib, libigl, OpenMesh, CoMISo, LEMON, libsatsuma, the xfield tracer and others); OpenMesh, CoMISo and the CMake library come from `graphics.rwth-aachen.de:9000`, a non-standard port the cloud build environment cannot reach, so the spike could not build it. It is also an application with a file-based multi-step pipeline (remesh, field, trace, quantise), not a library, so wrapping it means patching or driving its executable. Licences: QuadWild and VCGlib are GPL-3.0, compatible; Blossom V, which libsatsuma can use, is research-only, but it is off by default (`SATSUMA_ENABLE_BLOSSOM5=OFF`), and the original QuadWild's Gurobi dependency is replaced by the bi-MDF solver.

### Licences (Instant Meshes)
Instant Meshes is BSD-3-Clause; the parts it needs are pcg32 (Apache-2.0), dset (zlib) and parallel stable sort (BSD-3-Clause, Intel). All are compatible with GPL-3.0-or-later. Its GUI (nanogui, GLFW) and bundled TBB are not used.

## Decision
- Use Instant Meshes for the M2 quad layout. The adapter (`src/adapters/quad`) compiles only its field-optimisation and extraction sources, fetched at a pinned commit, with a small serial stand-in for the TBB API it uses, so it runs single-threaded and is deterministic (principle 6) and adds no TBB dependency.
- The adapter cleans the output into a closed, manifold, consistently oriented cage and fails with a clear error when it cannot.
- QuadWild stays the candidate to revisit if the M3 accuracy work shows that the cage layout, not the fitting, limits the deviation or the face count.

## Alternatives considered
- QuadWild bi-MDF: better patch layouts in its paper, but it could not be built here, needs many more dependencies, and is an application rather than a library. Revisit as above.
- A Catmull-Clark split of a simplified triangle mesh (every triangle into three quads): always valid and closed, but every patch touches an extraordinary vertex, so the cap count and face count would be several times higher.

## Consequences
- The cage loses handles smaller than about one target edge length, so the solid can have a different genus from the scan. The deviation map shows where; finer targets or M3's manual layout fix it.
- Extraordinary vertices are 2 to 17% of cage vertices on these scans, so caps are a large share of the patches until M3 merges patches.
- At the M3 gate: check whether layout quality limits accuracy and, if so, retry QuadWild on a machine that can reach its submodules.

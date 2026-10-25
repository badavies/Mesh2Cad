# Test meshes

Synthetic meshes made with Blender by [`scripts/blender/make_test_meshes.py`](../../scripts/blender/make_test_meshes.py). Each one has a defect or a ground truth known by construction, aimed at a group of commands. They are small stand-ins until the real scans of the golden set arrive (`docs/ARCHITECTURE.md` section 10); they are not golden scans and have no stored thresholds.

Units are millimetres. [`manifest.json`](manifest.json) records, for every mesh, what it is for, the construction facts (dimensions, defects, poses with the `align.transform` parameters that undo them, seed faces for selections) and statistics measured by reading each file back and welding it the way the importers do. The `measured` values agree with `mesh.info` in the app.

Each mesh has a `<name>.jsonl` session for `mesh2cad batch` that runs its commands. Paths in the sessions are relative to this folder, so run them from here:

```
cd examples/test-meshes
../../build/linux-gcc-debug/src/cli/mesh2cad batch bracket.jsonl
```

| Mesh | Faces | Files | Exercises | What the session shows |
| --- | ---: | --- | --- | --- |
| `bracket` | 284 | stl obj ply | `mesh.pick`, `select.crease`, `select.grow`, `plane.fit`, `measure.distance`, `mesh.remesh` | L-bracket 60 x 40 x 40 mm, 6 mm thick, 10 mm and 8 mm holes, watertight, volume 21792.12 mm³. Crease fill from the base top selects its 36 faces, plane fit gives normal +Z at z = 6 with zero RMS, the hole wall is 64 faces, the thickness measures 6.000 mm. |
| `bracket_misaligned` | 284 | stl obj ply | `align.principal`, `align.transform`, `align.reset`, `align.datum` | The bracket moved by a known pose (`pose` in the manifest). The stored undo transform, and 3-2-1 datum alignment on the bottom, front and back faces, both bring the bounds back to exactly [0, 60] x [0, 40] x [0, 40]. |
| `bracket_scan` | 19,828 | ply | `select.crease` with noise, `plane.fit`, `align.datum`, `select.brush`, `mesh.smooth`, `mesh.simplify` | Voxel-remeshed (1 mm) bracket with 0.03 mm normal noise and another pose. Plane fit RMS is about 0.03 mm (the noise), datum alignment lands within 0.1 mm of the design bounds. |
| `sphere_noisy` | 20,480 | ply | `mesh.smooth`, `select.brush/grow/shrink/invert`, `mesh.simplify`, `mesh.remesh` | Radius 25 mm with 0.15 mm Gaussian radial noise. Smoothing pulls the bounds in towards ±25 without shrinking the volume. |
| `cylinder_holes` | 2,318 | stl obj ply | `holes.fill` | Four open boundaries: wall holes of 4, 8 and 14 edges and a 48-edge opening where the top cap was removed. `max_edges` 20 fills three and skips the top; the default (up to 100 edges) fills all four. |
| `torus_shells` | 2,745 | stl obj ply | `shells.detect/select/split/clean/keep`, `select.shell` | Torus (2304 faces, the main part) plus six fragments: icospheres of 80, 20 and 320 faces, a single triangle, a two-triangle quad and an open 18-face grid. `shells.clean` with `min_faces` 100 keeps the torus and the 320-face sphere. |
| `box_defects` | 212 | stl obj ply | `mesh.repair`, `mesh.info`, `shells.detect` | A box with four flipped faces, a fin on a non-manifold edge, a tetrahedron touching at one vertex, an exact and a reversed duplicate face, a zero-area face and a cube pushed through the top (list in the manifest). Before repair: 9 non-manifold edges, 1 degenerate face, 3 shells. |
| `sphere_inside_out` | 1,280 | stl obj ply | `mesh.flip`, `mesh.repair` | Closed sphere with every face reversed: negative volume until flipped or repaired. |
| `soup_damaged` | 7,833 | ply | `mesh.wrap` | Two intersecting spheres and a rod, unjoined, with 25% of faces deleted, 2% flipped and 0.1 mm jitter (188 shells, 5797 boundary edges). `mesh.wrap` with `alpha_mm` 2 returns one watertight object. |
| `controller` | 12,100 | ply | `align.principal`, `mesh.remesh`, `mesh.simplify`, `measure.distance` | Smooth organic body, about 145 x 75 x 40 mm, grips and thumbstick bumps, no flat faces: a stand-in for the v1 target scan. |

The repository keeps only the PLY of meshes over 5000 faces (`--compact`). Running the script without `--compact` writes all three formats of every mesh, and `--large` adds `organic_large`, a 1.3M-face bumpy sphere for timing and memory, which is too big to commit.

Behaviour worth knowing about: `mesh.repair` on `box_defects` removes the duplicate and degenerate faces and the fin, and splits the bowtie vertex, leaving a watertight mesh. It still reports `self_intersecting` (the cube pushed through the top) until `remove_self_intersections` unites the cube with the box (volume 6117.33 mm³: box, the 108 mm³ of cube above the top and the tetrahedron). `holes.fill` with its default limits on `cylinder_holes` closes the top with a flat cap at the rim: the walls meet the opening at a right angle, so the patch is not faired into a dome. `mesh.simplify` takes `organic_large` from 1.3M to 200k faces in seconds.

## Regenerating

With Blender 4.2 or later (the files here were made with the version named in the manifest):

```
blender --background --factory-startup --python scripts/blender/make_test_meshes.py -- --out examples/test-meshes --compact
```

or with the `bpy` module (`pip install bpy`, which needs the Python version that bpy release was built for):

```
python3 scripts/blender/make_test_meshes.py --out examples/test-meshes --compact
```

Seeds are fixed and element order is canonicalised after Blender's multithreaded boolean and remesh, so two runs with the same Blender and NumPy write identical files. A different Blender version may change the meshes; check the manifest diff before committing regenerated files.

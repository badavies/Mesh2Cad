# Fusion 360 import checklist

The manual export check for each milestone gate (ARCHITECTURE.md section 7, Export acceptance,
and section 10, Manual). The test suite already re-imports every STEP file it writes with Open
CASCADE; this checklist covers what only Fusion 360 can show. Run it on every golden scan's STEP
file and record the result in the gate's pull request or issue.

## Making the files

For each golden scan (`tests/golden/README.md`):

```
mesh.import       {"path": "<scan>"}
quad.remesh       {"id": 1, "target_faces": 2000}
surface.build     {"id": 2, "mesh_id": 1}
solid.export      {"id": 3, "path": "<scan>.step"}
```

For the DJI controller, `ctest -L golden` runs this session (with the M1 repair and alignment
first) and leaves the file at `tests/golden/dji_controller_3_surface/scan.step`, with the
deviation report beside it as `scan.deviation.csv`.

Note from the commands' results: `surface.build` `summary` (face count, volume, `valid`,
`closed`), `seams.max_normal_angle_deg` and `fit.rms_distance_mm`, and `solid.export`
`check.passed`. Do not continue with a file whose `check.passed` is false.

## In Fusion 360

Open the file with **File > Open > Open from my computer** (or upload it to the Data Panel) and
open the translated design. Do not use **Insert > Insert Mesh**, which reads meshes, not solids.

| # | Check | Pass when |
|---|---|---|
| 1 | The translation finishes | No translation error in the Data Panel; the design opens. |
| 2 | Browser tree | Exactly one **Body** under Bodies, and no Surface bodies or extra components (for a file of the solid alone; see below for sketches and planes). |
| 3 | Solid, not surfaces | The body's icon is the solid icon; **Inspect > Properties** shows a volume. |
| 4 | Volume | The volume in Properties matches `summary.volume_mm3` within 0.1%. |
| 5 | Units and size | **Inspect > Measure** across the part gives the scan's size in mm (not inches or metres). |
| 6 | Position | The part sits where the aligned scan was; origin planes are where alignment put them. |
| 7 | Faces | Selecting faces works; the face count is in the hundreds to low thousands, as `summary.face_count`. |
| 8 | Smoothness | **Inspect > Zebra Analysis**: stripes run on across face boundaries with no visible breaks except at the caps, where kinks stay under `seams.max_normal_angle_deg`. |
| 9 | Editing | A **Split Body** with an origin plane and a **Shell** of 1 mm both succeed. |
| 10 | Section | **Inspect > Section Analysis** on an origin plane shows a closed, filled section. |
| 11 | Responsiveness | Orbit, select and the timeline stay usable (no multi-second pauses). |

## Sketches and planes

When `solid.export` was given `sketch_ids` or `plane_ids` (ADR-034), also check:

| # | Check | Pass when |
|---|---|---|
| 12 | Components | One component per exported object, each named after it (the solid's under `product_name` if given). |
| 13 | Sketch | Each sketch's curves come through as sketch curves (or, if Fusion shows them as 3D curves, as edges that **Project** into a sketch), closed, with arcs as arcs, where the section was cut. |
| 14 | Plane | Each plane is a surface body lying on the fitted face; **Construct > Plane at Angle** or **Offset Plane** on it gives a construction plane, and a sketch can be started on it. |
| 15 | Solid | The solid still passes checks 3 to 6. |

## Recording the result

For each file give: the scan, the Mesh2Cad commit, the Fusion 360 version, the face count and
volume Fusion reports, pass or fail for each numbered check, and a screenshot of the zebra
analysis. A milestone gate passes only when every golden file passes checks 1 to 6 and 10; a
failure of 7 to 9 or 11 is recorded as an issue against the surface build.

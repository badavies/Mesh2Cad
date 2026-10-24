# Organic parts

An organic part, such as a controller, a grip or a figure, is rebuilt in two steps. First a quad cage is laid over the scan. Then a smooth surface is fitted through the cage and turned into B-spline faces, and those faces are sewn into one solid. The cage keeps the surface continuous from face to face, so Fusion 360 opens the result as a single solid body.

Start from a prepared mesh: watertight and aligned ([Mesh preparation](mesh-preparation.md)).

## Quad layout

`quad.remesh` makes the quad cage. `target_faces` sets roughly how many quads it has: more quads follow the scan more closely, but give the solid more faces. 2,000 is a good start for a hand-sized part. The cage shows as orange edges over the mesh.

- `crease_angle_deg` lines cage edges up with the part's sharp edges (edges sharper than that angle). To keep those edges sharp, give `surface.build` the same `crease_angle_deg`.
- `guides` are polylines drawn on the mesh. Cage edges follow them, which lets you lay out the patches by hand where the automatic layout puts them badly.
- Thin parts the remesher misses, such as the head of a post, are joined back on with a tube of quads (`join_parts`), or left out and counted when they can't be joined.
- For a one-sided scan of a sheet, such as a lid, `keep_outline` leaves the outer boundary open. `surface.build` then needs `thickness_mm` to turn the fitted sheet into a wall.

## Surface build

`surface.build` fits the cage to the scan and makes the solid. The defaults suit most scans. The ones worth knowing:

| Parameter | Use |
| --- | --- |
| `refine_to_mm` | Stop only when 95% of the scan is within this distance. If the fit isn't that close, the fitted cage is subdivided (four times the quads) and fitted again, up to `max_refinements` times. |
| `smoothing`, `fairing` | How hard the surface resists the scan's noise. Higher values give calmer zebra stripes, but sit a little farther from the scan. |
| `crease_angle_deg` | Cage edges that turn more than this stay sharp in the solid. Off (0) by default, so the surface is smooth everywhere. Set it to the angle `quad.remesh` used. |
| `flat` | Areas held exactly flat: each is a selection and a plane fitted to it with `primitive.fit`. |
| `merge` | Merge rectangles of cage quads into larger faces. This gives far fewer faces, and the surface is the same. |
| `check_crossing` | Check that the surface doesn't cross itself. This can take up to a minute, so turn it off for quick trials. |

The result reports how close the fit is (`fit`), how well the faces meet (`seams`), the face count and volume (`summary`) and how evenly the surface bends (`fairness`). A surface that would fold over itself is fitted again with more smoothing, up to four times. If it still folds, the build fails and adds nothing.

## Deviation map and report

`deviation.compute` measures the signed distance from every point of the scan to the solid. Distances are positive outside the solid. In the app the mesh is coloured by the result, with a legend of the colours and statistics. **View > Colour-blind-safe deviation colours** changes the palette. The result gives the RMS, the 95th percentile and the share of the scan within `tolerance_mm`. It also lists hotspots, the patches out of tolerance, largest first. `select_hotspots` makes each hotspot a selection. `deviation.report` writes the statistics and a histogram to a CSV file.

```jsonl
{"command": "mesh.import", "params": {"path": "controller.ply"}}
{"command": "align.principal", "params": {"id": 1, "origin": "bounds_bottom_center"}}
{"command": "quad.remesh", "params": {"id": 1, "target_faces": 2000}}
{"command": "surface.build", "params": {"id": 2, "mesh_id": 1}}
{"command": "solid.info", "params": {"id": 3}}
{"command": "deviation.compute", "params": {"id": 1, "reference_id": 3, "tolerance_mm": 0.1}}
{"command": "deviation.report", "params": {"id": 4, "path": "controller-deviation.csv"}}
{"command": "solid.export", "params": {"id": 3, "path": "controller.step"}}
```

This makes a solid of about 180 faces from the controller scan in a few seconds, then writes the deviation report and a STEP file.

## Getting a better surface

- **Too far from the scan:** raise `target_faces`, or set `refine_to_mm` (for example 0.1).
- **Wavy zebra stripes:** raise `fairing` or `smoothing` a little, and compare `fairness` and the deviation from one build to the next.
- **Flat areas come out slightly curved:** select them, fit a plane with `primitive.fit`, and pass the pair in `flat`.
- **Sharp edges come out rounded:** set `crease_angle_deg` (for example 60) in `quad.remesh`, so the cage follows those edges, and the same in `surface.build`, so they stay sharp.
- **Holes should be true cylinders:** fit cylinders to the hole walls and cut them from the solid with `solid.boolean` (see [Prismatic and hybrid parts](prismatic-parts.md)).

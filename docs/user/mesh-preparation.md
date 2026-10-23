# Mesh preparation

Surfacing needs a closed, clean mesh that sits square to the origin. Scans rarely arrive like that. Work through the steps below in order, and check `mesh.info` (or the object's **Info**) as you go. The **Workflow** panel in the app lists the same steps.

## Repair

`mesh.repair` merges duplicate vertices and removes degenerate faces, duplicate faces and fins on non-manifold edges. It also splits non-manifold vertices and turns every face the same way, outward if the mesh is closed. Holes stay open: filling them is a separate step. If `mesh.info` still reports `self_intersecting` after a repair, run it again with `remove_self_intersections`. That unites the overlapping parts.

`shells.detect` lists the separate parts of a mesh (its shells), largest first.

```jsonl
{"command": "mesh.import", "params": {"path": "box_defects.ply"}}
{"command": "mesh.info", "params": {"id": 1}}
{"command": "mesh.repair", "params": {"id": 1}}
{"command": "shells.detect", "params": {"id": 1}}
```

Before the repair this box has 3 boundary edges and non-manifold edges. Afterwards it is watertight, and `shells.detect` lists the box and two small loose parts.

## Holes

`holes.fill` closes open boundaries with patches that blend into the surrounding surface. `max_edges` and `max_perimeter_mm` leave larger openings alone, so a real opening in the part stays open. In this example the three small holes in the wall are filled, and the 48-edge opening at the top is skipped:

```jsonl
{"command": "mesh.import", "params": {"path": "cylinder_holes.ply"}}
{"command": "holes.fill", "params": {"id": 1, "max_edges": 20}}
```

## Loose parts

Scans often pick up specks and fragments. `shells.clean` deletes every shell below a face count or area, and never deletes the main part. `shells.keep` keeps only the main part. `shells.split` moves each loose part into its own object, so you can look at them before deleting.

```jsonl
{"command": "mesh.import", "params": {"path": "torus_shells.ply"}}
{"command": "shells.clean", "params": {"id": 1, "min_faces": 100}}
```

## Smoothing and simplifying

`mesh.smooth` uses Taubin smoothing, which doesn't shrink the part, on the whole mesh or on a selection. With `preserve_sharp_edges`, edges sharper than `sharp_angle_deg` are kept. `mesh.simplify` reduces the triangle count to `target_faces` (or `target_ratio`), or until the error would pass `max_error_mm`. `mesh.remesh` rebuilds an even mesh at `target_edge_mm` and keeps sharp edges. Segmentation needs an even mesh like this (see [Prismatic parts](prismatic-parts.md)).

```jsonl
{"command": "mesh.import", "params": {"path": "sphere_noisy.ply"}}
{"command": "mesh.smooth", "params": {"id": 1, "iterations": 10}}
{"command": "mesh.simplify", "params": {"id": 1, "target_faces": 5000}}
```

You don't need to smooth for surfacing: `surface.build` fits through the noise. Smooth only when the noise is worse than the detail you want to keep.

## Badly damaged scans

When repair can't close a mesh (scattered faces, many shells, large gaps), `mesh.wrap` builds a new watertight mesh around it. Detail smaller than `alpha_mm` is lost, so pick the largest value that keeps the features you care about.

```jsonl
{"command": "mesh.import", "params": {"path": "soup_damaged.ply"}}
{"command": "mesh.wrap", "params": {"id": 1, "alpha_mm": 2}}
```

## Aligning

Scans come in at whatever angle the scanner left them. Line the part up before surfacing, so its faces sit on the origin planes in your CAD program:

- `align.principal` lines the part's longest axis up with X and its shortest with Z. The origin goes at the centroid, the centre of the bounding box, or the bottom centre of the box (`bounds_bottom_center`).
- `align.symmetry` puts the part's best mirror plane at X = 0. It reports how good the symmetry is.
- `align.datum` is 3-2-1 alignment: a primary selection (for example the base face) becomes Z = 0, a secondary one Y = 0 and a tertiary one X = 0.
- `align.transform` applies a rotation and translation you give it, and `align.reset` returns the mesh to the pose it was scanned in.

```jsonl
{"command": "mesh.import", "params": {"path": "bracket_misaligned.stl"}}
{"command": "align.principal", "params": {"id": 1, "origin": "bounds_bottom_center"}}
```

## Selecting and measuring

Most tools work on a selection of faces. In the app, use Brush (2), Lasso (3) or Crease (4), and **Edit > Grow selection** or **Edit > Invert selection**. As commands, use `select.brush`, `select.lasso`, `select.crease`, `select.grow`, `select.shrink`, `select.invert`, `select.all` and `select.shell`. `mesh.pick` finds the face under a ray, which gives you a starting face for `select.crease`.

`measure.distance` measures between two points and saves the result as a measurement object. A point given with a mesh id snaps to that mesh. `plane.fit` fits a plane to a selection, and reports its normal and how flat the selection is.

```jsonl
{"command": "mesh.import", "params": {"path": "bracket.stl"}}
{"command": "mesh.pick", "params": {"origin": [45, 30, 50], "direction": [0, 0, -1]}}
{"command": "select.crease", "params": {"id": 1, "face": 39, "name": "base top"}}
{"command": "plane.fit", "params": {"selection_id": 2}}
{"command": "measure.distance", "params": {"a": {"point": [30, 20, 0], "id": 1}, "b": {"point": [30, 20, 6], "id": 1}, "name": "thickness"}}
```

The pick lands on face 39 of the bracket's base. The crease fill selects the top of the base, which fits a plane at z = 6, and the base measures 6.000 mm thick.

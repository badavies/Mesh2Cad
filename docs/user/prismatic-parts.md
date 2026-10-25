# Prismatic and hybrid parts

A machined part, such as a bracket, a housing or a plate, is made of planes, cylinders, cones and spheres meeting at sharp edges. Mesh2Cad rebuilds each of those faces as the exact surface, so a hole comes out as a true cylinder and a face as a true plane. Neighbouring surfaces are trimmed against each other, so edges come out sharp. No quad cage is used.

## Segmenting the scan

`mesh.segment` splits the mesh into the regions that fit a primitive (planes and cylinders by default, and spheres too if you add them to `kinds`), and leaves the rest as one organic region. Each region becomes a selection. With `create_primitives`, each region also gets its fitted primitive, drawn in green. Run it on an even mesh: use `mesh.remesh` first, because the normals of a raw scan's small triangles are too noisy. `covered_area_fraction` in the result tells you which route suits the part. A value close to 1 means the part is all primitives (trim them, below). A low value means it is mostly organic (surface it, then add the primitives with booleans).

## Trimming primitives into a solid

`primitive.trim` bounds each primitive by the scan's box, splits them all against each other, keeps the split faces that lie on the scan, and sews those faces into a solid. If a primitive is missing, the solid is open there, and the command fails with a message saying where.

```jsonl
{"command": "mesh.import", "params": {"path": "bracket.stl"}}
{"command": "mesh.remesh", "params": {"id": 1, "target_edge_mm": 2}}
{"command": "mesh.segment", "params": {"id": 1, "create_primitives": true}}
{"command": "primitive.trim", "params": {"primitive_ids": [3, 5, 7, 9, 11, 13, 15, 17, 19, 21], "mesh_id": 1}}
{"command": "deviation.compute", "params": {"id": 1, "reference_id": 23}}
{"command": "solid.export", "params": {"id": 23, "path": "bracket.step"}}
```

Segmentation finds the bracket's eight planes and its two holes. That gives primitives 3 to 21, with each region's selection numbered just before its primitive, plus selection 22 for the organic rest. The trimmed solid, 23, has ten exact faces.

## Fitting primitives by hand

When segmentation misses a region, or you only need a few faces, select the region and use `primitive.fit`. With `kind` set to `auto`, it picks the simplest surface that fits about as well as any. It fails if 95% of the region isn't within `max_residual_mm`. `primitive.align` then makes primitives exactly parallel, perpendicular or at a set angle to each other or to a world axis, and refits them. A scanned face is always a little off, and this corrects it.

`primitive.box` fits a whole box to a selection in one step: six planes, at right angles. It can also make a box from a size, a centre and axes, or snap a fitted box to nominal sizes.

## Fillets

`fillet.fit` rounds an edge of a solid at the radius the scan shows. Give it a point near the edge (`edge_points`) and a selection of the fillet band on the mesh. It tries radii and keeps the one closest to the scan. You can also give `radius_mm` to round at a nominal value.

## Sketches, extrude and revolve

`sketch.section` cuts the mesh with a plane and fits lines, arcs and splines to the cut, as a sketch. The plane can be an origin plane, a fitted plane primitive, or a point and a normal, moved by `offset_mm`. In the app, the Section sketch form shows its plane in the view as a see-through square, moving as you change the plane or the offset. `solid.extrude` pushes the sketch's outer loop, less any loops inside it, along the plane's normal: `one_sided`, `symmetric` or `two_sided`. `solid.revolve` turns it about an axis in the sketch plane. Lines and arcs stay exact. Sketches are fitted, not constrained.

## Booleans

`solid.boolean` fuses, cuts or intersects two solids. `primitive.solid` turns a fitted cylinder or sphere into a solid you can cut with. Give it `radius_mm` to drill a hole clean at a nominal size. On an organic body this is how you get true-cylinder holes: surface the body, fit cylinders to the hole walls, and cut them out.

This session rebuilds the bracket from a section and its two holes:

```jsonl
{"command": "mesh.import", "params": {"path": "bracket.stl"}}
{"command": "mesh.remesh", "params": {"id": 1, "target_edge_mm": 2}}
{"command": "mesh.segment", "params": {"id": 1, "create_primitives": true}}
{"command": "sketch.section", "params": {"id": 1, "plane": "zx", "offset_mm": 5}}
{"command": "solid.extrude", "params": {"id": 23, "distance_mm": 35, "extent": "two_sided", "back_distance_mm": 5}}
{"command": "primitive.solid", "params": {"id": 19, "length_mm": 100}}
{"command": "solid.boolean", "params": {"id": 24, "tool_id": 25, "operation": "cut"}}
{"command": "primitive.solid", "params": {"id": 21, "length_mm": 100}}
{"command": "solid.boolean", "params": {"id": 26, "tool_id": 27, "operation": "cut"}}
{"command": "solid.export", "params": {"id": 28, "path": "bracket-rebuilt.step"}}
```

The section 5 mm in from the bracket's side is its L-shaped profile: six exact lines. Extruding it 35 mm one way and 5 mm the other spans the bracket's 40 mm depth. The two cylinders fitted to the holes are made into 100 mm long pins and cut through.

## Export with sketches and planes

`solid.export` can also write sketches (as curves Fusion 360 imports as a sketch) and fitted planes (as faces you can sketch on) beside the solid, through `sketch_ids` and `plane_ids`. See [Export](export.md).

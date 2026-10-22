# Troubleshooting

When a command fails, it changes nothing: the document stays as it was and no file is written. The message says what went wrong. The common ones are below, roughly in pipeline order.

| Message (start) | What it means | What to do |
| --- | --- | --- |
| `unsupported mesh file type` | The file isn't STL, OBJ or PLY. | Convert it to one of those, or export it again from the scanner software. |
| `mesh.info` shows boundary or non-manifold edges | The scan has holes or bad topology. | Run `mesh.repair`, then `holes.fill`. If it is still open, use `mesh.wrap`. |
| `self_intersecting` stays true after a repair | Parts of the mesh pass through each other. | Run `mesh.repair` with `remove_self_intersections`. |
| `the quad layout could not be closed` | The remesher left holes or non-manifold edges it could not close. This usually comes from a mesh that isn't watertight or has thin spikes. | Repair and fill the mesh first. Try a different `target_faces`, or smooth spiky areas. |
| `an open cage needs a thickness` | The cage was made with `keep_outline` (a one-sided sheet). | Give `surface.build` a `thickness_mm`. |
| `the surface folds over itself: faces meet at ...` | The fitted surface turns back on itself, usually at thin or very sharp features the cage is too coarse to follow. | Raise `target_faces`, or set `crease_angle_deg` in `quad.remesh` for sharp edges. Raise `smoothing` a little. |
| `the surface's faces cross each other` | The fit overshoots an edge sharper than the cage can follow. | As above. `check_crossing` set to false skips the check for quick trials, but don't export such a solid. |
| `the trimmed faces leave the solid open between ...` | `primitive.trim` is missing a surface at the place named. | Fit a primitive to that region (select it and use `primitive.fit`) and trim again, or lower `min_coverage` if a face is only partly scanned. |
| `the trimmed faces make N shells instead of one` | The primitives enclose more than one piece. | Leave out the primitive that cuts the part in two, or check its fit. |
| `the cylinder fits with a P95 residual of ... mm, above the limit of ... mm` (from `primitive.fit`) | The selection isn't the kind of surface asked for, or it reaches over an edge. | Shrink the selection away from the edges (`select.shrink`), or set `kind` to `auto`. |
| `every loop but the largest must be a hole in it` (from `solid.extrude`) | The section cut through a hole or across separate pieces. | Move the section plane (`offset_mm`) so the cut gives one outer loop. |
| `... is not closed (... open and ... non-manifold edges); nothing was written` (from `solid.export.mesh`) | The triangulation didn't close, usually because the solid has gaps where faces meet. | Check the solid with `solid.info` (`free_edges` should be 0), then try a smaller `chord_mm`. |
| `cancelled` | You pressed **Cancel**, or a script or agent cancelled the command. | Run it again when you are ready. Nothing was changed. |

## The solid looks wrong in Fusion 360

Check that `solid.export` reported `check.passed: true`. Open the file with **File > Open**, not **Insert Mesh**. If the zebra stripes are wavy, see "Getting a better surface" in [Organic parts](organic-parts.md).

## The part is the wrong size or the wrong way up in Blender

Import the `.glb` with Blender's own glTF importer and its default options. glTF is always in metres with Y up, and the importer converts that back to Z up, so a 143 mm part shows as 0.143 m. An `.obj` or `.stl` is in millimetres, so it comes in a thousand times larger unless you scale it on import.

## Slow commands

Long commands show their stage in the status bar, and can be cancelled. Surface building on a large cage takes the longest. For trials, use a smaller `target_faces` and turn off `check_crossing`. Deviation and the cage fit use all your processor cores.

## Reporting a problem

Save the project (`.m2c`) and note the command and its parameters. **History** lists every step. The session log (see **Help > Connect an AI agent**) has the steps in a form `mesh2cad batch` can replay.

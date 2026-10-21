# 0032. Flat areas are held on their plane inside the cage fit

- Status: Accepted
- Date: 2026-10-21

## Context
Manufactured parts have flat faces, but a scan of one is never flat: it has noise, dents and print texture. Surface build fits the Catmull-Clark cage to every scan point, so the solid follows those defects. Primitive fitting and trimming (`primitive.fit`, `primitive.trim`) make exact flat faces, but only for parts built entirely from primitives with sharp edges between them; a flat face that blends into an organic shape has no way through.

## Decision
- `fitCage` takes held planes (`CageFitParams::held`): each is a plane and a list of cage vertices. Each solve adds a stiff term (weight 10 times the samples per vertex) pulling those vertices onto their planes, and the vertices are then projected onto them exactly; their start positions are projected too. Where all of a patch's control points lie on one plane, its Catmull-Clark limit surface is that plane.
- The pipeline names the held vertices from a flat area, a selection of the scan and a fitted plane (`FlatArea`): those whose closest scan face is in the selection. This is worked out again for every cage fitted, so it survives refinement and retries.
- `surface.build` takes `flat`, a list of selection and plane primitive ids, and reports the held vertices per area. Nothing else changes when the list is empty.

## Alternatives considered
- Trim the flat face as a primitive and boolean it with the organic body: an exact plane, but a sharp edge where the face meets the rest, and the boolean is far less reliable than one fitted surface.
- Drop or down-weight the scan points over the area: the surface becomes smoother but not flat, and the plane is lost.
- Hard equality constraints in the solve (a KKT system): exact without the projection, but indefinite, so the solver would change from `SimplicialLDLT` for every fit.

## Consequences
- The surface is exactly flat over the patches whose vertices are all held, and blends tangent-continuously into the rest over one ring of patches at the area's edge; the selection should reach a little beyond the part that must be flat.
- A vertex held by two areas goes to (near) the line where their planes meet; this is not a fillet, which is ADR work for issue #85.
- The same mechanism could hold vertices on other primitives later (a cylinder, a sphere) by adding a held-surface kind.

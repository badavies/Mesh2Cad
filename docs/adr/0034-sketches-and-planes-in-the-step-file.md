# 0034. Sketches and planes go into the STEP file as named products beside the solid

- Status: Accepted
- Date: 2026-10-21

## Context
`solid.export` writes one solid per file (section 7, Export). Section sketches (`sketch.section`) and fitted planes (`primitive.fit`) never reach Fusion 360, so a user who wants to model on top of the scan there, for example to extrude a feature from a section or sketch on a datum face, has to redraw them by hand. STEP AP242 can carry wireframe curves and open surfaces beside solids, and Fusion imports STEP curves as sketch curves and open surfaces as surface bodies. STEP has no entity for a construction plane.

## Decision
- `solid.export` takes optional `sketch_ids` and `plane_ids` beside the solid's `id`, and the solid becomes optional: a file of sketches and planes alone is allowed.
- Each object is its own top-level product, named after the object (Open CASCADE's `<name> <n>` numbering is replaced by the name itself), written in order: solid, sketches, planes. Assembly splitting is off, so a sketch of several loops is one product.
- A sketch is written as one wire per loop, with lines and arcs exact and splines interpolated as `solid.extrude` builds them (a geometric curve set in the file).
- A plane primitive is written as one rectangular planar face: the plane bounded, along its own axes, by the projection of its fit's extent. In Fusion it is a surface body to sketch on or to make a construction plane from. Other primitive kinds are refused.
- The readback check reads the solids alone, as before (one valid closed solid, volume within 0.1%, or none when no solid was given), and also counts the faces outside any solid and the edges on no face, which must equal what was written. The result reports both counts.

## Alternatives considered
- One compound of everything: one product, so Fusion names nothing, and the solid shares a body with the planes.
- A separate command for sketches and planes: a second STEP path doing the same work, and two files to import in Fusion.
- Planes as construction geometry through XDE (`STEPCAFControl`): Open CASCADE's application framework is not in the build (ADR-026), and STEP still has no construction-plane entity for Fusion to read.
- Planes clipped exactly to the display mesh's outline: matches the viewport, but a rectangle is easier to pick in Fusion and a construction plane made from it is the same.

## Consequences
- A section or a datum plane arrives in Fusion where it was in Mesh2Cad, in the aligned frame, with its object's name.
- A file with sketches or planes opens in Fusion with one component per product; `docs/FUSION_CHECKLIST.md` check 2 (one body, no surface bodies) applies to solid-only files, and new checks cover sketches and planes.
- Product names in every STEP file are now the object's name (or `product_name`) without Open CASCADE's trailing number.
- Tests: a solid, a two-loop sketch and a plane written to one file and read back (one solid with the same volume, five loose edges, one loose face, three products with their names), the failures, and the command with and without a solid.

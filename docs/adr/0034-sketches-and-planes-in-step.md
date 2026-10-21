# 0034. Sketches and planes go to STEP as flat faces, each body a named product

- Status: Accepted
- Date: 2026-10-21

## Context
Section sketches and fitted planes are the references a person needs to finish a scanned part in Fusion 360: a sketch to extrude or revolve again, a plane to sketch on or square a feature to. Only solids could be exported (`solid.export`, one solid per file).

STEP can carry loose curves, but Autodesk's help says Fusion's STEP import brings in only faces and solids: "points, wires, trims, curves, sketches and drawings" are left out, and it suggests replacing a curve with a small face. Construction planes are Fusion's own objects and have no STEP equivalent Fusion reads.

## Decision
- A new command, `step.export`, writes any mix of solids, sketches and fitted planes to one STEP AP242 file, in the order given.
- A sketch is written as planar faces bounded by its loops: every loop not inside another bounds a face, the loops directly inside it are its holes, and a loop inside a hole bounds a face of its own. Lines and arcs stay exact (STEP lines and circles); a spline is interpolated through its points. Fusion imports it as a surface body; sketching on it and projecting its edges gives the curves back as an exact Fusion sketch.
- A fitted plane is written as a flat rectangle on the plane over the region it was fitted to, grown by 1% of the region's diagonal as the app draws it (`planeOutline`). Fusion imports it as a surface body to sketch on or build construction planes from.
- Each body is a product of its own, named exactly after its object. Open CASCADE's writer numbers product names ("Box 1"); the writer renames each product after the transfer, so Fusion's components carry the object names. `solid.export` gets the same exact name.
- The file is read back: each solid must come back as one valid closed solid of the same volume (within 0.1%), and each sketch or plane as its faces and no solid.

## Alternatives considered
- Write sketches as STEP curves (wires): the faithful encoding, and other CAD systems read it, but Fusion drops it, which is the target this export exists for. It can be added as an option later without changing the faces.
- DXF for sketches: Fusion inserts DXF as a native sketch, but only flat on a plane the person picks, so each sketch would need its plane placed again by hand, and it is a second writer to maintain. Left for later if projecting from faces proves too slow in use.
- Open CASCADE's XDE document writer (`STEPCAFControl`) for names: names products properly but needs three more toolkits and a document per export; renaming the products the plain writer makes is a few lines.
- Extending `solid.export` with extra ids: keeps one export command, but its result and checks describe one solid, and a file of only sketches and planes is a reasonable thing to want.

## Consequences
- Sketch and plane bodies are surface bodies in Fusion, not sketches or construction planes; the checklist (`docs/FUSION_CHECKLIST.md`) says how to turn them into those.
- A plane's rectangle covers the fitted region, not the whole part; a construction plane offset from it is unbounded in Fusion anyway.
- Cylinders, cones and spheres are refused for now; their axes and centres could follow as small faces or edges if they prove useful.

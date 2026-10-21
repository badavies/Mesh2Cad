# 0033. Scanned fillets are reconstructed exactly, at a radius fitted to the scan

- Status: Accepted
- Date: 2026-10-21

## Context
Section 1 lists fillets among the non-goals, under "a general parametric CAD modeller", because Fusion adds fillets after import. But manufactured parts already have them, internal and external, and the scan shows them. Today a prismatic part comes out of `primitive.trim` or `primitive.box` with sharp edges where the part is rounded, so the solid is off by up to the fillet's radius along every rounded edge and the user has to measure each radius by hand to put it back in Fusion. Surface build follows the rounding, but as free-form B-splines carrying the scan's noise, with no radius anyone can read or edit. Flat areas (ADR-032) clean up the faces either side, not the fillet between them.

## Decision
- Fillet **reconstruction** is in scope: recovering a constant-radius, rolling-ball fillet that is in the scan, on an edge of a solid between two faces. Fillet **modelling** stays a non-goal: adding fillets the scan does not have, variable-radius fillets, chamfers, and corner blends designed by hand.
- The kernel is Open CASCADE's `BRepFilletAPI_MakeFillet`, already a dependency, behind a new function in the OCCT adapter; only core types cross its header.
- The first version fillets edges between analytic faces (planes, cylinders, cones, spheres), as `primitive.trim`, `primitive.box` and the booleans make them. Edges of surface-build B-spline bodies come later if they are needed.
- The radius is fitted to a selection of the fillet's band on the mesh: a one-dimensional search over the radius, each candidate built by the kernel and scored by the band's distance to the new fillet face, and the best kept. The faces either side stay as they are. A typed radius replaces the fitted one, for example to snap it to a nominal value.
- Edges are named by points near them (the edge closest to each point), not by index: indices change with every boolean and mean nothing to the user, while a point is what a click or an agent's measurement gives.
- One command, `fillet.fit`, adds a new solid with the edges rounded and reports the radius and the band's RMS and P95 distance to the fillet. It fails, adding nothing, when the P95 distance is above a limit (0.1 mm by default), the edge is not between two faces, or the kernel cannot build the fillet (a radius too large for its faces).
- Section 1's non-goal reads "fillets (beyond reconstructing a scanned constant-radius fillet, ADR-033)".

## Alternatives considered
- Hold the cage on a cylinder in surface build, extending ADR-032: smooth, but still B-spline faces with no radius to edit, and the faces either side are not exact either.
- Fit a cylinder primitive to the band and trim it against its neighbours with `primitive.trim`: the cylinder meets the planes tangentially, which General Fuse handles poorly, and the result has sharp edges at the tangent lines, or seams that do not close.
- Leave fillets to Fusion: no code here, but the user must measure each radius on the scan, which Fusion cannot see after import.
- Fit the radius in closed form for each pair of face kinds: exact and fast for two planes, but a formula per pair; the search builds what will be exported and needs one path for all pairs.

## Consequences
- A prismatic part can come out with its real rounded edges, and Fusion sees exact fillet faces with a readable radius.
- Each fit builds the fillet about twenty times; on the kernel's analytic faces that is quick, but it is the slowest part of the command and its progress is reported.
- Variable-radius fillets, chamfers and fillets on B-spline bodies each need a new decision record.
- Tests: a scanned box with one convex rounded edge of known radius (the fitted radius within 0.05 mm, one valid solid), a concave (internal) fillet, a fillet between a plane and a cylinder, and the failures.

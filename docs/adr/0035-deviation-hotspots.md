# 0035. Deviation hotspots: out-of-tolerance faces clustered by sign

- Status: Accepted
- Date: 2026-10-21

## Context
`deviation.compute` returned only global statistics. An agent could see that a -6.4 mm error existed but not where, so it could not select the missed hole or boss and model it with `primitive.fit` and `solid.boolean` (issue #97). A person sees the colour map; an agent needs the same information as data.

## Decision
- `deviationHotspots` in `pipeline` (beside the distance functions) groups the out-of-tolerance faces into clusters. A face is out when the vertex with its largest |d| exceeds the field's tolerance; that vertex's sign is the face's sign. Faces of one sign connect through shared edges (`buildFaceAdjacency`); a face of the other sign or within tolerance separates clusters.
- Per cluster: `sign` (`+` outside the reference, `-` inside), `face_count`, `area_mm2`, `max_signed_mm` (the distance of largest magnitude, signed), `rms_mm` over the cluster's vertices, `seed_face` (the face holding that vertex, lowest index on a tie), the area-weighted `centroid` and `bounds`.
- Sorted by area, largest first, ties by seed face. Serial and in index order, so two runs give identical output.
- `deviation.compute` reports the largest `max_hotspots` (default 10, 0 for none) as `hotspots`. With `select_hotspots` each becomes a selection on the mesh named `Deviation +1`, `Deviation -1`, ... (numbered per sign), in the same undo step as the deviation. `deviation.info` reports the same hotspots without selecting them.

## Alternatives considered
- A face is out only when all three vertices are: misses the band of faces at the rim of a pocket or boss, so the wall's area comes out a ring short and the selection does not reach the edge `primitive.fit` needs.
- Cluster vertices instead of faces: selections are face sets, so the result would have to be converted anyway.
- Store the hotspots in the deviation object: they are cheap to recompute from the field and the mesh, and storing them would change the project file format.

## Consequences
- Including faces with only one vertex out widens a noisy scan's clusters by about one ring of faces. Raising `tolerance_mm` narrows them.
- A deviation with no hotspots returns `hotspots: []`. The existing `stats` are unchanged.
- Viewport markers for hotspots are a separate issue.

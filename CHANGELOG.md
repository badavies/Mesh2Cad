# Changelog

All notable changes to Mesh2Cad, by release. Pull request and issue numbers refer to
[badavies/Mesh2Cad](https://github.com/badavies/Mesh2Cad). The release workflow uses the section
for a version as the notes of its GitHub release.

## [1.0.0] - Unreleased

The first release: milestones M0 to M5 of `docs/ARCHITECTURE.md` section 12. The command API is
now version 1.0 (`commands.list` and `rpc.hello` report `api_version`), and from here on changes
to it are additive within 1.x.

### Export and release (M5)

- `solid.export.mesh` writes a solid as GLB, glTF, OBJ or STL, triangulated by Open CASCADE at a
  chord and angle you choose, with crease-split normals in glTF. It refuses to write a mesh that
  isn't closed and reports its volume error (#120, ADR-037).
- CI exports test solids as GLB and glTF and imports them into Blender; `docs/BLENDER_CHECKLIST.md`
  is the manual check for the golden scans (#121).
- Installers: an NSIS installer and a `.zip` for Windows, a `.deb` and a `.tar.gz` for Ubuntu
  24.04, each installed, smoke-tested and uninstalled on a clean machine in CI. The app now says
  so, instead of crashing, when the graphics driver has no OpenGL 3.3 (#122).
- The user guide in `docs/user`, installed with the programs and opened from **Help > User guide**.
  Every example session in it runs as a test (#123).
- Version 1.0.0, this changelog and a release workflow that attaches the installers to a GitHub
  release for each `v*` tag (#124).
- `plane.fit` adds the plane it fits as a plane primitive, so it shows in the Objects list and
  can be used in `surface.build`'s `flat` areas. In the app, `flat` is built by picking a
  selection and its plane in the Objects list and pressing **Add**. The Objects list shows each
  object's id before its name, and every group in the Tools menu starts with a capital.
- Fixes from a code audit: `mesh2cad-rpc` and a headless `mesh2cad-mcp` no longer drop replies
  and progress while `quad.remesh` runs; an agent's long command shows the app's Cancel button
  and stops when the agent disconnects; open cages draw all their boundary edges; commands
  refuse parameters their schemas rule out (a missing `visible` or `distance_mm`, `rings` out of
  range, unknown ids in `mesh.pick` and `view.render`); and refined cages fit with the original
  smoothing.
- In the app, a left drag that starts off the mesh orbits in Crease and Measure, as it already
  did in Brush.
- Tool forms list the objects and required values first and fold the tuning parameters with
  defaults under **Advanced**. Booleans are check boxes, numbers with a default spin boxes within
  the schema's limits, `mesh.segment`'s `kinds` a box for each kind, and **Reset to defaults**
  puts every setting back. `fillet.fit`'s edge points are counted, numbered in the viewport and
  taken back one at a time with **Undo last**.
- The Workflow panel covers the whole pipeline in four tracks (prepare and align, organic
  surfacing, prismatic parts and features, verify and export) and shows the current object's
  stage gate: whether a mesh is closed, manifold and one shell, a solid valid and closed, or a
  deviation within tolerance, and what to do if not. The Tools menu groups commands by task
  with titles, the command's name beside each.
- Right-clicking an object offers the tools for its kind with the object filled in, and **Change
  and run ... again** reopens the command that made it with its parameters. A double-click in
  History undoes or redoes to that step.

### Prismatic and hybrid parts (M4)

- Segmentation of a mesh into planes, cylinders, spheres and organic regions with CGAL region
  growing (`mesh.segment`), and least-squares fits of exact planes, cylinders, spheres and cones
  (`primitive.fit`), boxes (`primitive.box`) and their alignment to each other (`primitive.align`)
  (#54 to #58, #73, #87, #110).
- Trimming fitted primitives against each other into one solid (`primitive.trim`), booleans
  between solids (`solid.boolean`), and fitted cylinders as solids for true holes
  (`primitive.solid`) (#59 to #63).
- Section sketches: a mesh cut by a plane into lines, arcs, circles and splines (`sketch.section`),
  extruded or revolved into solids (`solid.extrude`, `solid.revolve`), and written into the STEP
  file beside the solid with the plane primitives (#65 to #68, #91, ADR-034).
- Fillets reconstructed at a radius fitted to the scan (`fillet.fit`, ADR-033) and flat areas held
  on a fitted plane in the cage fit (ADR-032) (#86, #88, #89).
- Deviation hotspots: connected out-of-tolerance patches, optionally as selections (#109, ADR-035).
- For agents: `view.render`, a software-rendered picture of the document (#74, #75, #108,
  ADR-030); commands that write files say so and name their file types (#80, ADR-031); progress
  with the stage a long command is at (#82); null arguments accepted in MCP tool calls (#78).
- In the app: object drop-downs and id lists in tool forms, sketches and primitives in the
  viewport, clicking solids and primitives, multi-select in the Objects list, a colour for each
  selection, and the deviation under the cursor (#70, #93, #106, #107, #111 to #113).

### Accuracy and control (M3)

- Cage fitting solved with conjugate gradients, and refined where it stays far from the scan
  (#44, #45).
- `surface.build` fails on folded, inside-out and self-crossing surfaces, keeps a part's sharp
  edges sharp when `crease_angle_deg` asks for them (off by default, so organic parts stay
  smooth), and fairs the shape it keeps and reports its fairness (#38, #114 to #116).
- Open sheets for one-sided scans, thickened into a solid wall, and cage edges laid along guide
  lines (#51 to #53).
- Long commands can be cancelled over JSON-RPC, MCP and from the app, and deviation and the
  cage fit's closest points run in parallel with oneTBB (#117 to #119, ADR-036).
- Static analysis findings fixed, and every file includes what it uses (#34, #35).

### Surfacing MVP (M2)

- Open CASCADE, OpenSubdiv and Instant Meshes in the build (ADR-026).
- `quad.remesh` lays out a quad cage (Instant Meshes, ADR-009); `surface.build` fits the cage to
  the scan through limit stencils, extracts B-spline faces with capped extraordinary vertices and
  sews them into one closed solid.
- `solid.info`, `solid.export` (STEP AP242, read back and checked), `deviation.compute` and
  `deviation.report`; project file format 2, which keeps quad layouts, solids and deviations.
- In the app: solids, quad cages, and deviation colour maps with a legend and a colour-blind-safe
  palette.

### Foundations and mesh preparation (M0 and M1)

- The command engine, with every action a registered command with a JSON schema, the same from
  the desktop app, the CLI (`mesh2cad`), JSON-RPC (`mesh2cad-rpc` and the app's local socket) and
  the MCP server (`mesh2cad-mcp`).
- STL, OBJ and PLY import and export, and the `.m2c` project file with undo history and
  provenance.
- The desktop app: an OpenGL viewport with a coarse proxy above two million faces, brush, lasso
  and crease selection, measurement, a form for every command, and history with the source of
  each change.
- Mesh tools: repair, hole filling, smoothing, simplification, remeshing, shrink-wrap and shells;
  datum, principal-axes and symmetry-plane alignment; point-to-point measurement.
- `select.defects` selects the spikes, folded slivers and small tunnels a scanner leaves, up to
  a face count, to delete before filling the holes; the Workflow panel has it after Repair.
- CI on Windows (MSVC) and Linux (GCC, a sanitizer build and the app under Xvfb).

[1.0.0]: https://github.com/badavies/Mesh2Cad/releases/tag/v1.0.0

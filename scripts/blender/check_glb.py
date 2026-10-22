# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a GLB or glTF file from solid.export.mesh opens cleanly in Blender (issue #121).

Run with the Blender application:

    blender --background --factory-startup --python scripts/blender/check_glb.py -- \
        part.glb [--size-mm X Y Z] [--tolerance-mm 0.01]

It imports the file with Blender's glTF importer into an empty scene and checks:

- the import finishes and gives exactly one mesh object;
- with --size-mm, the object's size along Blender's x, y and z is the solid's size along Mesh2Cad's
  x, y and z in millimetres (scene units are metres): the part is at its true size and Z is up;
- once vertices at the same point are merged (glTF splits them where the shading turns sharp), the
  mesh is closed and manifold, its faces are consistently wound and its volume is positive, so the
  normals face outward;
- it has custom split normals, so curved faces shade smoothly.

It prints one JSON line with what it measured and exits with 1 if any check fails.
"""

import json
import sys

import bmesh
import bpy

MERGE_DISTANCE_M = 1e-7  # 0.1 micrometre: glTF stores positions as float


def arguments() -> tuple[str, list[float] | None, float]:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        print(__doc__)
        sys.exit(2)
    path, size, tolerance = argv[0], None, 0.01
    i = 1
    while i < len(argv):
        if argv[i] == "--size-mm":
            size = [float(v) for v in argv[i + 1:i + 4]]
            i += 4
        elif argv[i] == "--tolerance-mm":
            tolerance = float(argv[i + 1])
            i += 2
        else:
            print(f"unknown option {argv[i]}")
            sys.exit(2)
    return path, size, tolerance


def main() -> int:
    path, size, tolerance = arguments()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    failures: list[str] = []
    result = bpy.ops.import_scene.gltf(filepath=path)
    if result != {"FINISHED"}:
        failures.append(f"the import returned {sorted(result)}")
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    report: dict = {"file": path, "mesh_objects": len(meshes)}
    if len(meshes) != 1:
        failures.append(f"{len(meshes)} mesh objects instead of one")
    if meshes:
        obj = meshes[0]
        mesh = obj.data
        report["name"] = obj.name
        # The importer may place the mesh under a node transform; measure in world space.
        corners = [obj.matrix_world @ v.co for v in mesh.vertices]
        lo = [min(c[k] for c in corners) for k in range(3)]
        hi = [max(c[k] for c in corners) for k in range(3)]
        measured = [round((hi[k] - lo[k]) * 1000.0, 4) for k in range(3)]
        report["size_mm"] = measured
        report["bounds_mm"] = [[round(v * 1000.0, 4) for v in lo], [round(v * 1000.0, 4) for v in hi]]
        if size is not None:
            for axis, want, got in zip("xyz", size, measured):
                if abs(want - got) > tolerance:
                    failures.append(f"size along {axis} is {got} mm, not {want} mm")

        report["custom_normals"] = bool(mesh.has_custom_normals)
        if not mesh.has_custom_normals:
            failures.append("no custom split normals, so the part shades flat")

        bm = bmesh.new()
        bm.from_mesh(mesh)
        bm.transform(obj.matrix_world)
        report["vertices"] = len(bm.verts)
        report["triangles"] = len(bm.faces)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=MERGE_DISTANCE_M)
        bm.edges.ensure_lookup_table()
        boundary = sum(1 for e in bm.edges if e.is_boundary)
        non_manifold = sum(1 for e in bm.edges if not e.is_manifold)
        # Two faces wound the same way along an edge meet it in the same direction.
        misoriented = 0
        for e in bm.edges:
            if len(e.link_faces) != 2:
                continue
            directions = []
            for f in e.link_faces:
                loop = next(l for l in f.loops if l.edge == e)
                directions.append(loop.vert == e.verts[0])
            misoriented += directions[0] == directions[1]
        volume_mm3 = bm.calc_volume(signed=True) * 1e9
        welded = len(bm.verts)
        bm.free()
        report.update({
            "welded_vertices": welded,
            "boundary_edges": boundary,
            "non_manifold_edges": non_manifold,
            "misoriented_edges": misoriented,
            "volume_mm3": round(volume_mm3, 3),
        })
        if boundary or non_manifold:
            failures.append(f"not closed: {boundary} boundary and {non_manifold} non-manifold edges")
        if misoriented:
            failures.append(f"{misoriented} edges between faces wound against each other")
        if not volume_mm3 > 0.0:
            failures.append(f"volume {volume_mm3} mm^3: the faces point inward")

    report["passed"] = not failures
    report["failures"] = failures
    print("CHECK_GLB " + json.dumps(report))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

# Export

## STEP for Fusion 360 and other CAD programs

`solid.export` writes a solid to a STEP AP242 file in millimetres. It then reads the file back and checks that it holds one valid, closed solid with the same volume. The result says whether that check passed (`check.passed`). If the solid itself isn't valid, nothing is written.

Pass `sketch_ids` to include sketches, which Fusion 360 imports as sketch curves. Pass `plane_ids` to include fitted planes, which come in as faces you can sketch on or build construction planes from. Each object is a separate named product in the file. `product_name` names the solid's product.

In Fusion 360, open the file with **File > Open > Open from my computer**, not **Insert Mesh**. The part comes in as one solid body, with its faces where the scan's are. `docs/FUSION_CHECKLIST.md` in the repository is the full check list.

## Meshes for Blender and 3D printing

`solid.export.mesh` writes a solid as triangles. The file type is chosen from the extension:

| Extension | For | Notes |
| --- | --- | --- |
| `.glb` | Blender, viewers, the web | Binary glTF 2.0: one file. Metres and Y up, as glTF requires, so Blender shows the part at its true size and the right way up. Smooth shading with sharp edges kept, and a plain grey material. |
| `.gltf` | The same, as text | glTF with the data embedded, so it is also one file. |
| `.obj` | Most 3D programs | Millimetres, Z up. Positions and faces only. |
| `.stl` | Slicers for 3D printing | Binary STL, in millimetres, Z up. |

The settings control how closely the triangles follow the surface:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `chord_mm` | 0.01 | The largest distance between a triangle and the surface. Smaller values give more triangles. |
| `angle_deg` | 10 | The largest angle between neighbouring triangles on a face. This keeps small, tight curves smooth. |
| `crease_angle_deg` | 30 | GLB and glTF only: edges that turn more than this are shaded sharp. |

The mesh is welded where faces meet and must come out closed, or nothing is written. The result gives the triangle count and the mesh's volume error against the solid, along with the limit it stays within.

```jsonl
{"command": "primitive.box", "params": {"size_mm": [30, 20, 10]}}
{"command": "solid.export.mesh", "params": {"id": 1, "path": "box.glb"}}
{"command": "mesh.import", "params": {"path": "sphere_noisy.ply"}}
{"command": "quad.remesh", "params": {"id": 2, "target_faces": 500}}
{"command": "surface.build", "params": {"id": 3, "mesh_id": 2}}
{"command": "solid.export.mesh", "params": {"id": 4, "path": "sphere.glb"}}
{"command": "solid.export.mesh", "params": {"id": 4, "path": "sphere.stl", "chord_mm": 0.05, "angle_deg": 15}}
```

In Blender, use **File > Import > glTF 2.0** with the default options. The part comes in as one mesh object at its real size. glTF splits vertices along sharp edges for shading. If you want to model on the mesh, merge them with **Mesh > Clean Up > Merge by Distance** at a very small distance, such as 0.000001 m. `docs/BLENDER_CHECKLIST.md` in the repository is the full check list.

For printing, 0.01 to 0.05 mm is plenty. Fine settings on a large part make big files: the controller at the defaults is about 125,000 triangles (6 MB as STL).

## Meshes and projects

`mesh.export` writes a mesh object itself, such as a repaired scan, as STL, OBJ or PLY. In the app this is **File > Export mesh** (Ctrl+E). `project.save` saves the whole document as a `.m2c` project, and `project.open` opens one again.

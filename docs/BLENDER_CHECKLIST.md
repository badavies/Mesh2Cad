# Blender import checklist

The manual mesh export check for each milestone gate from M5 on (ARCHITECTURE.md section 7,
Export, and section 10, Manual), beside `docs/FUSION_CHECKLIST.md` for STEP. CI already exports
four test solids as GLB and glTF and imports them into Blender
(`scripts/blender/check_exports.py`, which runs `scripts/blender/check_glb.py` on each); this
checklist covers the golden scans and what only a person looking at Blender can judge. Run it on
every golden scan's GLB file and record the result in the gate's pull request or issue.

## Making the files

For each golden scan (`tests/golden/README.md`), after the session that makes its solid (the
same as for the Fusion checklist):

```
solid.export.mesh {"id": 3, "path": "<scan>.glb"}
```

The defaults (`chord_mm` 0.01, `angle_deg` 10, `crease_angle_deg` 30) are what is checked. Note
from the result `triangle_count` and `volume_error`, which must be at most `volume_error_limit`.

Then run the automatic check on the file, with the solid's size from `solid.info` (`bounds`, max
minus min, in x, y, z):

```
blender --background --factory-startup --python scripts/blender/check_glb.py -- \
    <scan>.glb --size-mm <x> <y> <z>
```

It must print `"passed": true`. Do not continue with a file that fails.

## In Blender

Use Blender 4.0 or later. Open an empty scene (**File > New > General**, delete the cube) and
import with **File > Import > glTF 2.0 (.glb/.gltf)**, keeping the importer's default options.

| # | Check | Pass when |
|---|---|---|
| 1 | The import finishes | No error or warning in the status bar or the Info editor. |
| 2 | Outliner | One mesh object, named after the solid in Mesh2Cad, and no extra empties, cameras or lights. |
| 3 | Size | The N panel's **Item > Dimensions** read the solid's size in millimetres as metres scaled by 0.001 (for example 0.143 m for 143 mm), along the same x, y and z as in Mesh2Cad. |
| 4 | Up direction | The part stands the way it stands in Mesh2Cad: its top faces +Z (numpad 1, front view, shows the same side as Mesh2Cad's front view). |
| 5 | Position | The part sits where it was in Mesh2Cad relative to the origin (its bounds match `solid.info`'s, scaled to metres). |
| 6 | Shading | In Solid view, curved faces shade smoothly with no faceting or dark patches; sharp edges of the part (box edges, hole rims) stay sharp. |
| 7 | Normals | **Overlays > Face Orientation** shows the outside blue everywhere, with no red faces. |
| 8 | Closed | In Edit Mode, select all and **Mesh > Clean Up > Merge by Distance** with the distance set to 0.000001 m (1 µm, not the default 0.0001 m): it merges only the vertices along sharp edges, which glTF splits for shading. Then **Select > Select All by Trait > Non Manifold** selects nothing. |
| 9 | Material | The part is a plain light grey (the "Mesh2Cad grey" material) in Material Preview. |
| 10 | Responsiveness | Orbiting the part in Solid view stays smooth. |

## Recording the result

For each file give: the scan, the Mesh2Cad commit, the Blender version, the triangle count and
dimensions Blender reports, pass or fail for each numbered check, and a screenshot in Solid view.
A milestone gate passes only when every golden file passes the automatic check and checks 1 to 8;
a failure of 9 or 10 is recorded as an issue against the export.

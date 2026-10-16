# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the synthetic test meshes in examples/test-meshes with Blender.

Every mesh is built from code with fixed seeds, so a rerun with the same Blender and NumPy versions
writes the same geometry. Each one targets a group of commands (repair, holes, shells, alignment,
selection, smoothing, simplification, wrapping) and has a defect or ground truth that is known by
construction. The script then reads every exported file back, welds it the way the app's importers
do, and writes the measured statistics with the ground truth to manifest.json.

Run with the Blender application:

    blender --background --factory-startup --python scripts/blender/make_test_meshes.py -- \
        --out examples/test-meshes

or with the bpy module from PyPI (pip install bpy; it needs the Python version bpy was built for):

    python3 scripts/blender/make_test_meshes.py --out examples/test-meshes

Options: --large adds a 1.3M-triangle organic scan (too big for the repository), --only a,b builds a
subset, --formats stl,obj,ply picks the formats. Units are millimetres throughout.
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

import bpy  # first: bmesh and mathutils come with it
import bmesh
import numpy as np
from mathutils import Euler, Matrix, Vector, noise

FORMATS = ("stl", "obj", "ply")


# --------------------------------------------------------------------------------------------
# Scene helpers
# --------------------------------------------------------------------------------------------


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for collection in (bpy.data.meshes, bpy.data.metaballs):
        for block in list(collection):
            collection.remove(block)


def object_from_bmesh(name, bm):
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def object_from_arrays(name, verts, tris):
    """Build an object straight from vertex and triangle lists. Unlike bmesh this keeps duplicate
    and degenerate faces, which the defect meshes need."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(map(float, v)) for v in verts], [], [tuple(map(int, t)) for t in tris])
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def triangulate(bm):
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method="FIXED", ngon_method="EAR_CLIP")


def apply_modifiers(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = bpy.data.meshes.new_from_object(evaluated)
    obj.modifiers.clear()
    old = obj.data
    obj.data = mesh
    bpy.data.meshes.remove(old)


def canonicalize(obj):
    """Renumber vertices and faces in a fixed order. Blender's exact boolean and voxel remesh are
    multithreaded and do not give the same element order on every run; this does."""
    mesh = obj.data
    co = vertex_array(obj)
    order = np.lexsort((co[:, 2], co[:, 1], co[:, 0]))
    new_index = np.empty(len(order), dtype=np.int64)
    new_index[order] = np.arange(len(order))
    polys = []
    for poly in mesh.polygons:
        loop = [int(new_index[v]) for v in poly.vertices]
        k = loop.index(min(loop))
        polys.append(tuple(loop[k:] + loop[:k]))
    polys.sort()
    canonical = bpy.data.meshes.new(mesh.name)
    canonical.from_pydata([tuple(p) for p in co[order]], [], polys)
    obj.data = canonical
    bpy.data.meshes.remove(mesh)


def bmesh_of(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    return bm


def write_back(obj, bm):
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def vertex_array(obj):
    co = np.empty(len(obj.data.vertices) * 3)
    obj.data.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def set_vertex_array(obj, co):
    obj.data.vertices.foreach_set("co", np.ascontiguousarray(co, dtype=np.float64).ravel())
    obj.data.update()


def vertex_normals(obj):
    n = np.empty(len(obj.data.vertices) * 3)
    obj.data.vertices.foreach_get("normal", n)
    return n.reshape(-1, 3)


def rigid(euler_deg, translation):
    rotation = Euler([math.radians(a) for a in euler_deg], "XYZ").to_matrix()
    return Matrix.Translation(Vector(translation)) @ rotation.to_4x4()


def transform_record(matrix, euler_deg):
    """The pose applied to a mesh, plus the align.transform parameters that undo it."""
    r = np.array([[matrix[i][j] for j in range(3)] for i in range(3)])
    t = np.array([matrix[i][3] for i in range(3)])
    r_inv = r.T
    t_inv = -r_inv @ t
    return {
        "euler_xyz_deg": list(euler_deg),
        "rotation_row_major": [round(x, 12) for x in r.ravel()],
        "translation_mm": [round(x, 12) for x in t],
        "undo_align_transform": {
            "rotation": [round(x, 12) for x in r_inv.ravel()],
            "translation": [round(x, 12) for x in t_inv],
        },
    }


# --------------------------------------------------------------------------------------------
# Mesh builders. Each returns (object, facts): facts is the ground truth stored in the manifest.
# --------------------------------------------------------------------------------------------

BRACKET_PROFILE = [(0, 0), (60, 0), (60, 6), (6, 6), (6, 40), (0, 40)]  # (x, z), mm


def build_bracket_solid(name, segments=32):
    """L-bracket 60 x 40 x 40 mm, 6 mm thick, with a 10 mm hole in the base and an 8 mm hole in
    the upright. Flat faces meet at 90 degree creases."""
    bm = bmesh.new()
    front = [bm.verts.new((x, 0.0, z)) for x, z in BRACKET_PROFILE]
    face = bm.faces.new(front)
    extruded = bmesh.ops.extrude_face_region(bm, geom=[face])
    moved = [g for g in extruded["geom"] if isinstance(g, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, vec=(0.0, 40.0, 0.0), verts=moved)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    obj = object_from_bmesh(name, bm)

    cutters = []
    for cname, radius, location, rotation in (
        ("cut_base", 5.0, (35.0, 20.0, 3.0), (0.0, 0.0, 0.0)),
        ("cut_upright", 4.0, (3.0, 20.0, 26.0), (0.0, math.pi / 2, 0.0)),
    ):
        cbm = bmesh.new()
        bmesh.ops.create_cone(cbm, cap_ends=True, segments=segments, radius1=radius,
                              radius2=radius, depth=12.0)
        bmesh.ops.rotate(cbm, verts=cbm.verts[:], cent=(0, 0, 0),
                         matrix=Euler(rotation, "XYZ").to_matrix())
        bmesh.ops.translate(cbm, verts=cbm.verts[:], vec=location)
        cutter = object_from_bmesh(cname, cbm)
        cutters.append(cutter)
        mod = obj.modifiers.new(cname, "BOOLEAN")
        mod.operation = "DIFFERENCE"
        mod.solver = "EXACT"
        mod.object = cutter
        apply_modifiers(obj)
    for cutter in cutters:
        bpy.data.objects.remove(cutter, do_unlink=True)

    canonicalize(obj)
    bm = bmesh_of(obj)
    triangulate(bm)
    write_back(obj, bm)

    polygon_area = lambda r: 0.5 * segments * r * r * math.sin(2 * math.pi / segments)
    volume = (60 * 6 + 6 * 34) * 40 - 6 * polygon_area(5.0) - 6 * polygon_area(4.0)
    facts = {
        "design": {
            "envelope_mm": [60, 40, 40],
            "thickness_mm": 6,
            "base_hole": {"diameter_mm": 10, "axis": "z", "center_mm": [35, 20], "segments": segments},
            "upright_hole": {"diameter_mm": 8, "axis": "x", "center_mm": [20, 26], "segments": segments},
            "volume_mm3": round(volume, 6),
            "flat_regions": 8,
            "note": "Holes are 32-sided prisms, so volume uses polygon areas, not circles.",
        }
    }
    return obj, facts


def build_bracket(_rng):
    obj, facts = build_bracket_solid("bracket")
    facts["purpose"] = "Clean prismatic part with sharp 90 degree creases and two drilled holes."
    facts["commands"] = ["mesh.info", "select.crease", "plane.fit", "align.datum",
                         "measure.distance", "mesh.remesh", "mesh.pick"]
    return obj, facts


MISALIGNED_POSE = ((17.0, -32.0, 48.0), (123.4, -56.7, 89.0))


def build_bracket_misaligned(_rng):
    obj, facts = build_bracket_solid("bracket_misaligned")
    matrix = rigid(*MISALIGNED_POSE)
    obj.data.transform(matrix)
    obj.data.update()
    facts["pose"] = transform_record(matrix, MISALIGNED_POSE[0])
    facts["purpose"] = "The bracket moved by a known rigid transform, as a scan arrives."
    facts["commands"] = ["align.principal", "align.datum", "align.transform", "align.reset"]
    return obj, facts


SCAN_POSE = ((-8.0, 5.0, 23.0), (10.0, 20.0, 5.0))


def build_bracket_scan(rng):
    """Scan-like bracket: voxel remesh (edges slightly rounded, even triangles), normal noise and a
    known pose."""
    obj, facts = build_bracket_solid("bracket_scan")
    mod = obj.modifiers.new("remesh", "REMESH")
    mod.mode = "VOXEL"
    mod.voxel_size = 1.0
    mod.use_smooth_shade = False
    apply_modifiers(obj)
    canonicalize(obj)
    sigma = 0.03
    co = vertex_array(obj)
    normals = vertex_normals(obj)
    co += normals * rng.normal(0.0, sigma, size=(len(co), 1))
    set_vertex_array(obj, co)
    bm = bmesh_of(obj)
    triangulate(bm)
    write_back(obj, bm)
    matrix = rigid(*SCAN_POSE)
    obj.data.transform(matrix)
    obj.data.update()
    facts["design"]["note"] = "Voxel remeshed at 1 mm, so edges are rounded by about one voxel."
    facts["noise_sigma_mm"] = sigma
    facts["pose"] = transform_record(matrix, SCAN_POSE[0])
    facts["purpose"] = "Dense, noisy, misaligned bracket: the realistic version of 'bracket'."
    facts["commands"] = ["mesh.smooth", "mesh.simplify", "align.principal", "select.brush",
                         "select.lasso", "select.crease", "plane.fit"]
    return obj, facts


def build_sphere_noisy(rng):
    radius, sigma = 25.0, 0.15
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=6, radius=radius)
    obj = object_from_bmesh("sphere_noisy", bm)
    co = vertex_array(obj)
    unit = co / np.linalg.norm(co, axis=1, keepdims=True)
    co = unit * (radius + rng.normal(0.0, sigma, size=(len(co), 1)))
    set_vertex_array(obj, co)
    return obj, {
        "purpose": "Icosphere with Gaussian radial noise. Smoothing should bring the radius "
                   "spread down without shrinking the sphere.",
        "commands": ["mesh.smooth", "mesh.simplify", "mesh.remesh", "select.brush"],
        "design": {"radius_mm": radius, "noise_sigma_mm": sigma, "center_mm": [0, 0, 0]},
    }


def build_cylinder_holes(_rng):
    """Closed cylinder with three small holes in the wall and its top cap removed."""
    radius, height, segments, rings = 20.0, 60.0, 48, 24
    verts, tris = [], []
    grid = []
    for i in range(rings + 1):
        z = height * i / rings
        row = []
        for j in range(segments):
            a = 2 * math.pi * j / segments
            row.append(len(verts))
            verts.append((radius * math.cos(a), radius * math.sin(a), z))
        grid.append(row)
    removed = set()
    holes = {"A": (5, 3, 1, 1), "B": (12, 15, 2, 2), "C": (18, 30, 3, 4)}  # ring, segment, rows, cols
    for _, (r0, s0, nr, ns) in holes.items():
        removed |= {(r0 + a, s0 + b) for a in range(nr) for b in range(ns)}
    for i in range(rings):
        for j in range(segments):
            if (i, j) in removed:
                continue
            a, b = grid[i][j], grid[i][(j + 1) % segments]
            c, d = grid[i + 1][(j + 1) % segments], grid[i + 1][j]
            tris += [(a, b, c), (a, c, d)]
    bottom = len(verts)
    verts.append((0.0, 0.0, 0.0))
    for j in range(segments):
        tris.append((bottom, grid[0][(j + 1) % segments], grid[0][j]))
    obj = object_from_arrays("cylinder_holes", verts, tris)
    chord = 2 * radius * math.sin(math.pi / segments)
    ring = height / rings
    hole_list = []
    for name, (_, _, nr, ns) in holes.items():
        hole_list.append({"name": name, "quads": nr * ns, "boundary_edges": 2 * (nr + ns),
                          "perimeter_mm": round(2 * (nr * ring + ns * chord), 6)})
    hole_list.append({"name": "top", "quads": 0, "boundary_edges": segments,
                      "perimeter_mm": round(segments * chord, 6)})
    return obj, {
        "purpose": "Four open boundaries of known size: three small wall holes and a large "
                   "opening where the top cap was removed.",
        "commands": ["holes.fill", "mesh.info"],
        "design": {"radius_mm": radius, "height_mm": height, "segments": segments, "rings": rings,
                   "holes": hole_list,
                   "fill_hint": "holes.fill with max_edges 20 fills A, B and C and leaves 'top' open."},
    }


def build_torus_shells(_rng):
    parts = []
    bm = bmesh.new()

    def add(shape_bm, label):
        before = len(bm.faces)
        mesh = bpy.data.meshes.new("tmp")
        shape_bm.to_mesh(mesh)
        shape_bm.free()
        bm.from_mesh(mesh)
        bpy.data.meshes.remove(mesh)
        parts.append({"name": label, "faces": len(bm.faces) - before})

    torus = bmesh.new()
    major, minor, nu, nv = 30.0, 10.0, 48, 24
    ring = [[torus.verts.new(((major + minor * math.cos(v * 2 * math.pi / nv)) * math.cos(u * 2 * math.pi / nu),
                              (major + minor * math.cos(v * 2 * math.pi / nv)) * math.sin(u * 2 * math.pi / nu),
                              minor * math.sin(v * 2 * math.pi / nv)))
             for v in range(nv)] for u in range(nu)]
    for u in range(nu):
        for v in range(nv):
            torus.faces.new((ring[u][v], ring[(u + 1) % nu][v], ring[(u + 1) % nu][(v + 1) % nv],
                             ring[u][(v + 1) % nv]))
    triangulate(torus)
    add(torus, "torus (main)")

    for label, radius, subdiv, center in (("sphere_r1.5", 1.5, 2, (45, 0, 0)),
                                          ("sphere_r0.8", 0.8, 1, (0, 48, 5)),
                                          ("sphere_r3", 3.0, 3, (-20, -20, 20))):
        s = bmesh.new()
        bmesh.ops.create_icosphere(s, subdivisions=subdiv, radius=radius)
        bmesh.ops.translate(s, verts=s.verts[:], vec=center)
        add(s, label)

    t = bmesh.new()
    t.faces.new([t.verts.new(p) for p in ((0, 0, 30), (1.5, 0, 30), (0.75, 1.3, 30))])
    add(t, "single_triangle")

    q = bmesh.new()
    q.faces.new([q.verts.new(p) for p in ((10, -45, -8), (12, -45, -8), (12, -43, -8.5), (10, -43, -8))])
    triangulate(q)
    add(q, "quad_two_triangles")

    g = bmesh.new()
    bmesh.ops.create_grid(g, x_segments=3, y_segments=3, size=3.0)
    bmesh.ops.translate(g, verts=g.verts[:], vec=(0, 0, -20))
    triangulate(g)
    add(g, "open_grid_patch")

    obj = object_from_bmesh("torus_shells", bm)
    return obj, {
        "purpose": "A torus (main part) with six stray fragments of known face counts.",
        "commands": ["shells.detect", "shells.keep", "shells.clean", "shells.split",
                     "shells.select", "select.shell"],
        "design": {"shells": parts,
                   "clean_hint": "shells.clean with min_faces 100 removes all but the torus and sphere_r3."},
    }


def build_box_defects(_rng):
    """A 30 x 20 x 10 mm box (4 x 4 grid per side) with one of each classic scan defect."""
    sx, sy, sz, n = 30.0, 20.0, 10.0, 4
    verts, tris, lookup = [], [], {}

    def vid(p):
        key = tuple(round(c, 6) for c in p)
        if key not in lookup:
            lookup[key] = len(verts)
            verts.append(key)
        return lookup[key]

    axes = [((0, 0, 0), (0, sy, 0), (sx, 0, 0)),          # bottom, -z
            ((0, 0, sz), (sx, 0, 0), (0, sy, 0)),         # top, +z
            ((0, 0, 0), (sx, 0, 0), (0, 0, sz)),          # front, -y
            ((0, sy, 0), (0, 0, sz), (sx, 0, 0)),         # back, +y
            ((0, 0, 0), (0, 0, sz), (0, sy, 0)),          # left, -x
            ((sx, 0, 0), (0, sy, 0), (0, 0, sz))]         # right, +x
    face_ranges = {}
    names = ["bottom", "top", "front", "back", "left", "right"]
    for name, (o, u, v) in zip(names, axes):
        start = len(tris)
        for i in range(n):
            for j in range(n):
                p = lambda a, b: tuple(o[k] + u[k] * a / n + v[k] * b / n for k in range(3))
                a, b, c, d = vid(p(i, j)), vid(p(i + 1, j)), vid(p(i + 1, j + 1)), vid(p(i, j + 1))
                tris += [(a, b, c), (a, c, d)]
        face_ranges[name] = (start, len(tris))
    clean_faces = len(tris)
    defects = []

    # Inconsistent orientation: flip four triangles of the front face.
    front0 = face_ranges["front"][0]
    for f in range(front0, front0 + 4):
        a, b, c = tris[f]
        tris[f] = (a, c, b)
    defects.append({"kind": "flipped_faces", "faces": list(range(front0, front0 + 4))})

    # Non-manifold edge: a fin standing on an edge of the top face (3 faces share it).
    a, b = vid((15.0, 10.0, sz)), vid((22.5, 10.0, sz))
    tip = len(verts)
    verts.append((18.75, 10.0, sz + 6.0))
    tris.append((a, b, tip))
    defects.append({"kind": "non_manifold_edge_fin", "faces": [len(tris) - 1]})

    # Non-manifold vertex (bowtie): a closed tetrahedron touching the box only at a corner.
    corner = vid((sx, sy, sz))
    t1, t2, t3 = (len(verts) + k for k in range(3))
    verts += [(sx + 4, sy, sz + 1), (sx, sy + 4, sz + 1), (sx + 1, sy + 1, sz + 4)]
    first = len(tris)
    tris += [(corner, t2, t1), (corner, t1, t3), (corner, t3, t2), (t1, t2, t3)]
    defects.append({"kind": "non_manifold_vertex_tetra", "faces": list(range(first, len(tris)))})

    # Duplicate faces: one exact copy and one copy with reversed winding.
    bottom0 = face_ranges["bottom"][0]
    tris.append(tris[bottom0])
    a, b, c = tris[bottom0 + 5]
    tris.append((a, c, b))
    defects.append({"kind": "duplicate_faces", "faces": [len(tris) - 2, len(tris) - 1]})

    # Degenerate face: three collinear vertices along the bottom front edge (exactly zero area).
    tris.append((vid((0, 0, 0)), vid((7.5, 0, 0)), vid((15, 0, 0))))
    defects.append({"kind": "degenerate_face", "faces": [len(tris) - 1]})

    # Self-intersection: a separate 6 mm cube pushed halfway through the top face.
    first = len(tris)
    base = len(verts)
    for x in (12.0, 18.0):
        for y in (7.0, 13.0):
            for z in (sz - 3.0, sz + 3.0):
                verts.append((x, y, z))
    idx = lambda x, y, z: base + 4 * x + 2 * y + z
    cube = [(0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3), (0, 4, 5), (0, 5, 1),
            (2, 3, 7), (2, 7, 6), (0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5)]
    for f in cube:
        tris.append(tuple(idx((k >> 2) & 1, (k >> 1) & 1, k & 1) for k in f))
    defects.append({"kind": "self_intersecting_cube", "faces": list(range(first, len(tris)))})

    obj = object_from_arrays("box_defects", verts, tris)
    return obj, {
        "purpose": "One of each defect mesh.repair handles, on an otherwise clean box.",
        "commands": ["mesh.repair", "mesh.info", "shells.detect"],
        "design": {"box_mm": [sx, sy, sz], "clean_box_faces": clean_faces, "defects": defects},
    }


def build_sphere_inside_out(_rng):
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=4, radius=15.0)
    bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
    obj = object_from_bmesh("sphere_inside_out", bm)
    return obj, {
        "purpose": "Closed sphere with every face reversed (normals point inward, volume < 0).",
        "commands": ["mesh.flip", "mesh.repair", "mesh.info"],
        "design": {"radius_mm": 15.0},
    }


def build_soup_damaged(rng):
    """Overlapping, unjoined primitives with a quarter of the faces missing and jittered vertices:
    too broken to repair, which is what mesh.wrap is for."""
    bm = bmesh.new()
    for radius, center in ((20.0, (0, 0, 0)), (15.0, (22, 0, 0))):
        s = bmesh.new()
        bmesh.ops.create_icosphere(s, subdivisions=5, radius=radius)
        bmesh.ops.translate(s, verts=s.verts[:], vec=center)
        mesh = bpy.data.meshes.new("tmp")
        s.to_mesh(mesh)
        s.free()
        bm.from_mesh(mesh)
        bpy.data.meshes.remove(mesh)
    c = bmesh.new()
    bmesh.ops.create_cone(c, cap_ends=True, segments=24, radius1=4.0, radius2=4.0, depth=50.0)
    bmesh.ops.rotate(c, verts=c.verts[:], cent=(0, 0, 0), matrix=Euler((0, math.pi / 2, 0)).to_matrix())
    bmesh.ops.translate(c, verts=c.verts[:], vec=(11, 0, 0))
    mesh = bpy.data.meshes.new("tmp")
    c.to_mesh(mesh)
    c.free()
    bm.from_mesh(mesh)
    bpy.data.meshes.remove(mesh)
    triangulate(bm)
    bm.faces.ensure_lookup_table()
    total = len(bm.faces)
    drop = rng.random(total) < 0.25
    flip = (rng.random(total) < 0.02) & ~drop
    bmesh.ops.reverse_faces(bm, faces=[f for f, k in zip(bm.faces, flip) if k])
    bmesh.ops.delete(bm, geom=[f for f, k in zip(bm.faces, drop) if k], context="FACES_ONLY")
    obj = object_from_bmesh("soup_damaged", bm)
    co = vertex_array(obj)
    co += rng.normal(0.0, 0.1, size=co.shape)
    set_vertex_array(obj, co)
    return obj, {
        "purpose": "Two intersecting spheres and a rod, not joined, 25% of faces missing, 2% "
                   "flipped and 0.1 mm vertex jitter.",
        "commands": ["mesh.wrap", "mesh.repair", "holes.fill", "shells.detect"],
        "design": {"faces_before_damage": total, "dropped_faces": int(drop.sum()),
                   "flipped_faces": int(flip.sum()), "jitter_sigma_mm": 0.1,
                   "wrap_hint": "mesh.wrap with alpha_mm 2 gives one closed, manifold shell."},
    }


def build_controller(_rng):
    """Organic game-controller-like body from metaballs: no flat faces or sharp edges."""
    meta = bpy.data.metaballs.new("controller_meta")
    meta.resolution = 1.6
    meta.render_resolution = 1.6
    meta.threshold = 0.6
    obj = bpy.data.objects.new("controller_meta", meta)
    bpy.context.scene.collection.objects.link(obj)
    elements = [
        ("ELLIPSOID", (0, 0, 0), 20.0, (3.2, 1.5, 1.0), (0, 0, 0)),             # body
        ("ELLIPSOID", (-42, -22, -4), 14.0, (1.4, 2.4, 1.3), (0, 0, 0.45)),     # left grip
        ("ELLIPSOID", (42, -22, -4), 14.0, (1.4, 2.4, 1.3), (0, 0, -0.45)),     # right grip
        ("BALL", (-24, 6, 11), 8.0, (1, 1, 1), (0, 0, 0)),                      # left stick
        ("BALL", (24, -6, 11), 8.0, (1, 1, 1), (0, 0, 0)),                      # right stick
    ]
    for kind, co, radius, size, rot in elements:
        el = meta.elements.new(type=kind)
        el.co = co
        el.radius = radius
        el.size_x, el.size_y, el.size_z = size
        el.rotation = Euler(rot).to_quaternion()
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph))
    bpy.data.objects.remove(obj, do_unlink=True)
    result = bpy.data.objects.new("controller", mesh)
    bpy.context.scene.collection.objects.link(result)
    bm = bmesh_of(result)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
    bmesh.ops.scale(bm, vec=(1.3, 1.3, 1.3), verts=bm.verts[:])  # about 145 x 75 x 40 mm
    triangulate(bm)
    write_back(result, bm)
    return result, {
        "purpose": "Smooth organic body with grips and thumbstick bumps, like the v1 target scan.",
        "commands": ["align.principal", "mesh.remesh", "mesh.simplify", "mesh.smooth",
                     "select.brush", "select.lasso", "measure.distance"],
        "design": {"metaball_resolution_mm": 1.6, "scale": 1.3, "shells": 1},
    }


def build_organic_large(rng, subdivisions=9):
    radius = 60.0
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=subdivisions, radius=radius)
    obj = object_from_bmesh("organic_large", bm)
    co = vertex_array(obj)
    unit = co / np.linalg.norm(co, axis=1, keepdims=True)
    noise.seed_set(7)
    bumps = np.array([noise.fractal(Vector(p * 2.0), 0.6, 2.0, 5, noise_basis="PERLIN_ORIGINAL")
                      for p in unit])
    r = radius + 6.0 * bumps + rng.normal(0.0, 0.02, size=len(co))
    set_vertex_array(obj, unit * r[:, None])
    return obj, {
        "purpose": "Large organic scan for timing and memory (import, display, simplify, smooth).",
        "commands": ["mesh.import", "mesh.info", "mesh.simplify", "mesh.smooth", "mesh.remesh"],
        "design": {"radius_mm": radius, "icosphere_subdivisions": subdivisions},
    }


BUILDERS = {
    "bracket": build_bracket,
    "bracket_misaligned": build_bracket_misaligned,
    "bracket_scan": build_bracket_scan,
    "sphere_noisy": build_sphere_noisy,
    "cylinder_holes": build_cylinder_holes,
    "torus_shells": build_torus_shells,
    "box_defects": build_box_defects,
    "sphere_inside_out": build_sphere_inside_out,
    "soup_damaged": build_soup_damaged,
    "controller": build_controller,
}
LARGE_BUILDERS = {"organic_large": build_organic_large}


# --------------------------------------------------------------------------------------------
# Export with Blender's own exporters (Z up, no axis conversion, no scaling)
# --------------------------------------------------------------------------------------------


def export(obj, path, fmt):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    common = dict(filepath=str(path), export_selected_objects=True, global_scale=1.0,
                  forward_axis="Y", up_axis="Z", apply_modifiers=True)
    if fmt == "stl":
        bpy.ops.wm.stl_export(ascii_format=False, use_scene_unit=False, **common)
    elif fmt == "obj":
        bpy.ops.wm.obj_export(export_uv=False, export_normals=False, export_colors=False,
                              export_materials=False, export_triangulated_mesh=True, **common)
    elif fmt == "ply":
        bpy.ops.wm.ply_export(ascii_format=False, export_uv=False, export_normals=False,
                              export_colors="NONE", export_attributes=False,
                              export_triangulated_mesh=True, **common)


# --------------------------------------------------------------------------------------------
# Read the files back and measure them the way the app does (exact weld, then stats)
# --------------------------------------------------------------------------------------------


def read_stl(path):
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 80)[0]
    rec = np.dtype([("n", "<f4", 3), ("v", "<f4", 9), ("attr", "<u2")])
    tri = np.frombuffer(data, rec, count, 84)["v"].reshape(-1, 3) + np.float32(0.0)
    keys, inverse = np.unique(tri, axis=0, return_inverse=True)
    return keys.astype(np.float64), inverse.reshape(-1, 3)


def read_obj(path):
    verts, tris = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                verts.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("f "):
                idx = [int(tok.split("/")[0]) for tok in line.split()[1:]]
                idx = [i - 1 if i > 0 else len(verts) + i for i in idx]
                tris += [(idx[0], idx[k - 1], idx[k]) for k in range(2, len(idx))]
    return np.array(verts), np.array(tris, dtype=np.int64)


def read_ply(path):
    data = path.read_bytes()
    end = data.index(b"end_header\n") + len(b"end_header\n")
    header = data[:end].decode("ascii").splitlines()
    assert "format binary_little_endian 1.0" in header, "expected binary little-endian PLY"
    types = {"float": "<f4", "float32": "<f4", "double": "<f8", "uchar": "u1", "uint8": "u1",
             "int": "<i4", "int32": "<i4", "uint": "<u4", "uint32": "<u4"}
    elements, current = [], None
    for line in header:
        parts = line.split()
        if parts[0] == "element":
            current = (parts[1], int(parts[2]), [])
            elements.append(current)
        elif parts[0] == "property":
            current[2].append(parts[1:])
    offset, verts, tris = end, None, None
    for name, count, props in elements:
        if name == "vertex":
            dtype = np.dtype([(p[-1], types[p[0]]) for p in props])
            rows = np.frombuffer(data, dtype, count, offset)
            verts = np.stack([rows["x"], rows["y"], rows["z"]], axis=1).astype(np.float64)
            offset += dtype.itemsize * count
        elif name == "face":
            _, count_type, index_type, _ = props[0]
            dtype = np.dtype([("n", types[count_type]), ("i", types[index_type], 3)])
            rows = np.frombuffer(data, dtype, count, offset)
            assert (rows["n"] == 3).all(), "expected a triangulated PLY"
            tris = rows["i"].astype(np.int64)
            offset += dtype.itemsize * count
    return verts, tris


def weld(verts, tris):
    keys, inverse = np.unique(verts + 0.0, axis=0, return_inverse=True)
    return keys, inverse.reshape(-1)[tris]


def components(count, pairs):
    parent = np.arange(count)

    def find(x):
        root = x
        while parent[root] != root:
            root = parent[root]
        while parent[x] != root:
            parent[x], x = root, parent[x]
        return root

    for a, b in pairs:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    return len({find(i) for i in range(count)})


def measure(verts, tris):
    v, t = weld(verts, tris)
    repeated = (t[:, 0] == t[:, 1]) | (t[:, 1] == t[:, 2]) | (t[:, 2] == t[:, 0])
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    cr = np.cross(b - a, c - a)
    area = 0.5 * np.linalg.norm(cr, axis=1)
    area[repeated] = 0.0
    volume = np.einsum("ij,ij->i", a, np.cross(b, c)) / 6.0
    volume[repeated] = 0.0
    good = np.flatnonzero(~repeated)
    e = np.sort(np.concatenate([t[good][:, [0, 1]], t[good][:, [1, 2]], t[good][:, [2, 0]]]), axis=1)
    edge_face = np.concatenate([good, good, good])
    uniq, inv, uses = np.unique(e, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    boundary = uniq[uses == 1]
    stats = {
        "vertices": int(len(np.unique(t))),
        "faces": int(len(t)),
        "edges": int(len(uniq)),
        "boundary_edges": int(len(boundary)),
        "non_manifold_edges": int((uses > 2).sum()),
        "degenerate_faces": int((repeated | (area <= 0.0)).sum()),
        "boundary_loops": 0,
        "shells": 0,
        "surface_area_mm2": round(float(area.sum()), 4),
        "signed_volume_mm3": round(float(volume.sum()), 4),
        "bounds_mm": [[round(float(x), 4) for x in v[np.unique(t)].min(axis=0)],
                      [round(float(x), 4) for x in v[np.unique(t)].max(axis=0)]],
    }
    if len(boundary):
        ids, local = np.unique(boundary, return_inverse=True)
        stats["boundary_loops"] = components(len(ids), local.reshape(-1, 2))
    # Shells: faces joined through shared edges (as m2c::faceComponents does).
    order = np.argsort(inv, kind="stable")
    sorted_inv, sorted_face = inv[order], edge_face[order]
    same = sorted_inv[1:] == sorted_inv[:-1]
    pairs = np.stack([sorted_face[:-1][same], sorted_face[1:][same]], axis=1)
    stats["shells"] = components(len(t), pairs) - int(repeated.sum())
    return stats


READERS = {"stl": read_stl, "obj": read_obj, "ply": read_ply}


# --------------------------------------------------------------------------------------------
# Probes: face indices and points found in the exported file, for the example sessions. Face
# order survives export and import, so these indices are valid in the app.
# --------------------------------------------------------------------------------------------


def faces_where(v, t, test):
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    return np.flatnonzero(test(a) & test(b) & test(c))


def nearest_face(v, t, point, normal):
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    n = np.cross(b - a, c - a)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-30)
    d = np.linalg.norm((a + b + c) / 3 - np.asarray(point), axis=1)
    d[n @ np.asarray(normal) < 0.9] = np.inf
    return int(np.argmin(d))


def undo_pose(v, facts):
    if "pose" not in facts:
        return v
    r = np.array(facts["pose"]["rotation_row_major"]).reshape(3, 3)
    return (v - np.array(facts["pose"]["translation_mm"])) @ r


def pose_point(p, facts):
    if "pose" not in facts:
        return [float(x) for x in p]
    r = np.array(facts["pose"]["rotation_row_major"]).reshape(3, 3)
    return [round(float(x), 6) for x in r @ np.asarray(p, dtype=float) + np.array(facts["pose"]["translation_mm"])]


def probe_bracket(v, t, facts):
    local = undo_pose(v, facts)
    eps = 1e-3
    x, y, z = (lambda k: (lambda p: p[:, k]))(0), (lambda k: (lambda p: p[:, k]))(1), (lambda k: (lambda p: p[:, k]))(2)
    regions = {
        "base_top": faces_where(local, t, lambda p: (abs(z(p) - 6) < eps) & (x(p) > 6 - eps)),
        "base_bottom": faces_where(local, t, lambda p: abs(z(p)) < eps),
        "front_y0": faces_where(local, t, lambda p: abs(y(p)) < eps),
        "upright_back_x0": faces_where(local, t, lambda p: abs(x(p)) < eps),
        "base_hole_wall": faces_where(local, t, lambda p: abs(np.hypot(x(p) - 35, y(p) - 20) - 5) < eps),
        "upright_hole_wall": faces_where(local, t, lambda p: abs(np.hypot(y(p) - 20, z(p) - 26) - 4) < eps),
    }
    return {name: {"seed_face": int(f[0]), "faces": int(len(f))} for name, f in regions.items()}


def probe_bracket_scan(v, t, facts):
    local = undo_pose(v, facts)
    r = np.array(facts["pose"]["rotation_row_major"]).reshape(3, 3)
    seeds = {}
    for name, point, normal in (("base_top", (40, 30, 6), (0, 0, 1)),
                                ("base_bottom", (30, 20, 0), (0, 0, -1)),
                                ("front_y0", (30, 0, 3), (0, -1, 0)),
                                ("upright_back_x0", (0, 20, 20), (-1, 0, 0))):
        face = nearest_face(local, t, point, normal)
        seeds[name] = {"seed_face": face, "point_mm": pose_point(point, facts)}
    return seeds


def probe_torus_shells(v, t, facts):
    return {"first_fragment_face": facts["design"]["shells"][0]["faces"]}


PROBES = {"bracket": probe_bracket, "bracket_misaligned": probe_bracket,
          "bracket_scan": probe_bracket_scan, "torus_shells": probe_torus_shells}


# --------------------------------------------------------------------------------------------
# Example sessions for `mesh2cad batch`, one per mesh. Run them from this folder. Object ids are
# given out in order: the imported mesh is 1 and each new selection, measurement or mesh takes
# the next id.
# --------------------------------------------------------------------------------------------


def session(name, facts, fmt):
    imp = ("mesh.import", {"path": f"{name}.{fmt}"})
    info = lambda i=1: ("mesh.info", {"id": i})
    p = facts.get("probes", {})
    if name == "bracket":
        return [imp, info(),
                ("mesh.pick", {"origin": [45, 30, 50], "direction": [0, 0, -1]}),
                ("select.crease", {"id": 1, "face": p["base_top"]["seed_face"], "name": "base top"}),
                ("plane.fit", {"selection_id": 2}),
                ("select.crease", {"id": 1, "face": p["base_hole_wall"]["seed_face"], "name": "hole"}),
                ("select.grow", {"selection_id": 3}),
                ("measure.distance", {"a": {"point": [30, 20, 0], "id": 1},
                                      "b": {"point": [30, 20, 6], "id": 1}, "name": "thickness"}),
                ("mesh.remesh", {"id": 1, "target_edge_mm": 2.0, "sharp_angle_deg": 60}),
                info(), ("doc.undo", {}), info()]
    if name == "bracket_misaligned":
        undo = facts["pose"]["undo_align_transform"]
        return [imp, info(),
                ("align.principal", {"id": 1, "origin": "bounds_bottom_center"}), info(),
                ("align.reset", {"id": 1}),
                ("align.transform", {"id": 1, **undo}), info(),
                ("align.reset", {"id": 1}),
                ("select.crease", {"id": 1, "face": p["base_bottom"]["seed_face"], "name": "primary"}),
                ("select.crease", {"id": 1, "face": p["front_y0"]["seed_face"], "name": "secondary"}),
                ("select.crease", {"id": 1, "face": p["upright_back_x0"]["seed_face"], "name": "tertiary"}),
                ("align.datum", {"id": 1, "primary": 2, "secondary": 3, "tertiary": 4}), info()]
    if name == "bracket_scan":
        return [imp, info(),
                ("select.crease", {"id": 1, "face": p["base_bottom"]["seed_face"], "max_angle_deg": 15, "name": "primary"}),
                ("select.crease", {"id": 1, "face": p["front_y0"]["seed_face"], "max_angle_deg": 15, "name": "secondary"}),
                ("select.crease", {"id": 1, "face": p["upright_back_x0"]["seed_face"], "max_angle_deg": 15, "name": "tertiary"}),
                ("plane.fit", {"selection_id": 2}),
                ("align.datum", {"id": 1, "primary": 2, "secondary": 3, "tertiary": 4}), info(),
                # After align.datum the scan sits in the bracket's own frame, so design points apply.
                ("select.brush", {"id": 1, "center": [40, 30, 6], "radius_mm": 8, "name": "brush"}),
                ("mesh.smooth", {"id": 1, "selection_id": 5, "iterations": 10}),
                ("mesh.smooth", {"id": 1, "iterations": 5, "preserve_sharp_edges": True}),
                ("mesh.simplify", {"id": 1, "target_faces": 5000}), info()]
    if name == "sphere_noisy":
        return [imp, info(),
                ("mesh.smooth", {"id": 1, "iterations": 10}), info(),
                ("select.brush", {"id": 1, "center": [0, 0, 25], "radius_mm": 6, "name": "cap"}),
                ("select.grow", {"selection_id": 2, "rings": 2}),
                ("select.shrink", {"selection_id": 2, "rings": 1}),
                ("select.invert", {"selection_id": 2}),
                ("mesh.simplify", {"id": 1, "target_ratio": 0.25}), info(),
                ("mesh.remesh", {"id": 1, "target_edge_mm": 2.0, "sharp_angle_deg": 0}), info()]
    if name == "cylinder_holes":
        return [imp, info(),
                ("holes.fill", {"id": 1, "max_edges": 20}), info(),
                ("doc.undo", {}),
                ("holes.fill", {"id": 1}), info()]
    if name == "torus_shells":
        return [imp, info(), ("shells.detect", {"id": 1}),
                ("shells.select", {"id": 1, "shells": [1, 2]}),
                ("select.shell", {"id": 1, "face": p["first_fragment_face"], "name": "fragment"}),
                ("shells.split", {"id": 1}), ("doc.list", {}), ("doc.undo", {}),
                ("shells.clean", {"id": 1, "min_faces": 100}), ("shells.detect", {"id": 1}),
                ("doc.undo", {}),
                ("shells.keep", {"id": 1}), info()]
    if name == "box_defects":
        return [imp, info(), ("shells.detect", {"id": 1}),
                ("mesh.repair", {"id": 1}), info(),
                ("mesh.repair", {"id": 1, "remove_self_intersections": True}), info()]
    if name == "sphere_inside_out":
        return [imp, info(), ("mesh.flip", {"id": 1}), info(),
                ("doc.undo", {}), ("mesh.repair", {"id": 1}), info()]
    if name == "soup_damaged":
        return [imp, info(), ("shells.detect", {"id": 1}),
                ("mesh.wrap", {"id": 1, "alpha_mm": 2.0, "name": "wrapped"}), info(2),
                ("object.show", {"id": 1, "visible": False})]
    if name == "controller":
        return [imp, info(),
                ("align.principal", {"id": 1, "origin": "bounds_bottom_center"}), info(),
                ("mesh.remesh", {"id": 1, "target_edge_mm": 1.5}), info(),
                ("mesh.simplify", {"id": 1, "target_faces": 4000}), info(),
                ("measure.distance", {"a": {"point": [-80, 0, 20], "id": 1},
                                      "b": {"point": [80, 0, 20], "id": 1}, "name": "width"})]
    if name == "organic_large":
        return [imp, info(), ("mesh.simplify", {"id": 1, "target_faces": 200000}), info(),
                ("mesh.smooth", {"id": 1, "iterations": 3}), info()]
    return [imp, info()]


def write_session(path, steps):
    with open(path, "w") as f:
        for command, params in steps:
            f.write(json.dumps({"command": command, "params": params} if params else {"command": command}) + "\n")


# --------------------------------------------------------------------------------------------


SESSION_FORMAT = {"bracket": "stl", "bracket_misaligned": "obj", "cylinder_holes": "stl",
                  "torus_shells": "obj", "sphere_inside_out": "stl", "soup_damaged": "obj"}
COMPACT_FACES = 5000
STAT_KEYS = ("vertices", "faces", "edges", "boundary_edges", "non_manifold_edges",
             "degenerate_faces", "boundary_loops", "shells")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--large", action="store_true", help="also build organic_large (1.3M faces)")
    parser.add_argument("--only", default="", help="comma-separated mesh names")
    parser.add_argument("--formats", default=",".join(FORMATS))
    parser.add_argument("--compact", action="store_true",
                        help=f"keep only the PLY of meshes over {COMPACT_FACES} faces (the repository set)")
    parser.add_argument("--seed", type=int, default=20261016)
    args = parser.parse_args(argv)

    builders = dict(BUILDERS)
    if args.large:
        builders.update(LARGE_BUILDERS)
    names = [n for n in args.only.split(",") if n] or list(builders)
    formats = [f for f in args.formats.split(",") if f]
    args.out.mkdir(parents=True, exist_ok=True)

    manifest_path = args.out / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    manifest["generator"] = {"script": "scripts/blender/make_test_meshes.py",
                             "blender": bpy.app.version_string, "numpy": np.__version__,
                             "seed": args.seed, "units": "mm"}
    meshes = manifest.setdefault("meshes", {})

    for name in names:
        clear_scene()
        # Each mesh has its own stream, so building a subset gives the same meshes.
        rng = np.random.default_rng([args.seed, sorted({**BUILDERS, **LARGE_BUILDERS}).index(name)])
        obj, facts = builders[name](rng)
        files, measured, arrays = {}, {}, {}
        for fmt in formats:
            path = args.out / f"{name}.{fmt}"
            export(obj, path, fmt)
            arrays[fmt] = READERS[fmt](path)
            measured[fmt] = measure(*arrays[fmt])
            files[fmt] = {"file": path.name, "bytes": path.stat().st_size}
        first = measured[formats[0]]
        mismatch = [f for f in formats[1:]
                    if [measured[f][k] for k in STAT_KEYS] != [first[k] for k in STAT_KEYS]]
        if name in PROBES:
            facts["probes"] = PROBES[name](*arrays[formats[0]], facts)
        if args.compact and first["faces"] > COMPACT_FACES and "ply" in files:
            for fmt in [f for f in files if f != "ply"]:
                (args.out / files.pop(fmt)["file"]).unlink()
        fmt = SESSION_FORMAT.get(name, "ply")
        fmt = fmt if fmt in files else next(iter(files))
        write_session(args.out / f"{name}.jsonl", session(name, facts, fmt))
        entry = {**facts, "files": files, "session": f"{name}.jsonl", "measured": first}
        if mismatch:
            entry["measured_by_format"] = measured
        meshes[name] = entry
        print(f"{name}: {first['faces']} faces, {first['shells']} shells, "
              f"{first['boundary_loops']} boundary loops, {first['non_manifold_edges']} "
              f"non-manifold edges" + (f"  [formats differ: {mismatch}]" if mismatch else ""))

    manifest["meshes"] = dict(sorted(meshes.items()))
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {manifest_path}")

if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])

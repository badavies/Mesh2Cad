#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export test solids as GLB and glTF with Mesh2Cad and check each one in Blender (issue #121).

Usage:

    python3 scripts/blender/check_exports.py --rpc build/src/rpc/mesh2cad-rpc \
        [--blender blender] [--out DIR]

It drives a headless engine (mesh2cad-rpc) to make four solids from the meshes in
examples/test-meshes: an exact box, a plate with a cylindrical hole cut by a boolean, a sphere surfaced
from a noisy scan, and the controller surfaced from its scan. It writes each with solid.export.mesh
and runs check_glb.py on it in Blender, with the solid's size from solid.info. It prints a line per
file and exits with 1 if any check fails. The files stay in --out (default: a temporary folder).
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MESHES = ROOT / "examples" / "test-meshes"
CHECK = Path(__file__).resolve().parent / "check_glb.py"


class Engine:
    """A mesh2cad-rpc process, one JSON-RPC request per line."""

    def __init__(self, rpc: str):
        self.process = subprocess.Popen([rpc], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True)
        self.next = 1

    def __call__(self, method: str, **params) -> dict:
        request = {"jsonrpc": "2.0", "id": self.next, "method": method, "params": params}
        self.next += 1
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        while True:  # skip notifications (progress, document changes)
            reply = json.loads(self.process.stdout.readline())
            if reply.get("id") == request["id"]:
                break
        if "error" in reply:
            raise RuntimeError(f"{method} {params}: {reply['error']['message']}")
        return reply["result"]

    def close(self) -> None:
        self.process.stdin.close()
        self.process.wait(timeout=30)


def surfaced(engine: Engine, mesh: str, faces: int) -> int:
    scan = engine("mesh.import", path=str(MESHES / mesh))["id"]
    cage = engine("quad.remesh", id=scan, target_faces=faces)["id"]
    return engine("surface.build", id=cage, mesh_id=scan)["id"]


def holed_box(engine: Engine) -> int:
    # The cylinder test mesh's side is fitted as a cylinder along z through the origin; a 6 mm
    # radius solid of it drills a true-cylinder hole through a plate around the axis.
    box = engine("primitive.box", size_mm=[60, 50, 10], center=[0, 0, 5], name="holed box")["id"]
    scan = engine("mesh.import", path=str(MESHES / "cylinder_holes.ply"))["id"]
    segments = engine("mesh.segment", id=scan, kinds=["cylinder"], create_primitives=True)
    cylinder = next(r["primitive_id"] for r in segments["regions"] if r["kind"] == "cylinder")
    pin = engine("primitive.solid", id=cylinder, length_mm=100, radius_mm=6)["id"]
    return engine("solid.boolean", id=box, tool_id=pin, operation="cut")["id"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rpc", required=True, help="the mesh2cad-rpc executable")
    parser.add_argument("--blender", default="blender", help="the Blender executable")
    parser.add_argument("--out", help="folder for the exported files")
    args = parser.parse_args()
    out = Path(args.out or tempfile.mkdtemp(prefix="mesh2cad-glb-"))
    out.mkdir(parents=True, exist_ok=True)

    engine = Engine(args.rpc)
    parts = {
        "box": lambda: engine("primitive.box", size_mm=[30, 20, 10], center=[5, 10, 2])["id"],
        "holed_box": lambda: holed_box(engine),
        "sphere": lambda: surfaced(engine, "sphere_noisy.ply", 500),
        "controller": lambda: surfaced(engine, "controller.ply", 2000),
    }
    files = []
    for name, make in parts.items():
        solid = make()
        bounds = engine("solid.info", id=solid)["summary"]["bounds"]
        size = [bounds["max"][k] - bounds["min"][k] for k in range(3)]
        for ext in ("glb", "gltf") if name == "box" else ("glb",):
            path = out / f"{name}.{ext}"
            written = engine("solid.export.mesh", id=solid, path=str(path))
            files.append((path, size, written["triangle_count"]))
    engine.close()

    failed = 0
    for path, size, triangles in files:
        run = subprocess.run(
            [args.blender, "--background", "--factory-startup", "--python", str(CHECK), "--",
             str(path), "--size-mm", *(f"{s:.6f}" for s in size)],
            capture_output=True, text=True)
        lines = [l for l in run.stdout.splitlines() if l.startswith("CHECK_GLB ")]
        report = json.loads(lines[-1][len("CHECK_GLB "):]) if lines else {"passed": False,
                                                                         "failures": [run.stderr]}
        if report.get("triangles") not in (None, triangles):
            report["passed"] = False
            report["failures"].append(f"{report['triangles']} triangles, not {triangles}")
        status = "ok  " if report["passed"] and run.returncode == 0 else "FAIL"
        failed += status == "FAIL"
        print(f"{status} {path.name}: {report.get('size_mm')} mm, {triangles} triangles"
              + "".join(f"\n     {f}" for f in report["failures"]))
    print(f"{len(files) - failed} of {len(files)} files open cleanly in Blender ({out})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

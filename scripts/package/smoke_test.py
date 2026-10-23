#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test an installed Mesh2Cad (issue #122): every program starts and does real work.

Usage:

    python3 scripts/package/smoke_test.py --bin <folder with the programs> \
        --docs <folder with the installed docs and examples> --version <x.y.z> [--no-app]

Checks, each printed as it passes:
- mesh2cad --version names the version;
- a batch session on the installed examples: an exact box and a surfaced sphere, each written as
  STEP (read back as one valid closed solid) and GLB, which exercises Open CASCADE, OpenSubdiv,
  Instant Meshes and oneTBB from the install;
- mesh2cad-rpc answers rpc.hello and a command;
- mesh2cad-mcp answers initialize, lists the tools and calls one;
- mesh2cad-app renders an example mesh to a PNG and quits (--screenshot), unless --no-app; on
  Linux run it under a display (xvfb-run), on Windows it uses Qt's software OpenGL.
- the licence, third-party notes and command reference are installed.

Exits with 1 at the first failure.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 300


def program(bin_dir: Path, name: str) -> str:
    exe = bin_dir / (name + (".exe" if os.name == "nt" else ""))
    if not exe.is_file():
        sys.exit(f"FAIL {exe} is not installed")
    return str(exe)


def check(ok: bool, what: str, detail: str = "") -> None:
    if not ok:
        sys.exit(f"FAIL {what}" + (f": {detail}" if detail else ""))
    print(f"ok   {what}")


def json_lines(text: str) -> list[dict]:
    return [json.loads(line) for line in text.splitlines() if line.strip().startswith("{")]


def talk(exe: str, messages: list[dict]) -> list[dict]:
    """Sends JSON-RPC lines to a stdio server and returns its replies (notifications dropped)."""
    run = subprocess.run([exe], input="".join(json.dumps(m) + "\n" for m in messages),
                         capture_output=True, text=True, timeout=TIMEOUT)
    return [r for r in json_lines(run.stdout) if "id" in r]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--docs", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--no-app", action="store_true")
    args = parser.parse_args()
    examples = args.docs / "examples"
    work = Path(tempfile.mkdtemp(prefix="mesh2cad-smoke-"))

    for doc in ("LICENSE", "THIRD_PARTY.md", "COMMANDS.md", "README.md"):
        check((args.docs / doc).is_file(), f"{doc} is installed")

    cli = program(args.bin, "mesh2cad")
    run = subprocess.run([cli, "--version"], capture_output=True, text=True, timeout=TIMEOUT)
    check(run.returncode == 0 and args.version in run.stdout, "mesh2cad --version",
          run.stdout + run.stderr)

    steps = [
        ("primitive.box", {"size_mm": [30, 20, 10]}),
        ("solid.export", {"id": 1, "path": str(work / "box.step")}),
        ("solid.export.mesh", {"id": 1, "path": str(work / "box.glb")}),
        ("mesh.import", {"path": str(examples / "test-meshes" / "sphere_noisy.ply")}),
        ("quad.remesh", {"id": 2, "target_faces": 300}),
        ("surface.build", {"id": 3, "mesh_id": 2}),
        ("solid.export", {"id": 4, "path": str(work / "sphere.step")}),
        ("solid.export.mesh", {"id": 4, "path": str(work / "sphere.glb")}),
    ]
    session = "".join(json.dumps({"command": c, "params": p}) + "\n" for c, p in steps)
    run = subprocess.run([cli, "batch"], input=session, capture_output=True, text=True,
                         timeout=TIMEOUT)
    results = json_lines(run.stdout)
    check(run.returncode == 0 and len(results) == len(steps), "mesh2cad batch runs the session",
          run.stdout[-2000:] + run.stderr[-2000:])
    for (command, params), reply in zip(steps, results):
        check("result" in reply, f"{command} {json.dumps(params)[:60]}", json.dumps(reply)[:500])
        if command == "solid.export":
            check(reply["result"]["check"]["passed"], "  the STEP file reads back as the solid")
        if command == "solid.export.mesh":
            check(Path(params["path"]).stat().st_size > 100, "  the GLB file is written")

    replies = talk(program(args.bin, "mesh2cad-rpc"), [
        {"jsonrpc": "2.0", "id": 1, "method": "rpc.hello"},
        {"jsonrpc": "2.0", "id": 2, "method": "primitive.box", "params": {"size_mm": [1, 2, 3]}},
    ])
    check(len(replies) == 2 and "result" in replies[0] and "result" in replies[1],
          "mesh2cad-rpc answers", json.dumps(replies)[:500])

    replies = talk(program(args.bin, "mesh2cad-mcp"), [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "smoke", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "primitive_box", "arguments": {"size_mm": [1, 2, 3]}}},
    ])
    tools = [t["name"] for t in replies[1]["result"]["tools"]] if len(replies) > 1 else []
    check(len(replies) == 3 and "solid_export_mesh" in tools
          and replies[2]["result"]["isError"] is False, "mesh2cad-mcp answers",
          json.dumps(replies)[:500])

    if not args.no_app:
        png = work / "cube.png"
        env = dict(os.environ)
        if os.name == "nt":
            env["QT_OPENGL"] = "software"  # CI machines have no OpenGL 3.3 driver
            env["QT_FORCE_STDERR_LOGGING"] = "1"  # a GUI program's warnings go to the debugger
        run = subprocess.run([program(args.bin, "mesh2cad-app"), "--screenshot", str(png),
                              str(examples / "cube.stl")],
                             capture_output=True, text=True, timeout=TIMEOUT, env=env)
        check(run.returncode == 0 and png.is_file() and png.stat().st_size > 1000,
              "mesh2cad-app renders a mesh",
              f"exit {run.returncode:#x}, {'a' if png.is_file() else 'no'} picture\n"
              + run.stdout[-2000:] + run.stderr[-2000:])

    print(f"Mesh2Cad {args.version} works from {args.bin}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

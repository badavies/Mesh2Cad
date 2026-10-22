#!/usr/bin/env python3
"""Fail if the user guide (docs/user) has gone out of date with the commands (issue #123).

Every ```jsonl session in the guide must use commands that exist, with parameters their schemas
accept, and must run to the end with `mesh2cad batch` in a fresh copy of examples/test-meshes, the
way the guide tells readers to run it. Every command named in `backticks` must exist, and every
local link must lead to a file.

    python3 scripts/check_user_guide.py <path to the mesh2cad executable>
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUIDE = ROOT / "docs" / "user"
MESHES = ROOT / "examples" / "test-meshes"

SESSION = re.compile(r"^```jsonl\n(.*?)^```", re.MULTILINE | re.DOTALL)
INLINE = re.compile(r"`([a-z]+(?:\.[a-z]+)+)`")
LINK = re.compile(r"\]\(([^)#\s]+)(?:#[^)]*)?\)")


def check_params(where: str, name: str, params: object, schema: dict) -> list[str]:
    if not isinstance(params, dict):
        return [f"{where}: {name} params are not an object"]
    errors = []
    properties = schema.get("properties", {})
    for key in schema.get("required", []):
        if key not in params:
            errors.append(f"{where}: {name} is missing the required parameter '{key}'")
    for key, value in params.items():
        if key not in properties:
            errors.append(f"{where}: {name} has no parameter '{key}'")
        elif "enum" in properties[key] and value not in properties[key]["enum"]:
            errors.append(f"{where}: {name} '{key}' must be one of {properties[key]['enum']}")
    return errors


def run_session(cli: str, where: str, lines: list[str]) -> list[str]:
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp) / "test-meshes"
        shutil.copytree(MESHES, work)
        done = subprocess.run([cli, "batch"], input="\n".join(lines) + "\n", cwd=work,
                              capture_output=True, text=True, check=False)
    results = [json.loads(line) for line in done.stdout.splitlines() if line.strip()]
    for n, result in enumerate(results):
        if not result.get("ok"):
            message = result.get("error", {}).get("message", done.stderr.strip())
            return [f"{where}, line {n + 1}: {message}"]
    if done.returncode != 0 or len(results) != len(lines):
        return [f"{where}: ran {len(results)} of {len(lines)} lines, exit {done.returncode}"]
    return []


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    cli = str(Path(sys.argv[1]).resolve())
    listed = subprocess.run([cli, "list"], capture_output=True, text=True, check=True)
    schemas = {c["name"]: c["params_schema"] for c in json.loads(listed.stdout)["result"]["commands"]}
    groups = {name.split(".")[0] for name in schemas}

    errors: list[str] = []
    sessions = 0
    for page in sorted(GUIDE.glob("*.md")):
        text = page.read_text(encoding="utf-8")
        for target in LINK.findall(text):
            if "://" not in target and not (page.parent / target).exists():
                errors.append(f"{page.name}: the link to {target} leads nowhere")
        for name in set(INLINE.findall(text)):
            if name.split(".")[0] in groups and name not in schemas:
                errors.append(f"{page.name}: there is no command `{name}`")
        for block in SESSION.finditer(text):
            first = text.count("\n", 0, block.start()) + 2
            where = f"{page.name}:{first}"
            lines = [line for line in block.group(1).splitlines() if line.strip()]
            bad = []
            for n, line in enumerate(lines):
                request = json.loads(line)
                name = request.get("command")
                if name not in schemas:
                    bad.append(f"{where}, line {n + 1}: there is no command {name}")
                else:
                    bad += check_params(f"{where}, line {n + 1}", name,
                                        request.get("params", {}), schemas[name])
            errors += bad or run_session(cli, where, lines)
            sessions += 1

    for error in errors:
        print(error, file=sys.stderr)
    print(f"{sessions} sessions in {len(list(GUIDE.glob('*.md')))} pages, {len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

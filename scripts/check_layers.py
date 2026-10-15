#!/usr/bin/env python3
"""Fail if a source file includes a module it must not depend on (ARCHITECTURE.md sections 4 and 9).

Modules are the top-level folders of src/. Public headers are included as "m2c/<module>/File.h".
Kernel libraries (CGAL, Open CASCADE, OpenSubdiv) may only be included below src/adapters/.
Run from anywhere:  python3 scripts/check_layers.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# module -> modules it may include (itself is always allowed).
ALLOWED: dict[str, set[str]] = {
    "core": set(),
    "adapters": {"core"},
    "io": {"core"},
    "document": {"core"},
    "pipeline": {"core", "adapters"},
    "commands": {"core", "document", "io", "pipeline"},
    "app": {"core", "document", "commands", "rpc"},
    "cli": {"core", "document", "commands"},
    "rpc": {"core", "document", "commands"},
    "mcp": {"core", "document", "commands", "rpc"},
}

KERNEL_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](CGAL/|opensubdiv/|opencascade/|TopoDS|BRep|Standard_|gp_)', re.M)
PROJECT_INCLUDE = re.compile(r'^\s*#\s*include\s*"m2c/([a-z_]+)/', re.M)
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".cxx", ".ipp"}


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    errors: list[str] = []

    for module_dir in sorted(p for p in src.iterdir() if p.is_dir()):
        module = module_dir.name
        if module not in ALLOWED:
            errors.append(f"src/{module}: unknown module; add it to scripts/check_layers.py")
            continue
        allowed = ALLOWED[module] | {module}
        for path in sorted(module_dir.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(src.parent)
            if module != "adapters" and KERNEL_INCLUDE.search(text):
                errors.append(f"{rel}: kernel library header included outside src/adapters")
            for dep in PROJECT_INCLUDE.findall(text):
                if dep not in allowed:
                    errors.append(f"{rel}: module '{module}' must not include module '{dep}'")

    for error in errors:
        print(error, file=sys.stderr)
    if not errors:
        print("layering ok")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

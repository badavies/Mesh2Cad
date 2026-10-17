#!/usr/bin/env python3
"""Fail if a source file includes a module it must not depend on (ARCHITECTURE.md sections 4 and 9).

Modules are the top-level folders of src/. Public headers are included as "m2c/<module>/File.h".
Kernel libraries (CGAL, Open CASCADE, OpenSubdiv) may only be included below src/adapters/.
Run from anywhere:  python3 scripts/check_layers.py

With --baseline it instead prints, as Markdown, the module dependencies actually used, the lines of
code per module and the TODO count: the figures section 11 rule 14 compares at each milestone gate
(docs/BASELINE.md).
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


def baseline(src: Path) -> int:
    """Prints the module graph, module sizes and TODO count for docs/BASELINE.md."""
    root = src.parent
    graph: dict[str, set[str]] = {}
    sizes: dict[str, tuple[int, int]] = {}
    for module_dir in sorted(p for p in src.iterdir() if p.is_dir()):
        module = module_dir.name
        deps: set[str] = set()
        files = lines = 0
        for path in module_dir.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            deps.update(d for d in PROJECT_INCLUDE.findall(text) if d != module)
            files += 1
            lines += sum(1 for line in text.splitlines() if line.strip())
        graph[module] = deps
        sizes[module] = (files, lines)
    todo = re.compile(r"\b(TODO|FIXME|XXX|HACK)\b")
    todos = 0
    for folder in ("src", "tests", "scripts", "cmake"):
        for path in (root / folder).rglob("*"):
            if path.resolve() == Path(__file__).resolve():
                continue  # this script names the markers
            if path.is_file() and path.suffix in SOURCE_SUFFIXES | {".py", ".cmake", ".txt"}:
                todos += len(todo.findall(path.read_text(encoding="utf-8", errors="replace")))

    print("| Module | Includes | Files | Non-blank lines |")
    print("| --- | --- | --- | --- |")
    for module in graph:
        files, lines = sizes[module]
        print(f"| {module} | {', '.join(sorted(graph[module])) or '-'} | {files} | {lines} |")
    print(f"| total | | {sum(f for f, _ in sizes.values())} | {sum(n for _, n in sizes.values())} |")
    print()
    print(f"TODO, FIXME, XXX and HACK markers in src, tests, scripts and cmake: {todos}")
    return 0


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    if "--baseline" in sys.argv[1:]:
        return baseline(src)
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

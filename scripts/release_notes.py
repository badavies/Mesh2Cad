#!/usr/bin/env python3
"""Print a version's section of CHANGELOG.md, the notes of its GitHub release (issue #124).

    python3 scripts/release_notes.py v1.0.0

Fails unless the version is the one in CMakeLists.txt and its changelog section has a date, so a
tag cannot be released before the version and the changelog are ready.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    version = sys.argv[1].removeprefix("refs/tags/").removeprefix("v")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    project = re.search(r"project\(Mesh2Cad\s+VERSION\s+(\S+)", cmake)
    if project is None or project.group(1) != version:
        print(f"CMakeLists.txt has version {project and project.group(1)}, not {version}",
              file=sys.stderr)
        return 1

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    heading = re.search(rf"^## \[{re.escape(version)}\] - (.+)$", changelog, re.MULTILINE)
    if heading is None:
        print(f"CHANGELOG.md has no section for {version}", file=sys.stderr)
        return 1
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", heading.group(1).strip()):
        print(f"CHANGELOG.md's section for {version} is dated '{heading.group(1)}', "
              "not YYYY-MM-DD", file=sys.stderr)
        return 1
    rest = changelog[heading.end():]
    end = re.search(r"^(## |\[[^\]]+\]: )", rest, re.MULTILINE)
    print(rest[: end.start() if end else len(rest)].strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())

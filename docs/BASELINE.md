# Milestone baseline

Section 11 rule 14 of `ARCHITECTURE.md`: at each milestone gate, compare the module dependency graph, the module sizes and the TODO count with the previous gate. Regenerate the tables with `python3 scripts/check_layers.py --baseline` and add a new section; keep the old ones.

## M1 gate (16 October 2026)

Measured on main with the M1 work merged (issues #7 to #11). "Includes" lists the project modules a module's files actually include, which is narrower than what section 4 allows.

| Module | Includes | Files | Non-blank lines |
| --- | --- | --- | --- |
| adapters | core | 8 | 665 |
| app | commands, core, rpc | 11 | 2471 |
| cli | commands, document | 1 | 127 |
| commands | core, document, io, pipeline | 13 | 1640 |
| core | - | 17 | 1011 |
| document | core | 2 | 401 |
| io | core | 12 | 1219 |
| mcp | commands, core, rpc | 3 | 266 |
| pipeline | adapters, core | 12 | 917 |
| rpc | commands, core | 5 | 444 |
| total | | 84 | 9161 |

Tests: 19 files, 2745 non-blank lines. TODO, FIXME, XXX and HACK markers in `src`, `tests`, `scripts` and `cmake`: 0.

What to watch at the M2 gate: `app` is the largest module and should not grow geometry code; `io` gains the `adapters` edge only for BREP in the project file; a new `adapters/occt` must not include `adapters/cgal`.

## M2 gate (17 October 2026)

Measured on the M2 branch (issues #14 to #23, pull request #24). Since M1: `adapters` gained Open CASCADE, OpenSubdiv and Instant Meshes (Instant Meshes itself is fetched at configure time and not counted); `io` now includes `adapters`, for BREP in the project file and for STEP, as planned; `adapters/occt` does not include `adapters/cgal`.

| Module | Includes | Files | Non-blank lines |
| --- | --- | --- | --- |
| adapters | core | 20 | 2106 |
| app | commands, core, rpc | 12 | 2753 |
| cli | commands, document | 1 | 127 |
| commands | core, document, io, pipeline | 16 | 2033 |
| core | - | 24 | 1663 |
| document | core | 2 | 460 |
| io | adapters, core | 15 | 1448 |
| mcp | commands, core, rpc | 3 | 266 |
| pipeline | adapters, core | 18 | 1398 |
| rpc | commands, core | 5 | 444 |
| total | | 116 | 12698 |

Tests: 23 files, 3626 non-blank lines. TODO, FIXME, XXX and HACK markers in `src`, `tests`, `scripts` and `cmake`: 0.

`app` grew by 282 lines, all drawing (solids, cage edges, deviation colours and legend); it still holds no geometry code. What to watch at the M3 gate: patch merging and deviation-driven refinement belong in `pipeline` and `adapters/osd`, not in `commands`; `adapters/osd/Subdivision.cpp` is the largest adapter file and should be split before it grows further.

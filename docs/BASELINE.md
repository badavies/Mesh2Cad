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

## v1.0.0 release candidate (22 October 2026)

Measured on the M5 branch (issues #120 to #124), after M3 and M4, which added no baseline of their own. Since M2: `app`, `cli`, `mcp` and `rpc` now include `document` directly, which section 4 allows; no other edge is new, and `adapters/occt` still does not include `adapters/cgal`.

| Module | Includes | Files | Non-blank lines |
| --- | --- | --- | --- |
| adapters | core | 39 | 4518 |
| app | commands, core, document, rpc | 12 | 3657 |
| cli | commands, core, document | 1 | 130 |
| commands | core, document, io, pipeline | 20 | 3974 |
| core | - | 36 | 3029 |
| document | core | 2 | 528 |
| io | adapters, core | 20 | 1997 |
| mcp | commands, core, document, rpc | 3 | 517 |
| pipeline | adapters, core | 33 | 4373 |
| rpc | commands, core, document | 5 | 580 |
| total | | 171 | 23303 |

Tests: 40 files, 9962 non-blank lines. TODO, FIXME, XXX and HACK markers in `src`, `tests`, `scripts` and `cmake`: 0.

`pipeline` and `adapters` grew the most, with the M3 and M4 geometry, as planned; `commands` grew by 1,941 lines for 14 new commands, all parameter handling and reports. `app` grew by 904 lines, mostly viewport drawing and tool forms. What to watch after v1.0: `adapters` and `pipeline` hold most of the code, and any 1.x change to the command API must stay additive.

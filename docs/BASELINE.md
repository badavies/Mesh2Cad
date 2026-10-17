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

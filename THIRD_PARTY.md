# Third-party software

Mesh2Cad is licensed GPL-3.0-or-later (see [`LICENSE`](LICENSE)). Every dependency must be compatible with that licence and must have a row here **and** a decision record in `docs/adr/` before it is added to `vcpkg.json` (AI guideline 5).

Dependencies come from vcpkg when a vcpkg preset is used. Otherwise `cmake/Mesh2CadDependencies.cmake` fetches the pinned versions below from GitHub (ADR 0020).

## In use

| Dependency | Version | Licence | Used for | Where |
| --- | --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | vcpkg baseline (3.12.x); fallback v3.12.0 | MIT | JSON for the command engine and CLI | `src/commands`, `src/cli` |
| [Catch2](https://github.com/catchorg/Catch2) | vcpkg baseline (v3); fallback v3.16.1 | BSL-1.0 | Unit and command tests | `tests/` (not shipped) |
| [CGAL](https://www.cgal.org) | 5.6+ from vcpkg/system; fallback 6.0.1 | GPL-3.0-or-later (PMP, Alpha Wrap); LGPL-3.0 (kernel) | Repair, hole filling, simplification, remeshing, shrink-wrap (ADR-004, ADR-022) | `src/adapters/cgal` only |
| [Boost](https://www.boost.org) (headers) | from vcpkg/system with CGAL; fallback 1.86.0 | BSL-1.0 | Required by CGAL | via CGAL |
| [Eigen](https://eigen.tuxfamily.org) | 3.3+ from vcpkg/system; fallback 3.4.0 | MPL-2.0 | Plane fitting, principal axes, CGAL fairing (ADR-022) | `src/pipeline`, `src/adapters/cgal` |
| [miniz](https://github.com/richgel999/miniz) | vcpkg baseline; fallback 3.0.2 | MIT | Project file (zip) (ADR-017) | `src/io` |
| [Qt 6](https://www.qt.io) (Widgets, OpenGL, OpenGLWidgets, Network) | 6.4+ from the Qt installer, vcpkg (`app` feature) or the system; never fetched | LGPL-3.0 (dynamically linked) | Desktop app and viewport (ADR-008, ADR-023) | `src/app` only |

## Build tooling (not distributed)

| Tool | Licence | Used for |
| --- | --- | --- |
| [vcpkg](https://github.com/microsoft/vcpkg) | MIT | Dependency management; the baseline in `vcpkg.json` pins versions |
| CMake, Ninja | BSD-3-Clause, Apache-2.0 | Build |

## Planned (not yet used; each needs its decision record before first use)

| Dependency | Licence | Milestone | Purpose |
| --- | --- | --- | --- |
| oneTBB | Apache-2.0 | When profiling needs it (not needed in M1) | Parallelism (`parallel_deterministic_reduce`) |
| json-schema-validator | MIT | When schemas are enforced (not needed in M1) | Command parameter validation |
| spdlog | MIT | When structured logs are needed (not needed in M1) | Logging |
| Open CASCADE (OCCT) | LGPL-2.1 with exception | M2 | CAD kernel, STEP export |
| OpenSubdiv | Apache-2.0 | M2 | Subdivision limit stencils |
| QuadWild or Instant Meshes | GPL-3.0 / BSD-3-Clause | M2 | Quad layout (ADR-009, after the spike) |

OBJ and PLY are read by our own parsers (ADR-021), so tinyobjloader and happly are not used.

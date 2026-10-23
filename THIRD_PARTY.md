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
| [Open CASCADE (OCCT)](https://github.com/Open-Cascade-SAS/OCCT) | 7.6+ from vcpkg/system; fallback V8_0_1 built from source | LGPL-2.1 with the OCCT exception (dynamically linked) | B-spline faces, sewing, solid checks, STEP AP242 export (ADR-003, ADR-026) | `src/adapters/occt` only |
| [OpenSubdiv](https://github.com/PixarAnimationStudios/OpenSubdiv) | 3.5 from vcpkg/system; fallback v3_5_0 built from source | Apache-2.0 (modified) | Catmull-Clark limit stencils and patch extraction (ADR-015, ADR-026) | `src/adapters/osd` only |
| [oneTBB](https://github.com/uxlfoundation/oneTBB) | 2021.5+ from vcpkg/system; fallback v2022.3.0 built as a subproject | Apache-2.0 | Deterministic parallel loops (ADR-036) | `src/core` (`Parallel.cpp`) only |
| [Instant Meshes](https://github.com/wjakob/instant-meshes) (field and extraction sources) | commit 7b31608 | BSD-3-Clause | Quad layout (ADR-009) | `src/adapters/quad` only |
| [pcg32](https://github.com/wjakob/pcg32) | commit 0ef13e6 | Apache-2.0 | Random numbers inside Instant Meshes | via Instant Meshes |
| [dset](https://github.com/wjakob/dset) | commit 7967ef0 | zlib | Union-find inside Instant Meshes | via Instant Meshes |
| [Qt 6](https://www.qt.io) (Widgets, OpenGL, OpenGLWidgets, Network) | 6.4+ from the Qt installer, vcpkg (`app` feature) or the system; never fetched | LGPL-3.0 (dynamically linked) | Desktop app and viewport (ADR-008, ADR-023) | `src/app` only |

## Build tooling (not distributed)

| Tool | Licence | Used for |
| --- | --- | --- |
| [vcpkg](https://github.com/microsoft/vcpkg) | MIT | Dependency management; the baseline in `vcpkg.json` pins versions |
| CMake, Ninja | BSD-3-Clause, Apache-2.0 | Build |
| CPack (part of CMake) | BSD-3-Clause | Installers and archives (`cmake/Mesh2CadPackaging.cmake`) |
| [NSIS](https://nsis.sourceforge.io) | zlib (its installer stub is part of the Windows installer) | Windows installer |
| [install-qt-action](https://github.com/jurplel/install-qt-action) and aqtinstall | MIT | A prebuilt Qt for the Windows installer build in CI |
| [Mesa](https://www.mesa3d.org) for Windows ([mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 24.3.4, pinned by hash) | MIT | Software OpenGL for the app smoke test on the GPU-less Windows CI machine; not shipped |

## Planned (not yet used; each needs its decision record before first use)

| Dependency | Licence | Milestone | Purpose |
| --- | --- | --- | --- |
| json-schema-validator | MIT | When schemas are enforced (not needed in M1) | Command parameter validation |
| spdlog | MIT | When structured logs are needed (not needed in M1) | Logging |
| QuadWild (bi-MDF) | GPL-3.0 | M3, to revisit (ADR-009) | Feature-aligned quad layout |

OBJ and PLY are read by our own parsers (ADR-021), so tinyobjloader and happly are not used.

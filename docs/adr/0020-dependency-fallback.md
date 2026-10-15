# 0020. FetchContent fallback when vcpkg is not configured

- Status: Accepted
- Date: 2026-10-14

## Context
The first CLion load used CLion's default "Debug" profile, not a vcpkg preset. `find_package()` could not find nlohmann/json or Catch2, the CMake configure failed, no targets existed, and every run configuration reported that its executable could not be found.

## Decision
`cmake/Mesh2CadDependencies.cmake` declares each dependency with `FetchContent_Declare(... FIND_PACKAGE_ARGS ...)`. CMake first tries `find_package()` (vcpkg presets, system packages); only if that fails does it clone the same pinned tag from GitHub. vcpkg remains the primary mechanism and the one CI uses.

Test discovery stays in Catch2's default post-build mode. `DISCOVERY_MODE PRE_TEST` was tried and rejected: CLion runs two `ctest` processes at once, both rewrite the discovered test list, and every test was registered twice. Static linking of the MinGW runtime (`-static`) was also tried and rejected: with CLion's bundled MinGW (GCC 15.2) it fails to link (`__intrinsic_setjmpex`, `__ms_vsnprintf` undefined). CLion puts the MinGW `bin` folder on the PATH for builds and runs, so the runtime DLLs are found without it.

## Alternatives considered
- Require vcpkg for every configure: fragile first-run experience, which is what failed.
- Vendor the sources into the repository: bloats the repository and duplicates vcpkg.

## Consequences
- Any new dependency needs both a vcpkg entry and a `FetchContent_Declare` with the same version, or a note in its ADR explaining why it is vcpkg-only (likely for CGAL, OCCT and Qt, which are too large to fetch).
- The versions in `vcpkg.json` (via the baseline) and in `Mesh2CadDependencies.cmake` can drift; check them at each milestone gate.
- The fallback needs git and network access on the first configure.

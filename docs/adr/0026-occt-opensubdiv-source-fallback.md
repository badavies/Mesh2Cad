# 0026. Open CASCADE and OpenSubdiv from vcpkg or the system, else built from source at configure time

- Status: Accepted (refines ADR-003, ADR-015 and ADR-020)
- Date: 2026-10-17

## Context
M2 needs Open CASCADE (B-spline faces, sewing, solid checks, STEP AP242 export; ADR-003) and OpenSubdiv (Catmull-Clark limit stencils and patch extraction; ADR-015). Both are compiled libraries, unlike CGAL, so the header-only fallback of ADR-022 does not apply, yet ADR-020 requires the first CLion load without vcpkg to configure and build. The supported environments are the vcpkg presets (CI on Linux and Windows), Ubuntu packages (the CI app job) and CLion with Qt's MinGW 13.1 and no vcpkg. Roadmap issue: #15.

## Decision
- **vcpkg presets:** `opencascade` (8.0.1 at the current baseline, default features off, so no FreeType or Fontconfig) and `opensubdiv` (3.5.0) in `vcpkg.json`. vcpkg builds OCCT with its visualisation toolkits, which need the system OpenGL and X11 headers on Linux (installed in CI).
- **System packages:** `find_package(OpenCASCADE CONFIG)` finds Ubuntu's 7.6.3 (`libocct-data-exchange-dev`). OpenSubdiv is found with `find_path`/`find_library` (`libosd-dev`) rather than its package config, because Ubuntu's config names a static library the package does not ship.
- **Otherwise, at configure time:** the pinned release (OCCT `V8_0_1`, OpenSubdiv `v3_6_1`) is fetched with FetchContent and built and installed into `<build>/_deps/<name>-install` with `execute_process`, then found from there. OCCT is built as shared libraries with only the foundation, modelling and STEP toolkits (`TKDESTEP`), no visualisation, Draw or optional third-party libraries; OpenSubdiv as a static CPU-only library. A stamp file keyed on the options and compiler skips the build on later configures. With MSVC the build runs with `_CL_=/WX-` in its environment, so a dependency that turns on `/WX` for itself (OpenSubdiv does) cannot stop the configure on a warning from a newer compiler.
- The source build uses this build's generator and compilers; MSVC Debug builds get a Debug OCCT (runtimes cannot be mixed), everything else gets Release. MinGW adds `-Wa,-mbig-obj`; Linux installs with an `$ORIGIN` run path.
- Windows executables get every runtime DLL of their imported targets copied next to them before Catch2's test discovery runs (`m2c_copy_runtime`).
- The adapters compile against OCCT 7.6 and 8.0: the STEP toolkits are `TKSTEP*` up to 7.7 and `TKDESTEP` from 7.8; only API common to both is used.
- Kernel headers stay in `src/adapters` (`scripts/check_layers.py` now rejects any `.hxx` include elsewhere).

## Alternatives considered
- **ExternalProject at build time:** `find_package` needs the installed config during configure, so this would need a superbuild; the project would no longer open directly in CLion.
- **FetchContent with `add_subdirectory`:** OCCT's build is not designed to be a subproject (global options, hundreds of targets, its own install layout), and OpenSubdiv's adds GPU back-ends.
- **Require vcpkg for M2:** the first-run failure ADR-020 was written to avoid.
- **Static OCCT:** larger executables and very long links for every test binary; shared libraries are linked once.

## Consequences
- The first configure without vcpkg or system packages builds OCCT, which takes a long time (25 minutes on 4 cores on Linux; MinGW is similar); CLion shows the build output while it runs. Later configures reuse the install.
- Windows MinGW builds carry about 30 OCCT DLLs next to each executable.
- Upgrading OCCT or OpenSubdiv means changing the tag here and the vcpkg baseline together.

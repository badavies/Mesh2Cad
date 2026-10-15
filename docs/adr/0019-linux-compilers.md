# 0019. Linux builds use GCC 13+; Clang needs libc++

- Status: Accepted
- Date: 2026-10-14

## Context
ARCHITECTURE.md listed "Clang 17+" as a supported compiler. While setting up M0, Clang 18 on Ubuntu 24.04 failed to compile `<expected>`: it uses libstdc++ 13 by default, and libstdc++ 13 only defines `std::expected` when the compiler reports `__cpp_concepts >= 202002L`, which Clang does not do before version 20. `std::expected` is the project's error type (ADR-012).

## Decision
- Supported: MSVC 19.40+ (Visual Studio 2022 17.10), GCC 13+, and Clang only together with libc++ 16+ (or a newer libstdc++ that provides `std::expected` to that Clang).
- Linux presets and CI use GCC. There are no Clang presets until someone needs one and a Clang + libc++ CI job exists to keep it working (vcpkg libraries would then have to be built against libc++ too).
- `cmake/Mesh2CadBuild.cmake` compiles a one-line `std::expected` test at configure time and stops with an explanation if it fails.

## Alternatives considered
- A `std::expected` polyfill (tl::expected): adds a dependency to work around a toolchain issue.
- Raising the minimum to Clang 20: not available on current Ubuntu LTS.

## Consequences
CLion on Linux uses the GCC toolchain. Windows uses MSVC. Section 3 and section 9 of ARCHITECTURE.md are updated to match.

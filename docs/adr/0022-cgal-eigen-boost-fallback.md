# 0022. CGAL with Eigen; fetched CGAL and Boost when not installed

- Status: Accepted (refines ADR-004 and ADR-020)
- Date: 2026-10-15

## Context
M1 uses CGAL's Polygon Mesh Processing (repair, hole filling, simplification, isotropic remeshing, alpha wrap) and Eigen (plane fit, principal axes, hole fairing). ADR-020 expected CGAL to be vcpkg-only because it is large, but the first CLion load on a machine without vcpkg must still configure.

## Decision
- `find_package(CGAL 5.6)` first (vcpkg or system). If it is missing, fetch the official `CGAL-6.0.1-library.tar.xz` release and the `boost-1.86.0-b2-nodocs.tar.xz` release (headers only, about 50 MB), both pinned by SHA-256, and use them header-only with `CGAL_DISABLE_GMP` (only the EPICK kernel is used).
- Eigen 3.4.0 comes from vcpkg or the system, else from the pinned GitLab archive.
- Kernel headers stay inside `src/adapters/cgal` (checked by `scripts/check_layers.py`). The adapter converts to and from `TriMesh` and turns every CGAL exception into an `Error`.
- The code builds against CGAL 5.6 and 6.0.

## Alternatives considered
- Require vcpkg for CGAL: the first-run failure ADR-020 was written to avoid.
- Vendor CGAL: far too large for the repository.

## Consequences
- The first configure without vcpkg downloads about 60 MB and takes a few minutes; later configures use the cache in the build folder.
- Without GMP, exact-construction kernels (EPECK) are unavailable; any stage that needs them must add GMP/MPFR through vcpkg first (new decision).
- MinGW needs `-Wa,-mbig-obj` and MSVC `/bigobj` for the CGAL translation units (set in `cmake/Mesh2CadDependencies.cmake`).

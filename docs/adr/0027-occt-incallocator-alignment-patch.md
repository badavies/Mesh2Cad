# 0027. vcpkg's Open CASCADE 8.0.1 carries a patch that aligns NCollection_IncAllocator

- Status: Proposed (refines ADR-026)
- Date: 2026-10-19

## Context
Trimming fitted primitives into a solid (#48) triangulates the result with Open CASCADE's `BRepMesh_IncrementalMesh` for display. In OCCT 8.0.1 `NCollection_IncAllocator::AllocateOptimal` bump-allocates the requested size without rounding it, although the class promises aligned blocks; 7.6 rounded every size up to 8 bytes, and master still does not. BRepMesh keeps its data model in such an allocator, and a wire with an odd number of edges (a hole bounded by one circle, for example) allocates 4 bytes of edge orientations. Every face allocated after it sits at an address that is 4 bytes off 8-byte alignment. This is undefined behaviour; x86-64 tolerates it, but the `linux-gcc-sanitize` CI job stops on it, because OCCT's Debug libraries call the instrumented copy of `TopoDS_Shape`'s inline copy constructor. Three trim stage tests fail there; Release builds hide the same misalignment by inlining.

## Decision
The vcpkg presets build Open CASCADE from an overlay port, `cmake/vcpkg-ports/opencascade`, listed in `vcpkg.json`. It is vcpkg's own port at the baseline (portfile, manifest and patches 0001 to 0005 unchanged apart from `port-version`), plus `0006-align-incallocator-allocations.patch`, which rounds each allocation up to 8 bytes. The overlay is removed once the vcpkg baseline ships an OCCT with the fix.

## Alternatives considered
- **Suppress the report:** make alignment checks recoverable and suppress them in Open CASCADE headers. UBSan ignores suppressions for non-recoverable checks, so this weakens the sanitizer flags for the whole project and needs `UBSAN_OPTIONS` wherever tests run.
- **Own triangulation of trimmed faces:** avoids BRepMesh, but adds a few hundred lines of geometry code to work around a one-line bug, and STEP and BREP import still mesh with BRepMesh.
- **Turn off `-fsanitize=alignment`:** hides our own misaligned accesses too.

## Consequences
- Each CI job and each local vcpkg build rebuilds Open CASCADE once, because the port's hash changed; the cache keeps it afterwards.
- Raising the vcpkg baseline means comparing the overlay with vcpkg's new port, or deleting it once upstream is fixed.
- System packages (Ubuntu's 7.6.3) are unaffected. The configure-time source build of ADR-026 is not patched: it builds Release on GCC and MinGW, where the misalignment is not reported, and the sanitizer job always uses vcpkg.

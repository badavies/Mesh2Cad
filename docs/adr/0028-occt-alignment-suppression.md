# 0028. The sanitizer build drops alignment reports inside Open CASCADE's headers

- Status: Accepted (supersedes ADR-027)
- Date: 2026-10-19

## Context
Trimming fitted primitives into a solid (#48) triangulates the result with Open CASCADE's `BRepMesh_IncrementalMesh` for display. In OCCT 8.0.1 `NCollection_IncAllocator::AllocateOptimal` bump-allocates the requested size without rounding it, although the class promises aligned blocks; 7.6 rounded every size up to 8 bytes, and master still does not. BRepMesh keeps its data model in such an allocator, and a wire with an odd number of edges (a hole bounded by one circle, for example) allocates 4 bytes of edge orientations. Every face allocated after it sits at an address that is 4 bytes off 8-byte alignment. This is undefined behaviour that x86-64 tolerates. The `linux-gcc-sanitize` CI job stops on it, because OCCT's Debug libraries call the instrumented copy of `TopoDS_Shape`'s inline copy constructor, defined in an Open CASCADE header; three trim stage tests failed there. Release builds hide the same misalignment by inlining.

## Decision
- With `MESH2CAD_SANITIZE`, alignment reports are recoverable (`-fsanitize-recover=alignment`); every other UBSan check stays fatal at compile time. UBSan ignores suppressions for fatal checks, so this is needed for the next point.
- `cmake/ubsan-suppressions.txt` drops alignment reports located in Open CASCADE's headers (`alignment:/opencascade/`) and nothing else.
- The `linux-gcc-sanitize` test preset sets `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=...`, so any other alignment report still stops the test. The nightly fuzz step sets `halt_on_error=1` the same way.
- The suppression is removed once the vcpkg baseline ships an Open CASCADE whose allocator aligns.

## Alternatives considered
- **Patch Open CASCADE through a vcpkg overlay port (ADR-027):** fixes the cause with a one-line patch, but the project carries a copy of vcpkg's port to compare on every baseline update, and every CI job and local vcpkg build rebuilds Open CASCADE. The overlay is removed.
- **Own triangulation of trimmed faces:** avoids BRepMesh, but adds a few hundred lines of geometry code to work around a one-line bug, and STEP and BREP import still mesh with BRepMesh.
- **Turn off `-fsanitize=alignment`:** hides our own misaligned accesses too.

## Consequences
- An alignment bug of our own inside an Open CASCADE header call, on an object we misaligned, would be hidden; our code allocates OCCT objects only with `new` and on the stack.
- A sanitized executable run outside `ctest --preset linux-gcc-sanitize` reports alignment errors but carries on, unless `UBSAN_OPTIONS` contains `halt_on_error=1`.
- Raising the vcpkg baseline past a fixed Open CASCADE means deleting the suppression, the recover flag and the `UBSAN_OPTIONS` entries.

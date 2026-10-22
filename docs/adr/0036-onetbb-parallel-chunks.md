# 0036. oneTBB for deterministic parallel loops

- Status: Accepted
- Date: 2026-10-22

## Context
Issue #31: every stage ran on one thread. Deviation to a solid projects each scan point onto a B-spline face with Open CASCADE, and the cage fit finds every sample's closest point on the limit surface in each round. Both are independent work per point. ARCHITECTURE.md section 3 names oneTBB for parallelism, and section 9 requires results that are identical from run to run.

Measured on a 4-core machine (Release), 180,000 points against a 3,000-quad sphere solid: deviation to the solid took 35.5 s on one thread and 9.5 s on four; deviation to the mesh 2.4 s and 0.8 s. The cage fit's closest-point search is about a tenth of a fit; most of the rest is building and solving the least-squares system, which this change leaves alone.

## Decision
- oneTBB (Apache-2.0) is a dependency: `tbb` in `vcpkg.json`, a system package elsewhere (Ubuntu's `libtbb-dev`, already installed for Open CASCADE), or v2022.3.0 built as a static library in a CMake subproject when neither is found (oneTBB discourages static builds because two copies would run two schedulers; only `core` links it, so there is one). Only `core` links it, privately.
- One entry point, `parallelChunks(count, chunk, fn)` in `m2c/core/Parallel.h`: fixed chunks `[0, chunk)`, `[chunk, 2 chunk)`, ..., one TBB task each. The chunks do not depend on the thread count or on how TBB splits work.
- Each item writes its own output slot. Sums, maxima, percentiles and the fit's least-squares rows are made afterwards, serially and in index order. There are no parallel reductions, so no floating-point sum depends on scheduling.
- Work that keeps state does so per chunk: deviation to a solid gives each chunk its own `ShapeAnalysis_Surface` projectors, because a projector keeps state between projections and may not be shared between threads.
- Parallel now: the closest-point search of `fitCage`, `deviationToMesh` and `occt::signedDistances` (so `deviationToSolid` and `fitFillet`'s measurements). Each chunk checks the cancel token first (#31), and the caller fails with Cancelled after the loop.
- `ThreadLimit` caps the threads while it lives (`tbb::global_control`), for front ends and for tests that compare one thread with many.

## Alternatives considered
- `std::thread` chunks of our own: no dependency, but a thread pool and work stealing are what oneTBB is for, and CGAL and Open CASCADE can use the same scheduler.
- C++17 parallel algorithms (`std::execution::par`): libstdc++ implements them with oneTBB anyway, MSVC with its own pool, and they give no control over chunks.
- OpenMP: compiler flags differ per toolchain, and MinGW and MSVC support differs; chunking is not fixed.
- `tbb::parallel_reduce` for the sums: its split points vary between runs, so sums would differ in the last bits. `parallel_deterministic_reduce` would be the choice if a reduction ever needs to be parallel.

## Consequences
- Results are bit-identical for any thread count; a stage test checks this for the fit and both deviations with one thread against all of them.
- Deviation to a solid may differ in the last bits from the single-threaded version before this change, since each chunk now starts its projectors afresh. Golden metrics have tolerances far above that.
- A Windows build without vcpkg builds oneTBB from source on first configure. With MinGW, oneTBB's profiling hooks are turned off (`TBB_USE_PROFILING_TOOLS=0`), as its headers do not compile with them there.
- The least-squares solve now dominates the cage fit. Making it faster (a better preconditioner, a sparse product in parallel) is separate work.

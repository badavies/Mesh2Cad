# 0024. Lasso selection by CPU projection for M1 (amends ADR-018)

- Status: Accepted
- Date: 2026-10-15

## Context
ADR-018 planned area selection through an R32UI id buffer rendered by the viewport. That makes the result depend on the UI's renderer, so an agent or a CLI script could not run the same lasso, and the command would need pixels rather than plain data.

## Decision
`select.lasso` takes the polygon in normalised device coordinates, the view-projection matrix and the eye position. It selects faces whose centres project inside the polygon and face the camera; with `visible_only` it also casts a ray from the eye to each candidate centre against the mesh's BVH and skips hidden faces. The viewport sends exactly these values, so the UI, the CLI and agents get the same selection from the same input.

## Alternatives considered
- R32UI id buffer (ADR-018): exact per pixel and fast on huge meshes, but tied to the GPU and not reproducible from the command log.

## Consequences
- Cost grows with the number of faces inside the lasso (one ray per face with `visible_only`). On multi-million-face scans a large lasso may take seconds on the engine thread. If that is too slow at the M0 gate, add the id buffer as an accelerator inside the viewport that produces the same selection, not as a different command.
- Faces partly inside the lasso are selected by their centre only.

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

#include "m2c/core/TriMesh.h"

namespace m2c {

/// Decimated stand-in drawn while the camera moves, so large scans orbit smoothly
/// (ARCHITECTURE.md section 8, level of detail). Vertex clustering on a uniform grid: fast and
/// good enough to look at, not for geometry work (picking and tools always use the full mesh).
/// Returns a copy of `mesh` when it already has at most `targetFaces` faces.
[[nodiscard]] TriMesh buildDisplayProxy(const TriMesh& mesh, std::size_t targetFaces);

} // namespace m2c

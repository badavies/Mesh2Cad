// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the OpenSubdiv adapter: a tessellation of the limit surface and closest points on
// it, shared by the cage fit and the display tessellation.

#include <cstddef>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Limit.h"

namespace m2c::osd {

/// A tessellation of the limit surface that remembers where each vertex lies: vertex
/// (n + 1)^2 * patch + (n + 1) * j + i is at (s, t) = (i / n, j / n) of that patch.
struct Tessellation {
    TriMesh mesh;
    int n{0};
};

/// Tessellates every patch on an n x n grid, from patch points (Limit::patchPoints).
[[nodiscard]] Result<Tessellation>
tessellate(const Limit& limit, const std::vector<Vec3d>& points, int n);

struct SurfacePoint {
    std::size_t patch{0};
    double s{0.0};
    double t{0.0};
    double distance{0.0};
    Vec3d normal; ///< Of the tessellation triangle, unit length (zero if degenerate).
};

/// Closest point of the tessellated limit surface, as patch parameters.
[[nodiscard]] SurfacePoint locate(const Tessellation& tess, const Vec3d& q);

} // namespace m2c::osd

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fitted primitives as exact Open CASCADE surfaces (ARCHITECTURE.md section 7, "Primitive
// fitting"). Only core types cross this header.

#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Vec3.h"

namespace m2c::occt {

/// Distance from each point to the primitive made as an Open CASCADE surface (Geom_Plane,
/// Geom_CylindricalSurface, Geom_SphericalSurface or Geom_ConicalSurface, all unbounded), by
/// projection onto it. A cone's surface has both halves. Fails for a primitive with no surface:
/// a zero direction, a radius that is not positive or a half-angle outside (0, 90) degrees.
[[nodiscard]] Result<std::vector<double>> surfaceDistances(const Primitive& primitive,
                                                           const std::vector<Vec3d>& points);

} // namespace m2c::occt

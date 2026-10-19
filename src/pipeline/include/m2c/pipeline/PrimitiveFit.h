// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Least-squares fitting of planes, cylinders and spheres to scan points (ARCHITECTURE.md
// section 7, "Primitive fitting").

#include <span>

#include "m2c/core/Primitive.h"
#include "m2c/core/Vec3.h"

namespace m2c {

struct PrimitiveFit {
    Primitive primitive;
    double rmsMm{0.0}; ///< Distance of the points to the fitted surface.
    double maxMm{0.0};
};

/// Fits a primitive of start.kind to the points by least squares on their distances to it. A
/// plane comes from the points' principal axes, its normal on start.direction's side. A cylinder
/// or sphere starts from `start` (for example a region-growing estimate) and is improved by
/// Gauss-Newton steps; the start is kept if no step fits better. A cylinder's axis point ends
/// level with the points' centroid. Deterministic. Needs at least one point (four for a sphere,
/// five for a cylinder, to be determined).
[[nodiscard]] PrimitiveFit fitPrimitive(const Primitive& start, std::span<const Vec3d> points);

/// How far a cylinder or sphere turns over the points, in radians: around the axis (the full
/// circle less the largest gap), or the widest angle at the centre between a point and the
/// points' mean direction, doubled. Zero for a plane. A strip of a plane also fits a large
/// cylinder within a distance limit, but turns through very little of it.
[[nodiscard]] double turnAngle(const Primitive& s, std::span<const Vec3d> points);

} // namespace m2c

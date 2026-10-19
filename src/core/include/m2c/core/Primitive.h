// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An unbounded exact surface fitted to a region of a scan (ARCHITECTURE.md section 7,
// "Primitive fitting"): a plane, cylinder or sphere.

#include "m2c/core/Vec3.h"

namespace m2c {

enum class PrimitiveKind {
    Plane,
    Cylinder,
    Sphere
};

struct Primitive {
    PrimitiveKind kind{PrimitiveKind::Plane};
    /// Plane: a point on it. Cylinder: a point on the axis. Sphere: the centre.
    Vec3d point{};
    /// Plane: unit normal. Cylinder: unit axis direction. Sphere: unused.
    Vec3d direction{};
    double radiusMm{0.0}; ///< Cylinder and sphere.
};

/// Distance from p to the surface.
[[nodiscard]] double distanceTo(const Primitive& s, const Vec3d& p) noexcept;

/// Unit normal of the surface at its point nearest p: a plane's normal, or away from a
/// cylinder's axis or a sphere's centre (zero on the axis or at the centre).
[[nodiscard]] Vec3d normalAt(const Primitive& s, const Vec3d& p) noexcept;

} // namespace m2c

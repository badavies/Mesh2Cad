// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An unbounded exact surface fitted to a region of a scan (ARCHITECTURE.md section 7,
// "Primitive fitting"): a plane, cylinder, sphere or cone.

#include "m2c/core/Vec3.h"

namespace m2c {

enum class PrimitiveKind {
    Plane,
    Cylinder,
    Sphere,
    Cone
};

struct Primitive {
    PrimitiveKind kind{PrimitiveKind::Plane};
    /// Plane: a point on it. Cylinder: a point on the axis. Sphere: the centre. Cone: the apex.
    Vec3d point{};
    /// Plane: unit normal. Cylinder: unit axis direction. Sphere: unused. Cone: unit axis
    /// direction from the apex into the cone.
    Vec3d direction{};
    double radiusMm{0.0};     ///< Cylinder and sphere.
    double halfAngleRad{0.0}; ///< Cone: between the axis and the surface, above 0 and below pi/2.
};

/// "plane", "cylinder", "sphere" or "cone".
[[nodiscard]] const char* primitiveKindName(PrimitiveKind kind) noexcept;

/// Distance from p to the surface (a cone's: to its half beyond the apex).
[[nodiscard]] double distanceTo(const Primitive& s, const Vec3d& p) noexcept;

/// Unit normal of the surface at its point nearest p: a plane's normal, or away from a
/// cylinder's or cone's axis or a sphere's centre (zero on the axis or at the centre).
[[nodiscard]] Vec3d normalAt(const Primitive& s, const Vec3d& p) noexcept;

} // namespace m2c

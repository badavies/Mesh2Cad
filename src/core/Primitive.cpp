// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Primitive.h"

#include <cmath>

#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

/// From the axis or centre to p, across the axis for a cylinder.
Vec3d radial(const Primitive& s, const Vec3d& p) noexcept {
    const Vec3d v = p - s.point;
    return s.kind == PrimitiveKind::Cylinder ? v - s.direction * dot(v, s.direction) : v;
}

} // namespace

double distanceTo(const Primitive& s, const Vec3d& p) noexcept {
    if (s.kind == PrimitiveKind::Plane) return std::abs(dot(p - s.point, s.direction));
    return std::abs(length(radial(s, p)) - s.radiusMm);
}

Vec3d normalAt(const Primitive& s, const Vec3d& p) noexcept {
    if (s.kind == PrimitiveKind::Plane) return s.direction;
    const Vec3d r = radial(s, p);
    const double n = length(r);
    return n > 0.0 ? r * (1.0 / n) : Vec3d{};
}

} // namespace m2c

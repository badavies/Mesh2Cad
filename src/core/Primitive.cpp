// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Primitive.h"

#include <cmath>
#include <optional>
#include <string_view>

#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

/// From the axis or centre to p, across the axis for a cylinder or cone.
Vec3d radial(const Primitive& s, const Vec3d& p) noexcept {
    const Vec3d v = p - s.point;
    return s.kind == PrimitiveKind::Cylinder || s.kind == PrimitiveKind::Cone
               ? v - s.direction * dot(v, s.direction)
               : v;
}

} // namespace

const char* primitiveKindName(const PrimitiveKind kind) noexcept {
    switch (kind) {
    case PrimitiveKind::Plane:
        return "plane";
    case PrimitiveKind::Cylinder:
        return "cylinder";
    case PrimitiveKind::Sphere:
        return "sphere";
    case PrimitiveKind::Cone:
        return "cone";
    }
    return "";
}

std::optional<PrimitiveKind> primitiveKindFromName(const std::string_view name) noexcept {
    for (const PrimitiveKind kind : {PrimitiveKind::Plane,
                                     PrimitiveKind::Cylinder,
                                     PrimitiveKind::Sphere,
                                     PrimitiveKind::Cone}) {
        if (name == primitiveKindName(kind)) return kind;
    }
    return std::nullopt;
}

double distanceTo(const Primitive& s, const Vec3d& p) noexcept {
    if (s.kind == PrimitiveKind::Plane) return std::abs(dot(p - s.point, s.direction));
    if (s.kind == PrimitiveKind::Cone) {
        // In the plane of the axis and p: height h along the axis, distance r from it.
        const double h = dot(p - s.point, s.direction);
        const double r = length(radial(s, p));
        const double c = std::cos(s.halfAngleRad);
        const double t = std::sin(s.halfAngleRad);
        if (h * c + r * t < 0.0) return length(p - s.point); // nearest the apex
        return std::abs(r * c - h * t);
    }
    return std::abs(length(radial(s, p)) - s.radiusMm);
}

Vec3d normalAt(const Primitive& s, const Vec3d& p) noexcept {
    if (s.kind == PrimitiveKind::Plane) return s.direction;
    const Vec3d r = radial(s, p);
    const double n = length(r);
    if (!(n > 0.0)) return Vec3d{};
    if (s.kind == PrimitiveKind::Cone) {
        return r * (std::cos(s.halfAngleRad) / n) - s.direction * std::sin(s.halfAngleRad);
    }
    return r * (1.0 / n);
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An unbounded exact surface fitted to a region of a scan (ARCHITECTURE.md section 7,
// "Primitive fitting"): a plane, cylinder, sphere or cone.

#include <array>
#include <optional>
#include <string_view>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
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

/// A primitive fitted to a region of a scan: how well it fits, and the region's bounding box, to
/// which the app clips the unbounded surface when it draws it.
struct FittedPrimitive {
    Primitive surface;
    Aabb extent{};
    double rmsMm{0.0};
    double maxMm{0.0};
    double p95Mm{0.0};
};

/// A cuboid at any angle (primitive.box).
struct OrientedBox {
    Vec3d center{};
    /// Unit, perpendicular and right-handed.
    std::array<Vec3d, 3> axes{
        {{.x = 1, .y = 0, .z = 0}, {.x = 0, .y = 1, .z = 0}, {.x = 0, .y = 0, .z = 1}}};
    Vec3d sizeMm{}; ///< Edge lengths along axes[0], axes[1] and axes[2].
};

/// Distance from p to the box's surface, from inside or outside.
[[nodiscard]] double distanceTo(const OrientedBox& box, const Vec3d& p) noexcept;

/// "plane", "cylinder", "sphere" or "cone".
[[nodiscard]] const char* primitiveKindName(PrimitiveKind kind) noexcept;

/// The kind primitiveKindName gives that name, if any.
[[nodiscard]] std::optional<PrimitiveKind> primitiveKindFromName(std::string_view name) noexcept;

/// Distance from p to the surface (a cone's: to its half beyond the apex).
[[nodiscard]] double distanceTo(const Primitive& s, const Vec3d& p) noexcept;

/// Unit normal of the surface at its point nearest p: a plane's normal, or away from a
/// cylinder's or cone's axis or a sphere's centre (zero on the axis or at the centre).
[[nodiscard]] Vec3d normalAt(const Primitive& s, const Vec3d& p) noexcept;

/// The surface clipped to the fit's extent (grown by 1% of its diagonal), for drawing: a grid of
/// `cells` x `cells` quads over the part of the surface across the box (all the way round a
/// cylinder, cone or sphere), keeping the triangles inside it, each facing the way normalAt
/// does. Fails if no triangle is inside.
[[nodiscard]] Result<TriMesh> primitiveDisplay(const FittedPrimitive& fit, int cells = 64);

} // namespace m2c

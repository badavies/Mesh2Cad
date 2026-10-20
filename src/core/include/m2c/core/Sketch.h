// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A 2D profile on a plane, fitted to a section of a scan (ARCHITECTURE.md section 7, "Section
// sketches"): closed loops of lines, arcs and splines. Sketches are fitted, not constrained.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "m2c/core/Vec3.h"

namespace m2c {

/// A point or vector in a sketch's plane, in millimetres.
struct Vec2d {
    double x{0.0};
    double y{0.0};

    friend constexpr bool operator==(const Vec2d&, const Vec2d&) = default;
};

[[nodiscard]] constexpr Vec2d operator+(const Vec2d& a, const Vec2d& b) noexcept {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] constexpr Vec2d operator-(const Vec2d& a, const Vec2d& b) noexcept {
    return {a.x - b.x, a.y - b.y};
}

[[nodiscard]] constexpr Vec2d operator*(const Vec2d& a, double s) noexcept {
    return {a.x * s, a.y * s};
}

[[nodiscard]] constexpr double dot(const Vec2d& a, const Vec2d& b) noexcept {
    return a.x * b.x + a.y * b.y;
}

/// The z component of the 3D cross product: positive when b turns anticlockwise from a.
[[nodiscard]] constexpr double cross(const Vec2d& a, const Vec2d& b) noexcept {
    return a.x * b.y - a.y * b.x;
}

[[nodiscard]] inline double length(const Vec2d& a) noexcept {
    return std::sqrt(dot(a, a));
}

/// A sketch's plane: an origin and two perpendicular unit axes, with normal u x v.
struct SketchPlane {
    Vec3d origin{};
    Vec3d u{1, 0, 0};
    Vec3d v{0, 1, 0};

    [[nodiscard]] Vec3d normal() const noexcept { return cross(u, v); }
    [[nodiscard]] Vec3d toWorld(const Vec2d& p) const noexcept {
        return origin + u * p.x + v * p.y;
    }
    /// The point's position projected onto the plane.
    [[nodiscard]] Vec2d toPlane(const Vec3d& p) const noexcept {
        return {dot(p - origin, u), dot(p - origin, v)};
    }
};

/// The plane through `point` with the given normal (need not be unit length, must not be zero).
/// u is the x axis projected onto the plane, or the y axis when the normal is near x, so the
/// z = 0 plane gets u = x and v = y.
[[nodiscard]] SketchPlane planeThrough(const Vec3d& point, const Vec3d& normal) noexcept;

enum class CurveKind {
    Line,
    Arc,
    Spline
};

/// "line", "arc" or "spline".
[[nodiscard]] const char* curveKindName(CurveKind kind) noexcept;

/// The kind curveKindName gives that name, if any.
[[nodiscard]] std::optional<CurveKind> curveKindFromName(std::string_view name) noexcept;

/// One curve of a loop.
struct SketchCurve {
    CurveKind kind{CurveKind::Line};
    /// Line: its start and end. Spline: the points it passes through in order, start to end.
    /// Arc: empty.
    std::vector<Vec2d> points;
    Vec2d centre{};       ///< Arc.
    double radiusMm{0.0}; ///< Arc.
    double startRad{0.0}; ///< Arc: angle of the start from the centre, from u towards v.
    double sweepRad{0.0}; ///< Arc: signed, positive anticlockwise; +-2 pi for a full circle.

    [[nodiscard]] Vec2d start() const noexcept;
    [[nodiscard]] Vec2d end() const noexcept;
};

/// Distance from p to the curve (a spline's: to the polyline through its points).
[[nodiscard]] double distanceTo(const SketchCurve& curve, const Vec2d& p) noexcept;

/// A closed profile: each curve ends where the next starts, and the last where the first starts.
struct SketchLoop {
    std::vector<SketchCurve> curves;
};

struct Sketch {
    SketchPlane plane;
    std::vector<SketchLoop> loops;
};

/// A sketch's curves as world-space line segments for drawing.
struct SketchLines {
    std::vector<Vec3d> points;
    std::vector<std::uint32_t> segments; ///< Pairs of point indices.
};

/// The curves as polylines: lines and splines through their points, arcs in steps of at most
/// 5 degrees.
[[nodiscard]] SketchLines sketchLines(const Sketch& sketch);

} // namespace m2c

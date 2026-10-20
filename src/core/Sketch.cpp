// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Sketch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>

#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

double distanceToSegment(const Vec2d& a, const Vec2d& b, const Vec2d& p) noexcept {
    const Vec2d ab = b - a;
    const double len2 = dot(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return length(p - (a + ab * t));
}

Vec2d onCircle(const SketchCurve& arc, double angle) noexcept {
    return arc.centre + Vec2d{std::cos(angle), std::sin(angle)} * arc.radiusMm;
}

} // namespace

SketchPlane planeThrough(const Vec3d& point, const Vec3d& normal) noexcept {
    const Vec3d n = normal * (1.0 / length(normal));
    const Vec3d axis = std::abs(n.x) < 0.9 ? Vec3d{1, 0, 0} : Vec3d{0, 1, 0};
    const Vec3d along = axis - n * dot(axis, n);
    const Vec3d u = along * (1.0 / length(along));
    return {point, u, cross(n, u)};
}

const char* curveKindName(const CurveKind kind) noexcept {
    switch (kind) {
    case CurveKind::Line:
        return "line";
    case CurveKind::Arc:
        return "arc";
    case CurveKind::Spline:
        return "spline";
    }
    return "line";
}

std::optional<CurveKind> curveKindFromName(const std::string_view name) noexcept {
    for (const CurveKind kind : {CurveKind::Line, CurveKind::Arc, CurveKind::Spline}) {
        if (name == curveKindName(kind)) return kind;
    }
    return std::nullopt;
}

Vec2d SketchCurve::start() const noexcept {
    if (kind == CurveKind::Arc) return onCircle(*this, startRad);
    return points.empty() ? Vec2d{} : points.front();
}

Vec2d SketchCurve::end() const noexcept {
    if (kind == CurveKind::Arc) return onCircle(*this, startRad + sweepRad);
    return points.empty() ? Vec2d{} : points.back();
}

double distanceTo(const SketchCurve& curve, const Vec2d& p) noexcept {
    if (curve.kind == CurveKind::Arc) {
        const Vec2d d = p - curve.centre;
        // How far round from the start p lies, in the arc's direction, in [0, 2 pi).
        double turn = std::atan2(d.y, d.x) - curve.startRad;
        if (curve.sweepRad < 0.0) turn = -turn;
        turn = std::fmod(turn, 2.0 * std::numbers::pi);
        if (turn < 0.0) turn += 2.0 * std::numbers::pi;
        if (turn <= std::abs(curve.sweepRad)) return std::abs(length(d) - curve.radiusMm);
        return std::min(length(p - curve.start()), length(p - curve.end()));
    }
    if (curve.points.size() == 1) return length(p - curve.points.front());
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < curve.points.size(); ++i) {
        best = std::min(best, distanceToSegment(curve.points[i - 1], curve.points[i], p));
    }
    return best;
}

SketchLines sketchLines(const Sketch& sketch) {
    SketchLines out;
    for (const SketchLoop& loop : sketch.loops) {
        for (const SketchCurve& c : loop.curves) {
            std::vector<Vec2d> along = c.points;
            if (c.kind == CurveKind::Arc) {
                const int n =
                    std::max(2, static_cast<int>(std::ceil(std::abs(c.sweepRad) / 0.0873)));
                along.clear();
                for (int k = 0; k <= n; ++k)
                    along.push_back(onCircle(c, c.startRad + c.sweepRad * k / n));
            }
            for (std::size_t k = 0; k < along.size(); ++k) {
                if (k > 0) {
                    const auto last = static_cast<std::uint32_t>(out.points.size());
                    out.segments.insert(out.segments.end(), {last - 1, last});
                }
                out.points.push_back(sketch.plane.toWorld(along[k]));
            }
        }
    }
    return out;
}

} // namespace m2c

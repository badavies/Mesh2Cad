// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Primitive.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
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

Result<TriMesh> primitiveDisplay(const FittedPrimitive& fit, const int cells) {
    const Primitive& s = fit.surface;
    const double m = 0.01 * length(fit.extent.max - fit.extent.min) + 1e-9;
    const Aabb box{.min = fit.extent.min - Vec3d{m, m, m}, .max = fit.extent.max + Vec3d{m, m, m}};
    // The grid's edges lie on the box, give or take rounding.
    const double e = 1e-9 * (1.0 + length(box.max - box.min));
    const auto inside = [&](const Vec3d& p) {
        return p.x >= box.min.x - e && p.y >= box.min.y - e && p.z >= box.min.z - e &&
               p.x <= box.max.x + e && p.y <= box.max.y + e && p.z <= box.max.z + e;
    };
    const Vec3d d = s.kind == PrimitiveKind::Sphere ? Vec3d{0, 0, 1} : s.direction;
    const Vec3d e1 = std::abs(d.x) < 0.9 ? cross(d, Vec3d{1, 0, 0}) : cross(d, Vec3d{0, 1, 0});
    const Vec3d u = e1 * (1.0 / length(e1));
    const Vec3d w = cross(d, u);
    // The box corners' coordinates along u, w and d, from the primitive's point.
    std::array<double, 3> lo{}, hi{};
    lo.fill(std::numeric_limits<double>::max());
    hi.fill(std::numeric_limits<double>::lowest());
    for (int c = 0; c < 8; ++c) {
        const Vec3d corner{(c & 1) != 0 ? box.max.x : box.min.x,
                           (c & 2) != 0 ? box.max.y : box.min.y,
                           (c & 4) != 0 ? box.max.z : box.min.z};
        const Vec3d v = corner - s.point;
        const std::array<double, 3> at{dot(v, u), dot(v, w), dot(v, d)};
        for (std::size_t k = 0; k < 3; ++k) {
            lo.at(k) = std::min(lo.at(k), at.at(k));
            hi.at(k) = std::max(hi.at(k), at.at(k));
        }
    }
    constexpr double kTurn = 2.0 * std::numbers::pi;
    const auto ring = [&](double a) { return u * std::cos(a) + w * std::sin(a); };
    // (a, b) in [0, 1]^2 to a point on the surface.
    const auto at = [&](double a, double b) -> Vec3d {
        switch (s.kind) {
        case PrimitiveKind::Plane:
            return s.point + u * (lo[0] + a * (hi[0] - lo[0])) + w * (lo[1] + b * (hi[1] - lo[1]));
        case PrimitiveKind::Cylinder:
            return s.point + d * (lo[2] + b * (hi[2] - lo[2])) + ring(kTurn * a) * s.radiusMm;
        case PrimitiveKind::Sphere:
            return s.point + (ring(kTurn * a) * std::sin(std::numbers::pi * b) +
                              d * std::cos(std::numbers::pi * b)) *
                                 s.radiusMm;
        case PrimitiveKind::Cone: {
            const double h = std::max(lo[2], 0.0) + b * (hi[2] - std::max(lo[2], 0.0));
            return s.point + d * h + ring(kTurn * a) * (h * std::tan(s.halfAngleRad));
        }
        }
        return s.point;
    };
    const int n = std::max(cells, 1);
    std::vector<Vec3d> grid;
    grid.reserve(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(n + 1));
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) grid.push_back(at(1.0 * i / n, 1.0 * j / n));
    }
    std::vector<std::uint32_t> used(grid.size(), std::numeric_limits<std::uint32_t>::max());
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    const auto vertex = [&](std::uint32_t g) {
        if (used[g] == std::numeric_limits<std::uint32_t>::max()) {
            used[g] = static_cast<std::uint32_t>(positions.size());
            positions.push_back(grid[g]);
        }
        return used[g];
    };
    const auto id = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            for (std::array<std::uint32_t, 3> t :
                 {std::array{id(i, j), id(i + 1, j), id(i + 1, j + 1)},
                  std::array{id(i, j), id(i + 1, j + 1), id(i, j + 1)}}) {
                const Vec3d& a = grid[t[0]];
                const Vec3d& b = grid[t[1]];
                const Vec3d& c = grid[t[2]];
                if (!inside(a) || !inside(b) || !inside(c)) continue;
                const Vec3d normal = cross(b - a, c - a);
                if (!(length(normal) > 0.0)) continue; // at a pole or the apex
                if (dot(normal, normalAt(s, (a + b + c) * (1.0 / 3.0))) < 0.0)
                    std::swap(t[1], t[2]);
                for (const std::uint32_t g : t) indices.push_back(vertex(g));
            }
        }
    }
    if (indices.empty()) {
        return fail(ErrorCode::InvalidState, "the primitive does not cross its extent");
    }
    return TriMesh::create(std::move(positions), std::move(indices));
}

} // namespace m2c

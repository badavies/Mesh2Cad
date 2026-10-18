// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/SpatialIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr std::uint32_t kLeafSize = 4;

Vec3d vmin(const Vec3d& a, const Vec3d& b) noexcept {
    return {.x = std::min(a.x, b.x), .y = std::min(a.y, b.y), .z = std::min(a.z, b.z)};
}
Vec3d vmax(const Vec3d& a, const Vec3d& b) noexcept {
    return {.x = std::max(a.x, b.x), .y = std::max(a.y, b.y), .z = std::max(a.z, b.z)};
}
double axis(const Vec3d& v, const int k) noexcept {
    return k == 0 ? v.x : k == 1 ? v.y : v.z;
}

/// Squared distance from p to the box; 0 inside.
double boxDistance2(const Vec3d& p, const Vec3d& lo, const Vec3d& hi) noexcept {
    const double dx = std::max({lo.x - p.x, 0.0, p.x - hi.x});
    const double dy = std::max({lo.y - p.y, 0.0, p.y - hi.y});
    const double dz = std::max({lo.z - p.z, 0.0, p.z - hi.z});
    return dx * dx + dy * dy + dz * dz;
}

/// Closest point to p on the segment from a to b.
Vec3d closestOnSegment(const Vec3d& p, const Vec3d& a, const Vec3d& b) noexcept {
    const Vec3d ab = b - a;
    const double len2 = dot(ab, ab);
    if (!(len2 > 0.0)) return a;
    const double t = std::clamp(dot(p - a, ab) / len2, 0.0, 1.0);
    return a + ab * t;
}

/// Slab test; returns the entry distance or +inf on a miss.
double rayBox(const Vec3d& o,
              const Vec3d& inv,
              const Vec3d& lo,
              const Vec3d& hi,
              const double tMax) noexcept {
    double t0 = 0.0;
    double t1 = tMax;
    for (int k = 0; k < 3; ++k) {
        double a = (axis(lo, k) - axis(o, k)) * axis(inv, k);
        double b = (axis(hi, k) - axis(o, k)) * axis(inv, k);
        if (a > b) std::swap(a, b);
        // NaN (0 * inf, ray in the slab plane) compares false and leaves the bounds unchanged.
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return std::numeric_limits<double>::infinity();
    }
    return t0;
}

} // namespace

Vec3d closestPointOnTriangle(const Vec3d& p,
                             const Vec3d& a,
                             const Vec3d& b,
                             const Vec3d& c) noexcept {
    const Vec3d ab = b - a;
    const Vec3d ac = c - a;
    // A triangle without area is a segment (or a point): the Voronoi regions below assume
    // three distinct corners, so take the nearest point on its edges instead.
    if (const Vec3d n = cross(ab, ac); !(dot(n, n) > 0.0)) {
        Vec3d best = closestOnSegment(p, a, b);
        for (const Vec3d& q : {closestOnSegment(p, b, c), closestOnSegment(p, c, a)}) {
            if (dot(q - p, q - p) < dot(best - p, best - p)) best = q;
        }
        return best;
    }
    const Vec3d ap = p - a;
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return a;

    const Vec3d bp = p - b;
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return b;

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double denom = d1 - d3;
        return denom > 0.0 ? a + ab * (d1 / denom) : a;
    }

    const Vec3d cp = p - c;
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return c;

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double denom = d2 - d6;
        return denom > 0.0 ? a + ac * (d2 / denom) : a;
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        const double denom = d4 - d3 + (d5 - d6);
        return denom > 0.0 ? b + (c - b) * ((d4 - d3) / denom) : b;
    }

    const double sum = va + vb + vc;
    if (!(sum > 0.0)) return a; // degenerate triangle
    const double v = vb / sum;
    const double w = vc / sum;
    return a + ab * v + ac * w;
}

SpatialIndex::SpatialIndex(PositionBuffer positions, IndexBuffer indices)
    : positions_(std::move(positions)), indices_(std::move(indices)) {
    const auto faceCount = static_cast<std::uint32_t>(indices_->size() / 3);
    faces_.resize(faceCount);
    std::vector<Vec3d> centroid(faceCount);
    std::vector<Vec3d> lo(faceCount);
    std::vector<Vec3d> hi(faceCount);
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        faces_[f] = f;
        const Vec3d a = corner(f, 0);
        const Vec3d b = corner(f, 1);
        const Vec3d c = corner(f, 2);
        lo[f] = vmin(vmin(a, b), c);
        hi[f] = vmax(vmax(a, b), c);
        centroid[f] = (a + b + c) * (1.0 / 3.0);
    }

    nodes_.reserve(faceCount > 0 ? 2 * (faceCount / kLeafSize + 1) : 1);
    nodes_.push_back(Node{.min = {}, .max = {}, .first = 0, .count = faceCount});
    struct Task {
        std::uint32_t node, first, count;
    };
    std::vector<Task> stack{{.node = 0, .first = 0, .count = faceCount}};
    while (!stack.empty()) {
        const auto [node, first, count] = stack.back();
        stack.pop_back();
        Vec3d bmin{.x = std::numeric_limits<double>::infinity(),
                   .y = std::numeric_limits<double>::infinity(),
                   .z = std::numeric_limits<double>::infinity()};
        Vec3d bmax = bmin * -1.0;
        Vec3d cmin = bmin;
        Vec3d cmax = bmax;
        for (std::uint32_t i = first; i < first + count; ++i) {
            const std::uint32_t f = faces_[i];
            bmin = vmin(bmin, lo[f]);
            bmax = vmax(bmax, hi[f]);
            cmin = vmin(cmin, centroid[f]);
            cmax = vmax(cmax, centroid[f]);
        }
        nodes_[node].min = bmin;
        nodes_[node].max = bmax;
        if (count <= kLeafSize) {
            nodes_[node].first = first;
            nodes_[node].count = count;
            continue;
        }
        const auto [x, y, z] = cmax - cmin;
        const int k = x >= y && x >= z ? 0 : y >= z ? 1 : 2;
        const std::uint32_t half = count / 2;
        const auto begin = faces_.begin() + first;
        // Ties broken by face index so the tree is the same on every run.
        std::nth_element(
            begin, begin + half, begin + count, [&](const std::uint32_t l, const std::uint32_t r) {
                const double cl = axis(centroid[l], k);
                const double cr = axis(centroid[r], k);
                return cl < cr || (cl == cr && l < r);
            });
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_.push_back(Node{});
        nodes_[node].first = left;
        nodes_[node].count = 0;
        stack.push_back({.node = left, .first = first, .count = half});
        stack.push_back({.node = left + 1, .first = first + half, .count = count - half});
    }
}

std::optional<RayHit>
SpatialIndex::raycast(const Vec3d& origin, const Vec3d& direction, const double maxDistance) const {
    const double len = length(direction);
    if (faces_.empty() || !(len > 0.0)) return std::nullopt;
    // "+ 0.0" turns -0.0 into +0.0: 1 / -0.0 = -inf would make the slab test miss boxes whose
    // face the origin lies on.
    const Vec3d dir = direction * (1.0 / len) + Vec3d{.x = 0.0, .y = 0.0, .z = 0.0};
    const Vec3d inv{.x = 1.0 / dir.x, .y = 1.0 / dir.y, .z = 1.0 / dir.z};

    std::optional<RayHit> best;
    double bestT = maxDistance;
    std::array<std::uint32_t, 128> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const auto& [nodeMin, nodeMax, first, count] = nodes_[stack[--top]];
        if (rayBox(origin, inv, nodeMin, nodeMax, bestT) > bestT) continue;
        if (count == 0) {
            // Visit the child the ray enters first, so its hit can prune the other.
            const Node& l = nodes_[first];
            const Node& r = nodes_[first + 1];
            const bool leftFirst = rayBox(origin, inv, l.min, l.max, bestT) <=
                                   rayBox(origin, inv, r.min, r.max, bestT);
            stack[top++] = leftFirst ? first + 1 : first;
            stack[top++] = leftFirst ? first : first + 1;
            continue;
        }
        for (std::uint32_t i = first; i < first + count; ++i) {
            const std::uint32_t f = faces_[i];
            const Vec3d a = corner(f, 0);
            const Vec3d e1 = corner(f, 1) - a;
            const Vec3d e2 = corner(f, 2) - a;
            const Vec3d p = cross(dir, e2);
            const double det = dot(e1, p);
            if (std::abs(det) < 1e-300) continue;
            const double invDet = 1.0 / det;
            const Vec3d s = origin - a;
            const double u = dot(s, p) * invDet;
            if (u < 0.0 || u > 1.0) continue;
            const Vec3d q = cross(s, e1);
            const double v = dot(dir, q) * invDet;
            if (v < 0.0 || u + v > 1.0) continue;
            const double t = dot(e2, q) * invDet;
            if (t < 0.0 || t > bestT) continue;
            if (best && t == bestT && f > best->face) continue;
            bestT = t;
            best = RayHit{.face = f,
                          .distance = t,
                          .point = origin + dir * t,
                          .barycentric = {.x = 1.0 - u - v, .y = u, .z = v}};
        }
    }
    return best;
}

ClosestPoint SpatialIndex::closestPoint(const Vec3d& query) const {
    ClosestPoint best{.face = 0, .distance = std::numeric_limits<double>::infinity(), .point = {}};
    if (faces_.empty()) return best;
    double best2 = std::numeric_limits<double>::infinity();
    std::array<std::uint32_t, 128> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const auto& [nodeMin, nodeMax, first, count] = nodes_[stack[--top]];
        if (boxDistance2(query, nodeMin, nodeMax) > best2) continue;
        if (count == 0) {
            // Visit the nearer child first so pruning starts early.
            const Node& l = nodes_[first];
            const Node& r = nodes_[first + 1];
            const bool leftFirst =
                boxDistance2(query, l.min, l.max) <= boxDistance2(query, r.min, r.max);
            stack[top++] = leftFirst ? first + 1 : first;
            stack[top++] = leftFirst ? first : first + 1;
            continue;
        }
        for (std::uint32_t i = first; i < first + count; ++i) {
            const std::uint32_t f = faces_[i];
            const Vec3d c = closestPointOnTriangle(query, corner(f, 0), corner(f, 1), corner(f, 2));
            const Vec3d d = c - query;
            if (const double d2 = dot(d, d); d2 < best2 || (d2 == best2 && f < best.face)) {
                best2 = d2;
                best = ClosestPoint{.face = f, .distance = 0.0, .point = c};
            }
        }
    }
    best.distance = std::sqrt(best2);
    return best;
}

std::vector<std::uint32_t> SpatialIndex::facesWithin(const Vec3d& center,
                                                     const double radius) const {
    std::vector<std::uint32_t> result;
    if (faces_.empty() || !(radius >= 0.0)) return result;
    const double r2 = radius * radius;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const auto& [nodeMin, nodeMax, first, count] = nodes_[stack.back()];
        stack.pop_back();
        if (boxDistance2(center, nodeMin, nodeMax) > r2) continue;
        if (count == 0) {
            stack.push_back(first);
            stack.push_back(first + 1);
            continue;
        }
        for (std::uint32_t i = first; i < first + count; ++i) {
            const std::uint32_t f = faces_[i];
            const Vec3d c =
                closestPointOnTriangle(center, corner(f, 0), corner(f, 1), corner(f, 2));
            if (const Vec3d d = c - center; dot(d, d) <= r2) result.push_back(f);
        }
    }
    std::ranges::sort(result);
    return result;
}

} // namespace m2c

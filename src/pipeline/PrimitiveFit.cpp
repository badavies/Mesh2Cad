// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/PrimitiveFit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <Eigen/Dense>

#include "m2c/core/Primitive.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

Vec3d unit(const Vec3d& v) {
    const double n = length(v);
    return n > 0.0 ? v * (1.0 / n) : Vec3d{};
}

/// Two unit vectors that make a right-handed frame with unit d.
std::pair<Vec3d, Vec3d> across(const Vec3d& d) {
    const Vec3d u = unit(std::abs(d.x) < 0.9 ? cross(d, Vec3d{1, 0, 0}) : cross(d, Vec3d{0, 1, 0}));
    return {u, cross(d, u)};
}

double rms(const Primitive& s, std::span<const Vec3d> points) {
    double sum2 = 0.0;
    for (const Vec3d& p : points) sum2 += distanceTo(s, p) * distanceTo(s, p);
    return std::sqrt(sum2 / static_cast<double>(std::max<std::size_t>(points.size(), 1)));
}

Vec3d centroid(std::span<const Vec3d> points) {
    Vec3d c;
    for (const Vec3d& p : points) c = c + p;
    return c * (1.0 / static_cast<double>(std::max<std::size_t>(points.size(), 1)));
}

Primitive fitPlane(const Primitive& start, std::span<const Vec3d> points) {
    const Vec3d c = centroid(points);
    Eigen::Matrix3d m = Eigen::Matrix3d::Zero();
    for (const Vec3d& p : points) {
        const Eigen::Vector3d d(p.x - c.x, p.y - c.y, p.z - c.z);
        m += d * d.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(m);
    const Eigen::Vector3d e = solver.eigenvectors().col(0); // the smallest eigenvalue's
    Vec3d n{.x = e.x(), .y = e.y(), .z = e.z()};
    if (dot(n, start.direction) < 0.0) n = n * -1.0;
    return {.kind = PrimitiveKind::Plane, .point = c, .direction = n};
}

/// The primitive moved by a step of its parameters: a sphere's centre and radius, or a move of
/// a cylinder's axis point across the axis, a tilt of the axis and the radius.
Primitive moved(const Primitive& s, const Eigen::VectorXd& step) {
    Primitive t = s;
    if (s.kind == PrimitiveKind::Sphere) {
        t.point = s.point + Vec3d{step(0), step(1), step(2)};
        t.radiusMm = s.radiusMm + step(3);
        return t;
    }
    const auto [u, w] = across(s.direction);
    t.point = s.point + u * step(0) + w * step(1);
    t.direction = unit(s.direction + u * step(2) + w * step(3));
    t.radiusMm = s.radiusMm + step(4);
    return t;
}

constexpr int kMaxSteps = 20;

} // namespace

PrimitiveFit fitPrimitive(const Primitive& start, std::span<const Vec3d> points) {
    Primitive best = start;
    if (start.kind == PrimitiveKind::Plane) {
        best = fitPlane(start, points);
    } else {
        const Eigen::Index n = start.kind == PrimitiveKind::Sphere ? 4 : 5;
        const auto m = static_cast<Eigen::Index>(points.size());
        const auto residuals = [&](const Primitive& s) {
            Eigen::VectorXd f(m);
            for (Eigen::Index i = 0; i < m; ++i) {
                const Vec3d& p = points[static_cast<std::size_t>(i)];
                f(i) = dot(p - s.point, normalAt(s, p)) - s.radiusMm;
            }
            return f;
        };
        double bestRms = rms(start, points);
        Primitive at = start;
        for (int it = 0; it < kMaxSteps && m >= n; ++it) {
            // Forward differences: the residuals are smooth away from the axis or centre.
            const Eigen::VectorXd f = residuals(at);
            const double h = 1e-7 * std::max(1.0, at.radiusMm);
            Eigen::MatrixXd j(m, n);
            for (Eigen::Index k = 0; k < n; ++k) {
                Eigen::VectorXd step = Eigen::VectorXd::Zero(n);
                step(k) = h;
                j.col(k) = (residuals(moved(at, step)) - f) / h;
            }
            const Eigen::VectorXd step = (j.transpose() * j).ldlt().solve(-j.transpose() * f);
            if (!step.allFinite()) break;
            at = moved(at, step);
            if (const double now = rms(at, points); now < bestRms) {
                bestRms = now;
                best = at;
            }
            if (step.norm() < 1e-10 * std::max(1.0, at.radiusMm)) break;
        }
        if (best.kind == PrimitiveKind::Cylinder) {
            best.point =
                best.point + best.direction * dot(centroid(points) - best.point, best.direction);
        }
    }
    PrimitiveFit out{.primitive = best, .rmsMm = rms(best, points)};
    for (const Vec3d& p : points) out.maxMm = std::max(out.maxMm, distanceTo(best, p));
    return out;
}

double turnAngle(const Primitive& s, std::span<const Vec3d> points) {
    if (s.kind == PrimitiveKind::Plane || points.empty()) return 0.0;
    if (s.kind == PrimitiveKind::Sphere) {
        Vec3d mean;
        for (const Vec3d& p : points) mean = mean + normalAt(s, p);
        mean = unit(mean);
        double widest = 0.0;
        for (const Vec3d& p : points) {
            widest = std::max(widest, std::acos(std::clamp(dot(normalAt(s, p), mean), -1.0, 1.0)));
        }
        return std::min(2.0 * widest, 2.0 * std::numbers::pi);
    }
    const auto [u, w] = across(s.direction);
    std::vector<double> angles;
    angles.reserve(points.size());
    for (const Vec3d& p : points) {
        const Vec3d d = p - s.point;
        angles.push_back(std::atan2(dot(d, w), dot(d, u)));
    }
    std::ranges::sort(angles);
    double gap = angles.front() + 2.0 * std::numbers::pi - angles.back();
    for (std::size_t i = 1; i < angles.size(); ++i) gap = std::max(gap, angles[i] - angles[i - 1]);
    return 2.0 * std::numbers::pi - gap;
}

} // namespace m2c

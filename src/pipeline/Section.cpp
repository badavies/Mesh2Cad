// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Section.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

double distanceToSegment(const Vec2d& a, const Vec2d& b, const Vec2d& p) noexcept {
    SketchCurve line;
    line.points = {a, b};
    return distanceTo(line, p);
}

double angleOf(const Vec2d& v) noexcept {
    return std::atan2(v.y, v.x);
}

/// Least-squares circle through the points: the algebraic (Kasa) fit, then Gauss-Newton steps on
/// the distances. Empty if the points are (nearly) on a line.
std::optional<SketchCurve> fitCircle(std::span<const Vec2d> points) {
    Vec2d mean;
    for (const Vec2d& p : points) mean = mean + p;
    mean = mean * (1.0 / static_cast<double>(points.size()));
    Eigen::Matrix3d a = Eigen::Matrix3d::Zero();
    Eigen::Vector3d b = Eigen::Vector3d::Zero();
    for (const Vec2d& p : points) {
        const Vec2d q = p - mean;
        const Eigen::Vector3d row(q.x, q.y, 1.0);
        a += row * row.transpose();
        b -= row * dot(q, q);
    }
    const Eigen::Vector3d k = a.ldlt().solve(b);
    Eigen::Vector3d c(-k.x() / 2.0, -k.y() / 2.0, 0.0);
    c.z() = std::sqrt(std::max(0.0, c.x() * c.x() + c.y() * c.y() - k.z()));
    for (int step = 0; step < 8; ++step) {
        Eigen::Matrix3d jtj = Eigen::Matrix3d::Zero();
        Eigen::Vector3d jtr = Eigen::Vector3d::Zero();
        for (const Vec2d& p : points) {
            const Vec2d d = p - mean - Vec2d{c.x(), c.y()};
            const double len = length(d);
            if (len <= 0.0) continue;
            const Eigen::Vector3d j(-d.x / len, -d.y / len, -1.0);
            jtj += j * j.transpose();
            jtr += j * (len - c.z());
        }
        c -= jtj.ldlt().solve(jtr);
    }
    if (!c.allFinite() || !(c.z() > 0.0)) return std::nullopt;
    SketchCurve circle;
    circle.kind = CurveKind::Arc;
    circle.centre = mean + Vec2d{c.x(), c.y()};
    circle.radiusMm = c.z();
    return circle;
}

/// The largest j in [first, n] for which fits(j) holds, searching by doubling then halving, or 0
/// if fits(first) fails.
template <class Fits>
std::size_t extend(std::size_t first, std::size_t n, Fits&& fits) {
    if (first > n || !fits(first)) return 0;
    std::size_t good = first, step = 1;
    std::size_t bad = n + 1;
    while (good < n) {
        const std::size_t next = std::min(n, good + step);
        if (!fits(next)) {
            bad = next;
            break;
        }
        good = next;
        step *= 2;
    }
    while (bad <= n && bad - good > 1) {
        const std::size_t mid = good + (bad - good) / 2;
        (fits(mid) ? good : bad) = mid;
    }
    return good;
}

bool lineFits(std::span<const Vec2d> q, std::size_t i, std::size_t j, double tol) {
    if (!(length(q[j] - q[i]) > 0.0)) return false;
    for (std::size_t k = i + 1; k < j; ++k) {
        if (distanceToSegment(q[i], q[j], q[k]) > tol) return false;
    }
    return true;
}

/// q is the polyline from `start`, closed: q[n] is q[0].
std::vector<Vec2d> closedFrom(std::span<const Vec2d> p, std::size_t start) {
    std::vector<Vec2d> q(p.size() + 1);
    for (std::size_t k = 0; k < q.size(); ++k) q[k] = p[(start + k) % p.size()];
    return q;
}

} // namespace

Result<LoopFit> fitLoop(std::span<const Vec2d> points, const SectionParams& params) {
    const double tol = params.maxErrorMm;
    if (!(tol > 0.0)) return fail(ErrorCode::InvalidArgument, "maximum error must be positive");
    std::vector<Vec2d> p;
    for (const Vec2d& v : points) {
        if (p.empty() || length(v - p.back()) > 1e-9) p.push_back(v);
    }
    while (p.size() > 1 && length(p.back() - p.front()) <= 1e-9) p.pop_back();
    const std::size_t n = p.size();
    if (n < 3) return fail(ErrorCode::InvalidArgument, "a loop needs at least three points");

    double area = 0.0;
    for (std::size_t k = 0; k < n; ++k) area += cross(p[k], p[(k + 1) % n]);
    if (const auto circle = fitCircle(p)) {
        double worst = 0.0;
        for (const Vec2d& v : p)
            worst = std::max(worst, std::abs(length(v - circle->centre) - circle->radiusMm));
        if (worst <= tol) {
            LoopFit out;
            SketchCurve full = *circle;
            full.startRad = angleOf(p.front() - full.centre);
            full.sweepRad = area < 0.0 ? -kTwoPi : kTwoPi;
            out.loop.curves.push_back(full);
            out.maxErrorMm = worst;
            return out;
        }
    }

    // Start at the sharpest corner, so a line does not run on round it.
    std::size_t start = 0;
    double sharpest = 2.0;
    for (std::size_t k = 0; k < n; ++k) {
        const Vec2d in = p[k] - p[(k + n - 1) % n], out = p[(k + 1) % n] - p[k];
        const double c = dot(in, out) / (length(in) * length(out));
        if (c < sharpest) {
            sharpest = c;
            start = k;
        }
    }
    const std::vector<Vec2d> q = closedFrom(p, start);

    LoopFit out;
    for (std::size_t i = 0; i < n;) {
        const std::size_t j = std::max(
            i + 1, extend(i + 1, n, [&](std::size_t end) { return lineFits(q, i, end, tol); }));
        SketchCurve line;
        line.points = {q[i], q[j]};
        for (std::size_t k = i; k <= j; ++k) {
            out.maxErrorMm = std::max(out.maxErrorMm, distanceTo(line, q[k]));
        }
        out.loop.curves.push_back(std::move(line));
        i = j;
    }
    return out;
}

Result<SectionResult>
sectionMesh(const TriMesh& mesh, const SketchPlane& plane, const SectionParams& params) {
    auto cut = cgal::slice(mesh, plane.origin, plane.normal());
    if (!cut) return std::unexpected(cut.error());
    SectionResult out;
    out.sketch.plane = plane;
    for (const auto& polyline : *cut) {
        if (polyline.size() < 4 || !(polyline.front() == polyline.back())) {
            ++out.openPieces;
            continue;
        }
        std::vector<Vec2d> flat;
        flat.reserve(polyline.size());
        for (const Vec3d& v : polyline) flat.push_back(plane.toPlane(v));
        auto fit = fitLoop(flat, params);
        if (!fit) return std::unexpected(fit.error());
        out.maxErrorMm = std::max(out.maxErrorMm, fit->maxErrorMm);
        out.sketch.loops.push_back(std::move(fit->loop));
    }
    if (out.sketch.loops.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    out.openPieces > 0
                        ? "the plane cuts the mesh only where it is open, so no profile closes"
                        : "the plane does not cut the mesh");
    }
    return out;
}

} // namespace m2c

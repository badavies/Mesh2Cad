// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/PrimitiveFit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "m2c/adapters/cgal/RegionGrowing.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
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

/// The primitive moved by a step of its parameters: a sphere's centre and radius, a cone's apex,
/// a tilt of its axis and its half-angle, or a move of a cylinder's axis point across the axis,
/// a tilt of the axis and the radius.
Primitive moved(const Primitive& s, const Eigen::VectorXd& step) {
    Primitive t = s;
    if (s.kind == PrimitiveKind::Sphere) {
        t.point = s.point + Vec3d{step(0), step(1), step(2)};
        t.radiusMm = s.radiusMm + step(3);
        return t;
    }
    const auto [u, w] = across(s.direction);
    if (s.kind == PrimitiveKind::Cone) {
        t.point = s.point + Vec3d{step(0), step(1), step(2)};
        t.direction = unit(s.direction + u * step(3) + w * step(4));
        t.halfAngleRad = std::clamp(s.halfAngleRad + step(5), 1e-6, std::numbers::pi / 2 - 1e-6);
        return t;
    }
    t.point = s.point + u * step(0) + w * step(1);
    t.direction = unit(s.direction + u * step(2) + w * step(3));
    t.radiusMm = s.radiusMm + step(4);
    return t;
}

constexpr int kMaxSteps = 20;

struct FaceSamples {
    std::vector<Vec3d> centroids;
    std::vector<Vec3d> normals; ///< Unit.
    std::vector<double> areas;
    Vec3d normalSum; ///< Area-weighted.
};

FaceSamples faceSamples(const TriMesh& mesh, std::span<const std::uint32_t> faces) {
    FaceSamples out;
    const auto& p = mesh.positions();
    for (const std::uint32_t f : faces) {
        const auto [a, b, c] = mesh.face(f);
        const Vec3d n = cross(p[b] - p[a], p[c] - p[a]);
        if (!(length(n) > 0.0)) continue;
        out.centroids.push_back((p[a] + p[b] + p[c]) * (1.0 / 3.0));
        out.normals.push_back(unit(n));
        out.areas.push_back(0.5 * length(n));
        out.normalSum = out.normalSum + n * 0.5;
    }
    return out;
}

Eigen::Vector3d eigen(const Vec3d& v) {
    return {v.x, v.y, v.z};
}

/// Every tangent plane of a cone passes through its apex, and its normals make one angle with
/// the axis, so they lie in a plane across it.
Result<Primitive> estimateCone(const FaceSamples& s) {
    const auto none = [] { return fail(ErrorCode::InvalidState, "no cone fits these faces"); };
    if (s.normals.size() < 6) return none();
    Vec3d mean;
    for (std::size_t i = 0; i < s.normals.size(); ++i) mean = mean + s.normals[i] * s.areas[i];
    double total = 0.0;
    for (const double a : s.areas) total += a;
    mean = mean * (1.0 / total);
    Eigen::Matrix3d spread = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d planes = Eigen::Matrix3d::Zero();
    Eigen::Vector3d rhs = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < s.normals.size(); ++i) {
        const Eigen::Vector3d n = eigen(s.normals[i]);
        const Eigen::Vector3d d = n - eigen(mean);
        spread += s.areas[i] * d * d.transpose();
        planes += s.areas[i] * n * n.transpose();
        rhs += s.areas[i] * n * dot(s.normals[i], s.centroids[i]);
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> planeSolver(planes);
    // Normals nearly in a plane through the origin: a cylinder or plane, with no apex.
    if (!(planeSolver.eigenvalues()(0) > 1e-6 * planeSolver.eigenvalues()(2))) return none();
    const Eigen::Vector3d x = planes.ldlt().solve(rhs);
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> spreadSolver(spread);
    const Eigen::Vector3d e = spreadSolver.eigenvectors().col(0);
    Primitive cone{.kind = PrimitiveKind::Cone,
                   .point = Vec3d{x.x(), x.y(), x.z()},
                   .direction = unit(Vec3d{e.x(), e.y(), e.z()})};
    double height = 0.0;
    for (const Vec3d& c : s.centroids) height += dot(c - cone.point, cone.direction);
    if (height < 0.0) cone.direction = cone.direction * -1.0;
    double angle = 0.0;
    for (std::size_t i = 0; i < s.centroids.size(); ++i) {
        const Vec3d v = s.centroids[i] - cone.point;
        const double h = dot(v, cone.direction);
        angle += s.areas[i] * std::atan2(length(v - cone.direction * h), h);
    }
    cone.halfAngleRad = angle / total;
    if (!x.allFinite() || !(cone.halfAngleRad > 0.0 && cone.halfAngleRad < std::numbers::pi / 2)) {
        return none();
    }
    return cone;
}

Result<PrimitiveFit> fitKind(const TriMesh& mesh,
                             std::span<const std::uint32_t> faces,
                             const FaceSamples& samples,
                             std::span<const Vec3d> points,
                             const PrimitiveKind kind) {
    Result<Primitive> start = Primitive{.kind = kind, .direction = samples.normalSum};
    if (kind == PrimitiveKind::Cone) {
        start = estimateCone(samples);
    } else if (kind != PrimitiveKind::Plane) {
        start = cgal::estimatePrimitive(mesh, faces, kind);
    }
    if (!start) return std::unexpected(start.error());
    return fitPrimitive(*start, points);
}

} // namespace

PrimitiveFit fitPrimitive(const Primitive& start, std::span<const Vec3d> points) {
    Primitive best = start;
    if (start.kind == PrimitiveKind::Plane) {
        best = fitPlane(start, points);
    } else {
        const Eigen::Index n = start.kind == PrimitiveKind::Sphere     ? 4
                               : start.kind == PrimitiveKind::Cylinder ? 5
                                                                       : 6;
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
    std::vector<double> distances;
    distances.reserve(points.size());
    for (const Vec3d& p : points) distances.push_back(distanceTo(best, p));
    if (!distances.empty()) {
        out.maxMm = std::ranges::max(distances);
        const auto at =
            distances.begin() + static_cast<std::ptrdiff_t>(
                                    std::ceil(0.95 * static_cast<double>(distances.size())) - 1.0);
        std::ranges::nth_element(distances, at);
        out.p95Mm = *at;
    }
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

std::vector<Vec3d> regionPoints(const TriMesh& mesh, std::span<const std::uint32_t> faces) {
    std::vector<std::uint32_t> vertices;
    for (const std::uint32_t f : faces) {
        const auto [a, b, c] = mesh.face(f);
        vertices.insert(vertices.end(), {a, b, c});
    }
    std::ranges::sort(vertices);
    const auto [first, last] = std::ranges::unique(vertices);
    vertices.erase(first, last);
    std::vector<Vec3d> points;
    points.reserve(vertices.size());
    for (const std::uint32_t v : vertices) points.push_back(mesh.positions()[v]);
    return points;
}

Result<PrimitiveFit> fitRegion(const TriMesh& mesh,
                               std::span<const std::uint32_t> faces,
                               const RegionFitParams& params) {
    if (!(params.maxResidualMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the residual limit must be positive");
    }
    if (std::ranges::any_of(faces, [&](std::uint32_t f) { return f >= mesh.faceCount(); })) {
        return fail(ErrorCode::InvalidArgument, "face index out of range");
    }
    const FaceSamples samples = faceSamples(mesh, faces);
    const std::vector<Vec3d> points = regionPoints(mesh, faces);
    if (samples.normals.size() < 3) {
        return fail(ErrorCode::InvalidArgument, "the region needs at least three faces");
    }
    Result<PrimitiveFit> best = fail(ErrorCode::InvalidState, "no primitive fits the region");
    if (params.kind) {
        best = fitKind(mesh, faces, samples, points, *params.kind);
    } else {
        std::vector<PrimitiveFit> fits;
        for (const PrimitiveKind kind : {PrimitiveKind::Plane,
                                         PrimitiveKind::Sphere,
                                         PrimitiveKind::Cylinder,
                                         PrimitiveKind::Cone}) {
            if (Result<PrimitiveFit> fit = fitKind(mesh, faces, samples, points, kind)) {
                fits.push_back(*fit);
            }
        }
        if (!fits.empty()) {
            const double lowest = std::ranges::min(fits, {}, &PrimitiveFit::rmsMm).rmsMm;
            best = *std::ranges::find_if(
                fits, [&](const PrimitiveFit& f) { return f.rmsMm <= 1.2 * lowest + 1e-9; });
        }
    }
    if (!best) return best;
    if (!(best->p95Mm <= params.maxResidualMm)) {
        return fail(ErrorCode::InvalidState,
                    std::string("the ") + primitiveKindName(best->primitive.kind) +
                        " fits with a P95 residual of " + std::to_string(best->p95Mm) +
                        " mm, above the limit of " + std::to_string(params.maxResidualMm) + " mm");
    }
    return best;
}

} // namespace m2c

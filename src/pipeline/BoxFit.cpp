// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/BoxFit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"
#include "m2c/pipeline/PrimitiveFit.h"

namespace m2c {

namespace {

constexpr double kFacingDeg = 10.0; // a face's normal this close to a direction faces it
constexpr int kCandidates = 256;    // face normals tried as the start's axes
constexpr int kSteps = 50;          // most Gauss-Newton steps
constexpr double kDamping = 1e-12;  // keeps sides no vertex reaches where they started
constexpr double kStepTolerance = 1e-10;

Vec3d unit(const Vec3d& v) {
    const double n = length(v);
    return n > 0.0 ? v * (1.0 / n) : Vec3d{};
}

struct Facet {
    Vec3d normal; ///< Unit.
    double area;
};

/// The direction (either sign) most of the facets' area faces, refined to the principal axis of
/// those facing it; zero when no facet has a normal.
Vec3d mostFaced(const std::vector<Facet>& facets) {
    const double facing = std::cos(kFacingDeg * std::numbers::pi / 180.0);
    const std::size_t stride = std::max<std::size_t>(1, facets.size() / kCandidates);
    Vec3d best;
    double bestArea = 0.0;
    for (std::size_t i = 0; i < facets.size(); i += stride) {
        double area = 0.0;
        for (const Facet& f : facets) {
            if (std::abs(dot(f.normal, facets[i].normal)) > facing) area += f.area;
        }
        if (area > bestArea) {
            bestArea = area;
            best = facets[i].normal;
        }
    }
    if (bestArea == 0.0) return {};
    Eigen::Matrix3d m = Eigen::Matrix3d::Zero();
    for (const Facet& f : facets) {
        if (std::abs(dot(f.normal, best)) > facing) {
            const Eigen::Vector3d n(f.normal.x, f.normal.y, f.normal.z);
            m += f.area * n * n.transpose();
        }
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(m);
    const Eigen::Vector3d e = solver.eigenvectors().col(2); // the largest eigenvalue's
    return unit({.x = e.x(), .y = e.y(), .z = e.z()});
}

/// Start axes from the faces' normals: the most-faced direction, then the most-faced direction
/// across it (any perpendicular one when no face is across it).
std::array<Vec3d, 3> startAxes(const TriMesh& mesh, std::span<const std::uint32_t> faces) {
    std::vector<Facet> facets;
    for (const std::uint32_t f : faces) {
        const auto [i, j, k] = mesh.face(f);
        const Vec3d& a = mesh.positions()[i];
        const Vec3d n = cross(mesh.positions()[j] - a, mesh.positions()[k] - a);
        if (length(n) > 0.0) facets.push_back({.normal = unit(n), .area = length(n) / 2.0});
    }
    Vec3d first = mostFaced(facets);
    if (length(first) == 0.0) first = {.x = 0, .y = 0, .z = 1};
    const double across = std::sin(kFacingDeg * std::numbers::pi / 180.0);
    std::vector<Facet> side;
    for (const Facet& f : facets) {
        const Vec3d flat = f.normal - first * dot(f.normal, first);
        if (std::abs(dot(f.normal, first)) < across && length(flat) > 0.0) {
            side.push_back({.normal = unit(flat), .area = f.area});
        }
    }
    Vec3d second = mostFaced(side);
    second = unit(second - first * dot(second, first));
    if (length(second) == 0.0) {
        second = unit(std::abs(first.x) < 0.9 ? cross(first, Vec3d{.x = 1, .y = 0, .z = 0})
                                              : cross(first, Vec3d{.x = 0, .y = 1, .z = 0}));
    }
    return {first, second, cross(first, second)};
}

/// The box around the points along the axes.
OrientedBox extentBox(std::span<const Vec3d> points, const std::array<Vec3d, 3>& axes) {
    std::array<double, 3> lo{}, hi{};
    lo.fill(std::numeric_limits<double>::infinity());
    hi.fill(-std::numeric_limits<double>::infinity());
    for (const Vec3d& p : points) {
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], dot(p, axes[k]));
            hi[k] = std::max(hi[k], dot(p, axes[k]));
        }
    }
    OrientedBox box{
        .center = {}, .axes = axes, .sizeMm = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}};
    for (std::size_t k = 0; k < 3; ++k) box.center = box.center + axes[k] * ((lo[k] + hi[k]) / 2.0);
    return box;
}

double& size(OrientedBox& box, std::size_t k) {
    return k == 0 ? box.sizeMm.x : k == 1 ? box.sizeMm.y : box.sizeMm.z;
}

double size(const OrientedBox& box, std::size_t k) {
    return k == 0 ? box.sizeMm.x : k == 1 ? box.sizeMm.y : box.sizeMm.z;
}

double rmsDistance(const OrientedBox& box, std::span<const Vec3d> points) {
    double sum2 = 0.0;
    for (const Vec3d& p : points) sum2 += distanceTo(box, p) * distanceTo(box, p);
    return std::sqrt(sum2 / static_cast<double>(points.size()));
}

/// v turned by the rotation vector w (Rodrigues).
Vec3d turned(const Vec3d& v, const Vec3d& w) {
    const double angle = length(w);
    if (angle == 0.0) return v;
    const Vec3d k = w * (1.0 / angle);
    return v * std::cos(angle) + cross(k, v) * std::sin(angle) +
           k * (dot(k, v) * (1.0 - std::cos(angle)));
}

/// One Gauss-Newton step: each point's residual is its distance outside (or inside) the plane of
/// the box face nearest it, over the centre, a turn of the axes and the half-sizes.
OrientedBox step(const OrientedBox& box, std::span<const Vec3d> points) {
    Eigen::Matrix<double, 9, 9> jtj = Eigen::Matrix<double, 9, 9>::Zero();
    Eigen::Matrix<double, 9, 1> jtr = Eigen::Matrix<double, 9, 1>::Zero();
    OrientedBox b = box;
    for (const Vec3d& p : points) {
        const Vec3d d = p - box.center;
        const Vec3d q{dot(d, box.axes[0]), dot(d, box.axes[1]), dot(d, box.axes[2])};
        const std::array qa{q.x, q.y, q.z};
        std::size_t k = 0;
        double worst = -std::numeric_limits<double>::infinity();
        for (std::size_t a = 0; a < 3; ++a) {
            const double g = std::abs(qa[a]) - size(b, a) / 2.0;
            if (g > worst) {
                worst = g;
                k = a;
            }
        }
        const double s = qa[k] < 0.0 ? -1.0 : 1.0;
        const Vec3d e{k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0};
        const Vec3d dc = box.axes[k] * -s; // centre
        const Vec3d dw = cross(e, q) * s;  // turn, in the box's frame
        Eigen::Matrix<double, 9, 1> j = Eigen::Matrix<double, 9, 1>::Zero();
        j << dc.x, dc.y, dc.z, dw.x, dw.y, dw.z, 0.0, 0.0, 0.0;
        j(6 + static_cast<Eigen::Index>(k)) = -1.0; // half-size
        jtj += j * j.transpose();
        jtr += j * worst;
    }
    jtj += kDamping * std::max(jtj.trace(), 1.0) * Eigen::Matrix<double, 9, 9>::Identity();
    const Eigen::Matrix<double, 9, 1> x = jtj.ldlt().solve(-jtr);
    b.center = b.center + Vec3d{x(0), x(1), x(2)};
    const Vec3d w = box.axes[0] * x(3) + box.axes[1] * x(4) + box.axes[2] * x(5);
    const Vec3d a0 = unit(turned(box.axes[0], w));
    const Vec3d a1 = unit(turned(box.axes[1], w) - a0 * dot(turned(box.axes[1], w), a0));
    b.axes = {a0, a1, cross(a0, a1)};
    for (std::size_t k = 0; k < 3; ++k) size(b, k) += 2.0 * x(6 + static_cast<Eigen::Index>(k));
    return b;
}

/// The same box with its axes ordered and signed to follow the world's x, y and z.
OrientedBox worldOrdered(const OrientedBox& box) {
    OrientedBox out = box;
    std::array used{false, false, false};
    for (std::size_t world = 0; world < 2; ++world) {
        std::size_t pick = 0;
        double best = -1.0;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::array c{box.axes[k].x, box.axes[k].y, box.axes[k].z};
            if (!used[k] && std::abs(c[world]) > best) {
                best = std::abs(c[world]);
                pick = k;
            }
        }
        used[pick] = true;
        const std::array c{box.axes[pick].x, box.axes[pick].y, box.axes[pick].z};
        out.axes[world] = c[world] < 0.0 ? box.axes[pick] * -1.0 : box.axes[pick];
        size(out, world) = size(box, pick);
    }
    const std::size_t last = !used[0] ? 0 : !used[1] ? 1 : 2;
    out.axes[2] = cross(out.axes[0], out.axes[1]);
    size(out, 2) = size(box, last);
    return out;
}

} // namespace

Result<BoxFit>
fitBox(const TriMesh& mesh, std::span<const std::uint32_t> faces, const BoxFitParams& params) {
    if (!(params.maxResidualMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the residual limit must be positive");
    }
    if (faces.empty()) return fail(ErrorCode::InvalidArgument, "the region has no faces");
    if (std::ranges::any_of(faces, [&](std::uint32_t f) { return f >= mesh.faceCount(); })) {
        return fail(ErrorCode::InvalidArgument, "face index out of range");
    }
    const std::vector<Vec3d> points = regionPoints(mesh, faces);
    OrientedBox best = extentBox(points, startAxes(mesh, faces));
    double bestRms = rmsDistance(best, points);
    OrientedBox current = best;
    for (int i = 0; i < kSteps; ++i) {
        const OrientedBox next = step(current, points);
        const double moved =
            length(next.center - current.center) + length(next.sizeMm - current.sizeMm) +
            length(next.axes[0] - current.axes[0]) + length(next.axes[1] - current.axes[1]);
        current = next;
        const double rms = rmsDistance(current, points);
        if (rms < bestRms) {
            bestRms = rms;
            best = current;
        }
        if (moved < kStepTolerance * (1.0 + length(current.sizeMm))) break;
    }
    BoxFit fit{.box = worldOrdered(best), .rmsMm = bestRms};
    std::vector<double> d;
    d.reserve(points.size());
    for (const Vec3d& p : points) d.push_back(std::abs(distanceTo(fit.box, p)));
    fit.maxMm = std::ranges::max(d);
    const auto at = d.begin() + static_cast<std::ptrdiff_t>(
                                    std::ceil(0.95 * static_cast<double>(d.size())) - 1.0);
    std::ranges::nth_element(d, at);
    fit.p95Mm = *at;
    if (!(fit.p95Mm <= params.maxResidualMm)) {
        return fail(ErrorCode::InvalidState,
                    "the box fits with a P95 residual of " + std::to_string(fit.p95Mm) +
                        " mm, above the limit of " + std::to_string(params.maxResidualMm) + " mm");
    }
    return fit;
}

} // namespace m2c

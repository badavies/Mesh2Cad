// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Align.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <Eigen/Dense>

#include "m2c/core/MeshTopology.h"
#include "m2c/core/SpatialIndex.h"

namespace m2c {

namespace {

Eigen::Vector3d toEigen(const Vec3d& v) {
    return {v.x, v.y, v.z};
}
Vec3d fromEigen(const Eigen::Vector3d& v) {
    return {.x = v.x(), .y = v.y(), .z = v.z()};
}

/// Area-weighted first and second moments of a set of triangles (exact for the triangle
/// surfaces: integral of x x^T over a triangle is A/12 * (sum v_i v_i^T + s s^T), s = sum v_i).
struct Moments {
    double area{0.0};
    Eigen::Vector3d centroid{Eigen::Vector3d::Zero()};
    Eigen::Matrix3d covariance{Eigen::Matrix3d::Zero()};
    Eigen::Vector3d averageNormal{Eigen::Vector3d::Zero()};
};

template <class FaceFilter>
Moments surfaceMoments(const TriMesh& mesh, FaceFilter&& use) {
    Moments m;
    Eigen::Matrix3d second = Eigen::Matrix3d::Zero();
    Eigen::Vector3d first = Eigen::Vector3d::Zero();
    const auto& p = mesh.positions();
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        if (!use(f)) continue;
        const auto [a, b, c] = mesh.face(f);
        const Eigen::Vector3d va = toEigen(p[a]);
        const Eigen::Vector3d vb = toEigen(p[b]);
        const Eigen::Vector3d vc = toEigen(p[c]);
        const Eigen::Vector3d n = (vb - va).cross(vc - va);
        const double area = 0.5 * n.norm();
        if (!(area > 0.0)) continue;
        const Eigen::Vector3d s = va + vb + vc;
        m.area += area;
        first += area * s / 3.0;
        second +=
            area / 12.0 *
            (va * va.transpose() + vb * vb.transpose() + vc * vc.transpose() + s * s.transpose());
        m.averageNormal += n; // area-weighted (|n| = 2 area)
    }
    if (m.area > 0.0) {
        m.centroid = first / m.area;
        m.covariance = second / m.area - m.centroid * m.centroid.transpose();
    }
    return m;
}

Transform3d fromFrame(const Eigen::Vector3d& x,
                      const Eigen::Vector3d& y,
                      const Eigen::Vector3d& z,
                      const Eigen::Vector3d& origin) {
    Transform3d t = Transform3d::fromAxes(fromEigen(x), fromEigen(y), fromEigen(z), {});
    t.translation = t.rotate(fromEigen(origin)) * -1.0;
    return t;
}

/// Sign that makes the third central moment along `axis` positive (falls back to +1).
double skewSign(const TriMesh& mesh, const Eigen::Vector3d& centroid, const Eigen::Vector3d& axis) {
    double third = 0.0;
    double scale = 0.0;
    for (const Vec3d& v : mesh.positions()) {
        const double d = (toEigen(v) - centroid).dot(axis);
        third += d * d * d;
        scale += std::abs(d * d * d);
    }
    return third < -1e-9 * scale ? -1.0 : 1.0;
}

/// Shifts an aligned part so its bounding box is centred on the origin (or sits on Z = 0) as
/// `origin` asks. With `shiftX` false the X position is left alone.
void moveOrigin(const TriMesh& mesh, const AlignOrigin origin, const bool shiftX, Transform3d& t) {
    if (origin == AlignOrigin::Centroid) return;
    Aabb box{.min = t.apply(mesh.positions().front()), .max = t.apply(mesh.positions().front())};
    for (const Vec3d& v : mesh.positions()) {
        const auto [qx, qy, qz] = t.apply(v);
        box.min = {.x = std::min(box.min.x, qx),
                   .y = std::min(box.min.y, qy),
                   .z = std::min(box.min.z, qz)};
        box.max = {.x = std::max(box.max.x, qx),
                   .y = std::max(box.max.y, qy),
                   .z = std::max(box.max.z, qz)};
    }
    Vec3d shift = (box.min + box.max) * -0.5;
    if (origin == AlignOrigin::BoundsBottomCenter) shift.z = -box.min.z;
    if (!shiftX) shift.x = 0.0;
    t.translation = t.translation + shift;
}

} // namespace

Result<PlaneFit> fitPlane(const TriMesh& mesh, const MeshSelection& faces) {
    if (faces.size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    const auto [area, centroid, covariance, averageNormal] =
        surfaceMoments(mesh, [&](const std::size_t f) { return faces.test(f); });
    if (!(area > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "cannot fit a plane: the selection has no area");
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    Eigen::Vector3d normal = solver.eigenvectors().col(0); // smallest eigenvalue
    if (normal.dot(averageNormal) < 0.0) normal = -normal;

    double sum2 = 0.0;
    std::size_t count = 0;
    std::vector seen(mesh.vertexCount(), false);
    for (const std::uint32_t f : faces.indices()) {
        for (const std::uint32_t v : mesh.face(f)) {
            if (seen[v]) continue;
            seen[v] = true;
            const double d = (toEigen(mesh.positions()[v]) - centroid).dot(normal);
            sum2 += d * d;
            ++count;
        }
    }
    return PlaneFit{.point = fromEigen(centroid),
                    .normal = fromEigen(normal),
                    .rmsMm = count > 0 ? std::sqrt(sum2 / static_cast<double>(count)) : 0.0};
}

Result<Transform3d> principalAxesAlignment(const TriMesh& mesh, const AlignOrigin origin) {
    const Moments m = surfaceMoments(mesh, [](std::size_t) { return true; });
    if (!(m.area > 0.0)) return fail(ErrorCode::InvalidArgument, "mesh has no surface area");
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(m.covariance);
    // Eigenvalues ascend: column 2 is the largest spread (X), column 0 the smallest (Z).
    Eigen::Vector3d x = solver.eigenvectors().col(2);
    Eigen::Vector3d y = solver.eigenvectors().col(1);
    x *= skewSign(mesh, m.centroid, x);
    y *= skewSign(mesh, m.centroid, y);
    const Eigen::Vector3d z = x.cross(y);
    Transform3d t = fromFrame(x, y, z, m.centroid);
    moveOrigin(mesh, origin, true, t);
    return t;
}

Result<SymmetryAlignment> symmetryAlignment(const TriMesh& mesh, AlignOrigin origin) {
    const Moments m = surfaceMoments(mesh, [](std::size_t) { return true; });
    if (!(m.area > 0.0)) return fail(ErrorCode::InvalidArgument, "mesh has no surface area");
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(m.covariance);

    // Face centroids weighted by area stand for the surface, so a finely meshed patch counts no
    // more than a coarse one of the same size. An evenly spread subset keeps large scans quick.
    constexpr std::size_t maxSamples = 20000;
    const std::size_t stride = std::max<std::size_t>(1, mesh.faceCount() / maxSamples);
    std::vector<Eigen::Vector3d> samples;
    std::vector<double> weights;
    double totalWeight = 0.0;
    for (std::size_t f = 0; f < mesh.faceCount(); f += stride) {
        const auto [a, b, c] = mesh.face(f);
        const Eigen::Vector3d va = toEigen(mesh.positions()[a]);
        const Eigen::Vector3d vb = toEigen(mesh.positions()[b]);
        const Eigen::Vector3d vc = toEigen(mesh.positions()[c]);
        const double area = 0.5 * (vb - va).cross(vc - va).norm();
        if (!(area > 0.0)) continue;
        samples.emplace_back((va + vb + vc) / 3.0);
        weights.push_back(area);
        totalWeight += area;
    }
    if (samples.empty()) return fail(ErrorCode::InvalidArgument, "mesh has no surface area");

    const SpatialIndex& index = mesh.spatialIndex();
    // The plane is n . (x - centroid) = offset.
    struct Plane {
        Eigen::Vector3d normal;
        double offset{0.0};
        double residual{0.0};
    };
    struct Match {
        Eigen::Vector3d image;   // the sample mirrored in the plane
        Eigen::Vector3d nearest; // surface point nearest the image
        Eigen::Vector3d normal;  // surface normal there
        double gap{0.0};
    };
    std::vector<Match> matches(samples.size());
    // Mirrors every sample and finds the surface point nearest its image; returns the RMS gap.
    const auto match = [&](const Plane& plane) {
        double sum2 = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Eigen::Vector3d local = samples[i] - m.centroid;
            auto& [image, nearest, normal, gap] = matches[i];
            image = samples[i] - 2.0 * (plane.normal.dot(local) - plane.offset) * plane.normal;
            const auto [face, distance, point] = index.closestPoint(fromEigen(image));
            nearest = toEigen(point);
            normal = toEigen(faceNormal(mesh, face));
            gap = distance;
            sum2 += weights[i] * distance * distance;
        }
        return std::sqrt(sum2 / totalWeight);
    };

    std::optional<Plane> best;
    std::vector<std::size_t> order;
    for (int axis = 0; axis < 3; ++axis) {
        const Eigen::Vector3d n0 = solver.eigenvectors().col(axis);
        Plane plane{.normal = n0};
        plane.residual = match(plane);
        Plane bestHere = plane; // a step can make the fit worse when the left-out set changes
        int sinceBest = 0;
        // Gauss-Newton on the plane: move it so each mirrored sample lands on the tangent plane
        // of the surface point nearest it (point-to-plane ICP).
        for (int iteration = 0; iteration < 50; ++iteration) {
            // Matches further off than three times the (area-weighted) median are left out, so
            // an asymmetric feature does not pull the plane.
            order.resize(samples.size());
            std::iota(order.begin(), order.end(), std::size_t{0});
            std::ranges::sort(order, [&](const std::size_t i, const std::size_t j) {
                return matches[i].gap < matches[j].gap;
            });
            double below = 0.0;
            double median = 0.0;
            for (const std::size_t i : order) {
                below += weights[i];
                median = matches[i].gap;
                if (below >= 0.5 * totalWeight) break;
            }
            const double cutoff = 3.0 * median + 1e-9;

            // Unknowns: the normal tilts by a u + b v (u, v across it) and the offset by c.
            const Eigen::Vector3d u = plane.normal.unitOrthogonal();
            const Eigen::Vector3d v = plane.normal.cross(u);
            Eigen::Matrix3d a = Eigen::Matrix3d::Zero();
            Eigen::Vector3d b = Eigen::Vector3d::Zero();
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto& [image, nearest, normal, gap] = matches[i];
                if (gap > cutoff) continue;
                const Eigen::Vector3d local = samples[i] - m.centroid;
                const double h = plane.normal.dot(local) - plane.offset;
                const double sn = normal.dot(plane.normal);
                const Eigen::Vector3d row{-2.0 * (u.dot(local) * sn + h * normal.dot(u)),
                                          -2.0 * (v.dot(local) * sn + h * normal.dot(v)),
                                          2.0 * sn};
                const double r = normal.dot(image - nearest);
                a += weights[i] * row * row.transpose();
                b -= weights[i] * row * r;
            }
            const Eigen::Vector3d step = a.ldlt().solve(b);
            if (!step.allFinite()) break;
            Plane next{.normal = (plane.normal + step(0) * u + step(1) * v).normalized(),
                       .offset = plane.offset + step(2)};
            if (next.normal.dot(n0) < std::cos(30.0 * 3.141592653589793 / 180.0)) break; // ran off
            next.residual = match(next);
            plane = next;
            if (plane.residual < bestHere.residual * (1.0 - 1e-6)) {
                bestHere = plane;
                sinceBest = 0;
            } else if (++sinceBest == 5) {
                break; // wandering without getting better
            }
            if (step.head<2>().norm() < 1e-10 && std::abs(step(2)) < 1e-9) break;
        }
        if (!best || bestHere.residual < best->residual) best = bestHere;
    }
    best->offset += best->normal.dot(m.centroid); // now n . x = offset

    // X is the plane normal; Y and Z are the principal axes of the surface within the plane.
    const Eigen::Vector3d u = best->normal.unitOrthogonal();
    const Eigen::Vector3d v = best->normal.cross(u);
    Eigen::Matrix<double, 3, 2> basis;
    basis << u, v;
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> inPlane(basis.transpose() * m.covariance *
                                                                 basis);
    Eigen::Vector3d y = basis * inPlane.eigenvectors().col(1); // the larger spread
    Eigen::Vector3d z = best->normal.cross(y);
    y *= skewSign(mesh, m.centroid, y);
    z *= skewSign(mesh, m.centroid, z);
    const Eigen::Vector3d x = y.cross(z);
    // The centroid moved onto the plane.
    const Eigen::Vector3d onPlane =
        m.centroid - (x.dot(m.centroid) - x.dot(best->normal) * best->offset) * x;
    Transform3d t = fromFrame(x, y, z, onPlane);
    moveOrigin(mesh, origin, false, t);
    return SymmetryAlignment{.transform = t,
                             .planePoint = fromEigen(onPlane),
                             .planeNormal = fromEigen(x),
                             .residualMm = best->residual};
}

Result<DatumAlignResult> datumAlignment(const TriMesh& mesh, const DatumAlignParams& params) {
    DatumAlignResult result;
    auto primary = fitPlane(mesh, params.primary);
    if (!primary) return std::unexpected(primary.error());
    result.primary = *primary;

    // Outward normal of the datum face points to -Z: the part sits on top of the XY plane.
    Eigen::Vector3d z = -toEigen(primary->normal);
    if (params.flipPrimary) z = -z;

    Eigen::Vector3d y;
    if (params.secondary) {
        auto secondary = fitPlane(mesh, *params.secondary);
        if (!secondary) return std::unexpected(secondary.error());
        result.secondary = *secondary;
        const Eigen::Vector3d n2 = toEigen(secondary->normal);
        const Eigen::Vector3d projected = n2 - n2.dot(z) * z;
        if (projected.norm() < std::sin(10.0 * 3.141592653589793 / 180.0)) {
            return fail(ErrorCode::InvalidArgument,
                        "secondary plane is within 10 degrees of parallel to the primary plane");
        }
        y = -projected.normalized();
    } else {
        // Use the in-plane principal direction for X.
        auto principal = principalAxesAlignment(mesh);
        if (!principal) return std::unexpected(principal.error());
        const auto& r = principal->rotation;
        Eigen::Vector3d px{r[0], r[1], r[2]};
        px -= px.dot(z) * z;
        if (px.norm() < 1e-6) px = z.unitOrthogonal(); // the long axis is the datum normal
        y = z.cross(px.normalized());
    }
    const Eigen::Vector3d x = y.cross(z);

    // Origin: on the primary plane, on the secondary plane (if any), and on the tertiary plane or
    // at the centroid's X.
    const Moments all = surfaceMoments(mesh, [](std::size_t) { return true; });
    Eigen::Matrix3d a;
    Eigen::Vector3d b;
    a.row(0) = toEigen(primary->normal).transpose();
    b(0) = toEigen(primary->normal).dot(toEigen(primary->point));
    if (result.secondary) {
        a.row(1) = toEigen(result.secondary->normal).transpose();
        b(1) = toEigen(result.secondary->normal).dot(toEigen(result.secondary->point));
    } else {
        a.row(1) = y.transpose();
        b(1) = y.dot(all.centroid);
    }
    if (params.tertiary) {
        auto tertiary = fitPlane(mesh, *params.tertiary);
        if (!tertiary) return std::unexpected(tertiary.error());
        result.tertiary = *tertiary;
        const Eigen::Vector3d n3 = toEigen(tertiary->normal);
        if (std::abs(n3.dot(x)) < std::sin(10.0 * 3.141592653589793 / 180.0)) {
            return fail(ErrorCode::InvalidArgument,
                        "tertiary plane is within 10 degrees of parallel to another datum plane");
        }
        a.row(2) = n3.transpose();
        b(2) = n3.dot(toEigen(tertiary->point));
    } else {
        a.row(2) = x.transpose();
        b(2) = x.dot(all.centroid);
    }
    const Eigen::Vector3d origin = a.colPivHouseholderQr().solve(b);
    result.transform = fromFrame(x, y, z, origin);
    return result;
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Align.h"

#include <cmath>
#include <vector>

#include <Eigen/Dense>

#include "m2c/core/MeshTopology.h"

namespace m2c {

namespace {

Eigen::Vector3d toEigen(const Vec3d& v) {
    return {v.x, v.y, v.z};
}
Vec3d fromEigen(const Eigen::Vector3d& v) {
    return {v.x(), v.y(), v.z()};
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

} // namespace

Result<PlaneFit> fitPlane(const TriMesh& mesh, const MeshSelection& faces) {
    if (faces.size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    const Moments m = surfaceMoments(mesh, [&](std::size_t f) { return faces.test(f); });
    if (!(m.area > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "cannot fit a plane: the selection has no area");
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(m.covariance);
    Eigen::Vector3d normal = solver.eigenvectors().col(0); // smallest eigenvalue
    if (normal.dot(m.averageNormal) < 0.0) normal = -normal;

    double sum2 = 0.0;
    std::size_t count = 0;
    std::vector<bool> seen(mesh.vertexCount(), false);
    for (const std::uint32_t f : faces.indices()) {
        for (const std::uint32_t v : mesh.face(f)) {
            if (seen[v]) continue;
            seen[v] = true;
            const double d = (toEigen(mesh.positions()[v]) - m.centroid).dot(normal);
            sum2 += d * d;
            ++count;
        }
    }
    return PlaneFit{fromEigen(m.centroid),
                    fromEigen(normal),
                    count > 0 ? std::sqrt(sum2 / static_cast<double>(count)) : 0.0};
}

Result<Transform3d> principalAxesAlignment(const TriMesh& mesh, AlignOrigin origin) {
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

    if (origin != AlignOrigin::Centroid) {
        Aabb box{t.apply(mesh.positions().front()), t.apply(mesh.positions().front())};
        for (const Vec3d& v : mesh.positions()) {
            const Vec3d q = t.apply(v);
            box.min = {
                std::min(box.min.x, q.x), std::min(box.min.y, q.y), std::min(box.min.z, q.z)};
            box.max = {
                std::max(box.max.x, q.x), std::max(box.max.y, q.y), std::max(box.max.z, q.z)};
        }
        Vec3d shift = (box.min + box.max) * -0.5;
        if (origin == AlignOrigin::BoundsBottomCenter) shift.z = -box.min.z;
        t.translation = t.translation + shift;
    }
    return t;
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
        if (px.norm() < 1e-6) px = Eigen::Vector3d::UnitX() - z.x() * z;
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

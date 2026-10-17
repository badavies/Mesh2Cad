// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Measure.h"

#include <limits>
#include <vector>

namespace m2c {

PickedPoint snapToMesh(const TriMesh& mesh,
                       std::uint64_t objectId,
                       const Vec3d& query,
                       double snapToleranceMm) {
    const ClosestPoint nearest = mesh.spatialIndex().closestPoint(query);
    PickedPoint picked{nearest.point, objectId, SnapKind::Surface, nearest.face};
    if (!(snapToleranceMm > 0.0)) return picked;

    // The vertex or edge may belong to a neighbouring face, so look at every face in reach.
    const auto& p = mesh.positions();
    const std::vector<std::uint32_t> faces =
        mesh.spatialIndex().facesWithin(nearest.point, snapToleranceMm);
    double bestVertex = std::numeric_limits<double>::infinity();
    for (const std::uint32_t f : faces) {
        for (const std::uint32_t v : mesh.face(f)) {
            const double d = length(p[v] - nearest.point);
            if (d <= snapToleranceMm && d < bestVertex) {
                bestVertex = d;
                picked = {p[v], objectId, SnapKind::Vertex, v};
            }
        }
    }
    if (picked.snap == SnapKind::Vertex) return picked;

    double bestEdge = std::numeric_limits<double>::infinity();
    for (const std::uint32_t f : faces) {
        const auto corners = mesh.face(f);
        for (int k = 0; k < 3; ++k) {
            const Vec3d a = p[corners[static_cast<std::size_t>(k)]];
            const Vec3d b = p[corners[static_cast<std::size_t>((k + 1) % 3)]];
            const Vec3d ab = b - a;
            const double len2 = dot(ab, ab);
            if (!(len2 > 0.0)) continue;
            double t = dot(nearest.point - a, ab) / len2;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            const Vec3d onEdge = a + ab * t;
            const double d = length(onEdge - nearest.point);
            if (d <= snapToleranceMm && d < bestEdge) {
                bestEdge = d;
                picked = {onEdge, objectId, SnapKind::Edge, f};
            }
        }
    }
    return picked;
}

std::optional<RayHit> pickMesh(const TriMesh& mesh, const Vec3d& origin, const Vec3d& direction) {
    return mesh.spatialIndex().raycast(origin, direction);
}

} // namespace m2c

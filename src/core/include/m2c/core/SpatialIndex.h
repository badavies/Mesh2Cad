// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "m2c/core/Vec3.h"

namespace m2c {

struct RayHit {
    std::uint32_t face{0};
    double distance{0.0}; ///< Along the (normalised) ray direction, mm.
    Vec3d point;
    Vec3d barycentric; ///< Weights of the face's three corners.
};

struct ClosestPoint {
    std::uint32_t face{0};
    double distance{0.0};
    Vec3d point;
};

/// Bounding-volume hierarchy over the triangles of one mesh, for ray picking, nearest-point and
/// radius queries (ARCHITECTURE.md section 5). Built once per mesh by TriMesh::spatialIndex();
/// meshes are immutable, so it never goes stale. Queries are const and thread-safe.
class SpatialIndex {
public:
    using PositionBuffer = std::shared_ptr<const std::vector<Vec3d>>;
    using IndexBuffer = std::shared_ptr<const std::vector<std::uint32_t>>;

    /// Builds the tree over the mesh buffers. Use TriMesh::spatialIndex() instead of calling this.
    SpatialIndex(PositionBuffer positions, IndexBuffer indices);

    /// Nearest intersection with distance in [0, maxDistance]. Both triangle sides are hit.
    [[nodiscard]] std::optional<RayHit>
    raycast(const Vec3d& origin, const Vec3d& direction, double maxDistance = 1e300) const;

    /// Nearest point on the surface; ties go to the lowest face index.
    [[nodiscard]] ClosestPoint closestPoint(const Vec3d& query) const;

    /// Faces with at least one point within `radius` of `center`, in increasing order.
    [[nodiscard]] std::vector<std::uint32_t> facesWithin(const Vec3d& center, double radius) const;

private:
    struct Node {
        Vec3d min;
        Vec3d max;
        std::uint32_t first{0}; ///< Leaf: first entry in faces_; inner: index of left child.
        std::uint32_t count{0}; ///< Leaf: number of faces; inner: 0.
    };

    [[nodiscard]] Vec3d corner(std::uint32_t face, int k) const noexcept {
        return (*positions_)[(*indices_)[3 * std::size_t{face} + static_cast<std::size_t>(k)]];
    }

    PositionBuffer positions_;
    IndexBuffer indices_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> faces_;
};

/// Closest point to p on triangle (a, b, c) (Ericson, Real-Time Collision Detection 5.1.5).
[[nodiscard]] Vec3d
closestPointOnTriangle(const Vec3d& p, const Vec3d& a, const Vec3d& b, const Vec3d& c) noexcept;

} // namespace m2c

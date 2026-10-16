// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Vec3.h"

namespace m2c {

class SpatialIndex;
struct FaceAdjacency;
struct MeshCaches;

struct Aabb {
    Vec3d min;
    Vec3d max;
};

/// Immutable indexed triangle mesh. Buffers are shared (`shared_ptr<const>`), so a tool that
/// changes only positions can reuse the index buffer and undo history is cheap. Share a mesh by
/// copying the `TriMesh` object; never mutate buffers after creation.
class TriMesh {
public:
    using PositionBuffer = std::shared_ptr<const std::vector<Vec3d>>;
    using IndexBuffer = std::shared_ptr<const std::vector<std::uint32_t>>;

    /// Validates and wraps the buffers. Fails when a buffer is null or empty, the index count is
    /// not a multiple of three, an index is out of range, or a coordinate is not finite.
    [[nodiscard]] static Result<TriMesh> create(PositionBuffer positions, IndexBuffer indices);

    /// Convenience overload that takes ownership of the vectors.
    [[nodiscard]] static Result<TriMesh> create(std::vector<Vec3d> positions,
                                                std::vector<std::uint32_t> indices);

    [[nodiscard]] std::size_t vertexCount() const noexcept { return positions_->size(); }
    [[nodiscard]] std::size_t faceCount() const noexcept { return indices_->size() / 3; }

    [[nodiscard]] const std::vector<Vec3d>& positions() const noexcept { return *positions_; }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const noexcept { return *indices_; }

    [[nodiscard]] const PositionBuffer& sharedPositions() const noexcept { return positions_; }
    [[nodiscard]] const IndexBuffer& sharedIndices() const noexcept { return indices_; }

    /// Vertex indices of face `f` (counter-clockwise seen from outside). Precondition: f <
    /// faceCount().
    [[nodiscard]] std::array<std::uint32_t, 3> face(std::size_t f) const noexcept {
        const auto& idx = *indices_;
        return {idx[3 * f], idx[3 * f + 1], idx[3 * f + 2]};
    }

    /// Bounding box of all vertices.
    [[nodiscard]] const Aabb& bounds() const noexcept { return bounds_; }

    /// Bounding-volume hierarchy for picking and nearest-point queries. Built on first use
    /// (thread-safe) and shared by every copy of this mesh.
    [[nodiscard]] const SpatialIndex& spatialIndex() const;

    /// Face-to-face adjacency (MeshTopology.h). Built on first use (thread-safe) and shared by
    /// every copy of this mesh, so selection tools on a large scan do not rebuild it per call.
    [[nodiscard]] const FaceAdjacency& faceAdjacency() const;

private:
    TriMesh(PositionBuffer positions, IndexBuffer indices, Aabb bounds);

    PositionBuffer positions_;
    IndexBuffer indices_;
    Aabb bounds_;
    std::shared_ptr<MeshCaches> caches_;
};

} // namespace m2c

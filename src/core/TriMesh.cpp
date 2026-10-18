// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/TriMesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/SpatialIndex.h"

namespace m2c {

/// Derived data built on first use. Meshes are immutable, so it never goes stale.
struct MeshCaches {
    std::once_flag indexOnce;
    std::unique_ptr<SpatialIndex> index;
    std::once_flag adjacencyOnce;
    FaceAdjacency adjacency;
};

TriMesh::TriMesh(PositionBuffer positions, IndexBuffer indices, const Aabb& bounds)
    : positions_(std::move(positions)), indices_(std::move(indices)), bounds_(bounds),
      caches_(std::make_shared<MeshCaches>()) {}

const SpatialIndex& TriMesh::spatialIndex() const {
    std::call_once(caches_->indexOnce, [this] {
        caches_->index = std::make_unique<SpatialIndex>(positions_, indices_);
    });
    return *caches_->index;
}

const FaceAdjacency& TriMesh::faceAdjacency() const {
    std::call_once(caches_->adjacencyOnce,
                   [this] { caches_->adjacency = buildFaceAdjacency(*this); });
    return caches_->adjacency;
}

Result<TriMesh> TriMesh::create(PositionBuffer positions, IndexBuffer indices) {
    if (!positions || !indices) {
        return fail(ErrorCode::InvalidArgument, "mesh buffers must not be null");
    }
    if (positions->empty()) {
        return fail(ErrorCode::InvalidArgument, "mesh has no vertices");
    }
    if (indices->empty()) {
        return fail(ErrorCode::InvalidArgument, "mesh has no faces");
    }
    if (indices->size() % 3 != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "index count " + std::to_string(indices->size()) + " is not a multiple of 3");
    }
    if (positions->size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::InvalidArgument, "mesh has more than 2^32-1 vertices");
    }

    Aabb box{.min = positions->front(), .max = positions->front()};
    for (std::size_t i = 0; i < positions->size(); ++i) {
        const auto& [x, y, z] = (*positions)[i];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return fail(ErrorCode::InvalidArgument,
                        "vertex " + std::to_string(i) + " has a non-finite coordinate");
        }
        box.min = {
            .x = std::min(box.min.x, x), .y = std::min(box.min.y, y), .z = std::min(box.min.z, z)};
        box.max = {
            .x = std::max(box.max.x, x), .y = std::max(box.max.y, y), .z = std::max(box.max.z, z)};
    }

    const auto vertexCount = positions->size();
    for (std::size_t i = 0; i < indices->size(); ++i) {
        if ((*indices)[i] >= vertexCount) {
            return fail(ErrorCode::InvalidArgument,
                        "face " + std::to_string(i / 3) + " references vertex " +
                            std::to_string((*indices)[i]) + " but the mesh has only " +
                            std::to_string(vertexCount) + " vertices");
        }
    }

    return TriMesh(std::move(positions), std::move(indices), box);
}

Result<TriMesh> TriMesh::create(std::vector<Vec3d> positions, std::vector<std::uint32_t> indices) {
    return create(std::make_shared<const std::vector<Vec3d>>(std::move(positions)),
                  std::make_shared<const std::vector<std::uint32_t>>(std::move(indices)));
}

} // namespace m2c

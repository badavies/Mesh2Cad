// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/TriMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

#include "m2c/core/SpatialIndex.h"

namespace m2c {

struct LazySpatialIndex {
    std::once_flag once;
    std::unique_ptr<SpatialIndex> index;
};

TriMesh::TriMesh(PositionBuffer positions, IndexBuffer indices, Aabb bounds)
    : positions_(std::move(positions)), indices_(std::move(indices)), bounds_(bounds),
      index_(std::make_shared<LazySpatialIndex>()) {}

const SpatialIndex& TriMesh::spatialIndex() const {
    std::call_once(index_->once, [this] {
        index_->index = std::make_unique<SpatialIndex>(positions_, indices_);
    });
    return *index_->index;
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

    Aabb box{positions->front(), positions->front()};
    for (std::size_t i = 0; i < positions->size(); ++i) {
        const Vec3d& p = (*positions)[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return fail(ErrorCode::InvalidArgument,
                        "vertex " + std::to_string(i) + " has a non-finite coordinate");
        }
        box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
        box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Transform.h"

#include <memory>
#include <vector>

namespace m2c {

Result<TriMesh> transformMesh(const TriMesh& mesh, const Transform3d& transform) {
    std::vector<Vec3d> positions;
    positions.reserve(mesh.vertexCount());
    for (const Vec3d& p : mesh.positions()) positions.push_back(transform.apply(p));
    return TriMesh::create(std::make_shared<const std::vector<Vec3d>>(std::move(positions)),
                           mesh.sharedIndices());
}

} // namespace m2c

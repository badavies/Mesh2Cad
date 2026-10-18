// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/MeshTools.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

Result<TriMesh> deleteFaces(const TriMesh& mesh, const MeshSelection& faces) {
    if (faces.size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    std::vector<bool> keep(mesh.faceCount());
    for (std::size_t f = 0; f < keep.size(); ++f) keep[f] = !faces.test(f);
    return extractFaces(mesh, keep);
}

Result<TriMesh> keepFaces(const TriMesh& mesh, const MeshSelection& faces) {
    if (faces.size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    std::vector<bool> keep(mesh.faceCount());
    for (std::size_t f = 0; f < keep.size(); ++f) keep[f] = faces.test(f);
    return extractFaces(mesh, keep);
}

Result<TriMesh> flipFaces(const TriMesh& mesh) {
    std::vector<std::uint32_t> indices;
    indices.reserve(mesh.indices().size());
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        indices.insert(indices.end(), {a, c, b});
    }
    return TriMesh::create(mesh.sharedPositions(),
                           std::make_shared<const std::vector<std::uint32_t>>(std::move(indices)));
}

} // namespace m2c

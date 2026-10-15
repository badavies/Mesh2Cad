// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

#include "m2c/core/TriMesh.h"

namespace m2c {

/// Topology and size summary of a mesh. Computed serially in index order, so results are
/// reproducible on one platform.
struct MeshStats {
    std::size_t vertexCount{0};
    std::size_t faceCount{0};
    std::size_t edgeCount{0};            ///< Distinct undirected edges.
    std::size_t boundaryEdgeCount{0};    ///< Edges used by exactly one face (holes).
    std::size_t nonManifoldEdgeCount{0}; ///< Edges used by three or more faces.
    std::size_t degenerateFaceCount{
        0};                   ///< Zero-area faces, including faces that repeat a vertex index.
                              ///< Faces that repeat an index are ignored for topology counts.
    double surfaceArea{0.0};  ///< mm^2.
    double signedVolume{0.0}; ///< mm^3; meaningful only for closed, consistently oriented meshes.
    Aabb bounds{};

    /// Every edge is shared by exactly two faces. Does not check that faces are consistently
    /// oriented; orientation repair belongs to the M1 repair stage.
    [[nodiscard]] bool isWatertight() const noexcept {
        return faceCount > 0 && boundaryEdgeCount == 0 && nonManifoldEdgeCount == 0;
    }
};

[[nodiscard]] MeshStats computeStats(const TriMesh& mesh);

} // namespace m2c

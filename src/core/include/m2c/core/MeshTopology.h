// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "m2c/core/TriMesh.h"

namespace m2c {

/// Face-to-face adjacency across shared edges, in compressed rows: the neighbours of face f are
/// `neighbours[offsets[f] .. offsets[f+1])`, in increasing order. Faces meeting at a non-manifold
/// edge are all neighbours of each other. Faces that repeat a vertex have no neighbours.
struct FaceAdjacency {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> neighbours;
};

[[nodiscard]] FaceAdjacency buildFaceAdjacency(const TriMesh& mesh);

/// Connected component id per face (connected through shared edges). Components are numbered in
/// order of their lowest face index, so the numbering is deterministic.
[[nodiscard]] std::vector<std::uint32_t> faceComponents(const TriMesh& mesh,
                                                        std::uint32_t* componentCount = nullptr);

/// Unit normal of face f, or zero for a degenerate face.
[[nodiscard]] Vec3d faceNormal(const TriMesh& mesh, std::size_t f) noexcept;

[[nodiscard]] double faceArea(const TriMesh& mesh, std::size_t f) noexcept;

/// Keeps the listed faces (keep[f] == true) and drops vertices no kept face uses. Vertex and face
/// order are preserved. Fails if no face is kept.
[[nodiscard]] Result<TriMesh> extractFaces(const TriMesh& mesh, const std::vector<bool>& keep);

} // namespace m2c

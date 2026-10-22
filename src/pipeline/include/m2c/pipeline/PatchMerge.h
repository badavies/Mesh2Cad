// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Patch merging (ARCHITECTURE.md section 6, Keeping the face count low): the quads of a cage are
// split into rectangles, and the faces of each rectangle become one multi-span face.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SurfacePatch.h"

namespace m2c {

/// cellsU x cellsV cage quads forming a grid. Cell (i, j) is cage face faces[j * cellsU + i]; its
/// corner rotations[...] (an index into that face's vertices) is the cell's corner at (0, 0), and
/// the next vertex of the face its corner at (1, 0).
struct QuadRectangle {
    std::uint32_t cellsU{1};
    std::uint32_t cellsV{1};
    std::vector<std::uint32_t> faces;
    std::vector<std::uint8_t> rotations;
};

struct QuadPartition {
    std::vector<QuadRectangle> rectangles; ///< Every quad of the cage is in exactly one.
    std::size_t lines{0};                  ///< Partition lines traced.
    /// Quads of regions that were not rectangles (bands with no corner), left as one each.
    std::size_t unmergedQuads{0};
};

/// Splits the quads of a closed cage into rectangles whose corners lie at extraordinary vertices,
/// at the corners of faces with other than four sides, or where partition lines meet: from each
/// such vertex a line runs straight along cage edges until it reaches another such vertex or a
/// line (a motorcycle graph). Faces with other than four sides are in no rectangle. Fences,
/// edges given as vertex pairs (such as sharp edges between surfaces), bound rectangles too but
/// do not stop the lines, so the rectangles on both sides of a fence break at the same vertices.
/// Where fences meet other than as one straight line (three at a part's corner, or one that
/// ends or turns), the vertex is a corner too, and lines leave it along its other edges.
/// Deterministic.
[[nodiscard]] QuadPartition
partitionQuads(const QuadLayout& cage,
               const std::vector<std::array<std::uint32_t, 2>>& fences = {});

struct MergeResult {
    std::vector<SurfacePatch> faces;
    std::size_t mergedFaces{0}; ///< Faces made from more than one cage quad.
    QuadPartition partition;
};

/// Merges the faces of a closed cage's limit surface (one per ptex face, as extractPatches gives
/// them) along partitionQuads(): each rectangle of quads becomes one face whose pieces are its
/// quads' pieces, split where needed so every column and row of the grid has the same breaks.
/// The surface is unchanged. Faces of cage faces with other than four sides stay as they are.
[[nodiscard]] Result<MergeResult>
mergeFaces(const QuadLayout& cage,
           const std::vector<SurfacePatch>& faces,
           const std::vector<std::array<std::uint32_t, 2>>& fences = {});

} // namespace m2c

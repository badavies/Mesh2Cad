// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Selection tools (ARCHITECTURE.md section 7). Each returns a new selection for `mesh`.

/// Faces within `radiusMm` of `center`. With `connected`, only those reachable from the face
/// nearest the centre without leaving the brush (so a brush does not jump across thin walls).
[[nodiscard]] MeshSelection
brushSelect(const TriMesh& mesh, const Vec3d& center, double radiusMm, bool connected);

/// Grows from `seed` across edges whose dihedral angle is at most `maxAngleDeg`.
[[nodiscard]] Result<MeshSelection>
creaseFloodSelect(const TriMesh& mesh, std::uint32_t seed, double maxAngleDeg);

struct LassoParams {
    /// Column-major 4x4 view-projection matrix (OpenGL convention).
    std::array<double, 16> viewProjection{};
    /// Polygon in normalised device coordinates (x, y in -1..1).
    std::vector<std::array<double, 2>> polygon;
    /// Camera position: with it, faces turned away from the camera are skipped.
    std::optional<Vec3d> eye;
    /// Skip faces hidden behind other faces (needs `eye`).
    bool visibleOnly{false};
};

/// Faces whose centroid projects inside the polygon.
[[nodiscard]] Result<MeshSelection> lassoSelect(const TriMesh& mesh, const LassoParams& params);

/// Adds (rings > 0) or removes (rings < 0) rings of faces along the selection border.
[[nodiscard]] MeshSelection
growSelection(const TriMesh& mesh, const MeshSelection& selection, int rings);

/// Every face, or none.
[[nodiscard]] MeshSelection allFaces(const TriMesh& mesh);

} // namespace m2c

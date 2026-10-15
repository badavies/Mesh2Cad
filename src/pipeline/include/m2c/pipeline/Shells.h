// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "m2c/core/MeshSelection.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// One connected component (shell) of a mesh.
struct ShellInfo {
    std::uint32_t index{0}; ///< Numbered by lowest face index.
    std::size_t faceCount{0};
    double areaMm2{0.0};
    Aabb bounds{};
    bool isMain{false}; ///< The shell with the most faces (lowest index on a tie).
};

struct ShellReport {
    std::vector<std::uint32_t> faceShell; ///< Shell index of every face.
    std::vector<ShellInfo> shells;        ///< In index order.
};

/// Shell detection (ARCHITECTURE.md section 7): unconnected parts such as stray scan fragments.
[[nodiscard]] ShellReport detectShells(const TriMesh& mesh);

/// Faces of the shells whose index is listed.
[[nodiscard]] MeshSelection shellFaces(const ShellReport& report,
                                       const std::vector<std::uint32_t>& shells);

/// Faces of every shell that is smaller than the limits (a shell is small if it has fewer than
/// `minFaces` faces or less than `minAreaMm2` area; a zero limit is ignored). The main shell is
/// never included.
[[nodiscard]] MeshSelection
smallShellFaces(const ShellReport& report, std::size_t minFaces, double minAreaMm2);

} // namespace m2c

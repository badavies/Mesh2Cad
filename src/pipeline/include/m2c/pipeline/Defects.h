// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Progress.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Small scan defects left by stray points when a scanner meshes its point cloud: spikes (sharp
/// tips and slivers folded back on themselves) and tunnels (small handles through the surface).
/// They are found to be deleted and the holes filled. Expects a manifold, consistently oriented
/// mesh, as mesh.repair leaves it.
struct ScanDefectParams {
    /// Largest defect to select, in faces. Bigger ones are taken to be real features.
    std::size_t maxFaces{200};
    bool spikes{true};
    bool tunnels{true};
    /// A spike tip is a vertex whose face corners add up to less than this (360 on a smooth
    /// surface, 270 at the corner of a box, about 100 at the tip of a needle three edges long).
    double tipAngleDeg{150.0};
    /// A fold is an edge whose two faces' normals differ by more than this (90 at the edge of a
    /// box, near 180 where a sliver folds back on itself).
    double foldAngleDeg{150.0};
};

struct ScanDefectReport {
    MeshSelection faces;     ///< Every face of every defect found.
    std::size_t spikes{0};   ///< Spikes and folds selected (each a connected group of faces).
    std::size_t tunnels{0};  ///< Tunnels selected.
    std::size_t tooLarge{0}; ///< Spikes and folds left alone for having more than maxFaces faces.
};

/// Spikes are the faces around tips and along folds, grouped into connected clusters; clusters of
/// at most maxFaces faces are selected. A tunnel is a handle of the surface: for each vertex the
/// faces are taken in rings around it until they hold a handle, and the smallest such patch of at
/// most maxFaces faces is selected. Deleting a selected patch and filling its hole removes the
/// handle.
[[nodiscard]] Result<ScanDefectReport>
findScanDefects(const TriMesh& mesh, const ScanDefectParams& params, const Progress& progress = {});

/// Genus of the selected faces taken as a surface (after splitting any vertex where they only
/// touch): the number of handles they hold, summed over their connected parts. A disk, a sphere
/// or a tube has none, a torus one.
[[nodiscard]] std::size_t surfaceGenus(const TriMesh& mesh, const MeshSelection& faces);

} // namespace m2c

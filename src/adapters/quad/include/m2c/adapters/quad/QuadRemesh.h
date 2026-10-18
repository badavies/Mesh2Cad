// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Field-aligned quad remeshing with Instant Meshes (ADR-009). The header uses core types only;
// Instant Meshes and its Eigen types stay inside this adapter.

#include <cstddef>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::quad {

struct QuadRemeshParams {
    /// Approximate number of quads in the result.
    std::size_t targetFaces{2000};
    /// Align the field to edges sharper than this dihedral angle; 0 = no creases.
    double creaseAngleDeg{0.0};
    /// Smoothing and re-projection steps after extraction.
    unsigned smoothIterations{2};
    /// Manual patch layout: polylines on the mesh (at least two points each, within a cage edge
    /// length of the surface) that cage edges should follow. The field's orientation and
    /// position are held to each guide where it crosses the mesh.
    std::vector<std::vector<Vec3d>> guides;
};

/// Runs Instant Meshes single-threaded and deterministically (extrinsic 4-RoSy and 4-PoSy field,
/// pure-quad extraction). The result is the remesher's raw output: it can have open loops, loose
/// fragments and valence-2 vertices, so the pipeline cleans it into a closed cage
/// (pipeline/QuadLayout.h). Fails with InvalidArgument for an empty or degenerate mesh, or a
/// guide that is too short or off the mesh.
[[nodiscard]] Result<QuadLayout> remesh(const TriMesh& mesh, const QuadRemeshParams& params);

} // namespace m2c::quad

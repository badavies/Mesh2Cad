// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct SmoothParams {
    unsigned iterations{10};
    double strength{0.5}; ///< Taubin lambda, 0..1. mu is derived (no shrinkage).
    bool preserveSharpEdges{false};
    double sharpAngleDeg{40.0}; ///< Dihedral angle above which an edge counts as sharp.
};

/// Taubin lambda/mu smoothing (no shrinkage) on the whole mesh, or on the faces of `selection`.
/// Open-boundary vertices, vertices outside the selection and, if asked, vertices on sharp edges
/// stay fixed. Only positions change; the result shares the input's face buffer.
[[nodiscard]] Result<TriMesh> smoothTaubin(const TriMesh& mesh,
                                           const SmoothParams& params,
                                           const MeshSelection* selection = nullptr);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Quad layout stage (ARCHITECTURE.md section 6): a repaired scan in, a closed quad control cage
// out. The remesher comes from the quad adapter (ADR-009); this file turns its raw output into a
// cage the surface build accepts.

#include <cstddef>

#include "m2c/adapters/quad/QuadRemesh.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

using quad::QuadRemeshParams;

/// What closeCage() changed.
struct CageCleanup {
    std::size_t droppedFaces{0};      ///< Faces on edges shared by more than two faces.
    std::size_t droppedComponents{0}; ///< Loose parts apart from the main one, left out.
    std::size_t droppedComponentFaces{0};
    std::size_t joinedComponents{0}; ///< Closed parts joined to the main one with a tube.
    std::size_t joinedComponentFaces{0};
    std::size_t splitVertices{0};     ///< Copies made where faces met only at a vertex.
    std::size_t filledLoops{0};       ///< Open loops closed with one polygon each.
    std::size_t largestLoop{0};       ///< Vertices of the largest polygon added.
    std::size_t dissolvedVertices{0}; ///< Vertices shared by only two faces, removed.
    bool flipped{false};              ///< Faces were reversed to point outward.
};

struct CageCloseParams {
    /// Join loose parts to the main one instead of leaving them out. The remesher can miss the
    /// stem of a thin feature (a thumbstick post), leaving its head as a separate closed part.
    bool joinParts{true};
    /// Parts with fewer faces are left out as noise.
    std::size_t minPartFaces{16};
    /// Keep the main part's longest open loop open: the cage of a sheet (a scan of one side of
    /// a thin part) keeps its outline. Other loops are still filled. The main part is then
    /// left facing the way the remesher made it, which follows the mesh.
    bool keepOutline{false};
};

/// Makes a cage closed, manifold and outward: drops faces on non-manifold edges, orients faces
/// consistently, splits vertices where separate fans meet, closes each open loop with one
/// polygon, points faces outward and dissolves valence-2 vertices, all on the largest connected
/// part. Each other part that closes the same way is joined to it by a tube of quads from the
/// nearest pair of quads facing each other, no farther apart than twice the part's size; the rest
/// are left out. Fails with InvalidState when the main part cannot be oriented consistently or
/// nothing valid is left. With keepOutline the result is open along one loop (see above).
[[nodiscard]] Result<QuadLayout>
closeCage(const QuadLayout& raw, CageCleanup& report, const CageCloseParams& params = {});

struct QuadRemeshResult {
    QuadLayout cage;
    CageCleanup cleanup;
    CageStats stats;
    double maxDistanceMm{0.0};  ///< From a cage vertex to the scan.
    double meanDistanceMm{0.0}; ///< Mean over cage vertices.
};

/// Remeshes a scan into a quad cage and closes it (closeCage()). Reports each stage to `progress`
/// and stops with Cancelled at the next one once it is cancelled.
[[nodiscard]] Result<QuadRemeshResult> quadRemesh(const TriMesh& scan,
                                                  const QuadRemeshParams& params,
                                                  const CageCloseParams& close = {},
                                                  const Progress& progress = {});

} // namespace m2c

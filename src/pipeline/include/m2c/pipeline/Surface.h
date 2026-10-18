// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Surface build (ARCHITECTURE.md section 6): fit a closed quad cage to the scan, turn its
// Catmull-Clark limit surface into B-spline faces (OpenSubdiv adapter) and sew them into a solid
// (Open CASCADE adapter).

#include "m2c/adapters/occt/Brep.h"
#include "m2c/adapters/osd/Subdivision.h"
#include "m2c/core/Solid.h"
#include "m2c/pipeline/PatchMerge.h"

namespace m2c {

using osd::CageFitParams;
using osd::CageFitResult;
using osd::extractPatches;
using osd::fitCage;
using osd::PatchParams;
using osd::PatchResult;
using osd::tessellateLimit;

struct SurfaceBuildParams {
    bool fit{true};        ///< Fit the cage to the scan first; off = use the cage as it is.
    CageFitParams fitting; ///< Its isolation level is used for the faces too.
    /// A fit whose faces meet at a larger angle has folded the surface and is tried again with
    /// twice the smoothing, up to fitTries fits in all.
    double maxSeamAngleDeg{10.0};
    unsigned fitTries{4};
    double sewToleranceMm{1e-4};
    /// Merge rectangles of cage quads into one face each (patch merging; mergeFaces()).
    bool merge{true};
    int displayGrid{4}; ///< Display triangles: 2 n^2 per face.
};

struct SurfaceBuildResult {
    Solid solid;
    QuadLayout cage;       ///< The cage the faces come from (fitted unless fit was off).
    CageFitResult fit;     ///< Zero samples when fit was off.
    double smoothing{0.0}; ///< The smoothing of the fit that was kept.
    unsigned fits{0};      ///< Fits made (more than one when a fit folded the surface).
    std::size_t regularPieces{0};
    std::size_t capPieces{0};
    double maxCapErrorMm{0.0};
    SeamReport seams; ///< Gaps and normal angles where faces and pieces meet, before merging.
    std::size_t facesBeforeMerge{0}; ///< One per cage quad (or corner quad of another face).
    std::size_t mergedFaces{0};      ///< Faces made from more than one cage quad.
    std::size_t unmergedQuads{0};    ///< Quads left single because their region was no rectangle.
};

/// Fits, extracts and sews in one go. The cage must be closed; `scan` may be null when fit is off.
/// Fails unless the result is one valid closed solid. Deterministic for the same inputs.
[[nodiscard]] Result<SurfaceBuildResult>
buildSurface(const QuadLayout& cage, const TriMesh* scan, const SurfaceBuildParams& params);

} // namespace m2c

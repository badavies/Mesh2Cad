// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Catmull-Clark limit surface of a quad cage with OpenSubdiv (ADR-006, ADR-015): fitting the cage
// to a scan through limit stencils, and turning the limit surface into bicubic patches.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::osd {

/// Cage vertices held on a plane while fitting: where all of a patch's control points are held,
/// the limit surface there is that plane.
struct HeldPlane {
    Vec3d point;  ///< On the plane.
    Vec3d normal; ///< Unit.
    std::vector<std::uint32_t> vertices;
};

struct CageFitParams {
    unsigned iterations{3};        ///< Rounds of closest-point parameters and least squares.
    double smoothing{0.05};        ///< Weight of keeping the cage's own shape; 0 = none.
    std::size_t maxSamples{60000}; ///< Scan vertices used (evenly thinned beyond this).
    int isolationLevel{2};         ///< Adaptive refinement around irregular vertices (1-4).
    /// Samples farther than this from the surface are left out (parts of the scan the cage does
    /// not cover); 0 = the mean cage edge length.
    double maxSampleDistanceMm{0.0};
    /// Accept an open cage (a sheet with a boundary) as well as a closed one.
    bool open{false};
    /// Vertices kept on planes: a stiff term in each solve, then an exact projection. A vertex on
    /// two planes ends up on (or near, for planes far from perpendicular) their intersection.
    std::vector<HeldPlane> held{};
};

struct CageFitResult {
    QuadLayout cage;
    std::size_t samples{0};        ///< Used in the last round.
    std::size_t ignoredSamples{0}; ///< Left out in the last round, too far from the surface.
    double rmsDistanceMm{0.0};     ///< From the used samples to the fitted limit surface.
    double maxDistanceMm{0.0};
    double p95DistanceMm{0.0};            ///< 95th percentile of the used samples' distances.
    std::vector<double> rmsBeforeRoundMm; ///< RMS distance at the start of each round.
};

/// Moves the cage vertices so its limit surface passes close to the scan: each sample gets the
/// closest point of the current limit surface, then the vertices solve one sparse least-squares
/// problem (distance to the sample's tangent plane, plus a little of the distance to the sample),
/// with a smoothing term on how far neighbouring vertices move apart. The best round is kept.
/// Held vertices (params.held) start and stay on their planes. The cage must be closed, or open
/// with params.open. Deterministic.
[[nodiscard]] Result<CageFitResult>
fitCage(const QuadLayout& cage, const TriMesh& scan, const CageFitParams& params);

struct PatchParams {
    int isolationLevel{2}; ///< As in CageFitParams; caps get smaller with each level.
    bool open{false};      ///< As in CageFitParams.
    /// Of an open cage: boundary vertices where its edge turns sharply, as it does at vertices
    /// used by one face (to match a neighbouring surface that has one there).
    std::vector<std::uint32_t> corners{};
    /// Cage edges (vertex pairs) along which the surface has a sharp edge.
    std::vector<std::array<std::uint32_t, 2>> creases{};
};

struct PatchResult {
    std::vector<SurfacePatch> faces; ///< One per cage quad (n per n-sided cage face).
    /// Per face, which of its pieces (j * piecesU + i) are caps.
    std::vector<std::vector<bool>> caps;
    std::size_t regularPieces{0};
    std::size_t capPieces{0};
    double maxCapErrorMm{0.0}; ///< Largest distance from a cap to the limit surface it replaces.
};

/// The limit surface as one SurfacePatch per cage quad. OpenSubdiv's adaptive patches are
/// converted exactly where the surface is regular; each irregular patch (Gregory basis) becomes a
/// cap with the same boundary curves and a least-squares interior. Coarser patches are split so
/// each face is a uniform grid of pieces. Neighbouring faces can have different numbers of spans
/// but meet along the same limit curves. The cage must be closed, or open with params.open.
[[nodiscard]] Result<PatchResult> extractPatches(const QuadLayout& cage, const PatchParams& params);

/// One uniform Catmull-Clark step: every n-sided face becomes n quads. The limit surface is the
/// same and so are the irregular vertices, so a fit of the result starts where the coarse fit
/// ended, with four times the control. The cage must be a consistently oriented manifold
/// (closed, or open with a boundary).
[[nodiscard]] Result<QuadLayout> subdivideCage(const QuadLayout& cage);

/// Unit normal of the limit surface at the limit point of each cage vertex, on the side the
/// cage's faces face (zero where the surface is degenerate). The cage must be a consistently
/// oriented manifold, closed or open.
[[nodiscard]] Result<std::vector<Vec3d>> limitNormals(const QuadLayout& cage);

/// Points of the limit surface on a grid of n x n per patch (two triangles per grid cell), with
/// duplicated vertices along patch edges. For display and distance checks.
[[nodiscard]] Result<TriMesh> tessellateLimit(const QuadLayout& cage, int isolationLevel, int n);

} // namespace m2c::osd

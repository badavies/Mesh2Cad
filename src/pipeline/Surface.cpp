// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Surface.h"

#include <cmath>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"
#include "m2c/pipeline/PatchMerge.h"

namespace m2c {

namespace {

constexpr int kSeamSamples = 8;           // samples per face edge for the seam report
constexpr double kSeamToleranceMm = 1e-2; // edge samples closer than this are the same point

} // namespace

Result<SurfaceBuildResult>
buildSurface(const QuadLayout& cage, const TriMesh* scan, const SurfaceBuildParams& params) {
    if (params.fit && scan == nullptr) {
        return fail(ErrorCode::InvalidArgument, "fitting needs a scan");
    }
    if (!(params.maxSeamAngleDeg > 0.0) || !(params.maxFoldAngleDeg > 0.0) || params.fitTries < 1 ||
        !(params.refineToMm >= 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "the angle limits must be positive, the fit tried "
                    "at least once and the refinement target at least 0");
    }
    // A fit that folds the surface shows as a large angle between the normals where faces meet
    // (the limit surface is smooth, apart from the small kinks at caps). Such a fit is tried
    // again with twice the smoothing; the last try is kept if none passes.
    // A fit still farther from the scan than refineToMm (95th percentile) is refined: its cage
    // gets one Catmull-Clark step, which keeps the surface and its layout, and is fitted again.
    CageFitParams fitting = params.fitting;
    QuadLayout start = cage;
    unsigned tries = 0;
    std::vector<double> p95BeforeRefinement;
    for (;;) {
        CageFitResult fit{.cage = start,
                          .samples = 0,
                          .ignoredSamples = 0,
                          .rmsDistanceMm = 0.0,
                          .maxDistanceMm = 0.0,
                          .p95DistanceMm = 0.0,
                          .rmsBeforeRoundMm = {}};
        if (params.fit) {
            auto fitted = fitCage(start, *scan, fitting);
            if (!fitted) return std::unexpected(fitted.error());
            fit = std::move(*fitted);
        }
        ++tries;
        auto patches = extractPatches(fit.cage, PatchParams{fitting.isolationLevel});
        if (!patches) return std::unexpected(patches.error());
        auto seams = measureSeams(patches->faces, kSeamSamples, kSeamToleranceMm, patches->caps);
        if (!seams) return std::unexpected(seams.error());
        if (params.fit && seams->maxNormalAngleDeg > params.maxSeamAngleDeg &&
            tries < params.fitTries) {
            fitting.smoothing = fitting.smoothing > 0.0 ? 2.0 * fitting.smoothing : 0.01;
            continue;
        }
        if (seams->maxNormalAngleDeg > params.maxFoldAngleDeg) {
            return fail(ErrorCode::InvalidState,
                        "the surface folds over itself: faces meet at " +
                            std::to_string(std::lround(seams->maxNormalAngleDeg)) +
                            " degrees after " + std::to_string(tries) +
                            (tries == 1 ? " fit" : " fits") +
                            "; the cage has folded faces or is too coarse for thin parts");
        }
        if (params.fit && params.refineToMm > 0.0 && fit.p95DistanceMm > params.refineToMm &&
            p95BeforeRefinement.size() < params.maxRefinements) {
            auto finer = subdivideCage(fit.cage);
            if (!finer) return std::unexpected(finer.error());
            p95BeforeRefinement.push_back(fit.p95DistanceMm);
            start = std::move(*finer);
            tries = 0;
            continue;
        }
        const std::size_t before = patches->faces.size();
        std::size_t mergedFaces = 0, unmergedQuads = 0;
        if (params.merge) {
            auto merged = mergeFaces(fit.cage, patches->faces);
            if (!merged) return std::unexpected(merged.error());
            patches->faces = std::move(merged->faces);
            mergedFaces = merged->mergedFaces;
            unmergedQuads = merged->partition.unmergedQuads;
        }
        auto display = tessellate(patches->faces, params.displayGrid);
        if (!display) return std::unexpected(display.error());
        auto solid = occt::makeSolid(std::move(patches->faces),
                                     std::make_shared<const FaceTessellation>(std::move(*display)),
                                     occt::SewParams{params.sewToleranceMm});
        if (!solid) return std::unexpected(solid.error());
        QuadLayout used = fit.cage;
        return SurfaceBuildResult{.solid = std::move(*solid),
                                  .cage = std::move(used),
                                  .fit = std::move(fit),
                                  .smoothing = fitting.smoothing,
                                  .fits = tries,
                                  .regularPieces = patches->regularPieces,
                                  .capPieces = patches->capPieces,
                                  .maxCapErrorMm = patches->maxCapErrorMm,
                                  .seams = *seams,
                                  .facesBeforeMerge = before,
                                  .mergedFaces = mergedFaces,
                                  .unmergedQuads = unmergedQuads,
                                  .p95BeforeRefinementMm = std::move(p95BeforeRefinement)};
    }
}

} // namespace m2c

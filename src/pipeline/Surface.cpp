// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Surface.h"

#include <utility>

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
    if (!(params.maxSeamAngleDeg > 0.0) || params.fitTries < 1) {
        return fail(ErrorCode::InvalidArgument,
                    "the seam angle limit must be positive and the fit tried at least once");
    }
    // A fit that folds the surface shows as a large angle between the normals where faces meet
    // (the limit surface is smooth, apart from the small kinks at caps). Such a fit is tried
    // again with twice the smoothing; the last try is kept if none passes.
    CageFitParams fitting = params.fitting;
    unsigned tries = 0;
    for (;;) {
        CageFitResult fit{cage, 0, 0, 0.0, 0.0, {}};
        if (params.fit) {
            auto fitted = fitCage(cage, *scan, fitting);
            if (!fitted) return std::unexpected(fitted.error());
            fit = std::move(*fitted);
        }
        ++tries;
        auto patches = extractPatches(fit.cage, PatchParams{fitting.isolationLevel});
        if (!patches) return std::unexpected(patches.error());
        auto seams = measureSeams(patches->faces, kSeamSamples, kSeamToleranceMm);
        if (!seams) return std::unexpected(seams.error());
        if (params.fit && seams->maxNormalAngleDeg > params.maxSeamAngleDeg &&
            tries < params.fitTries) {
            fitting.smoothing = fitting.smoothing > 0.0 ? 2.0 * fitting.smoothing : 0.01;
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
        return SurfaceBuildResult{std::move(*solid),
                                  std::move(used),
                                  std::move(fit),
                                  fitting.smoothing,
                                  tries,
                                  patches->regularPieces,
                                  patches->capPieces,
                                  patches->maxCapErrorMm,
                                  *seams,
                                  before,
                                  mergedFaces,
                                  unmergedQuads};
    }
}

} // namespace m2c

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
    CageFitResult fit{cage, 0, 0, 0.0, 0.0, {}};
    if (params.fit) {
        if (scan == nullptr) return fail(ErrorCode::InvalidArgument, "fitting needs a scan");
        auto fitted = fitCage(cage, *scan, params.fitting);
        if (!fitted) return std::unexpected(fitted.error());
        fit = std::move(*fitted);
    }
    auto patches = extractPatches(fit.cage, PatchParams{params.fitting.isolationLevel});
    if (!patches) return std::unexpected(patches.error());
    auto seams = measureSeams(patches->faces, kSeamSamples, kSeamToleranceMm);
    if (!seams) return std::unexpected(seams.error());
    auto display = tessellate(patches->faces, params.displayGrid);
    if (!display) return std::unexpected(display.error());
    auto solid = occt::makeSolid(std::move(patches->faces),
                                 std::make_shared<const TriMesh>(std::move(*display)),
                                 occt::SewParams{params.sewToleranceMm});
    if (!solid) return std::unexpected(solid.error());
    QuadLayout used = fit.cage;
    return SurfaceBuildResult{std::move(*solid),
                              std::move(used),
                              std::move(fit),
                              patches->regularPieces,
                              patches->capPieces,
                              patches->maxCapErrorMm,
                              *seams};
}

} // namespace m2c

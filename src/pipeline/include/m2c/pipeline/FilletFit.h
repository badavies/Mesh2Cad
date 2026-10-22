// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fitted fillets (ADR-033): a solid's edges rounded at the radius a scanned fillet has.

#include <cstddef>
#include <optional>
#include <vector>

#include "m2c/adapters/occt/Fillet.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"

namespace m2c {

using occt::filletEdges;

struct FilletFitParams {
    std::optional<double> radiusMm; ///< Use this radius instead of fitting one.
    double maxResidualMm{0.1};      ///< The fit fails when its P95 distance is above this.
};

struct FilletFitResult {
    Solid solid;
    double radiusMm{0.0};
    double rmsMm{0.0}; ///< From the band's points to the rounded solid.
    double maxMm{0.0};
    double p95Mm{0.0};
    std::size_t builds{0}; ///< Fillets the kernel built while fitting.
};

/// Rounds the edges of `solid` nearest edgePoints (filletEdges) at the radius that brings the
/// rounded solid closest to `band`, the scan's points on the fillet: radii growing by half each
/// time from 0.1 mm bracket the best, then a golden-section search narrows it to 0.1%. Radii the
/// kernel cannot build count as no fit. With params.radiusMm, rounds at that radius and only
/// measures. Fails as filletEdges does, with InvalidArgument for no band points, and with
/// InvalidState when no radius builds or the P95 distance is above params.maxResidualMm.
/// Deterministic. Stops with Cancelled before the next radius once `progress` is cancelled.
[[nodiscard]] Result<FilletFitResult> fitFillet(const Solid& solid,
                                                const std::vector<Vec3d>& edgePoints,
                                                const std::vector<Vec3d>& band,
                                                const FilletFitParams& params,
                                                const Progress& progress = {});

} // namespace m2c

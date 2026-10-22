// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/FilletFit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/adapters/occt/Fillet.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr double kFirstRadiusMm = 0.1; // the bracket's first radius
constexpr double kGrowth = 1.5;        // each next radius, until the fit gets worse twice
constexpr int kMostRadii = 30;         // 0.1 mm * 1.5^30 is about 19 m
constexpr double kRelativeTolerance = 1e-3;
constexpr double kSame = 1e-6; // fits closer than this share are equal

struct Trial {
    std::optional<Solid> solid; ///< Empty when the kernel cannot build this radius.
    std::vector<double> distances;
    double rms{std::numeric_limits<double>::infinity()};
};

} // namespace

Result<FilletFitResult> fitFillet(const Solid& solid,
                                  const std::vector<Vec3d>& edgePoints,
                                  const std::vector<Vec3d>& band,
                                  const FilletFitParams& params,
                                  const Progress& progress) {
    if (band.empty()) return fail(ErrorCode::InvalidArgument, "the fillet's band has no points");
    if (!(params.maxResidualMm > 0.0) || (params.radiusMm && !(*params.radiusMm > 0.0))) {
        return fail(ErrorCode::InvalidArgument, "the radius and residual limit must be positive");
    }
    std::map<double, Trial> trials;
    // Builds and measures one radius; an error that no radius would avoid (no edge, a wrong
    // edge) is returned, any other leaves the trial without a solid.
    const auto trial = [&](const double r) -> Result<const Trial*> {
        if (const auto it = trials.find(r); it != trials.end()) return &it->second;
        if (auto go = checkpoint(progress,
                                 "rounding the edges at " + std::to_string(r) + " mm (try " +
                                     std::to_string(trials.size() + 1) + ")");
            !go)
            return std::unexpected(go.error());
        Trial t;
        auto rounded = filletEdges(solid, edgePoints, r);
        if (!rounded && rounded.error().code == ErrorCode::InvalidArgument) {
            return std::unexpected(rounded.error());
        }
        if (rounded) {
            auto distances = occt::signedDistances(*rounded, band, progress);
            if (!distances) return std::unexpected(distances.error());
            double sum2 = 0.0;
            for (double& d : *distances) {
                d = std::abs(d);
                sum2 += d * d;
            }
            t.rms = std::sqrt(sum2 / static_cast<double>(band.size()));
            t.distances = std::move(*distances);
            t.solid = std::move(*rounded);
        }
        return &trials.emplace(r, std::move(t)).first->second;
    };

    double best = 0.0;
    if (params.radiusMm) {
        best = *params.radiusMm;
    } else {
        // Bracket: radii growing until the fit is worse twice running or the kernel gives up.
        std::vector<double> radii;
        std::size_t bestIndex = 0;
        double bestRms = std::numeric_limits<double>::infinity();
        int worse = 0;
        double r = kFirstRadiusMm;
        for (int k = 0; k < kMostRadii && worse < 2; ++k, r *= kGrowth) {
            auto t = trial(r);
            if (!t) return std::unexpected(t.error());
            radii.push_back(r);
            if (!(*t)->solid && !std::isinf(bestRms)) break; // too large from here on
            // Small radii change nothing the band sees, so equal fits neither help nor count
            // as worse.
            if ((*t)->rms < bestRms * (1.0 - kSame)) {
                bestRms = (*t)->rms;
                bestIndex = radii.size() - 1;
                worse = 0;
            } else if ((*t)->rms > bestRms * (1.0 + kSame)) {
                ++worse;
            }
        }
        if (std::isinf(bestRms)) {
            return fail(ErrorCode::InvalidState,
                        "Open CASCADE cannot round the edges at any radius");
        }
        // Golden-section search between the best radius's neighbours.
        double lo = bestIndex > 0 ? radii[bestIndex - 1] : radii[0] / kGrowth;
        double hi =
            bestIndex + 1 < radii.size() ? radii[bestIndex + 1] : radii[bestIndex] * kGrowth;
        const double g = (std::sqrt(5.0) - 1.0) / 2.0;
        double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
        auto ta = trial(a);
        if (!ta) return std::unexpected(ta.error());
        auto tb = trial(b);
        if (!tb) return std::unexpected(tb.error());
        double fa = (*ta)->rms, fb = (*tb)->rms;
        while (hi - lo > kRelativeTolerance * (hi + lo) / 2.0) {
            if (fa <= fb) {
                hi = b;
                b = a;
                fb = fa;
                a = hi - g * (hi - lo);
                auto next = trial(a);
                if (!next) return std::unexpected(next.error());
                fa = (*next)->rms;
            } else {
                lo = a;
                a = b;
                fa = fb;
                b = lo + g * (hi - lo);
                auto next = trial(b);
                if (!next) return std::unexpected(next.error());
                fb = (*next)->rms;
            }
        }
        best = radii[bestIndex];
        for (const auto& [radius, t] : trials) {
            if (t.rms < trials.at(best).rms) best = radius;
        }
    }
    auto chosen = trial(best);
    if (!chosen) return std::unexpected(chosen.error());
    if (!(*chosen)->solid) {
        return fail(ErrorCode::InvalidState,
                    "Open CASCADE cannot round the edges at " + std::to_string(best) +
                        " mm (too large for the faces either side?)");
    }
    FilletFitResult out{.solid = *(*chosen)->solid,
                        .radiusMm = best,
                        .rmsMm = (*chosen)->rms,
                        .builds = trials.size()};
    std::vector<double> d = (*chosen)->distances;
    out.maxMm = std::ranges::max(d);
    const auto at = d.begin() + static_cast<std::ptrdiff_t>(
                                    std::ceil(0.95 * static_cast<double>(d.size())) - 1.0);
    std::ranges::nth_element(d, at);
    out.p95Mm = *at;
    if (!(out.p95Mm <= params.maxResidualMm)) {
        return fail(ErrorCode::InvalidState,
                    "the fillet of radius " + std::to_string(best) +
                        " mm fits with a P95 residual of " + std::to_string(out.p95Mm) +
                        " mm, above the limit of " + std::to_string(params.maxResidualMm) + " mm");
    }
    return out;
}

} // namespace m2c

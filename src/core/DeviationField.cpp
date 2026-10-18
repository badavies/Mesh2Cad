// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/DeviationField.h"

#include <algorithm>
#include <cmath>

namespace m2c {

Result<DeviationField> DeviationField::create(std::vector<double> distancesMm,
                                              const double toleranceMm) {
    if (distancesMm.empty()) return fail(ErrorCode::InvalidArgument, "no distances");
    if (!(toleranceMm > 0.0) || !std::isfinite(toleranceMm)) {
        return fail(ErrorCode::InvalidArgument, "the tolerance must be positive");
    }
    DeviationStats s;
    s.count = distancesMm.size();
    s.toleranceMm = toleranceMm;
    s.minMm = distancesMm.front();
    s.maxMm = distancesMm.front();
    double sum = 0.0, sum2 = 0.0;
    std::size_t within = 0;
    std::vector<double> magnitudes;
    magnitudes.reserve(distancesMm.size());
    for (const double d : distancesMm) {
        if (!std::isfinite(d)) return fail(ErrorCode::InvalidArgument, "a distance is not finite");
        s.minMm = std::min(s.minMm, d);
        s.maxMm = std::max(s.maxMm, d);
        sum += d;
        sum2 += d * d;
        if (std::abs(d) <= toleranceMm) ++within;
        magnitudes.push_back(std::abs(d));
    }
    const auto n = static_cast<double>(s.count);
    s.meanMm = sum / n;
    s.rmsMm = std::sqrt(sum2 / n);
    s.withinTolerance = static_cast<double>(within) / n;
    // Nearest-rank percentile: the smallest value with at least 95% of the values at or below it.
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * n)) - 1;
    std::ranges::nth_element(magnitudes, magnitudes.begin() + static_cast<std::ptrdiff_t>(rank));
    s.p95AbsMm = magnitudes[rank];
    return DeviationField(std::make_shared<const std::vector<double>>(std::move(distancesMm)), s);
}

Result<DeviationHistogram> histogram(const DeviationField& field, const std::size_t bins) {
    if (bins < 1 || bins > 1000) return fail(ErrorCode::InvalidArgument, "bins must be 1 to 1000");
    const DeviationStats& s = field.stats();
    DeviationHistogram h;
    h.lowMm = s.minMm;
    h.binWidthMm = (s.maxMm - s.minMm) / static_cast<double>(bins);
    h.counts.assign(bins, 0);
    for (const double d : field.distances()) {
        std::size_t bin = 0;
        if (h.binWidthMm > 0.0) {
            bin = std::min(static_cast<std::size_t>((d - h.lowMm) / h.binWidthMm), bins - 1);
        }
        ++h.counts[bin];
    }
    return h;
}

} // namespace m2c

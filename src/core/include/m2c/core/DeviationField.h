// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"

namespace m2c {

/// Statistics of a deviation field. Distances are signed, positive outside the reference.
struct DeviationStats {
    std::size_t count{0};
    double minMm{0.0};
    double maxMm{0.0};
    double meanMm{0.0};
    double rmsMm{0.0};
    double p95AbsMm{0.0}; ///< 95th percentile of the absolute distance.
    double toleranceMm{0.0};
    double withinTolerance{0.0}; ///< Share of distances with |d| <= toleranceMm, 0 to 1.
};

/// Signed distance from every vertex of a mesh to a reference surface (ARCHITECTURE.md section 7,
/// Deviation analysis), with its statistics. Immutable and cheap to copy.
class DeviationField {
public:
    /// Fails unless there is at least one distance, every distance is finite and the tolerance
    /// is positive.
    [[nodiscard]] static Result<DeviationField> create(std::vector<double> distancesMm,
                                                       double toleranceMm);

    [[nodiscard]] const std::vector<double>& distances() const noexcept { return *distances_; }
    [[nodiscard]] const DeviationStats& stats() const noexcept { return stats_; }

private:
    DeviationField(std::shared_ptr<const std::vector<double>> distances, DeviationStats stats)
        : distances_(std::move(distances)), stats_(stats) {}

    std::shared_ptr<const std::vector<double>> distances_;
    DeviationStats stats_;
};

/// Counts of distances in `bins` equal bins from the smallest to the largest distance.
struct DeviationHistogram {
    double lowMm{0.0};
    double binWidthMm{0.0};
    std::vector<std::size_t> counts;
};

[[nodiscard]] Result<DeviationHistogram> histogram(const DeviationField& field, std::size_t bins);

} // namespace m2c

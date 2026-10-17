// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Deviation report (ARCHITECTURE.md section 7, Report export).

#include <cstddef>
#include <filesystem>

#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"

namespace m2c {

/// Writes a CSV file: a `statistic,value` table, a blank line, then the histogram as
/// `bin_low_mm,bin_high_mm,count` rows. Numbers use '.' whatever the locale.
[[nodiscard]] Result<void> writeDeviationReport(const DeviationField& field,
                                                std::size_t bins,
                                                const std::filesystem::path& path);

} // namespace m2c

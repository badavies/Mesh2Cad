// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Report.h"

#include <array>
#include <charconv>
#include <fstream>
#include <string>

namespace m2c {

namespace {

std::string number(double v) {
    std::array<char, 32> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
    return {buffer.data(), end};
}

} // namespace

Result<void> writeDeviationReport(const DeviationField& field,
                                  std::size_t bins,
                                  const std::filesystem::path& path) {
    auto h = histogram(field, bins);
    if (!h) return std::unexpected(h.error());
    const DeviationStats& s = field.stats();
    std::string text = "statistic,value\n";
    text += "count," + std::to_string(s.count) + '\n';
    text += "min_mm," + number(s.minMm) + '\n';
    text += "max_mm," + number(s.maxMm) + '\n';
    text += "mean_mm," + number(s.meanMm) + '\n';
    text += "rms_mm," + number(s.rmsMm) + '\n';
    text += "p95_abs_mm," + number(s.p95AbsMm) + '\n';
    text += "tolerance_mm," + number(s.toleranceMm) + '\n';
    text += "within_tolerance," + number(s.withinTolerance) + '\n';
    text += "\nbin_low_mm,bin_high_mm,count\n";
    for (std::size_t b = 0; b < h->counts.size(); ++b) {
        const double low = h->lowMm + h->binWidthMm * static_cast<double>(b);
        const double high = b + 1 == h->counts.size() ? s.maxMm : low + h->binWidthMm;
        text += number(low) + ',' + number(high) + ',' + std::to_string(h->counts[b]) + '\n';
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for writing");
    file << text;
    file.flush();
    if (!file) return fail(ErrorCode::Io, "failed while writing the deviation report");
    return {};
}

} // namespace m2c

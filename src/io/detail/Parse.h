// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private helpers shared by the text and binary mesh readers. Not part of the io public API.

#include <charconv>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c::io_detail {

/// Reads a stream line by line in large chunks and hands out views into its buffer. A view is
/// valid until the next call. Handles "\n" and "\r\n" endings and a last line without one.
class LineReader {
public:
    explicit LineReader(std::istream& in, std::size_t chunkSize = std::size_t{1} << 20)
        : in_(in), chunk_(chunkSize) {}

    /// Next line without its ending, or nullopt at end of input.
    std::optional<std::string_view> next();

    /// 1-based number of the line last returned.
    [[nodiscard]] std::size_t lineNumber() const noexcept { return line_; }

private:
    std::istream& in_;
    std::size_t chunk_;
    std::string buffer_;
    std::size_t pos_{0};
    std::size_t line_{0};
    bool eof_{false};
};

/// Splits off the next whitespace-separated token; returns an empty view when none is left.
[[nodiscard]] inline std::string_view nextToken(std::string_view& s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
    std::size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r') ++j;
    const std::string_view token = s.substr(i, j - i);
    s.remove_prefix(j);
    return token;
}

[[nodiscard]] inline std::optional<double> parseDouble(std::string_view s) noexcept {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    double value = 0.0;
    const char* const end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

[[nodiscard]] inline std::optional<std::int64_t> parseInt(std::string_view s) noexcept {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    std::int64_t value = 0;
    const char* const end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

/// Normalises an indexed mesh read from a file (ARCHITECTURE.md section 6, import gate): vertices
/// with bit-identical coordinates are welded, vertices no face uses are dropped, and the result is
/// validated by TriMesh::create. Kept vertices stay in file order, so a clean file round-trips
/// with unchanged numbering.
[[nodiscard]] Result<TriMesh> weldAndCompact(const std::vector<Vec3d>& positions,
                                             const std::vector<std::uint32_t>& indices,
                                             std::string_view format);

/// Appends polygon `poly` as a triangle fan. Polygons with fewer than three corners are ignored.
inline void appendFan(const std::vector<std::uint32_t>& poly, std::vector<std::uint32_t>& out) {
    for (std::size_t k = 2; k < poly.size(); ++k) {
        out.push_back(poly[0]);
        out.push_back(poly[k - 1]);
        out.push_back(poly[k]);
    }
}

/// Reserve hint that cannot be abused by a lying header to allocate gigabytes up front.
[[nodiscard]] inline std::size_t safeReserve(std::uint64_t count) noexcept {
    constexpr std::uint64_t kCap = std::uint64_t{1} << 24;
    return static_cast<std::size_t>(count < kCap ? count : kCap);
}

} // namespace m2c::io_detail

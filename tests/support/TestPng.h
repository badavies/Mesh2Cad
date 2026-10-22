// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Reads back the PNG files view.render writes (8-bit RGB, one IDAT chunk, filter 0 on every row,
// as encodePng makes them), so tests can look at their pixels.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <optional>
#include <string>
#include <vector>

#include "m2c/core/Raster.h"

namespace m2c::test {

inline std::optional<Image> decodePng(const std::vector<std::uint8_t>& png) {
    const auto be32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = v << 8 | png[at + k];
        return v;
    };
    if (png.size() < 45 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return {};
    Image image{
        .width = static_cast<int>(be32(16)), .height = static_cast<int>(be32(20)), .rgb = {}};
    if (std::memcmp(png.data() + 37, "IDAT", 4) != 0) return {};
    std::size_t size = 0;
    void* raw = tinfl_decompress_mem_to_heap(
        png.data() + 41, be32(33), &size, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (raw == nullptr) return {};
    const auto row = 1 + 3 * static_cast<std::size_t>(image.width);
    const auto* rows = static_cast<const std::uint8_t*>(raw);
    if (size == row * static_cast<std::size_t>(image.height)) {
        for (std::size_t y = 0; y < static_cast<std::size_t>(image.height); ++y)
            image.rgb.insert(image.rgb.end(), rows + y * row + 1, rows + (y + 1) * row);
    }
    mz_free(raw);
    if (image.rgb.empty()) return {};
    return image;
}

inline std::optional<Image> readPng(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    return decodePng(bytes);
}

} // namespace m2c::test

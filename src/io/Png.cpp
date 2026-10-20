// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Png.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <string>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Raster.h"

namespace m2c {

Result<std::vector<std::uint8_t>> encodePng(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.rgb.size() !=
            3 * static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height)) {
        return fail(ErrorCode::InvalidArgument, "the image is empty or the wrong size");
    }
    std::size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(
        image.rgb.data(), image.width, image.height, 3, &size, MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (png == nullptr) return fail(ErrorCode::Io, "could not encode the PNG");
    const auto* bytes = static_cast<const std::uint8_t*>(png);
    std::vector<std::uint8_t> out(bytes, bytes + size);
    mz_free(png);
    return out;
}

Result<void> writePng(const Image& image, const std::filesystem::path& path) {
    const auto png = encodePng(image);
    if (!png) return std::unexpected(png.error());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for writing");
    file.write(reinterpret_cast<const char*>(png->data()),
               static_cast<std::streamsize>(png->size()));
    if (!file) return fail(ErrorCode::Io, "failed while writing '" + path.string() + "'");
    return {};
}

} // namespace m2c

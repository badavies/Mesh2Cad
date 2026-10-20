// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// PNG pictures of the document (view.render).

#include <cstdint>
#include <filesystem>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Raster.h"

namespace m2c {

/// The image as PNG bytes (8-bit RGB). Fails for an empty image.
[[nodiscard]] Result<std::vector<std::uint8_t>> encodePng(const Image& image);

/// Writes the image as a PNG file.
[[nodiscard]] Result<void> writePng(const Image& image, const std::filesystem::path& path);

} // namespace m2c

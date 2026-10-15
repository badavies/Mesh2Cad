// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Reads a mesh, choosing the format from the file extension (case-insensitive).
/// Supported: .stl, .obj, .ply.
[[nodiscard]] Result<TriMesh> readMesh(const std::filesystem::path& path);

/// Writes a mesh, choosing the format from the file extension. Supported: .stl (binary), .obj,
/// .ply (binary, double precision).
[[nodiscard]] Result<void> writeMesh(const TriMesh& mesh, const std::filesystem::path& path);

} // namespace m2c

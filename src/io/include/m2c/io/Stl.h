// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iosfwd>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Reads a binary or ASCII STL from a seekable stream (open files in binary mode). Binary is
/// recognised by the exact size rule 84 + 50 * triangleCount. STL stores triangle soup, so
/// vertices with identical coordinates are welded into one; no other repair is done.
[[nodiscard]] Result<TriMesh> readStl(std::istream& in);

/// Writes a binary STL. Coordinates are narrowed to float, as the format requires.
[[nodiscard]] Result<void> writeStl(const TriMesh& mesh, std::ostream& out);

} // namespace m2c

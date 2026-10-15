// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iosfwd>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Reads an ASCII or binary (little- or big-endian) PLY. Uses the x, y, z properties of the
/// `vertex` element and the `vertex_indices` (or `vertex_index`) list of the `face` element;
/// every other element and property is skipped. Polygons are split into triangle fans.
/// Duplicate positions are welded and unused vertices dropped. Open files in binary mode.
[[nodiscard]] Result<TriMesh> readPly(std::istream& in);

/// Writes a binary little-endian PLY with double-precision coordinates, so nothing is lost. This
/// is also the mesh format inside the project file.
[[nodiscard]] Result<void> writePly(const TriMesh& mesh, std::ostream& out);

} // namespace m2c

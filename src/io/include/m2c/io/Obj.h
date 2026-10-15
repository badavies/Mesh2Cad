// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iosfwd>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Reads the geometry of a Wavefront OBJ: `v` positions and `f` faces (all index forms, including
/// negative relative indices). Polygons are split into triangle fans; texture coordinates,
/// normals, groups and materials are ignored. Duplicate positions are welded and unused vertices
/// dropped.
[[nodiscard]] Result<TriMesh> readObj(std::istream& in);

/// Writes `v` and `f` lines with shortest round-trip number formatting, so a mesh read back is
/// bit-identical.
[[nodiscard]] Result<void> writeObj(const TriMesh& mesh, std::ostream& out);

} // namespace m2c

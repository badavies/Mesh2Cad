// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Deviation analysis (ARCHITECTURE.md section 7): signed distance from every vertex of a mesh to
// a solid or to another mesh, positive outside the reference.

#include "m2c/core/DeviationField.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// To a solid's faces, exactly (the Open CASCADE adapter projects onto each face's surface).
[[nodiscard]] Result<DeviationField>
deviationToSolid(const TriMesh& mesh, const Solid& reference, double toleranceMm);

/// To another mesh's triangles, through its spatial index; outside is where its normals point.
[[nodiscard]] Result<DeviationField>
deviationToMesh(const TriMesh& mesh, const TriMesh& reference, double toleranceMm);

} // namespace m2c

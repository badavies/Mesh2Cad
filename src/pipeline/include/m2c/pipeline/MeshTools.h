// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Mesh tidy-up tools. Repair, hole filling, simplification, remeshing and shrink-wrap come from
// the CGAL adapter (its header uses core types only); the rest is here.

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

using cgal::fillHoles;
using cgal::HoleFillParams;
using cgal::HoleFillResult;
using cgal::RemeshParams;
using cgal::repair;
using cgal::RepairParams;
using cgal::RepairResult;
using cgal::shrinkWrap;
using cgal::SimplifyParams;
using cgal::WrapParams;

/// Simplification to a face count (CGAL Garland-Heckbert edge collapse).
[[nodiscard]] inline Result<TriMesh> simplifyMesh(const TriMesh& mesh,
                                                  const SimplifyParams& params) {
    return cgal::simplify(mesh, params);
}

/// Isotropic remeshing (CGAL).
[[nodiscard]] inline Result<TriMesh> remeshMesh(const TriMesh& mesh, const RemeshParams& params) {
    return cgal::remesh(mesh, params);
}

/// Removes the selected faces and any vertex left unused.
[[nodiscard]] Result<TriMesh> deleteFaces(const TriMesh& mesh, const MeshSelection& faces);

/// Keeps only the selected faces.
[[nodiscard]] Result<TriMesh> keepFaces(const TriMesh& mesh, const MeshSelection& faces);

/// Reverses the orientation of every face.
[[nodiscard]] Result<TriMesh> flipFaces(const TriMesh& mesh);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Deviation analysis (ARCHITECTURE.md section 7): signed distance from every vertex of a mesh to
// a solid or to another mesh, positive outside the reference.

#include <cstdint>
#include <vector>

#include "m2c/core/DeviationField.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

/// To a solid's faces, exactly (the Open CASCADE adapter projects onto each face's surface).
/// Both stop with Cancelled within a few thousand vertices of `progress` being cancelled.
[[nodiscard]] Result<DeviationField> deviationToSolid(const TriMesh& mesh,
                                                      const Solid& reference,
                                                      double toleranceMm,
                                                      const Progress& progress = {});

/// To another mesh's triangles, through its spatial index; outside is where its normals point.
[[nodiscard]] Result<DeviationField> deviationToMesh(const TriMesh& mesh,
                                                     const TriMesh& reference,
                                                     double toleranceMm,
                                                     const Progress& progress = {});

/// A connected patch of a mesh out of tolerance on one side (ADR-035): where a scan has a hole,
/// boss or pocket the reference lacks, or misses one it has.
struct DeviationHotspot {
    int sign{1}; ///< +1 outside the reference (material to add), -1 inside (to remove).
    std::vector<std::uint32_t> faces; ///< In increasing order.
    double areaMm2{0.0};
    double maxSignedMm{0.0};   ///< The distance of largest magnitude, with its sign.
    double rmsMm{0.0};         ///< Over the vertices of its faces.
    std::uint32_t seedFace{0}; ///< The face with the vertex of largest |d| (lowest index on a tie).
    Vec3d centroid;            ///< Area-weighted centre of its faces.
    Aabb bounds;
};

/// The out-of-tolerance clusters of a deviation field on its mesh: faces whose vertex of largest
/// |d| exceeds the field's tolerance, grouped by the sign of that vertex and connected through
/// shared edges. Largest area first, ties by seed face. Deterministic and serial. Precondition:
/// the field has one distance per vertex of `mesh`.
[[nodiscard]] std::vector<DeviationHotspot> deviationHotspots(const TriMesh& mesh,
                                                              const DeviationField& field);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Intersect and trim (ARCHITECTURE.md section 7): fitted primitives split against each other and
// closed into a solid with sharp edges. Only core types cross this header.

#include <cstddef>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"

namespace m2c::occt {

struct TrimParams {
    /// A sample of a split face lies on the scan when it is at most this far from it.
    double maxDistanceMm{0.5};
    /// A split face is kept when at least this share of its area lies on the scan.
    double minCoverage{0.9};
    double sewToleranceMm{1e-3}; ///< Edges closer than this are joined.
};

struct TrimResult {
    Solid solid;
    std::size_t splitFaces{0}; ///< Faces after the primitives were split against each other.
    std::size_t keptFaces{0};  ///< Split faces on the scan, before faces of one surface merge.
};

/// Bounds every primitive by the scan's box, grown by a tenth of its diagonal (at least ten times
/// maxDistanceMm), and splits them all against each other with Open CASCADE's General Fuse. A
/// split face is kept when minCoverage of its area, sampled on a grid, lies within maxDistanceMm
/// of the scan. The kept faces are sewn and made an outward solid, and neighbouring faces of one
/// surface are merged, so a face crossed by another primitive's extension is one face again. The
/// solid's faces are the primitives' own planes, cylinders, spheres and cones; it stores no
/// B-spline faces (faces() is empty) and its display mesh is the kernel's triangulation.
/// Fails with InvalidState naming where the kept faces leave the solid open, or unless the result
/// is one valid closed solid of positive volume; InvalidArgument for no primitives or a primitive
/// with no surface. A cone keeps the half on the side of its apex where its extent is.
/// Deterministic.
[[nodiscard]] Result<TrimResult> trimPrimitives(const std::vector<FittedPrimitive>& primitives,
                                                const TriMesh& scan,
                                                const TrimParams& params);

} // namespace m2c::occt

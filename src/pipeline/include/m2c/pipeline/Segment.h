// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Segmentation of a mesh into planes, cylinders, spheres and organic regions (ARCHITECTURE.md
// section 6, "Segment").

#include <cstdint>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct SegmentParams {
    double maxDistanceMm{0.1}; ///< Largest distance from a vertex to its region's primitive.
    double maxAngleDeg{10.0};  ///< Largest angle between a face normal and the primitive's.
    /// Smaller regions are left organic; 0 = 1% of the mesh's area.
    double minAreaMm2{0.0};
    bool planes{true};
    bool cylinders{true};
    bool spheres{false};
};

struct SegmentRegion {
    /// Least-squares fit to the region's vertices. A plane's point is the region's centroid
    /// projected onto it and its normal faces the way the faces do; a cylinder's point is level
    /// with the centroid.
    Primitive primitive;
    std::vector<std::uint32_t> faces{}; ///< In increasing order.
    double areaMm2{0.0};
    double rmsMm{0.0}; ///< Distance of the region's vertices to the primitive.
    double maxMm{0.0};
};

struct SegmentResult {
    std::vector<SegmentRegion> regions;      ///< Planes first, then cylinders, then spheres.
    std::vector<std::uint32_t> organicFaces; ///< Faces in no region, in increasing order.
};

/// Labels every face. Spheres grow first, then cylinders, then planes over the faces left (CGAL
/// region growing, best local fits first). Each region is refitted by least squares and grown
/// on over the free faces its primitive explains, then refitted again. It is kept only when its
/// area reaches the limit, its RMS distance is within maxDistanceMm, 95% of its faces lie within
/// the limits of the fitted primitive, and a cylinder or sphere turns through at least three
/// times maxAngleDeg (a strip of a plane also fits a large cylinder). The rest is organic.
/// Deterministic. The mesh must be a manifold triangle mesh (as mesh.repair leaves it).
[[nodiscard]] Result<SegmentResult> segmentMesh(const TriMesh& mesh, const SegmentParams& params);

} // namespace m2c

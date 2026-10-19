// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Region growing of planes, cylinders and spheres over a mesh's faces with CGAL Shape_detection,
// for segmentation (ARCHITECTURE.md section 6, "Segment").

#include <cstdint>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"

namespace m2c::cgal {

struct GrowParams {
    PrimitiveKind kind{PrimitiveKind::Plane};
    double maxDistanceMm{0.1}; ///< From a face (a cylinder's or sphere's: its centroid).
    double maxAngleDeg{10.0};  ///< Between a face normal and the primitive's.
    double maxRadiusMm{0.0};   ///< Largest cylinder or sphere; 0 = no limit.
};

struct GrownRegion {
    /// CGAL's estimate: fitted to the faces (planes) or to their centroids and normals.
    Primitive primitive;
    std::vector<std::uint32_t> faces{};
};

/// Grows regions of one kind over the faces marked in `free`, from the best local fits first,
/// across edges between free faces. A plane region needs one face, a cylinder or sphere six.
/// The regions do not overlap; they are rough, to be refitted. Deterministic. The mesh must be a
/// manifold triangle mesh (as mesh.repair leaves it).
[[nodiscard]] Result<std::vector<GrownRegion>>
growRegions(const TriMesh& mesh, const std::vector<char>& free, const GrowParams& params);

} // namespace m2c::cgal

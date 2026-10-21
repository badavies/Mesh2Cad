// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fitting an exact box to a region of a scan (ARCHITECTURE.md section 7, "Primitive fitting"):
// six planes, opposite ones parallel and neighbouring ones perpendicular.

#include <cstdint>
#include <span>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct BoxFitParams {
    double maxResidualMm{0.1}; ///< The fit fails when its P95 distance is above this.
};

struct BoxFit {
    OrientedBox box;
    double rmsMm{0.0}; ///< Distance of the region's vertices to the box's surface.
    double maxMm{0.0};
    double p95Mm{0.0};
};

/// Fits a box to the vertices of a region of faces. The axes start from the faces' normals (the
/// direction most of the area faces, then the most-faced direction across it) and the box from
/// the region's extent along them; Gauss-Newton steps then move the centre, turn the axes and
/// change the sizes so each vertex is close to the face nearest it. Sides the region does not
/// reach stay at its extent. The axes are ordered and signed to follow the world axes as
/// closely as they can. Fails with InvalidArgument for no faces, or InvalidState when the P95
/// distance is above params.maxResidualMm. Deterministic.
[[nodiscard]] Result<BoxFit>
fitBox(const TriMesh& mesh, std::span<const std::uint32_t> faces, const BoxFitParams& params);

} // namespace m2c

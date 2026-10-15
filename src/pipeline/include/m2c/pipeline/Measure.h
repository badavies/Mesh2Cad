// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include "m2c/core/Measurement.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// Snaps `query` onto the mesh: to a vertex or edge if one is within `snapToleranceMm` of the
/// nearest surface point, otherwise to the surface point itself (ARCHITECTURE.md section 7).
[[nodiscard]] PickedPoint
snapToMesh(const TriMesh& mesh, std::uint64_t objectId, const Vec3d& query, double snapToleranceMm);

/// Ray pick against the full-resolution mesh (the viewport may be drawing a decimated proxy).
[[nodiscard]] std::optional<RayHit>
pickMesh(const TriMesh& mesh, const Vec3d& origin, const Vec3d& direction);

} // namespace m2c

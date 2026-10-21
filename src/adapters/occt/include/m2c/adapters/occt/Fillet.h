// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Rounding a solid's edges with a constant-radius fillet (ADR-033): the fillet a scan shows,
// rebuilt exactly. Only core types cross this header.

#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"

namespace m2c::occt {

/// The solid with the edge nearest each point rounded by a rolling-ball fillet of radiusMm
/// (BRepFilletAPI_MakeFillet); points nearest the same edge round it once. Its faces are the
/// kernel's own (faces() empty); `solid` is unchanged. Fails with InvalidArgument for no points,
/// a radius that is not positive, or an edge that is not between two planes, cylinders, cones
/// or spheres; with InvalidState when the kernel cannot build the fillet (a radius too large
/// for the faces either side) or the result is not one valid closed solid.
[[nodiscard]] Result<Solid>
filletEdges(const Solid& solid, const std::vector<Vec3d>& edgePoints, double radiusMm);

} // namespace m2c::occt

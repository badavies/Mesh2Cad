// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Booleans between solids (ARCHITECTURE.md sections 6 and 7, "Combine and solidify"): fuse, cut
// and common of two solids. Only core types cross this header.

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"

namespace m2c::occt {

enum class BooleanOp {
    Fuse,
    Cut,
    Common
};

/// Fuses b to a, cuts b from a, or keeps what they share, with Open CASCADE's fuzzy booleans:
/// boundaries closer than fuzzyMm are treated as touching. The result's faces of one surface are
/// merged and the shape is fixed (ShapeFix). Its faces are the kernel's own (faces() empty) and
/// its display mesh the kernel's triangulation; a and b are unchanged. Fails with InvalidState
/// when the result is empty or more than one solid, or unless it is one valid closed solid of
/// positive volume; InvalidArgument for a negative fuzzyMm. Deterministic.
[[nodiscard]] Result<Solid>
booleanSolids(const Solid& a, const Solid& b, BooleanOp op, double fuzzyMm);

/// A fitted cylinder or sphere as a solid, to fuse to or cut from another (a true-cylinder hole).
/// A sphere is whole. A cylinder runs along its axis, centred on the fit's extent there, lengthMm
/// long, or as long as the extent along the axis when lengthMm is 0. Its faces are the exact
/// cylinder or sphere. Fails with InvalidArgument for a plane or cone, a radius that is not
/// positive, a negative lengthMm, or a cylinder of no length.
[[nodiscard]] Result<Solid> primitiveSolid(const FittedPrimitive& fit, double lengthMm);

/// The box as a solid of six exact planar faces (BRepPrimAPI_MakeBox). Fails with
/// InvalidArgument unless every size is positive and the axes are unit, perpendicular and
/// right-handed (within 1e-6).
[[nodiscard]] Result<Solid> boxSolid(const OrientedBox& box);

} // namespace m2c::occt

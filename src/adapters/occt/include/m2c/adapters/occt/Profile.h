// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Solids from section sketches (ARCHITECTURE.md section 7, "Section sketches"): a sketch's profile
// extruded along its plane's normal or revolved about an axis in its plane. Only core types cross
// this header.

#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"

namespace m2c::occt {

/// Extrudes the sketch's profile along its plane's normal, from backwardMm behind the plane to
/// forwardMm in front of it (Open CASCADE's BRepPrimAPI_MakePrism). The profile is the inside of
/// the loop of largest area less the inside of every other loop, each of which must lie inside it
/// and outside the others. Lines and arcs are exact; a spline is interpolated through its points.
/// Its faces are the kernel's own (faces() empty). Fails with InvalidArgument for a sketch with no
/// loop, an open loop (a curve not starting within 1e-6 mm of where the one before ends), a loop
/// outside the outer one or inside another, or forwardMm + backwardMm not positive; with
/// InvalidState unless the result is one valid closed solid.
[[nodiscard]] Result<Solid>
extrudeSketch(const Sketch& sketch, double forwardMm, double backwardMm);

/// Revolves the sketch's profile (as extrudeSketch takes it) by angleRad about the axis through
/// axisPoint along axisDirection, both in the sketch's plane coordinates (BRepPrimAPI_MakeRevol).
/// Fails as extrudeSketch does, and with InvalidArgument for a zero direction, an angle not in
/// (0, 2 pi], or a profile on both sides of the axis.
[[nodiscard]] Result<Solid> revolveSketch(const Sketch& sketch,
                                          const Vec2d& axisPoint,
                                          const Vec2d& axisDirection,
                                          double angleRad);

} // namespace m2c::occt

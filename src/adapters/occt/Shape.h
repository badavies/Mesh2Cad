// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the Open CASCADE adapter: what a SolidShape is.

#include <string>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"

namespace m2c {

struct SolidShape {
    TopoDS_Shape shape;
};

namespace occt {

/// Counts, checks and measures a shape (defined in Brep.cpp).
[[nodiscard]] SolidSummary summarise(const TopoDS_Shape& shape);

/// The kernel's triangulation of every face of a solid, at most 0.1% of its diagonal from it, in
/// TopExp::MapShapes order and turned to face outward (defined in Trim.cpp).
[[nodiscard]] Result<FaceTessellation> triangulate(const TopoDS_Shape& solid);

/// A Solid of the kernel's own faces (faces() empty) with the kernel's triangulation for display.
/// Fails with InvalidState, saying `what` (with its verb) "not one valid closed solid", unless the
/// shape is one valid closed solid of positive volume (defined in Trim.cpp).
[[nodiscard]] Result<Solid> kernelSolid(const TopoDS_Shape& solid, const std::string& what);

/// A sketch's loops as planar faces: every loop not inside another bounds a face, the loops
/// directly inside it are its holes, and a loop inside a hole bounds a face of its own. Lines and
/// arcs are exact; a spline is interpolated through its points. Fails with InvalidArgument for a
/// sketch with no loop or an open one (defined in Profile.cpp).
[[nodiscard]] Result<std::vector<TopoDS_Face>> sketchFaces(const Sketch& sketch);

} // namespace occt

} // namespace m2c

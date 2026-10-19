// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the Open CASCADE adapter: what a SolidShape is.

#include <TopoDS_Shape.hxx>

#include "m2c/core/Error.h"
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

} // namespace occt

} // namespace m2c

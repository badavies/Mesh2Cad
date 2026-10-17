// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the Open CASCADE adapter: what a SolidShape is.

#include <TopoDS_Shape.hxx>

namespace m2c {

struct SolidShape {
    TopoDS_Shape shape;
};

} // namespace m2c

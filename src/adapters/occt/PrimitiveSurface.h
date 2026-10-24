// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the Open CASCADE adapter: a fitted primitive as a Geom_Surface.

#include <Geom_Surface.hxx>
#include <Standard_Handle.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"

namespace m2c::occt {

/// The unbounded surface of the primitive. A cone's local origin is its apex (radius 0 there).
/// Fails as surfaceDistances does.
[[nodiscard]] Result<opencascade::handle<Geom_Surface>> toGeomSurface(const Primitive& s);

} // namespace m2c::occt

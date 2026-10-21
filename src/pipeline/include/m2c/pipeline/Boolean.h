// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Booleans between solids (ARCHITECTURE.md sections 6 and 7): fuse, cut and common of two solids
// (Open CASCADE adapter).

#include "m2c/adapters/occt/Boolean.h"

namespace m2c {

using occt::BooleanOp;
using occt::booleanSolids;
using occt::boxSolid;
using occt::primitiveSolid;

} // namespace m2c

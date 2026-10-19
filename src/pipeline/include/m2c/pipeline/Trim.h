// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Intersect and trim (ARCHITECTURE.md section 7): fitted primitives split against each other and
// closed into a solid with sharp edges (Open CASCADE adapter).

#include "m2c/adapters/occt/Trim.h"

namespace m2c {

using occt::TrimParams;
using occt::trimPrimitives;
using occt::TrimResult;

} // namespace m2c

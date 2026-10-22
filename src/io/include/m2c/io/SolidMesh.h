// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Meshes of solids for GLB, glTF, OBJ and STL files (ADR-005), triangulated by the Open CASCADE
// adapter.

#include "m2c/adapters/occt/Brep.h"

namespace m2c {

using occt::ExportTessellation;
using occt::exportTriangulation;

} // namespace m2c

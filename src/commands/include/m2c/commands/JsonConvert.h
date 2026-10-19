// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "m2c/commands/Command.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Transform.h"

namespace m2c {

/// JSON representations shared by every front end. Keys are snake_case, lengths are in mm.
[[nodiscard]] Json toJson(const Error& error);
[[nodiscard]] Json toJson(const Aabb& box);
[[nodiscard]] Json toJson(const MeshStats& stats);
[[nodiscard]] Json toJson(const Vec3d& v);
[[nodiscard]] Json toJson(const Transform3d& t);
[[nodiscard]] Json toJson(const PickedPoint& p);
[[nodiscard]] Json toJson(const Measurement& m);
/// Its kind and parameters: point and normal, axis point, direction and radius, centre and
/// radius, or apex, axis direction and half-angle in degrees.
[[nodiscard]] Json toJson(const Primitive& s);

} // namespace m2c

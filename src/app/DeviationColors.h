// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>

#include "m2c/core/DeviationColors.h"

namespace m2c::app {

/// m2c::deviationColor as a QColor.
inline QColor deviationColor(double d, double toleranceMm, double rangeMm, bool colorBlindSafe) {
    const Rgb8 c = m2c::deviationColor(d, toleranceMm, rangeMm, colorBlindSafe);
    return {c.r, c.g, c.b};
}

} // namespace m2c::app

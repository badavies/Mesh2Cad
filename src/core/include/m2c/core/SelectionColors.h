// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Selection colours shared by the app's viewport and view.render, so a region has the same colour
// in both.

#include <cmath>
#include <cstdint>

#include "m2c/core/Raster.h"

namespace m2c {

/// The current selection's faces are mixed with this orange.
inline constexpr Rgb kCurrentSelectionColor{1.0, 0.50, 0.10};

/// The colour of a selection that is not the current one, from its object id. Hues step by the
/// golden ratio, so consecutive ids (the regions mesh.segment makes) are far apart, and stay
/// between yellow-green and magenta, away from the current selection's orange; saturation and
/// value alternate so that hues close together still differ. Deterministic.
inline Rgb selectionColor(std::uint64_t id) {
    constexpr double kGolden = 0.6180339887498949;
    const double step = std::fmod(static_cast<double>(id % 1'000'000) * kGolden, 1.0);
    const double hue = 6.0 * (0.17 + 0.78 * step); // in sixths of the circle
    const double saturation = id % 2 != 0 ? 0.70 : 0.55;
    const double value = id % 3 != 0 ? 0.95 : 0.80;
    const double sector = std::floor(hue);
    const double f = hue - sector;
    const double p = value * (1.0 - saturation);
    const double q = value * (1.0 - saturation * f);
    const double t = value * (1.0 - saturation * (1.0 - f));
    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {value, t, p};
    case 1:
        return {q, value, p};
    case 2:
        return {p, value, t};
    case 3:
        return {p, q, value};
    case 4:
        return {t, p, value};
    default:
        return {value, p, q};
    }
}

} // namespace m2c

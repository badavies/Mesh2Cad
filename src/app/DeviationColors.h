// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <algorithm>
#include <cmath>

namespace m2c::app {

/// Colour of a signed deviation (ARCHITECTURE.md section 7, Deviation analysis): neutral inside
/// the tolerance band, then stronger outward (d > 0) and inward (d < 0) up to `range`, beyond
/// which colours stay at their strongest. The standard palette is the usual green, yellow to red
/// and cyan to blue; the colour-blind-safe one is ColorBrewer's purple-orange (PuOr), which keeps
/// the two directions apart for red-green colour blindness.
inline QColor deviationColor(double d, double toleranceMm, double rangeMm, bool colorBlindSafe) {
    struct Stops {
        QColor neutral, outLow, outHigh, inLow, inHigh;
    };
    static const Stops standard{QColor(80, 190, 90),
                                QColor(250, 215, 50),
                                QColor(215, 30, 30),
                                QColor(80, 205, 240),
                                QColor(30, 55, 215)};
    static const Stops safe{QColor(215, 215, 215),
                            QColor(253, 184, 99),
                            QColor(179, 88, 6),
                            QColor(178, 171, 210),
                            QColor(84, 39, 136)};
    const Stops& s = colorBlindSafe ? safe : standard;
    const double magnitude = std::abs(d);
    if (magnitude <= toleranceMm) return s.neutral;
    const double t =
        std::clamp((magnitude - toleranceMm) / std::max(rangeMm - toleranceMm, 1e-12), 0.0, 1.0);
    const QColor& low = d > 0 ? s.outLow : s.inLow;
    const QColor& high = d > 0 ? s.outHigh : s.inHigh;
    const auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return {
        mix(low.red(), high.red()), mix(low.green(), high.green()), mix(low.blue(), high.blue())};
}

/// The colour scale's reach: the 95th percentile of |d|, at least twice the tolerance, so a few
/// outliers do not wash out the map.
inline double deviationRange(double p95AbsMm, double toleranceMm) {
    return std::max(p95AbsMm, 2.0 * toleranceMm);
}

} // namespace m2c::app

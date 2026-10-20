// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace m2c {

/// An 8-bit colour.
struct Rgb8 {
    int r{0};
    int g{0};
    int b{0};
};

/// Colour of a signed deviation (ARCHITECTURE.md section 7, Deviation analysis): neutral inside
/// the tolerance band, then stronger outward (d > 0) and inward (d < 0) up to `range`, beyond
/// which colours stay at their strongest. The standard palette is the usual green, yellow to red
/// and cyan to blue; the colour-blind-safe one is ColorBrewer's purple-orange (PuOr), which keeps
/// the two directions apart for red-green colour blindness.
inline Rgb8 deviationColor(double d, double toleranceMm, double rangeMm, bool colorBlindSafe) {
    struct Stops {
        Rgb8 neutral, outLow, outHigh, inLow, inHigh;
    };
    static constexpr Stops standard{
        {80, 190, 90}, {250, 215, 50}, {215, 30, 30}, {80, 205, 240}, {30, 55, 215}};
    static constexpr Stops safe{
        {215, 215, 215}, {253, 184, 99}, {179, 88, 6}, {178, 171, 210}, {84, 39, 136}};
    const Stops& s = colorBlindSafe ? safe : standard;
    const double magnitude = std::abs(d);
    if (magnitude <= toleranceMm) return s.neutral;
    const double t =
        std::clamp((magnitude - toleranceMm) / std::max(rangeMm - toleranceMm, 1e-12), 0.0, 1.0);
    const Rgb8& low = d > 0 ? s.outLow : s.inLow;
    const Rgb8& high = d > 0 ? s.outHigh : s.inHigh;
    const auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return {mix(low.r, high.r), mix(low.g, high.g), mix(low.b, high.b)};
}

/// The colour scale's reach: the 95th percentile of |d|, at least twice the tolerance, so a few
/// outliers do not wash out the map.
inline double deviationRange(double p95AbsMm, double toleranceMm) {
    return std::max(p95AbsMm, 2.0 * toleranceMm);
}

} // namespace m2c

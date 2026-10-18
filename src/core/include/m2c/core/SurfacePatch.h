// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "m2c/core/Vec3.h"

namespace m2c {

/// Cubic Bernstein weights at t.
[[nodiscard]] constexpr std::array<double, 4> bernstein(double t) noexcept {
    const double s = 1.0 - t;
    return {s * s * s, 3.0 * s * s * t, 3.0 * s * t * t, t * t * t};
}

/// Derivatives of the cubic Bernstein weights at t.
[[nodiscard]] constexpr std::array<double, 4> bernsteinDerivative(double t) noexcept {
    const double s = 1.0 - t;
    return {-3.0 * s * s, 3.0 * s * s - 6.0 * s * t, 6.0 * s * t - 3.0 * t * t, 3.0 * t * t};
}

/// Bicubic Bezier patch over [0, 1]^2: poles[4 * j + i] is the control point in row j (along v)
/// and column i (along u).
struct BicubicPatch {
    std::array<Vec3d, 16> poles{};
};

[[nodiscard]] constexpr Vec3d evaluate(const BicubicPatch& patch, double u, double v) noexcept {
    const auto bu = bernstein(u);
    const auto bv = bernstein(v);
    Vec3d p{};
    for (std::size_t j = 0; j < 4; ++j) {
        for (std::size_t i = 0; i < 4; ++i) p = p + patch.poles[4 * j + i] * (bu[i] * bv[j]);
    }
    return p;
}

/// The unnormalised normal du x dv at (u, v).
[[nodiscard]] constexpr Vec3d normal(const BicubicPatch& patch, double u, double v) noexcept {
    const auto bu = bernstein(u);
    const auto bv = bernstein(v);
    const auto du = bernsteinDerivative(u);
    const auto dv = bernsteinDerivative(v);
    Vec3d pu{}, pv{};
    for (std::size_t j = 0; j < 4; ++j) {
        for (std::size_t i = 0; i < 4; ++i) {
            pu = pu + patch.poles[4 * j + i] * (du[i] * bv[j]);
            pv = pv + patch.poles[4 * j + i] * (bu[i] * dv[j]);
        }
    }
    return cross(pu, pv);
}

/// One face of the surface built from a quad cage (ARCHITECTURE.md section 6, Surface build): a
/// bicubic B-spline over [0, 1]^2 made of a grid of Bezier pieces that meet with position
/// continuity (knots of multiplicity three). It covers one cage quad, one corner quad of a cage
/// face with another number of sides, or a rectangle of cellsU x cellsV cage quads merged into one
/// face, each quad an equal share of [0, 1]^2. u x v points outward.
struct SurfacePatch {
    std::uint32_t cageFace{0}; ///< The cage face of its first quad.
    std::uint32_t cellsU{1};   ///< Cage quads along u.
    std::uint32_t cellsV{1};   ///< Cage quads along v.
    /// Parameters where the pieces meet along u and v, rising from 0 to 1.
    std::vector<double> uBreaks{0.0, 1.0};
    std::vector<double> vBreaks{0.0, 1.0};
    /// (3 piecesU() + 1) x (3 piecesV() + 1) poles, row-major with u fastest.
    std::vector<Vec3d> poles;
    /// Pieces that approximate an irregular region (caps): their boundary is exact, their
    /// interior a least-squares fit, so tangents across their edges are close but not equal.
    std::size_t capPieces{0};

    [[nodiscard]] std::size_t piecesU() const noexcept { return uBreaks.size() - 1; }
    [[nodiscard]] std::size_t piecesV() const noexcept { return vBreaks.size() - 1; }
    [[nodiscard]] std::size_t polesPerRow() const noexcept { return 3 * piecesU() + 1; }
    [[nodiscard]] std::size_t poleRows() const noexcept { return 3 * piecesV() + 1; }

    /// The Bezier piece in column i (along u) and row j (along v).
    [[nodiscard]] BicubicPatch piece(std::size_t i, std::size_t j) const {
        BicubicPatch b;
        const std::size_t row = polesPerRow();
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c)
                b.poles[4 * r + c] = poles[(3 * j + r) * row + 3 * i + c];
        }
        return b;
    }
};

/// n + 1 equally spaced breaks from 0 to 1: the breaks of n equal pieces.
[[nodiscard]] inline std::vector<double> uniformBreaks(std::size_t n) {
    std::vector<double> breaks(n + 1);
    for (std::size_t k = 0; k <= n; ++k) {
        breaks[k] = static_cast<double>(k) / static_cast<double>(n);
    }
    return breaks;
}

/// The piece holding parameter x of the given breaks, and x in that piece (0 to 1).
[[nodiscard]] inline std::pair<std::size_t, double> pieceAt(const std::vector<double>& breaks,
                                                            double x) {
    const auto it = std::upper_bound(breaks.begin() + 1, breaks.end() - 1, x);
    const auto i = static_cast<std::size_t>(it - breaks.begin()) - 1;
    return {i, (x - breaks[i]) / (breaks[i + 1] - breaks[i])};
}

[[nodiscard]] inline Vec3d evaluate(const SurfacePatch& patch, double u, double v) {
    const auto [i, s] = pieceAt(patch.uBreaks, u);
    const auto [j, t] = pieceAt(patch.vBreaks, v);
    return evaluate(patch.piece(i, j), s, t);
}

} // namespace m2c

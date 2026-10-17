// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
/// bicubic B-spline over [0, 1]^2 made of spans x spans Bezier pieces that meet with position
/// continuity (knots of multiplicity three). It covers one cage quad, or one corner quad of a
/// cage face with another number of sides. u x v points outward.
struct SurfacePatch {
    std::uint32_t cageFace{0};
    std::size_t spans{1};
    /// (3 spans + 1)^2 poles, row-major with u fastest.
    std::vector<Vec3d> poles;
    /// Pieces that approximate an irregular region (caps): their boundary is exact, their
    /// interior a least-squares fit, so tangents across their edges are close but not equal.
    std::size_t capPieces{0};

    [[nodiscard]] std::size_t polesPerRow() const noexcept { return 3 * spans + 1; }

    /// The Bezier piece in span (i, j).
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

[[nodiscard]] inline Vec3d evaluate(const SurfacePatch& patch, double u, double v) {
    const double n = static_cast<double>(patch.spans);
    const auto span = [&](double x) {
        return std::min(static_cast<std::size_t>(std::max(x * n, 0.0)), patch.spans - 1);
    };
    const std::size_t i = span(u);
    const std::size_t j = span(v);
    return evaluate(
        patch.piece(i, j), u * n - static_cast<double>(i), v * n - static_cast<double>(j));
}

} // namespace m2c

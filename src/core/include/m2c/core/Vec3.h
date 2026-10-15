// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace m2c {

/// Plain 3D vector or point in millimetres (project unit). Deliberately minimal: richer linear
/// algebra comes from Eigen when a stage needs it (new dependency => decision record).
struct Vec3d {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    friend constexpr bool operator==(const Vec3d&, const Vec3d&) = default;
};

[[nodiscard]] constexpr Vec3d operator+(const Vec3d& a, const Vec3d& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3d operator-(const Vec3d& a, const Vec3d& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3d operator*(const Vec3d& a, double s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}

[[nodiscard]] constexpr double dot(const Vec3d& a, const Vec3d& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3d cross(const Vec3d& a, const Vec3d& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline double length(const Vec3d& a) noexcept {
    return std::sqrt(dot(a, a));
}

} // namespace m2c

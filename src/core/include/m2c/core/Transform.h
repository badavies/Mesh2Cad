// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

/// Rigid transform p' = R p + t, with R a rotation matrix stored row-major. Produced by alignment
/// and stored on the document so the alignment can be inspected and redone.
struct Transform3d {
    std::array<double, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vec3d translation{};

    [[nodiscard]] static Transform3d identity() noexcept { return {}; }

    /// Builds a transform from the images of the x, y and z axes (the rows of R) and t.
    [[nodiscard]] static Transform3d
    fromAxes(const Vec3d& xAxis, const Vec3d& yAxis, const Vec3d& zAxis, const Vec3d& t) noexcept {
        return {{xAxis.x, xAxis.y, xAxis.z, yAxis.x, yAxis.y, yAxis.z, zAxis.x, zAxis.y, zAxis.z},
                t};
    }

    [[nodiscard]] Vec3d rotate(const Vec3d& v) const noexcept {
        const auto& r = rotation;
        return {r[0] * v.x + r[1] * v.y + r[2] * v.z,
                r[3] * v.x + r[4] * v.y + r[5] * v.z,
                r[6] * v.x + r[7] * v.y + r[8] * v.z};
    }

    [[nodiscard]] Vec3d apply(const Vec3d& p) const noexcept { return rotate(p) + translation; }

    /// (this * other)(p) = this(other(p)).
    [[nodiscard]] Transform3d operator*(const Transform3d& other) const noexcept {
        Transform3d out;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double sum = 0.0;
                for (int k = 0; k < 3; ++k) {
                    sum += rotation[static_cast<std::size_t>(3 * i + k)] *
                           other.rotation[static_cast<std::size_t>(3 * k + j)];
                }
                out.rotation[static_cast<std::size_t>(3 * i + j)] = sum;
            }
        }
        out.translation = rotate(other.translation) + translation;
        return out;
    }

    [[nodiscard]] Transform3d inverse() const noexcept {
        Transform3d out;
        const auto& r = rotation;
        out.rotation = {r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8]};
        out.translation = out.rotate(translation) * -1.0;
        return out;
    }
};

/// Returns the mesh with every vertex transformed. The index buffer is shared with the input.
[[nodiscard]] Result<TriMesh> transformMesh(const TriMesh& mesh, const Transform3d& transform);

} // namespace m2c

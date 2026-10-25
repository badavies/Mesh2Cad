// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::app {

/// Column-major 4x4 matrix in double precision (OpenGL layout), so scan coordinates far from the
/// origin keep their precision until the last step.
struct Mat4d {
    std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] double& at(int row, int col) {
        return m[static_cast<std::size_t>(col * 4 + row)];
    }
    [[nodiscard]] double at(int row, int col) const {
        return m[static_cast<std::size_t>(col * 4 + row)];
    }
    friend Mat4d operator*(const Mat4d& a, const Mat4d& b);
};

/// Orbit camera with +Z up (scans and CAD parts sit on the XY plane).
class Camera {
public:
    Vec3d target{};
    double distance{100.0};
    double yawDeg{-60.0};  ///< Around +Z, from +X.
    double pitchDeg{25.0}; ///< Elevation above the XY plane.
    double fovYDeg{35.0};
    double sceneRadius{50.0};

    [[nodiscard]] Vec3d eye() const;
    [[nodiscard]] Vec3d forward() const;
    [[nodiscard]] Vec3d right() const;
    [[nodiscard]] Vec3d up() const;

    /// View matrix with the target at the origin: geometry is drawn relative to the target.
    [[nodiscard]] Mat4d viewRelative() const;
    /// Full world view matrix.
    [[nodiscard]] Mat4d view() const;
    [[nodiscard]] Mat4d projection(double aspect) const;
    [[nodiscard]] double nearPlane() const;
    [[nodiscard]] double farPlane() const;

    void orbit(double dxDeg, double dyDeg);
    /// Pan by a fraction of the view height.
    void pan(double dxFraction, double dyFraction);
    void zoom(double factor);
    /// Frames a box.
    void fit(const Aabb& box);
    void setView(double yaw, double pitch);

    /// Camera-to-world transform: its columns are right, up, back (the camera looks down -Z) and
    /// the eye. This is the view the 3Dconnexion Navigation Library reads and writes.
    [[nodiscard]] Mat4d cameraToWorld() const;
    /// Moves the eye and turns the camera to a camera-to-world transform. The camera keeps +Z up,
    /// so roll is dropped and the pitch stops short of straight up or down. The target becomes
    /// the point on the new line of sight nearest the old one, so orbiting stays around it.
    void setCameraToWorld(const Mat4d& transform);

    /// World-space ray through a point given in normalised device coordinates.
    void ray(double ndcX, double ndcY, double aspect, Vec3d& origin, Vec3d& direction) const;
};

[[nodiscard]] inline Vec3d normalized(const Vec3d& v) {
    const double l = length(v);
    return l > 0.0 ? v * (1.0 / l) : v;
}

} // namespace m2c::app

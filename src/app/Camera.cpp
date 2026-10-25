// SPDX-License-Identifier: GPL-3.0-or-later
#include "Camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::app {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;
} // namespace

Mat4d operator*(const Mat4d& a, const Mat4d& b) {
    Mat4d r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a.at(i, k) * b.at(k, j);
            r.at(i, j) = s;
        }
    }
    return r;
}

Vec3d Camera::eye() const {
    const double y = yawDeg * kDeg;
    const double p = pitchDeg * kDeg;
    return target +
           Vec3d{.x = std::cos(p) * std::cos(y), .y = std::cos(p) * std::sin(y), .z = std::sin(p)} *
               distance;
}

Vec3d Camera::forward() const {
    return normalized(target - eye());
}

Vec3d Camera::right() const {
    const double y = yawDeg * kDeg;
    // Perpendicular to the view direction in the XY plane, which stays defined looking straight
    // down, then turned by the roll.
    const Vec3d level{.x = -std::sin(y), .y = std::cos(y), .z = 0.0};
    const double r = rollDeg * kDeg;
    return level * std::cos(r) + cross(level, forward()) * std::sin(r);
}

Vec3d Camera::up() const {
    return cross(right(), forward());
}

Mat4d Camera::viewRelative() const {
    const Vec3d f = forward();
    const Vec3d r = right();
    const Vec3d u = up();
    const Vec3d e = eye() - target;
    Mat4d v;
    v.at(0, 0) = r.x, v.at(0, 1) = r.y, v.at(0, 2) = r.z, v.at(0, 3) = -dot(r, e);
    v.at(1, 0) = u.x, v.at(1, 1) = u.y, v.at(1, 2) = u.z, v.at(1, 3) = -dot(u, e);
    v.at(2, 0) = -f.x, v.at(2, 1) = -f.y, v.at(2, 2) = -f.z, v.at(2, 3) = dot(f, e);
    return v;
}

Mat4d Camera::view() const {
    Mat4d t;
    t.at(0, 3) = -target.x;
    t.at(1, 3) = -target.y;
    t.at(2, 3) = -target.z;
    return viewRelative() * t;
}

double Camera::nearPlane() const {
    return std::max(distance - 2.0 * sceneRadius, distance * 0.01);
}

double Camera::farPlane() const {
    return distance + 3.0 * sceneRadius + nearPlane();
}

Mat4d Camera::projection(const double aspect) const {
    const double n = nearPlane();
    const double f = farPlane();
    const double t = 1.0 / std::tan(fovYDeg * kDeg / 2.0);
    Mat4d p;
    p.m.fill(0.0);
    p.at(0, 0) = t / aspect;
    p.at(1, 1) = t;
    p.at(2, 2) = -(f + n) / (f - n);
    p.at(2, 3) = -2.0 * f * n / (f - n);
    p.at(3, 2) = -1.0;
    return p;
}

void Camera::orbit(const double dxDeg, const double dyDeg) {
    yawDeg = std::fmod(yawDeg - dxDeg, 360.0);
    pitchDeg = std::clamp(pitchDeg + dyDeg, -89.9, 89.9);
}

void Camera::pan(const double dxFraction, const double dyFraction) {
    const double h = 2.0 * distance * std::tan(fovYDeg * kDeg / 2.0);
    target = target - right() * (dxFraction * h) + up() * (dyFraction * h);
}

void Camera::zoom(const double factor) {
    distance = std::clamp(distance * factor, sceneRadius * 1e-4, sceneRadius * 100.0);
}

void Camera::fit(const Aabb& box) {
    target = (box.min + box.max) * 0.5;
    sceneRadius = std::max(length(box.max - box.min) * 0.5, 1e-6);
    distance = sceneRadius / std::sin(fovYDeg * kDeg / 2.0) * 1.05;
}

void Camera::setView(const double yaw, const double pitch) {
    yawDeg = yaw;
    pitchDeg = pitch;
    rollDeg = 0.0;
}

Mat4d Camera::cameraToWorld() const {
    const std::array<Vec3d, 4> columns{right(), up(), forward() * -1.0, eye()};
    Mat4d m;
    for (int c = 0; c < 4; ++c) {
        const Vec3d& v = columns[static_cast<std::size_t>(c)];
        m.at(0, c) = v.x, m.at(1, c) = v.y, m.at(2, c) = v.z;
    }
    return m;
}

void Camera::setCameraToWorld(const Mat4d& transform, const std::optional<Vec3d> focus) {
    const Vec3d r{.x = transform.at(0, 0), .y = transform.at(1, 0), .z = transform.at(2, 0)};
    const Vec3d back = normalized(
        Vec3d{.x = transform.at(0, 2), .y = transform.at(1, 2), .z = transform.at(2, 2)});
    const Vec3d e{.x = transform.at(0, 3), .y = transform.at(1, 3), .z = transform.at(2, 3)};
    const double horizontal = std::hypot(back.x, back.y);
    pitchDeg = std::atan2(back.z, horizontal) / kDeg;
    // Looking straight up or down, the line of sight gives no heading; the right axis does.
    yawDeg = horizontal > 1e-9 ? std::atan2(back.y, back.x) / kDeg : std::atan2(-r.x, r.y) / kDeg;
    // The roll turns the level right axis onto the given one. Over the top, the heading has
    // swung round and the roll is near 180 degrees.
    rollDeg = 0.0;
    const Vec3d level = right();
    const Vec3d f = back * -1.0;
    rollDeg = std::atan2(dot(r, cross(level, f)), dot(r, level)) / kDeg;
    const double ahead = dot(focus.value_or(target) - e, f);
    if (ahead > sceneRadius * 1e-3) distance = std::min(ahead, sceneRadius * 100.0);
    target = e + f * distance;
}

void Camera::ray(const double ndcX,
                 const double ndcY,
                 const double aspect,
                 Vec3d& origin,
                 Vec3d& direction) const {
    const double t = std::tan(fovYDeg * kDeg / 2.0);
    origin = eye();
    direction = normalized(forward() + right() * (ndcX * t * aspect) + up() * (ndcY * t));
}

} // namespace m2c::app

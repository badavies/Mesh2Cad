// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "m2c/core/SelectionColors.h"

namespace m2c {

namespace {

Vec3d unit(const Vec3d& v) {
    return v * (1.0 / length(v));
}

Rgb mix(const Rgb& a, const Rgb& b, double t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb scaled(const Rgb& c, double s) {
    return {c.r * s, c.g * s, c.b * s};
}

std::uint8_t channel(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

} // namespace

Rgb Image::at(const int x, const int y) const {
    const auto i = 3 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x));
    return {rgb[i] / 255.0, rgb[i + 1] / 255.0, rgb[i + 2] / 255.0};
}

Raster::Raster(const int width,
               const int height,
               const Aabb& box,
               const Vec3d& from,
               const Vec3d& up,
               const Rgb background)
    : image_{.width = width, .height = height, .rgb = {}},
      depth_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
             std::numeric_limits<double>::infinity()) {
    forward_ = unit(from) * -1.0;
    right_ = unit(cross(forward_, up));
    up_ = cross(right_, forward_);
    // Frame the box's corners with a 5% margin on each side.
    double minX = std::numeric_limits<double>::infinity(), maxX = -minX, minY = minX, maxY = -minX;
    double minZ = minX, maxZ = -minX;
    for (int k = 0; k < 8; ++k) {
        const Vec3d c{(k & 1) != 0 ? box.max.x : box.min.x,
                      (k & 2) != 0 ? box.max.y : box.min.y,
                      (k & 4) != 0 ? box.max.z : box.min.z};
        minX = std::min(minX, dot(c, right_));
        maxX = std::max(maxX, dot(c, right_));
        minY = std::min(minY, dot(c, up_));
        maxY = std::max(maxY, dot(c, up_));
        minZ = std::min(minZ, dot(c, forward_));
        maxZ = std::max(maxZ, dot(c, forward_));
    }
    centreX_ = 0.5 * (minX + maxX);
    centreY_ = 0.5 * (minY + maxY);
    const double spanX = std::max(maxX - minX, 1e-9), spanY = std::max(maxY - minY, 1e-9);
    scale_ = 0.9 * std::min(width / spanX, height / spanY);
    lineBias_ = 1e-3 * std::max({spanX, spanY, maxZ - minZ});
    image_.rgb.resize(3 * depth_.size());
    for (std::size_t i = 0; i < depth_.size(); ++i) {
        image_.rgb[3 * i] = channel(background.r);
        image_.rgb[3 * i + 1] = channel(background.g);
        image_.rgb[3 * i + 2] = channel(background.b);
    }
}

Raster::Screen Raster::project(const Vec3d& p) const {
    return {0.5 * image_.width + (dot(p, right_) - centreX_) * scale_,
            0.5 * image_.height - (dot(p, up_) - centreY_) * scale_,
            dot(p, forward_)};
}

void Raster::put(const int x, const int y, const Rgb c) {
    const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
                   static_cast<std::size_t>(x);
    image_.rgb[3 * i] = channel(c.r);
    image_.rgb[3 * i + 1] = channel(c.g);
    image_.rgb[3 * i + 2] = channel(c.b);
}

void Raster::triangles(const std::span<const Vec3d> positions,
                       const std::span<const std::uint32_t> indices,
                       const Rgb front,
                       const Rgb back,
                       const std::span<const Rgb> vertexColors,
                       const std::span<const std::uint8_t> marks,
                       const double biasMm,
                       const std::span<const Rgb> markColors) {
    for (std::size_t f = 0; f + 2 < indices.size(); f += 3) {
        const std::array<std::uint32_t, 3> v{indices[f], indices[f + 1], indices[f + 2]};
        const Vec3d n = cross(positions[v[1]] - positions[v[0]], positions[v[2]] - positions[v[0]]);
        if (!(length(n) > 0.0)) continue;
        // The app's shading: ambient plus diffuse from a light at the eye.
        const double facing = dot(unit(n), forward_);
        const double shade = 0.25 + 0.75 * std::abs(facing);
        const bool isFront = facing < 0.0;
        std::array<Rgb, 3> color{front, front, front};
        for (std::size_t k = 0; k < 3; ++k) {
            if (!isFront) {
                color[k] = back;
            } else if (!vertexColors.empty()) {
                color[k] = vertexColors[v[k]];
            }
            if (const std::size_t m = f / 3 < marks.size() ? marks[f / 3] : 0; m != 0) {
                color[k] = mix(color[k],
                               m <= markColors.size() ? markColors[m - 1] : kCurrentSelectionColor,
                               0.8);
            }
        }
        const std::array<Screen, 3> s{
            project(positions[v[0]]), project(positions[v[1]]), project(positions[v[2]])};
        const double area =
            (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::abs(area) < 1e-12) continue;
        const int x0 =
            std::max(0, static_cast<int>(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
        const int x1 = std::min(image_.width - 1,
                                static_cast<int>(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
        const int y1 = std::min(image_.height - 1,
                                static_cast<int>(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                // Barycentric weights at the pixel centre.
                const double px = x + 0.5, py = y + 0.5;
                const auto edge = [&](const Screen& a, const Screen& b) {
                    return ((b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y)) / area;
                };
                const double w0 = edge(s[1], s[2]), w1 = edge(s[2], s[0]), w2 = edge(s[0], s[1]);
                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
                const double z = w0 * s[0].depth + w1 * s[1].depth + w2 * s[2].depth - biasMm;
                double& d =
                    depth_[static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
                           static_cast<std::size_t>(x)];
                if (!(z < d)) continue;
                d = z;
                const Rgb c{w0 * color[0].r + w1 * color[1].r + w2 * color[2].r,
                            w0 * color[0].g + w1 * color[1].g + w2 * color[2].g,
                            w0 * color[0].b + w1 * color[1].b + w2 * color[2].b};
                put(x, y, scaled(c, shade));
            }
        }
    }
}

void Raster::lines(const std::span<const Vec3d> points,
                   const std::span<const std::uint32_t> segments,
                   const Rgb color,
                   const bool onTop) {
    for (std::size_t k = 0; k + 1 < segments.size(); k += 2) {
        const Screen a = project(points[segments[k]]), b = project(points[segments[k + 1]]);
        const int steps = std::max(
            1, static_cast<int>(std::ceil(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y)))));
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            const int x = static_cast<int>(std::floor(a.x + (b.x - a.x) * t));
            const int y = static_cast<int>(std::floor(a.y + (b.y - a.y) * t));
            if (x < 0 || y < 0 || x >= image_.width || y >= image_.height) continue;
            const double z = a.depth + (b.depth - a.depth) * t - lineBias_;
            double& d =
                depth_[static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) +
                       static_cast<std::size_t>(x)];
            if (!onTop && !(z < d)) continue;
            if (!onTop) d = z;
            put(x, y, color);
        }
    }
}

} // namespace m2c

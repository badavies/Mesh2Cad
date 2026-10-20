// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A software rasteriser for pictures of the document without a GPU or display: orthographic,
// flat shaded like the app's viewport, depth tested, and deterministic.

#include <cstdint>
#include <span>
#include <vector>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

/// A colour with channels from 0 to 1.
struct Rgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};
};

/// 8-bit RGB pixels, row by row from the top left.
struct Image {
    int width{0};
    int height{0};
    std::vector<std::uint8_t> rgb;

    [[nodiscard]] Rgb at(int x, int y) const;
};

class Raster {
public:
    /// Looks at `box` from the direction `from` (towards the camera, any length) with `up` pointing
    /// up the picture, framing the box with a margin. Precondition: width and height are positive,
    /// from is not zero and not along up.
    Raster(
        int width, int height, const Aabb& box, const Vec3d& from, const Vec3d& up, Rgb background);

    /// Triangles with front faces in `front`, or `vertexColors` (one per position) blended across
    /// them, back faces in `back`, and faces with a nonzero `marks` entry (one per face) mixed
    /// with the selection orange. `biasMm` moves them towards the camera to draw over a surface
    /// they lie on.
    void triangles(std::span<const Vec3d> positions,
                   std::span<const std::uint32_t> indices,
                   Rgb front,
                   Rgb back,
                   std::span<const Rgb> vertexColors = {},
                   std::span<const std::uint8_t> marks = {},
                   double biasMm = 0.0);

    /// One-pixel lines between pairs of points; `onTop` draws them over everything.
    void lines(std::span<const Vec3d> points,
               std::span<const std::uint32_t> segments,
               Rgb color,
               bool onTop);

    [[nodiscard]] const Image& image() const noexcept { return image_; }

private:
    struct Screen {
        double x, y, depth;
    };
    [[nodiscard]] Screen project(const Vec3d& p) const;
    void put(int x, int y, Rgb c);

    Image image_;
    std::vector<double> depth_;
    Vec3d forward_, right_, up_;
    double centreX_{0.0}, centreY_{0.0}, scale_{1.0}, lineBias_{0.0};
};

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A software rasteriser for pictures of the document without a GPU or display: orthographic,
// flat shaded like the app's viewport, depth tested, and deterministic.

#include <cstdint>
#include <span>
#include <string_view>
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

/// How a Raster frames its box: tightly around the box's corners as seen from the view, or
/// around the box's bounding sphere, which gives the same scale and centre from every direction.
enum class Framing {
    Box,
    Sphere
};

class Raster {
public:
    /// Looks at `box` from the direction `from` (towards the camera, any length) with `up` pointing
    /// up the picture, framing the box with a margin. Precondition: width and height are positive,
    /// from is not zero and not along up.
    Raster(int width,
           int height,
           const Aabb& box,
           const Vec3d& from,
           const Vec3d& up,
           const Rgb& background,
           Framing framing = Framing::Box);

    /// Triangles with front faces in `front`, or `vertexColors` (one per position) blended across
    /// them, back faces in `back`, and faces with a nonzero `marks` entry (one per face) mixed
    /// with `markColors[mark - 1]`, or the selection orange where there is no such colour.
    /// `biasMm` moves them towards the camera to draw over a surface they lie on.
    void triangles(std::span<const Vec3d> positions,
                   std::span<const std::uint32_t> indices,
                   const Rgb& front,
                   const Rgb& back,
                   std::span<const Rgb> vertexColors = {},
                   std::span<const std::uint8_t> marks = {},
                   double biasMm = 0.0,
                   std::span<const Rgb> markColors = {});

    /// One-pixel lines between pairs of points; `onTop` draws them over everything.
    void lines(std::span<const Vec3d> points,
               std::span<const std::uint32_t> segments,
               const Rgb& color,
               bool onTop);

    /// The world axes as seen from the view, X red, Y green and Z blue as in the app, in the
    /// bottom-left corner (within 15% of the width and height), over everything.
    void axisTriad();

    /// Upper-case text in a 5 x 7 pixel font, `scale` pixels per font pixel, from its top-left
    /// corner. Only the letters A to Z and spaces are drawn; anything else leaves a gap.
    void label(int x, int y, std::string_view text, const Rgb& color, int scale = 1);

    [[nodiscard]] const Image& image() const noexcept { return image_; }

private:
    struct Screen {
        double x, y, depth;
    };
    [[nodiscard]] Screen project(const Vec3d& p) const;
    void put(int x, int y, const Rgb& c);

    Image image_;
    std::vector<double> depth_;
    Vec3d forward_, right_, up_;
    double centreX_{0.0}, centreY_{0.0}, scale_{1.0}, lineBias_{0.0};
};

} // namespace m2c

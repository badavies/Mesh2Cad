// SPDX-License-Identifier: GPL-3.0-or-later
// The software rasteriser and PNG writer behind view.render.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <miniz.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Raster.h"
#include "m2c/core/SelectionColors.h"
#include "m2c/io/Png.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr Rgb kBackground{0.1, 0.1, 0.1};
constexpr Rgb kFront{0.8, 0.6, 0.4};
constexpr Rgb kBack{1.0, 0.0, 0.0};

bool same(const Rgb& a, const Rgb& b) {
    return std::abs(a.r - b.r) < 0.003 && std::abs(a.g - b.g) < 0.003 &&
           std::abs(a.b - b.b) < 0.003;
}

} // namespace

TEST_CASE("A box seen from above fills the middle with its lit top", "[core][raster]") {
    const TriMesh box = makeBox(40, 20, 10, 2);
    Raster raster(100, 100, box.bounds(), {0, 0, 1}, {0, 1, 0}, kBackground);
    raster.triangles(box.positions(), box.indices(), kFront, kBack);
    const Image& image = raster.image();
    REQUIRE(image.rgb.size() == 3 * 100 * 100);
    // The top faces the camera, so it is lit fully: 40 x 20 mm framed at 90% of the width, so
    // 90 x 45 pixels from (5, 27.5).
    CHECK(same(image.at(50, 50), kFront));
    CHECK(same(image.at(10, 50), kFront));
    CHECK(same(image.at(50, 25), kBackground));
    CHECK(same(image.at(50, 29), kFront));
    CHECK(same(image.at(2, 50), kBackground));
    CHECK(same(image.at(0, 0), kBackground));

    // Inside out, the far faces show their backs.
    std::vector<std::uint32_t> flipped = box.indices();
    for (std::size_t f = 0; f < flipped.size(); f += 3) std::swap(flipped[f + 1], flipped[f + 2]);
    Raster inside(100, 100, box.bounds(), {0, 0, 1}, {0, 1, 0}, kBackground);
    inside.triangles(box.positions(), flipped, kFront, kBack);
    CHECK(same(inside.image().at(50, 50), kBack));

    // Marked faces mix with orange; lines on top show over the box, others only in front of it.
    const std::vector<std::uint8_t> marks(box.faceCount(), 1);
    Raster marked(100, 100, box.bounds(), {0, 0, 1}, {0, 1, 0}, kBackground);
    marked.triangles(box.positions(), box.indices(), kFront, kBack, {}, marks);
    CHECK(same(marked.image().at(50, 50), {0.96, 0.52, 0.16}));
    const std::vector<Vec3d> ends{{0, 10, -5}, {40, 10, -5}, {0, 10.5, 15}, {40, 10.5, 15}};
    marked.lines(ends, std::vector<std::uint32_t>{0, 1}, kBack, false);
    CHECK(same(marked.image().at(50, 50), {0.96, 0.52, 0.16}));
    marked.lines(ends, std::vector<std::uint32_t>{0, 1}, kBack, true);
    CHECK(same(marked.image().at(50, 50), kBack));
}

TEST_CASE("PNG files hold the image's pixels", "[io][raster]") {
    const TriMesh box = makeBox(40, 20, 10, 2);
    Raster raster(64, 48, box.bounds(), {1, -1, 1}, {0, 0, 1}, kBackground);
    raster.triangles(box.positions(), box.indices(), kFront, kBack);
    const auto png = encodePng(raster.image());
    REQUIRE(png.has_value());
    REQUIRE(png->size() > 8);
    CHECK(std::memcmp(png->data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(*png == *encodePng(raster.image())); // deterministic

    // One IDAT chunk after the 33 bytes of signature and header: rows each led by filter 0.
    std::uint32_t length = 0;
    for (int k = 0; k < 4; ++k) length = length << 8 | (*png)[33 + static_cast<std::size_t>(k)];
    REQUIRE(std::memcmp(png->data() + 37, "IDAT", 4) == 0);
    std::size_t size = 0;
    void* raw =
        tinfl_decompress_mem_to_heap(png->data() + 41, length, &size, TINFL_FLAG_PARSE_ZLIB_HEADER);
    REQUIRE(raw != nullptr);
    const auto* rows = static_cast<const std::uint8_t*>(raw);
    REQUIRE(size == 48 * (1 + 64 * 3));
    bool equal = true;
    for (std::size_t y = 0; y < 48; ++y) {
        equal = equal && rows[y * (1 + 64 * 3)] == 0 &&
                std::memcmp(rows + y * (1 + 64 * 3) + 1,
                            raster.image().rgb.data() + y * 64 * 3,
                            64 * 3) == 0;
    }
    mz_free(raw);
    CHECK(equal);
    CHECK_FALSE(encodePng(Image{}));
}

TEST_CASE("Selection colours are distinct, stable and never the current orange", "[core][raster]") {
    const auto distance = [](const Rgb& a, const Rgb& b) {
        return std::sqrt((a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) +
                         (a.b - b.b) * (a.b - b.b));
    };
    for (std::uint64_t id = 1; id <= 32; ++id) {
        CAPTURE(id);
        const Rgb c = selectionColor(id);
        CHECK(same(c, selectionColor(id)));
        CHECK(distance(c, kCurrentSelectionColor) >= 0.15);
        if (id > 1) CHECK(distance(c, selectionColor(id - 1)) >= 0.15);
        for (std::uint64_t other = 1; other < id; ++other)
            CHECK_FALSE(same(c, selectionColor(other)));
    }

    // A mark n draws in markColors[n - 1]; marks past the list stay orange.
    const TriMesh box = makeBox(40, 20, 10, 2);
    const std::vector<Rgb> colors{selectionColor(5)};
    Raster raster(100, 100, box.bounds(), {0, 0, 1}, {0, 1, 0}, kBackground);
    raster.triangles(box.positions(),
                     box.indices(),
                     kFront,
                     kBack,
                     {},
                     std::vector<std::uint8_t>(box.faceCount(), 1),
                     0.0,
                     colors);
    const Rgb& c = colors[0];
    CHECK(
        same(raster.image().at(50, 50),
             {0.2 * kFront.r + 0.8 * c.r, 0.2 * kFront.g + 0.8 * c.g, 0.2 * kFront.b + 0.8 * c.b}));
    Raster beyond(100, 100, box.bounds(), {0, 0, 1}, {0, 1, 0}, kBackground);
    beyond.triangles(box.positions(),
                     box.indices(),
                     kFront,
                     kBack,
                     {},
                     std::vector<std::uint8_t>(box.faceCount(), 2),
                     0.0,
                     colors);
    CHECK(same(beyond.image().at(50, 50), {0.96, 0.52, 0.16}));
}

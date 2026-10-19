// SPDX-License-Identifier: GPL-3.0-or-later
// Segmentation into planes, cylinders, spheres and organic regions.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "m2c/adapters/cgal/RegionGrowing.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

TEST_CASE("Region growing keeps to the free faces", "[stages][segment]") {
    const TriMesh box = addNoise(makeBox(20.0, 30.0, 10.0, 8), 0.02);
    std::vector<char> free(box.faceCount(), 1);
    const auto all = cgal::growRegions(box, free, {});
    REQUIRE(all);
    CHECK(all->size() >= 6);
    std::vector<int> seen(box.faceCount(), 0);
    for (const cgal::GrownRegion& r : *all) {
        CHECK(r.primitive.kind == PrimitiveKind::Plane);
        for (const std::uint32_t f : r.faces) ++seen[f];
    }
    CHECK(std::ranges::all_of(seen, [](int n) { return n <= 1; }));
    // The first 128 faces (the -z side) taken: no region uses them.
    std::fill_n(free.begin(), 128, 0);
    const auto rest = cgal::growRegions(box, free, {});
    REQUIRE(rest);
    for (const cgal::GrownRegion& r : *rest) {
        CHECK(std::ranges::all_of(r.faces, [](std::uint32_t f) { return f >= 128; }));
    }
    free.pop_back();
    CHECK(cgal::growRegions(box, free, {}).error().code == ErrorCode::InvalidArgument);
}

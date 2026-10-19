// SPDX-License-Identifier: GPL-3.0-or-later
// Segmentation into planes, cylinders, spheres and organic regions.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/adapters/cgal/RegionGrowing.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/pipeline/Segment.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

std::size_t countKind(const SegmentResult& r, const PrimitiveKind kind) {
    return static_cast<std::size_t>(std::ranges::count_if(
        r.regions, [kind](const SegmentRegion& s) { return s.primitive.kind == kind; }));
}

std::size_t labelledFaces(const SegmentResult& r) {
    std::size_t n = r.organicFaces.size();
    for (const SegmentRegion& s : r.regions) n += s.faces.size();
    return n;
}

} // namespace

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

TEST_CASE("A noisy box segments into its six faces", "[stages][segment]") {
    const TriMesh box = addNoise(makeBox(20.0, 30.0, 10.0, 8), 0.02);
    const auto result = segmentMesh(box, SegmentParams{});
    REQUIRE(result);
    CHECK(result->regions.size() == 6);
    CHECK(countKind(*result, PrimitiveKind::Plane) == 6);
    CHECK(result->organicFaces.empty());
    CHECK(labelledFaces(*result) == box.faceCount());
    double area = 0.0;
    for (const SegmentRegion& r : result->regions) {
        area += r.areaMm2;
        CHECK(r.rmsMm < 0.03);
        // Each normal is an axis, pointing out of the box.
        const Vec3d n = r.primitive.direction;
        const double largest = std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)});
        CHECK(largest > std::cos(0.2 * std::numbers::pi / 180.0));
        CHECK(dot(r.primitive.point - Vec3d{10.0, 15.0, 5.0}, n) > 4.9);
    }
    CHECK_THAT(area, WithinAbs(2.0 * (20 * 30 + 20 * 10 + 30 * 10), 5.0));
}

TEST_CASE("A cylinder segments into two planes and its side", "[stages][segment]") {
    const TriMesh cylinder = addNoise(makeCylinder(10.0, 30.0, 96, 30), 0.02);
    const auto result = segmentMesh(cylinder, SegmentParams{});
    REQUIRE(result);
    REQUIRE(countKind(*result, PrimitiveKind::Plane) == 2);
    REQUIRE(countKind(*result, PrimitiveKind::Cylinder) == 1);
    CHECK(result->regions.size() == 3);
    CHECK(result->organicFaces.empty());
    const SegmentRegion& region = result->regions[2];
    const Primitive& side = region.primitive;
    CHECK(side.kind == PrimitiveKind::Cylinder);
    CHECK_THAT(side.radiusMm, WithinAbs(10.0, 0.05));
    CHECK(std::abs(side.direction.z) > std::cos(0.2 * std::numbers::pi / 180.0));
    CHECK_THAT(std::hypot(side.point.x, side.point.y), WithinAbs(0.0, 0.05));
    CHECK(region.rmsMm < 0.03);
}

TEST_CASE("A sphere is found when spheres are asked for", "[stages][segment]") {
    const TriMesh ball = addNoise(makeSphere(15.0, 4, {1.0, 2.0, 3.0}), 0.02);
    SegmentParams p;
    p.spheres = true;
    const auto result = segmentMesh(ball, p);
    REQUIRE(result);
    REQUIRE(result->regions.size() == 1);
    const Primitive& s = result->regions[0].primitive;
    CHECK(s.kind == PrimitiveKind::Sphere);
    CHECK_THAT(s.radiusMm, WithinAbs(15.0, 0.05));
    CHECK(length(s.point - Vec3d{1.0, 2.0, 3.0}) < 0.05);
    CHECK(result->organicFaces.empty());

    // Without spheres no sphere is found. (Bands of the ball fit cylinders within 0.1 mm.)
    const auto without = segmentMesh(ball, SegmentParams{});
    REQUIRE(without);
    CHECK(countKind(*without, PrimitiveKind::Sphere) == 0);
    CHECK(labelledFaces(*without) == ball.faceCount());
    for (const SegmentRegion& r : without->regions) CHECK(r.rmsMm <= 0.1);
}

TEST_CASE("Segmentation rejects bad parameters", "[stages][segment]") {
    const TriMesh box = makeBox(1.0, 1.0, 1.0, 2);
    SegmentParams p;
    p.maxDistanceMm = 0.0;
    CHECK(segmentMesh(box, p).error().code == ErrorCode::InvalidArgument);
    p = {};
    p.maxAngleDeg = 95.0;
    CHECK(segmentMesh(box, p).error().code == ErrorCode::InvalidArgument);
}

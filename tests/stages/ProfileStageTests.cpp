// SPDX-License-Identifier: GPL-3.0-or-later
// Sketch profiles extruded and revolved into solids.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/pipeline/Profile.h"

#include "TestMeshes.h"

using namespace m2c;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using m2c::test::TempDir;

namespace {

constexpr double kPi = std::numbers::pi;

SketchCurve line(Vec2d a, Vec2d b) {
    SketchCurve c;
    c.points = {a, b};
    return c;
}

/// The rectangle [x0, x1] x [y0, y1], anticlockwise.
SketchLoop rectangle(double x0, double y0, double x1, double y1) {
    return {{line({x0, y0}, {x1, y0}),
             line({x1, y0}, {x1, y1}),
             line({x1, y1}, {x0, y1}),
             line({x0, y1}, {x0, y0})}};
}

SketchLoop circle(Vec2d centre, double r, double sweep = 2.0 * kPi) {
    SketchCurve c;
    c.kind = CurveKind::Arc;
    c.centre = centre;
    c.radiusMm = r;
    c.sweepRad = sweep;
    return {{c}};
}

void requireValid(const Solid& solid) {
    REQUIRE(solid.summary().valid);
    REQUIRE(solid.summary().closed);
    REQUIRE(solid.summary().solidCount == 1);
}

} // namespace

TEST_CASE("Extruding a rectangle gives the exact box", "[stages][profile]") {
    Sketch sketch;
    sketch.loops.push_back(rectangle(0, 0, 30, 20));
    const auto box = extrudeSketch(sketch, 10.0, 0.0);
    REQUIRE(box.has_value());
    requireValid(*box);
    CHECK(box->summary().faceCount == 6);
    CHECK_THAT(box->summary().volumeMm3, WithinRel(6000.0, 1e-9));
    CHECK_THAT(box->summary().bounds.max.z, WithinAbs(10.0, 1e-7));

    // Symmetric about the plane, and on a tilted plane: the same volume.
    const auto both = extrudeSketch(sketch, 5.0, 5.0);
    REQUIRE(both.has_value());
    CHECK_THAT(both->summary().bounds.min.z, WithinAbs(-5.0, 1e-7));
    CHECK_THAT(both->summary().volumeMm3, WithinRel(6000.0, 1e-9));
    sketch.plane = planeThrough({1, 2, 3}, {1, -2, 0.5});
    const auto tilted = extrudeSketch(sketch, 4.0, 6.0);
    REQUIRE(tilted.has_value());
    requireValid(*tilted);
    CHECK_THAT(tilted->summary().volumeMm3, WithinRel(6000.0, 1e-9));
}

TEST_CASE("A loop inside the profile is a hole", "[stages][profile]") {
    Sketch sketch;
    sketch.loops.push_back(circle({15, 10}, 4.0)); // given first: the larger loop is the outer one
    sketch.loops.push_back(rectangle(0, 0, 30, 20));
    const auto plate = extrudeSketch(sketch, 10.0, 0.0);
    REQUIRE(plate.has_value());
    requireValid(*plate);
    CHECK(plate->summary().faceCount == 7);
    CHECK_THAT(plate->summary().volumeMm3, WithinRel(6000.0 - kPi * 16.0 * 10.0, 1e-9));

    // A rounded slot: two lines and two half circles, the second arc clockwise in its loop.
    SketchCurve right;
    right.kind = CurveKind::Arc;
    right.centre = {20, 10};
    right.radiusMm = 3;
    right.startRad = -kPi / 2;
    right.sweepRad = kPi;
    SketchCurve left = right;
    left.centre = {10, 10};
    left.startRad = kPi / 2;
    Sketch slot;
    slot.loops.push_back({{line({10, 7}, {20, 7}), right, line({20, 13}, {10, 13}), left}});
    const auto bar = extrudeSketch(slot, 2.0, 0.0);
    REQUIRE(bar.has_value());
    requireValid(*bar);
    CHECK_THAT(bar->summary().volumeMm3, WithinRel((10.0 * 6.0 + kPi * 9.0) * 2.0, 1e-9));
}

TEST_CASE("A spline loop extrudes through its points", "[stages][profile]") {
    // An ellipse 20 x 8 as one closed spline through 64 points.
    SketchCurve spline;
    spline.kind = CurveKind::Spline;
    for (int k = 0; k <= 64; ++k) {
        const double a = 2.0 * kPi * (k % 64) / 64;
        spline.points.push_back({10.0 * std::cos(a), 4.0 * std::sin(a)});
    }
    Sketch sketch;
    sketch.loops.push_back({{spline}});
    const auto prism = extrudeSketch(sketch, 3.0, 0.0);
    REQUIRE(prism.has_value());
    requireValid(*prism);
    CHECK_THAT(prism->summary().volumeMm3, WithinRel(kPi * 10.0 * 4.0 * 3.0, 1e-4));
}

TEST_CASE("Revolving a rectangle off its axis gives the exact tube", "[stages][profile]") {
    Sketch sketch;
    sketch.loops.push_back(rectangle(5, 0, 8, 10));
    const auto tube = revolveSketch(sketch, {0, 0}, {0, 1}, 2.0 * kPi);
    REQUIRE(tube.has_value());
    requireValid(*tube);
    CHECK_THAT(tube->summary().volumeMm3, WithinRel(kPi * (64.0 - 25.0) * 10.0, 1e-9));

    const auto half = revolveSketch(sketch, {0, 3}, {0, -2}, kPi);
    REQUIRE(half.has_value());
    requireValid(*half);
    CHECK_THAT(half->summary().volumeMm3, WithinRel(kPi * (64.0 - 25.0) * 5.0, 1e-9));
}

TEST_CASE("Profiles that are open, outside, or across the axis fail", "[stages][profile]") {
    Sketch open;
    open.loops.push_back(
        {{line({0, 0}, {10, 0}), line({10, 0}, {10, 10}), line({10, 10}, {0, 9})}});
    const auto gap = extrudeSketch(open, 5.0, 0.0);
    REQUIRE_FALSE(gap.has_value());
    CHECK(gap.error().code == ErrorCode::InvalidArgument);

    Sketch apart;
    apart.loops.push_back(rectangle(0, 0, 10, 10));
    apart.loops.push_back(circle({20, 5}, 2.0));
    CHECK_FALSE(extrudeSketch(apart, 5.0, 0.0).has_value());

    Sketch islands;
    islands.loops.push_back(rectangle(0, 0, 30, 30));
    islands.loops.push_back(rectangle(5, 5, 25, 25));
    islands.loops.push_back(circle({15, 15}, 3.0));
    CHECK_FALSE(extrudeSketch(islands, 5.0, 0.0).has_value());

    CHECK_FALSE(extrudeSketch(Sketch{}, 5.0, 0.0).has_value());
    Sketch square;
    square.loops.push_back(rectangle(-2, 0, 2, 4));
    CHECK_FALSE(extrudeSketch(square, 0.0, 0.0).has_value());
    CHECK_FALSE(extrudeSketch(square, 3.0, -3.0).has_value());

    const auto across = revolveSketch(square, {0, 0}, {0, 1}, 2.0 * kPi);
    REQUIRE_FALSE(across.has_value());
    CHECK(across.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(revolveSketch(square, {5, 0}, {0, 0}, kPi).has_value());
    CHECK_FALSE(revolveSketch(square, {5, 0}, {0, 1}, 0.0).has_value());
    CHECK_FALSE(revolveSketch(square, {5, 0}, {0, 1}, 7.0).has_value());
}

TEST_CASE("A solid, a sketch and a plane go to one STEP file as named products",
          "[stages][profile][step]") {
    Sketch sketch;
    sketch.loops.push_back(rectangle(0, 0, 30, 20));
    sketch.loops.push_back(circle({15, 10}, 4.0));
    const auto box = extrudeSketch(sketch, 10.0, 0.0);
    REQUIRE(box.has_value());
    Sketch section = sketch;
    section.plane = planeThrough({0, 0, 5}, {0, 0, 1});
    const FittedPrimitive top{
        .surface = {.kind = PrimitiveKind::Plane, .point = {0, 0, 10}, .direction = {0, 0, 1}},
        .extent = {.min = {0, 0, 9.9}, .max = {30, 20, 10.1}}};
    const std::vector<occt::StepPart> parts{{.name = "Plate", .shape = &*box},
                                            {.name = "Mid section", .shape = &section},
                                            {.name = "Top plane", .shape = &top}};
    const TempDir dir;
    const auto file = dir.path() / "plate.step";
    const auto written = occt::writeStep(parts, file);
    REQUIRE(written.has_value());
    CHECK(written->looseEdges == 5); // four lines and a full circle
    CHECK(written->looseFaces == 1);

    const auto read = occt::readStep(file);
    REQUIRE(read.has_value());
    CHECK(read->solids.solidCount == 1);
    CHECK(read->solids.closed);
    CHECK(read->solids.valid);
    CHECK_THAT(read->solids.volumeMm3, WithinRel(box->summary().volumeMm3, 1e-9));
    CHECK(read->looseEdges == 5);
    CHECK(read->looseFaces == 1);

    std::ifstream in(file);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find("PRODUCT('Plate'") != std::string::npos);
    CHECK(text.find("PRODUCT('Mid section'") != std::string::npos);
    CHECK(text.find("PRODUCT('Top plane'") != std::string::npos);
    // One product each: a sketch is not split into a product per loop.
    std::size_t products = 0;
    for (auto at = text.find("= PRODUCT("); at != std::string::npos;
         at = text.find("= PRODUCT(", at + 1)) {
        ++products;
    }
    CHECK(products == 3);
}

TEST_CASE("STEP export rejects nothing, a primitive that is no plane and an open sketch",
          "[stages][profile][step]") {
    const TempDir dir;
    const auto file = dir.path() / "bad.step";
    CHECK(occt::writeStep(std::vector<occt::StepPart>{}, file).error().code ==
          ErrorCode::InvalidArgument);
    const FittedPrimitive rod{.surface = {.kind = PrimitiveKind::Cylinder,
                                          .point = {0, 0, 0},
                                          .direction = {0, 0, 1},
                                          .radiusMm = 2.0},
                              .extent = {.min = {-2, -2, 0}, .max = {2, 2, 10}}};
    const std::vector<occt::StepPart> cylinder{{.name = "Rod", .shape = &rod}};
    CHECK(occt::writeStep(cylinder, file).error().code == ErrorCode::InvalidArgument);
    Sketch open;
    open.loops.push_back({{line({0, 0}, {10, 0}), line({10, 0}, {10, 10})}});
    const std::vector<occt::StepPart> sketch{{.name = "Open", .shape = &open}};
    CHECK(occt::writeStep(sketch, file).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(file));
}

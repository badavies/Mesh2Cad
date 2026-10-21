// SPDX-License-Identifier: GPL-3.0-or-later
// Sketch profiles extruded and revolved into solids, and written to STEP with solids.

#include <cmath>
#include <cstddef>
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

TEST_CASE("Solids and sketches go to one STEP file as named bodies", "[stages][profile]") {
    // A sketch on the plane z = 4: a square with a square hole holding a round
    // island, and a separate small square. The island and the separate square are faces of their
    // own, so three faces in all, of exact lines and circles.
    Sketch sketch;
    sketch.plane = planeThrough({0, 0, 4}, {0, 0, 1});
    sketch.loops.push_back(rectangle(0, 0, 30, 30));
    sketch.loops.push_back(rectangle(5, 5, 25, 25));
    sketch.loops.push_back(circle({15, 15}, 3.0));
    sketch.loops.push_back(rectangle(40, 0, 50, 10));
    Sketch block;
    block.loops.push_back(rectangle(0, 0, 30, 20));
    const auto box = extrudeSketch(block, 10.0, 0.0);
    REQUIRE(box.has_value());

    const TempDir dir;
    const auto file = dir.path() / "bodies.step";
    const auto faces = occt::writeStep(
        {{.name = "Block", .shape = *box}, {.name = "Section A", .shape = sketch}}, file);
    REQUIRE(faces.has_value());
    CHECK(*faces == std::vector<std::size_t>{6, 3});

    const auto bodies = occt::readStepBodies(file);
    REQUIRE(bodies.has_value());
    REQUIRE(bodies->size() == 2);
    CHECK((*bodies)[0].solidCount == 1);
    CHECK((*bodies)[0].closed);
    CHECK_THAT((*bodies)[0].volumeMm3, WithinRel(6000.0, 1e-9));
    CHECK((*bodies)[1].solidCount == 0);
    CHECK((*bodies)[1].faceCount == 3);
    CHECK((*bodies)[1].valid);
    CHECK_THAT((*bodies)[1].areaMm2, WithinRel(900.0 - 400.0 + kPi * 9.0 + 100.0, 1e-6));
    CHECK_THAT((*bodies)[1].bounds.min.z, WithinAbs(4.0, 1e-6));
    CHECK_THAT((*bodies)[1].bounds.max.z, WithinAbs(4.0, 1e-6));

    // Each body is a product named after it, which Fusion 360 shows as the component's name.
    std::ifstream in(file);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find("PRODUCT('Block'") != std::string::npos);
    CHECK(text.find("PRODUCT('Section A'") != std::string::npos);

    // Nothing is written for an open loop, or for no bodies.
    Sketch open;
    open.loops.push_back({{line({0, 0}, {10, 0}), line({10, 0}, {10, 10})}});
    const auto bad = dir.path() / "bad.step";
    CHECK(occt::writeStep({{.name = "Open", .shape = open}}, bad).error().code ==
          ErrorCode::InvalidArgument);
    CHECK_FALSE(occt::writeStep(std::vector<occt::StepBody>{}, bad).has_value());
    CHECK_FALSE(std::filesystem::exists(bad));
}

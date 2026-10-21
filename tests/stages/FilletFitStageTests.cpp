// SPDX-License-Identifier: GPL-3.0-or-later
// Fitted fillets: a solid's edge rounded at the radius a scanned fillet has (issue #85, ADR-033).

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"
#include "m2c/pipeline/Boolean.h"
#include "m2c/pipeline/FilletFit.h"

using namespace m2c;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = std::numbers::pi;

Solid box(const Vec3d& corner, const Vec3d& size) {
    const auto solid = boxSolid(OrientedBox{.center = corner + size * 0.5, .sizeMm = size});
    REQUIRE(solid);
    return *solid;
}

/// Points of a quarter arc swept along x from 2 to 28: centre (y, z) + r (cos, sin) of angles
/// from a0 to a1, each moved up to `noise` in every coordinate.
std::vector<Vec3d> sweptArc(double cy, double cz, double r, double a0, double a1, double noise) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> d(-noise, noise);
    std::vector<Vec3d> out;
    for (int i = 0; i <= 26; ++i) {
        for (int j = 0; j <= 12; ++j) {
            const double a = a0 + (a1 - a0) * j / 12.0;
            out.push_back(Vec3d{2.0 + i, cy + r * std::cos(a), cz + r * std::sin(a)} +
                          Vec3d{d(rng), d(rng), d(rng)});
        }
    }
    return out;
}

} // namespace

TEST_CASE("A convex scanned fillet's radius is found and the edge rounded", "[stages][fillet]") {
    // The top back edge of a 30 x 20 x 10 box, rounded at 3 mm in the scan, with noise.
    const Solid block = box({0, 0, 0}, {30, 20, 10});
    const std::vector<Vec3d> band = sweptArc(17.0, 7.0, 3.0, 0.0, kPi / 2.0, 0.02);
    const auto fit = fitFillet(block, {{15, 20, 10}}, band, FilletFitParams{});
    if (!fit) FAIL(fit.error().message);
    CHECK_THAT(fit->radiusMm, WithinAbs(3.0, 0.05));
    CHECK(fit->rmsMm < 0.03);
    CHECK(fit->p95Mm <= fit->maxMm);
    CHECK(fit->builds < 40);
    const SolidSummary s = fit->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.faceCount == 7);
    // A quarter of a 3 x 3 square less a quarter circle comes off along the 30 mm edge.
    CHECK_THAT(s.volumeMm3, WithinAbs(6000.0 - 30.0 * (9.0 - kPi * 9.0 / 4.0), 0.5));

    // A typed radius is used as given and only measured.
    const auto snapped = fitFillet(block, {{15, 20, 10}}, band, FilletFitParams{.radiusMm = 3.0});
    REQUIRE(snapped);
    CHECK(snapped->radiusMm == 3.0);
    CHECK(snapped->builds == 1);
    CHECK_THAT(snapped->solid.summary().volumeMm3,
               WithinAbs(6000.0 - 30.0 * (9.0 - kPi * 9.0 / 4.0), 1e-3));
}

TEST_CASE("A concave scanned fillet adds material in the corner", "[stages][fillet]") {
    // A step: the slab z < 5 everywhere and a block y > 10 on it; the inside edge at y = 10,
    // z = 5 is rounded at 2 mm in the scan.
    const auto step = booleanSolids(
        box({0, 0, 0}, {30, 20, 10}), box({-1, -1, 5}, {32, 11, 6}), BooleanOp::Cut, 1e-3);
    REQUIRE(step);
    const std::vector<Vec3d> band = sweptArc(8.0, 7.0, 2.0, -kPi / 2.0, 0.0, 0.02);
    const auto fit = fitFillet(*step, {{15, 10, 5}}, band, FilletFitParams{});
    if (!fit) FAIL(fit.error().message);
    CHECK_THAT(fit->radiusMm, WithinAbs(2.0, 0.05));
    CHECK(fit->solid.summary().valid);
    CHECK(fit->solid.summary().volumeMm3 > step->summary().volumeMm3);
}

TEST_CASE("A fillet between a plane and a cylinder is fitted too", "[stages][fillet]") {
    // A post of radius 4 standing on the box's top, its foot rounded at 1.5 mm.
    FittedPrimitive post{.surface = {.kind = PrimitiveKind::Cylinder,
                                     .point = {15, 10, 0},
                                     .direction = {0, 0, 1},
                                     .radiusMm = 4.0},
                         .extent = {.min = {11, 6, 0}, .max = {19, 14, 20}}};
    const auto cylinder = primitiveSolid(post, 20.0);
    REQUIRE(cylinder);
    const auto body = booleanSolids(box({0, 0, 0}, {30, 20, 10}), *cylinder, BooleanOp::Fuse, 1e-3);
    REQUIRE(body);
    const double r = 1.5;
    std::vector<Vec3d> band;
    for (int i = 0; i < 48; ++i) {
        const double around = 2.0 * kPi * i / 48.0;
        for (int j = 0; j <= 8; ++j) {
            const double a = kPi + kPi / 2.0 * j / 8.0;
            const double radial = 4.0 + r + r * std::cos(a);
            band.push_back({15 + radial * std::cos(around),
                            10 + radial * std::sin(around),
                            10 + r + r * std::sin(a)});
        }
    }
    const auto fit = fitFillet(*body, {{19, 10, 10}}, band, FilletFitParams{});
    if (!fit) FAIL(fit.error().message);
    CHECK_THAT(fit->radiusMm, WithinAbs(1.5, 0.05));
    CHECK(fit->solid.summary().valid);
}

TEST_CASE("A fillet fit fails on bad input, a radius too large or a poor fit", "[stages][fillet]") {
    const Solid block = box({0, 0, 0}, {30, 20, 10});
    const std::vector<Vec3d> band = sweptArc(17.0, 7.0, 3.0, 0.0, kPi / 2.0, 0.02);
    const std::vector<Vec3d> edge{{15, 20, 10}};
    CHECK(fitFillet(block, edge, {}, FilletFitParams{}).error().code == ErrorCode::InvalidArgument);
    CHECK(fitFillet(block, {}, band, FilletFitParams{}).error().code == ErrorCode::InvalidArgument);
    CHECK(fitFillet(block, edge, band, FilletFitParams{.radiusMm = -1.0}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(fitFillet(block, edge, band, FilletFitParams{.radiusMm = 12.0}).error().code ==
          ErrorCode::InvalidState);
    // A fillet of 3 mm measured against a typed 1 mm is far off.
    CHECK(fitFillet(block, edge, band, FilletFitParams{.radiusMm = 1.0}).error().code ==
          ErrorCode::InvalidState);
    CHECK(filletEdges(block, edge, 0.0).error().code == ErrorCode::InvalidArgument);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Least-squares fits of planes, cylinders and spheres.

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Primitive.h"
#include "m2c/pipeline/PrimitiveFit.h"

using namespace m2c;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// Points on a quarter turn of a cylinder of radius 8 around a tilted axis through (1, 2, 3),
/// 20 mm long, each moved up to 0.02 mm along the radius.
std::vector<Vec3d> cylinderPatch(const Vec3d& axis, double turn = std::numbers::pi / 2) {
    const Vec3d u = Vec3d{1, 0, 0} - axis * axis.x;
    const Vec3d un = u * (1.0 / length(u));
    const Vec3d w = cross(axis, un);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> noise(-0.02, 0.02);
    std::vector<Vec3d> out;
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; j <= 20; ++j) {
            const double a = turn * i / 20;
            const Vec3d radial = un * std::cos(a) + w * std::sin(a);
            out.push_back(Vec3d{1, 2, 3} + axis * (j - 10.0) + radial * (8.0 + noise(rng)));
        }
    }
    return out;
}

} // namespace

TEST_CASE("A cylinder fit recovers the axis and radius from a rough start", "[stages][fit]") {
    const Vec3d axis = Vec3d{0.2, 0.3, 1.0} * (1.0 / length(Vec3d{0.2, 0.3, 1.0}));
    const std::vector<Vec3d> points = cylinderPatch(axis);
    // A start like region growing gives: axis 3 degrees off, centre 0.5 mm off, radius 5% off.
    const Primitive start{.kind = PrimitiveKind::Cylinder,
                          .point = Vec3d{1.5, 2.0, 3.0},
                          .direction = axis + Vec3d{0.05, 0.0, 0.0},
                          .radiusMm = 8.4};
    const PrimitiveFit fit = fitPrimitive(start, points);
    CHECK_THAT(fit.primitive.radiusMm, WithinAbs(8.0, 0.01));
    CHECK(std::abs(dot(fit.primitive.direction, axis)) > std::cos(0.1 * kDeg));
    const Vec3d off = fit.primitive.point - Vec3d{1, 2, 3};
    CHECK(length(off - axis * dot(off, axis)) < 0.01);
    CHECK(fit.rmsMm < 0.015);
    CHECK(fit.maxMm <= 0.025);
    CHECK_THAT(turnAngle(fit.primitive, points), WithinAbs(std::numbers::pi / 2, 0.01));
}

TEST_CASE("A sphere fit recovers the centre and radius", "[stages][fit]") {
    std::vector<Vec3d> points;
    for (int i = 1; i < 10; ++i) {
        for (int j = 0; j < 20; ++j) {
            const double polar = 0.6 * i / 10; // a cap of about 70 degrees
            const double a = 2.0 * std::numbers::pi * j / 20;
            points.push_back(Vec3d{-4, 0, 7} + Vec3d{std::sin(polar) * std::cos(a),
                                                     std::sin(polar) * std::sin(a),
                                                     std::cos(polar)} *
                                                   12.0);
        }
    }
    const Primitive start{
        .kind = PrimitiveKind::Sphere, .point = Vec3d{-3.5, 0.3, 6.5}, .radiusMm = 11.0};
    const PrimitiveFit fit = fitPrimitive(start, points);
    CHECK_THAT(fit.primitive.radiusMm, WithinAbs(12.0, 1e-6));
    CHECK(length(fit.primitive.point - Vec3d{-4, 0, 7}) < 1e-6);
    CHECK(fit.rmsMm < 1e-6);
    CHECK_THAT(turnAngle(fit.primitive, points), WithinAbs(2 * 0.54, 0.01));
}

TEST_CASE("A plane fit takes the principal axes and the start's side", "[stages][fit]") {
    std::vector<Vec3d> points;
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) points.push_back({1.0 * i, 2.0 * j, 5.0 + 0.1 * i});
    }
    const Primitive start{.kind = PrimitiveKind::Plane, .direction = Vec3d{0, 0, -1}};
    const PrimitiveFit fit = fitPrimitive(start, points);
    const Vec3d n = Vec3d{-0.1, 0, 1} * (-1.0 / length(Vec3d{-0.1, 0, 1}));
    CHECK(dot(fit.primitive.direction, n) > std::cos(1e-6));
    CHECK(fit.rmsMm < 1e-9);
    CHECK(turnAngle(fit.primitive, points) == 0.0);
}

TEST_CASE("Distances and normals of primitives", "[stages][fit]") {
    const Primitive cylinder{.kind = PrimitiveKind::Cylinder,
                             .point = Vec3d{0, 0, 0},
                             .direction = Vec3d{0, 0, 1},
                             .radiusMm = 2.0};
    CHECK_THAT(distanceTo(cylinder, {3, 0, 7}), WithinAbs(1.0, 1e-12));
    CHECK_THAT(distanceTo(cylinder, {0, 0.5, -3}), WithinAbs(1.5, 1e-12));
    CHECK(normalAt(cylinder, {0, 3, 5}) == Vec3d{0, 1, 0});
    CHECK(normalAt(cylinder, {0, 0, 5}) == Vec3d{});
    const Primitive plane{
        .kind = PrimitiveKind::Plane, .point = Vec3d{0, 0, 1}, .direction = Vec3d{0, 0, 1}};
    CHECK_THAT(distanceTo(plane, {4, 5, -1}), WithinAbs(2.0, 1e-12));
    CHECK(normalAt(plane, {4, 5, -1}) == Vec3d{0, 0, 1});
}

TEST_CASE("A fit with the direction held refits the rest of a cylinder", "[stages][fit]") {
    const Vec3d axis = Vec3d{0.2, 0.3, 1.0} * (1.0 / length(Vec3d{0.2, 0.3, 1.0}));
    const std::vector<Vec3d> points = cylinderPatch(axis);
    const Primitive start{.kind = PrimitiveKind::Cylinder,
                          .point = Vec3d{1.5, 2.0, 3.0},
                          .direction = axis,
                          .radiusMm = 8.4};
    const Result<PrimitiveFit> fit = fitWithDirection(start, points);
    REQUIRE(fit);
    CHECK(fit->primitive.direction == axis);
    CHECK_THAT(fit->primitive.radiusMm, WithinAbs(8.0, 0.01));
    const Vec3d off = fit->primitive.point - Vec3d{1, 2, 3};
    CHECK(length(off - axis * dot(off, axis)) < 0.01);
    CHECK(fit->rmsMm < 0.015);
}

TEST_CASE("A plane fit with the normal held goes through the centroid", "[stages][fit]") {
    std::vector<Vec3d> points;
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) points.push_back({1.0 * i, 2.0 * j, 5.0 + 0.02 * i});
    }
    const Primitive start{.kind = PrimitiveKind::Plane, .direction = Vec3d{0, 0, 1}};
    const Result<PrimitiveFit> fit = fitWithDirection(start, points);
    REQUIRE(fit);
    CHECK(fit->primitive.direction == Vec3d{0, 0, 1});
    CHECK_THAT(fit->primitive.point.z, WithinAbs(5.09, 1e-9));
    CHECK_THAT(fit->maxMm, WithinAbs(0.09, 1e-9));

    CHECK_FALSE(fitWithDirection(Primitive{.kind = PrimitiveKind::Sphere, .radiusMm = 1}, points));
    CHECK_FALSE(
        fitWithDirection(Primitive{.kind = PrimitiveKind::Plane, .direction = {0, 0, 2}}, points));
    CHECK_FALSE(fitWithDirection(start, {}));
}

TEST_CASE("Aligned directions are parallel, perpendicular or at an angle", "[stages][fit]") {
    const Vec3d z{0, 0, 1};
    const Vec3d tilted = Vec3d{0.05, 0, -1} * (1.0 / length(Vec3d{0.05, 0, -1}));
    // Parallel keeps the side the direction was on.
    const Result<Vec3d> parallel = alignedDirection(tilted, z, 0.0);
    REQUIRE(parallel);
    CHECK(*parallel == Vec3d{0, 0, -1});
    // Perpendicular turns towards where the direction already leant.
    const Result<Vec3d> square = alignedDirection(Vec3d{0.9, 0.1, 0.3}, z, std::numbers::pi / 2);
    REQUIRE(square);
    CHECK_THAT(dot(*square, z), WithinAbs(0.0, 1e-12));
    CHECK(dot(*square, Vec3d{0.9, 0.1, 0}) > 0.0);
    const Result<Vec3d> thirty = alignedDirection(Vec3d{1, 0, 1}, z, 30 * kDeg);
    REQUIRE(thirty);
    CHECK_THAT(dot(*thirty, z), WithinAbs(std::cos(30 * kDeg), 1e-12));
    CHECK_THAT(length(*thirty), WithinAbs(1.0, 1e-12));
    CHECK_THAT(thirty->y, WithinAbs(0.0, 1e-12));

    CHECK_FALSE(alignedDirection(z, z, 30 * kDeg));
    CHECK_FALSE(alignedDirection(z, Vec3d{}, 0.0));
    CHECK_FALSE(alignedDirection(z, Vec3d{1, 0, 0}, 100 * kDeg));
}

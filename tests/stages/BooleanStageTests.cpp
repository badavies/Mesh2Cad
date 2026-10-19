// SPDX-License-Identifier: GPL-3.0-or-later
// Booleans between solids: fuse, cut and common (issue #50).

#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "m2c/adapters/occt/Boolean.h"
#include "m2c/adapters/occt/Brep.h"
#include "m2c/adapters/occt/Trim.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = std::numbers::pi;

FittedPrimitive plane(const Vec3d& point, const Vec3d& normal) {
    return {.surface = {.kind = PrimitiveKind::Plane, .point = point, .direction = normal}};
}

TriMesh moved(const TriMesh& mesh, const Vec3d& by) {
    std::vector<Vec3d> p = mesh.positions();
    for (Vec3d& v : p) v = v + by;
    return makeMesh(std::move(p), mesh.indices());
}

/// The box from lo to lo + size, trimmed from its six planes.
Solid box(const Vec3d& lo, const Vec3d& size) {
    const Vec3d hi = lo + size;
    const std::vector<FittedPrimitive> planes{plane(lo, {-1, 0, 0}),
                                              plane(hi, {1, 0, 0}),
                                              plane(lo, {0, -1, 0}),
                                              plane(hi, {0, 1, 0}),
                                              plane(lo, {0, 0, -1}),
                                              plane(hi, {0, 0, 1})};
    auto trimmed = occt::trimPrimitives(planes, moved(makeBox(size.x, size.y, size.z, 4), lo), {});
    REQUIRE(trimmed.has_value());
    return trimmed->solid;
}

/// The cylinder of radius r along z from base to base + (0, 0, h), trimmed from its wall and ends.
Solid cylinder(const Vec3d& base, const double r, const double h) {
    const std::vector<FittedPrimitive> primitives{plane(base, {0, 0, -1}),
                                                  plane(base + Vec3d{0, 0, h}, {0, 0, 1}),
                                                  {.surface = {.kind = PrimitiveKind::Cylinder,
                                                               .point = base,
                                                               .direction = {0, 0, 1},
                                                               .radiusMm = r}}};
    auto trimmed = occt::trimPrimitives(primitives, moved(makeCylinder(r, h, 128), base), {});
    REQUIRE(trimmed.has_value());
    return trimmed->solid;
}

void checkSolid(const SolidSummary& s) {
    CHECK(s.solidCount == 1);
    CHECK(s.closed);
    CHECK(s.valid);
    CHECK(s.poleCount == 0);
}

} // namespace

TEST_CASE("Cutting a cylinder from a box leaves an exact cylindrical hole", "[stage][boolean]") {
    const Solid block = box({0, 0, 0}, {30, 20, 10});
    const double height = GENERATE(20.0, 10.0); // through the box, or flush with its faces
    const Solid pin = cylinder({15, 10, height > 10.0 ? -5.0 : 0.0}, 5, height);
    auto cut = occt::booleanSolids(block, pin, occt::BooleanOp::Cut, 1e-3);
    REQUIRE(cut.has_value());
    const SolidSummary& s = cut->summary();
    checkSolid(s);
    CHECK(s.faceCount == 7); // six planes and one cylinder
    CHECK_THAT(s.volumeMm3, WithinRel(6000.0 - kPi * 25.0 * 10.0, 1e-9));
    CHECK(cut->faces().empty());

    // Points on the hole's wall lie on the solid's surface.
    std::vector<Vec3d> wall;
    for (int k = 0; k < 16; ++k) {
        const double a = 2.0 * kPi * k / 16;
        wall.push_back({15 + 5 * std::cos(a), 10 + 5 * std::sin(a), 1.0 + 0.5 * k});
    }
    auto distances = occt::signedDistances(*cut, wall);
    REQUIRE(distances.has_value());
    for (const double d : *distances) CHECK_THAT(d, WithinAbs(0.0, 1e-6));

    // The inputs are unchanged.
    CHECK_THAT(block.summary().volumeMm3, WithinRel(6000.0, 1e-9));
    CHECK(block.summary().faceCount == 6);
}

TEST_CASE("Fusing and intersecting two overlapping boxes give exact volumes", "[stage][boolean]") {
    const Solid a = box({0, 0, 0}, {30, 20, 10});
    const Solid b = box({10, 5, 5}, {30, 20, 10});
    const double overlap = 20.0 * 15.0 * 5.0;

    auto fused = occt::booleanSolids(a, b, occt::BooleanOp::Fuse, 1e-3);
    REQUIRE(fused.has_value());
    checkSolid(fused->summary());
    CHECK_THAT(fused->summary().volumeMm3, WithinRel(12000.0 - overlap, 1e-9));

    auto common = occt::booleanSolids(a, b, occt::BooleanOp::Common, 1e-3);
    REQUIRE(common.has_value());
    checkSolid(common->summary());
    CHECK(common->summary().faceCount == 6);
    CHECK_THAT(common->summary().volumeMm3, WithinRel(overlap, 1e-9));

    auto cut = occt::booleanSolids(a, b, occt::BooleanOp::Cut, 1e-3);
    REQUIRE(cut.has_value());
    checkSolid(cut->summary());
    CHECK_THAT(cut->summary().volumeMm3, WithinRel(6000.0 - overlap, 1e-9));
}

TEST_CASE("Booleans of disjoint solids fail", "[stage][boolean]") {
    const Solid a = box({0, 0, 0}, {30, 20, 10});
    const Solid far = box({100, 0, 0}, {10, 10, 10});

    auto common = occt::booleanSolids(a, far, occt::BooleanOp::Common, 1e-3);
    REQUIRE_FALSE(common.has_value());
    CHECK(common.error().code == ErrorCode::InvalidState);
    CHECK_THAT(common.error().message, ContainsSubstring("nothing is left"));

    auto fused = occt::booleanSolids(a, far, occt::BooleanOp::Fuse, 1e-3);
    REQUIRE_FALSE(fused.has_value());
    CHECK(fused.error().code == ErrorCode::InvalidState);
    CHECK_THAT(fused.error().message, ContainsSubstring("2 separate solids"));

    // Cutting a solid from itself leaves nothing.
    auto all = occt::booleanSolids(a, a, occt::BooleanOp::Cut, 1e-3);
    REQUIRE_FALSE(all.has_value());
    CHECK(all.error().code == ErrorCode::InvalidState);

    auto negative = occt::booleanSolids(a, far, occt::BooleanOp::Fuse, -1.0);
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("A fitted cylinder becomes an exact solid that cuts a true hole", "[stage][boolean]") {
    // An axis given unnormalised, fitted to a region from z = -5 to 15.
    const FittedPrimitive fit{.surface = {.kind = PrimitiveKind::Cylinder,
                                          .point = {15, 10, 3},
                                          .direction = {0, 0, 2},
                                          .radiusMm = 5},
                              .extent = {.min = {10, 5, -5}, .max = {20, 15, 15}}};
    auto pin = occt::primitiveSolid(fit, 0.0);
    REQUIRE(pin.has_value());
    checkSolid(pin->summary());
    CHECK(pin->summary().faceCount == 3);
    CHECK_THAT(pin->summary().volumeMm3, WithinRel(kPi * 25.0 * 20.0, 1e-9));
    CHECK_THAT(pin->summary().bounds.min.z, WithinAbs(-5.0, 1e-3));
    CHECK_THAT(pin->summary().bounds.max.z, WithinAbs(15.0, 1e-3));

    // A length centres the cylinder on the region.
    auto shorter = occt::primitiveSolid(fit, 12.0);
    REQUIRE(shorter.has_value());
    CHECK_THAT(shorter->summary().volumeMm3, WithinRel(kPi * 25.0 * 12.0, 1e-9));
    CHECK_THAT(shorter->summary().bounds.min.z, WithinAbs(-1.0, 1e-3));
    CHECK_THAT(shorter->summary().bounds.max.z, WithinAbs(11.0, 1e-3));

    auto cut = occt::booleanSolids(box({0, 0, 0}, {30, 20, 10}), *pin, occt::BooleanOp::Cut, 1e-3);
    REQUIRE(cut.has_value());
    checkSolid(cut->summary());
    CHECK(cut->summary().faceCount == 7);
    CHECK_THAT(cut->summary().volumeMm3, WithinRel(6000.0 - kPi * 25.0 * 10.0, 1e-9));
}

TEST_CASE("A fitted sphere becomes a whole exact solid", "[stage][boolean]") {
    const FittedPrimitive fit{
        .surface = {.kind = PrimitiveKind::Sphere, .point = {1, 2, 3}, .radiusMm = 2},
        .extent = {.min = {1, 0, 1}, .max = {3, 4, 5}}};
    auto ball = occt::primitiveSolid(fit, 0.0);
    REQUIRE(ball.has_value());
    checkSolid(ball->summary());
    CHECK_THAT(ball->summary().volumeMm3, WithinRel(4.0 / 3.0 * kPi * 8.0, 1e-9));
    CHECK_THAT(ball->summary().bounds.min.x, WithinAbs(-1.0, 1e-3));
}

TEST_CASE("Only a cylinder or a sphere with a size becomes a solid", "[stage][boolean]") {
    const auto failsWith = [](const FittedPrimitive& fit, const double lengthMm) {
        auto made = occt::primitiveSolid(fit, lengthMm);
        REQUIRE_FALSE(made.has_value());
        return made.error();
    };
    CHECK(failsWith(plane({0, 0, 0}, {0, 0, 1}), 0.0).code == ErrorCode::InvalidArgument);
    const FittedPrimitive cone{.surface = {.kind = PrimitiveKind::Cone,
                                           .direction = {0, 0, 1},
                                           .radiusMm = 1,
                                           .halfAngleRad = 0.3}};
    CHECK_THAT(failsWith(cone, 0.0).message, ContainsSubstring("not a cone"));

    FittedPrimitive flat{
        .surface = {.kind = PrimitiveKind::Cylinder, .direction = {0, 0, 1}, .radiusMm = 5},
        .extent = {.min = {-5, -5, 2}, .max = {5, 5, 2}}};
    CHECK_THAT(failsWith(flat, 0.0).message, ContainsSubstring("give a length"));
    CHECK(occt::primitiveSolid(flat, 4.0).has_value());
    CHECK(failsWith(flat, -1.0).code == ErrorCode::InvalidArgument);
    flat.surface.radiusMm = 0.0;
    CHECK(failsWith(flat, 4.0).code == ErrorCode::InvalidArgument);
}

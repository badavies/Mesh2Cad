// SPDX-License-Identifier: GPL-3.0-or-later
// Triangulating a solid for GLB, glTF, OBJ and STL export at a chord and angle (issue #120).

#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/adapters/occt/Boolean.h"
#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/Primitive.h"

#include "TestMeshes.h"

using namespace m2c;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = std::numbers::pi;

Solid ball(const double radius) {
    const FittedPrimitive fit{
        .surface = {.kind = PrimitiveKind::Sphere, .point = {1, 2, 3}, .radiusMm = radius},
        .extent = {.min = {1 - radius, 2 - radius, 3 - radius},
                   .max = {1 + radius, 2 + radius, 3 + radius}}};
    auto solid = occt::primitiveSolid(fit, 0.0);
    REQUIRE(solid.has_value());
    return *solid;
}

Solid pin() {
    const FittedPrimitive fit{.surface = {.kind = PrimitiveKind::Cylinder,
                                          .point = {0, 0, 0},
                                          .direction = {0, 0, 1},
                                          .radiusMm = 5},
                              .extent = {.min = {-5, -5, 0}, .max = {5, 5, 20}}};
    auto solid = occt::primitiveSolid(fit, 0.0);
    REQUIRE(solid.has_value());
    return *solid;
}

/// Checks the mesh is closed and within the solid's area times the chord of its volume.
MeshStats checkClosed(const TriMesh& mesh, const Solid& solid, const double chordMm) {
    const MeshStats stats = computeStats(mesh);
    CHECK(stats.isWatertight());
    CHECK(stats.degenerateFaceCount == 0);
    CHECK(stats.signedVolume > 0.0); // facing outward
    CHECK(std::abs(stats.signedVolume - solid.summary().volumeMm3) <=
          solid.summary().areaMm2 * chordMm);
    return stats;
}

} // namespace

TEST_CASE("A box triangulates to twelve triangles of its exact volume", "[stage][export]") {
    auto box = occt::boxSolid({.center = {1, 2, 3}, .sizeMm = {30, 20, 10}});
    REQUIRE(box.has_value());
    auto mesh = occt::exportTriangulation(*box, {.chordMm = 0.01, .angleDeg = 10});
    REQUIRE(mesh.has_value());
    const MeshStats stats = checkClosed(*mesh, *box, 0.01);
    CHECK(stats.faceCount == 12);
    CHECK(stats.vertexCount == 8);
    CHECK_THAT(stats.signedVolume, WithinRel(6000.0, 1e-12));
}

TEST_CASE("A sphere and a cylinder triangulate closed, finer for a smaller chord",
          "[stage][export]") {
    const Solid sphere = ball(10.0);
    const std::size_t displayed = sphere.display().vertexCount();
    // Fine first, then coarse: the coarse one must not reuse the fine triangulation.
    auto fine = occt::exportTriangulation(sphere, {.chordMm = 0.005, .angleDeg = 10});
    auto coarse = occt::exportTriangulation(sphere, {.chordMm = 0.1, .angleDeg = 30});
    REQUIRE(fine.has_value());
    REQUIRE(coarse.has_value());
    const MeshStats f = checkClosed(*fine, sphere, 0.005);
    const MeshStats c = checkClosed(*coarse, sphere, 0.1);
    CHECK(f.faceCount > 4 * c.faceCount);
    const double exact = 4.0 / 3.0 * kPi * 1000.0;
    CHECK(std::abs(f.signedVolume - exact) < std::abs(c.signedVolume - exact));
    CHECK(sphere.display().vertexCount() == displayed); // the solid's own display is untouched

    // Every vertex is on the sphere, within the chord or closer.
    for (const Vec3d& p : fine->positions()) {
        CHECK(std::abs(length(p - Vec3d{1, 2, 3}) - 10.0) < 1e-6);
    }

    const Solid cylinder = pin();
    auto pinMesh = occt::exportTriangulation(cylinder, {.chordMm = 0.01, .angleDeg = 10});
    REQUIRE(pinMesh.has_value());
    checkClosed(*pinMesh, cylinder, 0.01);
    // The same settings give the same mesh.
    auto again = occt::exportTriangulation(cylinder, {.chordMm = 0.01, .angleDeg = 10});
    REQUIRE(again.has_value());
    CHECK(again->positions() == pinMesh->positions());
    CHECK(again->indices() == pinMesh->indices());
}

TEST_CASE("Export triangulation refuses bad settings", "[stage][export]") {
    const Solid sphere = ball(5.0);
    for (const occt::ExportTessellation bad : {occt::ExportTessellation{.chordMm = 0.0},
                                               occt::ExportTessellation{.chordMm = -1.0},
                                               occt::ExportTessellation{.angleDeg = 0.0},
                                               occt::ExportTessellation{.angleDeg = 91.0}}) {
        const auto mesh = occt::exportTriangulation(sphere, bad);
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::InvalidArgument);
    }
}

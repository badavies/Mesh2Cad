// SPDX-License-Identifier: GPL-3.0-or-later
// Intersecting and trimming fitted primitives into a solid with sharp edges (issue #48).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/adapters/occt/Trim.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"

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

/// The six planes of the box [0,sx]x[0,sy]x[0,sz].
std::vector<FittedPrimitive> boxPlanes(const double sx, const double sy, const double sz) {
    return {plane({0, 0, 0}, {-1, 0, 0}),
            plane({sx, 0, 0}, {1, 0, 0}),
            plane({0, 0, 0}, {0, -1, 0}),
            plane({0, sy, 0}, {0, 1, 0}),
            plane({0, 0, 0}, {0, 0, -1}),
            plane({0, 0, sz}, {0, 0, 1})};
}

/// A scan of the box [0,sx]x[0,sy]x[0,sz] with a hole of radius r through it along z at (cx, cy):
/// the four sides, the top and bottom as rings from the hole to the box's edge, and the hole's
/// wall. Not welded; only its distances matter.
TriMesh boxWithHole(const double sx,
                    const double sy,
                    const double sz,
                    const double cx,
                    const double cy,
                    const double r) {
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    const auto quad = [&](const Vec3d& a, const Vec3d& b, const Vec3d& c, const Vec3d& d) {
        const auto base = static_cast<std::uint32_t>(p.size());
        p.insert(p.end(), {a, b, c, d});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad({0, 0, 0}, {sx, 0, 0}, {sx, 0, sz}, {0, 0, sz});
    quad({sx, 0, 0}, {sx, sy, 0}, {sx, sy, sz}, {sx, 0, sz});
    quad({sx, sy, 0}, {0, sy, 0}, {0, sy, sz}, {sx, sy, sz});
    quad({0, sy, 0}, {0, 0, 0}, {0, 0, sz}, {0, sy, sz});
    // Rays from the hole's centre, with one through each corner so the rings reach them.
    std::vector<double> angles;
    for (int k = 0; k < 256; ++k) angles.push_back(2.0 * kPi * k / 256);
    for (const auto& [x, y] : {std::pair{sx, sy}, {0.0, sy}, {0.0, 0.0}, {sx, 0.0}}) {
        angles.push_back(std::fmod(std::atan2(y - cy, x - cx) + 2.0 * kPi, 2.0 * kPi));
    }
    std::ranges::sort(angles);
    const auto edge = [&](const double a) { // where the ray at angle a leaves the box
        const double dx = std::cos(a), dy = std::sin(a);
        double t = 1e300;
        if (dx > 1e-12) t = std::min(t, (sx - cx) / dx);
        if (dx < -1e-12) t = std::min(t, -cx / dx);
        if (dy > 1e-12) t = std::min(t, (sy - cy) / dy);
        if (dy < -1e-12) t = std::min(t, -cy / dy);
        return std::pair{cx + t * dx, cy + t * dy};
    };
    for (std::size_t k = 0; k < angles.size(); ++k) {
        const double a = angles[k], b = angles[(k + 1) % angles.size()];
        const auto [ax, ay] = edge(a);
        const auto [bx, by] = edge(b);
        const Vec3d ha{cx + r * std::cos(a), cy + r * std::sin(a), 0};
        const Vec3d hb{cx + r * std::cos(b), cy + r * std::sin(b), 0};
        for (const double z : {0.0, sz}) {
            quad({ha.x, ha.y, z}, {ax, ay, z}, {bx, by, z}, {hb.x, hb.y, z});
        }
        quad(ha, hb, {hb.x, hb.y, sz}, {ha.x, ha.y, sz});
    }
    return makeMesh(std::move(p), std::move(idx));
}

/// A scan of an L-shaped bracket: the profile (0,0) (40,0) (40,5) (5,5) (5,30) (0,30) in x and z,
/// 20 mm deep along y.
TriMesh bracket() {
    const std::vector<std::pair<double, double>> profile{
        {0, 0}, {40, 0}, {40, 5}, {5, 5}, {5, 30}, {0, 30}};
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    for (const double y : {0.0, 20.0}) {
        const auto base = static_cast<std::uint32_t>(p.size());
        for (const auto& [x, z] : profile) p.push_back({x, y, z});
        for (std::uint32_t k = 1; k + 1 < profile.size(); ++k) {
            idx.insert(idx.end(), {base, base + k, base + k + 1});
        }
    }
    for (std::size_t k = 0; k < profile.size(); ++k) {
        const auto& [x0, z0] = profile[k];
        const auto& [x1, z1] = profile[(k + 1) % profile.size()];
        const auto base = static_cast<std::uint32_t>(p.size());
        p.insert(p.end(), {{x0, 0, z0}, {x1, 0, z1}, {x1, 20, z1}, {x0, 20, z0}});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return makeMesh(std::move(p), std::move(idx));
}

} // namespace

TEST_CASE("Six planes of a box trim into a box with sharp edges", "[stage][trim]") {
    const TriMesh scan = makeBox(30, 20, 10, 4);
    auto trimmed = occt::trimPrimitives(boxPlanes(30, 20, 10), scan, {});
    REQUIRE(trimmed.has_value());
    const SolidSummary& s = trimmed->solid.summary();
    CHECK(s.solidCount == 1);
    CHECK(s.closed);
    CHECK(s.valid);
    CHECK(s.faceCount == 6);
    CHECK(s.edgeCount == 12);
    CHECK(s.vertexCount == 8);
    CHECK(s.poleCount == 0); // exact planes, no B-spline faces
    CHECK_THAT(s.volumeMm3, WithinRel(6000.0, 1e-9));
    CHECK_THAT(s.areaMm2, WithinRel(2200.0, 1e-9));
    CHECK_THAT(s.bounds.max.x - s.bounds.min.x, WithinAbs(30.0, 1e-6));
    CHECK(trimmed->keptFaces == 6);
    CHECK(trimmed->splitFaces > trimmed->keptFaces);
    CHECK(trimmed->solid.faces().empty());
    CHECK(trimmed->solid.display().faceCount() > 0);
    CHECK(trimmed->solid.displayMap().triangleFaces.size() == trimmed->solid.display().faceCount());
}

TEST_CASE("A box with a hole through it trims into seven faces", "[stage][trim]") {
    const TriMesh scan = boxWithHole(30, 20, 10, 15, 10, 4);
    std::vector<FittedPrimitive> primitives = boxPlanes(30, 20, 10);
    primitives.push_back({.surface = {.kind = PrimitiveKind::Cylinder,
                                      .point = {15, 10, 0},
                                      .direction = {0, 0, 1},
                                      .radiusMm = 4.0}});
    auto trimmed = occt::trimPrimitives(primitives, scan, {});
    REQUIRE(trimmed.has_value());
    const SolidSummary& s = trimmed->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.faceCount == 7);
    CHECK(s.poleCount == 0); // the hole is a true cylinder
    CHECK_THAT(s.volumeMm3, WithinRel(6000.0 - kPi * 16.0 * 10.0, 1e-6));
    CHECK_THAT(s.areaMm2, WithinRel(2200.0 - 2.0 * kPi * 16.0 + 2.0 * kPi * 4.0 * 10.0, 1e-6));
}

TEST_CASE("An L-shaped bracket trims into eight planar faces", "[stage][trim]") {
    const std::vector<FittedPrimitive> primitives{plane({0, 0, 0}, {-1, 0, 0}),
                                                  plane({40, 0, 0}, {1, 0, 0}),
                                                  plane({5, 0, 0}, {1, 0, 0}),
                                                  plane({0, 0, 0}, {0, 0, -1}),
                                                  plane({0, 0, 5}, {0, 0, 1}),
                                                  plane({0, 0, 30}, {0, 0, 1}),
                                                  plane({0, 0, 0}, {0, -1, 0}),
                                                  plane({0, 20, 0}, {0, 1, 0})};
    auto trimmed = occt::trimPrimitives(primitives, bracket(), {});
    REQUIRE(trimmed.has_value());
    const SolidSummary& s = trimmed->solid.summary();
    CHECK(s.valid);
    CHECK(s.faceCount == 8);
    CHECK(s.edgeCount == 18);
    CHECK(s.poleCount == 0);
    CHECK_THAT(s.volumeMm3, WithinRel((40.0 * 5.0 + 5.0 * 25.0) * 20.0, 1e-9));
    // Faces crossed by another plane's extension are one face again.
    CHECK(trimmed->keptFaces > s.faceCount);
}

TEST_CASE("A can and a frustum trim into an exact cylinder and cone", "[stage][trim]") {
    const FittedPrimitive bottom = plane({0, 0, 0}, {0, 0, -1});
    const FittedPrimitive top = plane({0, 0, 20}, {0, 0, 1});
    SECTION("can") {
        const std::vector<FittedPrimitive> primitives{bottom,
                                                      top,
                                                      {.surface = {.kind = PrimitiveKind::Cylinder,
                                                                   .point = {0, 0, 0},
                                                                   .direction = {0, 0, 1},
                                                                   .radiusMm = 10.0}}};
        auto trimmed = occt::trimPrimitives(primitives, makeCylinder(10, 20, 128), {});
        REQUIRE(trimmed.has_value());
        CHECK(trimmed->solid.summary().faceCount == 3);
        CHECK(trimmed->solid.summary().poleCount == 0);
        CHECK_THAT(trimmed->solid.summary().volumeMm3, WithinRel(kPi * 100.0 * 20.0, 1e-6));
    }
    SECTION("frustum") {
        // Radius 10 at z = 0 to 5 at z = 20: the apex is at z = 40, the axis pointing down.
        const double tanAngle = 5.0 / 20.0;
        std::vector<Vec3d> p = makeCylinder(10, 20, 128).positions();
        for (Vec3d& v : p) {
            const double scale = (10.0 - v.z * tanAngle) / 10.0;
            v = {v.x * scale, v.y * scale, v.z};
        }
        const TriMesh scan = makeMesh(std::move(p), makeCylinder(10, 20, 128).indices());
        const std::vector<FittedPrimitive> primitives{
            bottom,
            top,
            {.surface = {.kind = PrimitiveKind::Cone,
                         .point = {0, 0, 40},
                         .direction = {0, 0, -1},
                         .halfAngleRad = std::atan(tanAngle)},
             .extent = scan.bounds()}};
        auto trimmed = occt::trimPrimitives(primitives, scan, {});
        REQUIRE(trimmed.has_value());
        CHECK(trimmed->solid.summary().faceCount == 3);
        CHECK(trimmed->solid.summary().poleCount == 0);
        CHECK_THAT(trimmed->solid.summary().volumeMm3,
                   WithinRel(kPi * 20.0 / 3.0 * (100.0 + 50.0 + 25.0), 1e-6));
    }
}

TEST_CASE("A noisy scan with slightly tilted planes still trims into a box", "[stage][trim]") {
    // A fit to a real scan is a little off: planes tilted by 0.1 degrees and moved by 0.02 mm.
    const TriMesh scan = addNoise(makeBox(30, 20, 10, 16), 0.05);
    std::vector<FittedPrimitive> primitives = boxPlanes(30, 20, 10);
    const double tilt = 0.1 * kPi / 180.0;
    for (std::size_t k = 0; k < primitives.size(); ++k) {
        Primitive& s = primitives[k].surface;
        const Vec3d across = std::abs(s.direction.x) > 0.5 ? Vec3d{0, 1, 0} : Vec3d{1, 0, 0};
        const double sign = k % 2 == 0 ? 1.0 : -1.0;
        s.direction = s.direction * std::cos(tilt) + across * (sign * std::sin(tilt));
        s.point = s.point + s.direction * (0.02 * sign);
    }
    auto trimmed = occt::trimPrimitives(primitives, scan, {});
    REQUIRE(trimmed.has_value());
    const SolidSummary& s = trimmed->solid.summary();
    CHECK(s.valid);
    CHECK(s.faceCount == 6);
    CHECK(s.edgeCount == 12);
    CHECK_THAT(s.volumeMm3, WithinRel(6000.0, 0.01));
}

TEST_CASE("Trimming fails and names the open region when a primitive is left out",
          "[stage][trim]") {
    std::vector<FittedPrimitive> primitives = boxPlanes(30, 20, 10);
    primitives.pop_back(); // the top
    auto trimmed = occt::trimPrimitives(primitives, makeBox(30, 20, 10, 4), {});
    REQUIRE_FALSE(trimmed.has_value());
    CHECK(trimmed.error().code == ErrorCode::InvalidState);
    CHECK_THAT(trimmed.error().message, ContainsSubstring("open between"));
    CHECK_THAT(trimmed.error().message, ContainsSubstring("(30.0, 20.0, "));
}

TEST_CASE("A trimmed solid exports to STEP and reads back the same", "[stage][trim]") {
    std::vector<FittedPrimitive> primitives = boxPlanes(30, 20, 10);
    primitives.push_back({.surface = {.kind = PrimitiveKind::Cylinder,
                                      .point = {15, 10, 0},
                                      .direction = {0, 0, 1},
                                      .radiusMm = 4.0}});
    auto trimmed = occt::trimPrimitives(primitives, boxWithHole(30, 20, 10, 15, 10, 4), {});
    REQUIRE(trimmed.has_value());
    const TempDir dir;
    const auto file = dir.path() / "box.step";
    REQUIRE(occt::writeStep(trimmed->solid, file, "box").has_value());
    auto read = occt::readStep(file);
    REQUIRE(read.has_value());
    CHECK(read->solidCount == 1);
    CHECK(read->valid);
    CHECK(read->faceCount == 7);
    CHECK(read->edgeCount == trimmed->solid.summary().edgeCount);
    CHECK_THAT(read->volumeMm3, WithinRel(trimmed->solid.summary().volumeMm3, 1e-9));
}

TEST_CASE("Trimming rejects no primitives and bad parameters", "[stage][trim]") {
    const TriMesh scan = makeBox(1, 1, 1);
    CHECK(occt::trimPrimitives({}, scan, {}).error().code == ErrorCode::InvalidArgument);
    CHECK(occt::trimPrimitives(boxPlanes(1, 1, 1), scan, {.minCoverage = 0.0}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(occt::trimPrimitives({plane({0, 0, 0}, {0, 0, 0})}, scan, {}).error().code ==
          ErrorCode::InvalidArgument);
}

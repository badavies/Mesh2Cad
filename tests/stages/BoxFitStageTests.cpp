// SPDX-License-Identifier: GPL-3.0-or-later
// Fitting an exact box to a scan, and the box as a solid (issue #84).

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"
#include "m2c/pipeline/Boolean.h"
#include "m2c/pipeline/BoxFit.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// v turned by angle about the unit axis k.
Vec3d turn(const Vec3d& v, const Vec3d& k, double angle) {
    return v * std::cos(angle) + cross(k, v) * std::sin(angle) +
           k * (dot(k, v) * (1.0 - std::cos(angle)));
}

const Vec3d kTurnAxis = Vec3d{1, 2, 3} * (1.0 / std::sqrt(14.0));
constexpr double kTurn = 20.0 * kDeg;
const Vec3d kShift{5, -3, 7};

/// A 30 x 20 x 10 box centred on the origin, turned 20 degrees about (1, 2, 3) and moved to
/// (5, -3, 7), with every vertex moved up to 0.03 mm in each coordinate.
TriMesh turnedBoxScan() {
    const TriMesh box = makeBox(30, 20, 10, 12);
    std::vector<Vec3d> p = box.positions();
    for (Vec3d& v : p) v = turn(v - Vec3d{15, 10, 5}, kTurnAxis, kTurn) + kShift;
    return addNoise(makeMesh(std::move(p), box.indices()), 0.03);
}

std::vector<std::uint32_t> allFaces(const TriMesh& mesh) {
    std::vector<std::uint32_t> faces(mesh.faceCount());
    for (std::uint32_t f = 0; f < faces.size(); ++f) faces[f] = f;
    return faces;
}

double angleDeg(const Vec3d& a, const Vec3d& b) {
    return std::acos(std::min(1.0, dot(a, b) / (length(a) * length(b)))) / kDeg;
}

void checkTurnedBox(const OrientedBox& box) {
    CHECK_THAT(box.sizeMm.x, WithinAbs(30.0, 0.05));
    CHECK_THAT(box.sizeMm.y, WithinAbs(20.0, 0.05));
    CHECK_THAT(box.sizeMm.z, WithinAbs(10.0, 0.05));
    CHECK(angleDeg(box.axes[0], turn({1, 0, 0}, kTurnAxis, kTurn)) < 0.1);
    CHECK(angleDeg(box.axes[1], turn({0, 1, 0}, kTurnAxis, kTurn)) < 0.1);
    CHECK(angleDeg(box.axes[2], turn({0, 0, 1}, kTurnAxis, kTurn)) < 0.1);
}

} // namespace

TEST_CASE("Distance to a box's surface from inside and outside", "[stages][fit][box]") {
    const OrientedBox box{.center = {1, 1, 1}, .sizeMm = {4, 2, 2}};
    CHECK_THAT(distanceTo(box, {1, 1, 1}), WithinAbs(1.0, 1e-12));
    CHECK_THAT(distanceTo(box, {3.5, 1, 1}), WithinAbs(0.5, 1e-12));
    CHECK_THAT(distanceTo(box, {6, 6, 1}), WithinAbs(5.0, 1e-12)); // past an edge: 3 and 4
    CHECK_THAT(distanceTo(box, {2.5, 1, 1.5}), WithinAbs(0.5, 1e-12));
}

TEST_CASE("A box fit finds a turned, noisy box", "[stages][fit][box]") {
    const TriMesh scan = turnedBoxScan();
    const auto fit = fitBox(scan, allFaces(scan), BoxFitParams{});
    REQUIRE(fit);
    checkTurnedBox(fit->box);
    CHECK(length(fit->box.center - kShift) < 0.05);
    CHECK(dot(cross(fit->box.axes[0], fit->box.axes[1]), fit->box.axes[2]) > 0.999);
    CHECK(fit->rmsMm < 0.03);
    CHECK(fit->p95Mm < 0.05);
    CHECK(fit->p95Mm <= fit->maxMm);

    // Without its bottom, the box keeps the sides' extent there.
    std::vector<std::uint32_t> sides;
    const Vec3d down = turn({0, 0, -1}, kTurnAxis, kTurn);
    for (std::uint32_t f = 0; f < scan.faceCount(); ++f) {
        const auto [a, b, c] = scan.face(f);
        const Vec3d n = cross(scan.positions()[b] - scan.positions()[a],
                              scan.positions()[c] - scan.positions()[a]);
        if (dot(n, down) < 0.9 * length(n)) sides.push_back(f);
    }
    REQUIRE(sides.size() < scan.faceCount());
    const auto open = fitBox(scan, sides, BoxFitParams{});
    REQUIRE(open);
    checkTurnedBox(open->box);

    // The same every time.
    CHECK(fitBox(scan, allFaces(scan), BoxFitParams{})->box.center == fit->box.center);
}

TEST_CASE("A box fit fails on no faces or a shape that is no box", "[stages][fit][box]") {
    const TriMesh ball = makeSphere(10.0, 3);
    CHECK(fitBox(ball, {}, BoxFitParams{}).error().code == ErrorCode::InvalidArgument);
    CHECK(fitBox(ball,
                 std::vector<std::uint32_t>{static_cast<std::uint32_t>(ball.faceCount())},
                 BoxFitParams{})
              .error()
              .code == ErrorCode::InvalidArgument);
    CHECK(fitBox(ball, allFaces(ball), BoxFitParams{.maxResidualMm = 0.0}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(fitBox(ball, allFaces(ball), BoxFitParams{}).error().code == ErrorCode::InvalidState);
}

TEST_CASE("A box becomes an exact solid", "[stages][box]") {
    OrientedBox box{.center = {1, 2, 3}, .sizeMm = {30, 20, 10}};
    for (Vec3d& a : box.axes) a = turn(a, kTurnAxis, kTurn);
    const auto solid = boxSolid(box);
    REQUIRE(solid);
    const SolidSummary s = solid->summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.faceCount == 6);
    CHECK_THAT(s.volumeMm3, WithinAbs(6000.0, 1e-6));
    CHECK_THAT(s.areaMm2, WithinAbs(2.0 * (600 + 300 + 200), 1e-6));
    CHECK_THAT((s.bounds.min.x + s.bounds.max.x) / 2.0, WithinAbs(1.0, 1e-6));

    OrientedBox flat = box;
    flat.sizeMm.z = 0.0;
    CHECK(boxSolid(flat).error().code == ErrorCode::InvalidArgument);
    OrientedBox skew = box;
    skew.axes[1] = box.axes[0];
    CHECK(boxSolid(skew).error().code == ErrorCode::InvalidArgument);
    OrientedBox mirrored = box;
    mirrored.axes[2] = box.axes[2] * -1.0;
    CHECK(boxSolid(mirrored).error().code == ErrorCode::InvalidArgument);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Fitting an exact plane, cylinder, sphere or cone to a region of a mesh (issue #47).

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/pipeline/PrimitiveFit.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kPi = std::numbers::pi;

Vec3d unitOf(const Vec3d& v) {
    return v * (1.0 / length(v));
}

/// Two unit vectors across unit d, with e1 x e2 = d.
std::pair<Vec3d, Vec3d> frame(const Vec3d& d) {
    const Vec3d e1 = unitOf(cross(d, Vec3d{1, 0, 0}));
    return {e1, cross(d, e1)};
}

/// A 40 x 40 grid over (u, v) in [0, 1]^2, each vertex moved up to 0.02 mm along the surface
/// normal. Faces face along d/du x d/dv.
template <class At, class Normal>
TriMesh patch(At at, Normal normal) {
    constexpr int n = 40;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-0.02, 0.02);
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const double u = 1.0 * i / n;
            const double v = 1.0 * j / n;
            p.push_back(at(u, v) + normal(u, v) * noise(rng));
        }
    }
    const auto id = [](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            idx.insert(idx.end(), {id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            idx.insert(idx.end(), {id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    }
    return makeMesh(std::move(p), std::move(idx));
}

std::vector<std::uint32_t> allFaces(const TriMesh& mesh) {
    std::vector<std::uint32_t> faces(mesh.faceCount());
    for (std::uint32_t f = 0; f < faces.size(); ++f) faces[f] = f;
    return faces;
}

const Vec3d kNormal = unitOf(Vec3d{0.3, -0.2, 1.0});
const Vec3d kAxis = unitOf(Vec3d{0.2, 0.4, 1.0});

TriMesh planePatch() {
    const auto [e1, e2] = frame(kNormal);
    return patch([&](double u, double v) { return Vec3d{1, 2, 3} + e1 * (30 * u) + e2 * (30 * v); },
                 [&](double, double) { return kNormal; });
}

/// Radius 8 around kAxis through (1, 2, 3), half a turn, 30 mm long.
TriMesh cylinderPatch() {
    const auto [e1, e2] = frame(kAxis);
    const auto radial = [&](double u) { return e1 * std::cos(kPi * u) + e2 * std::sin(kPi * u); };
    return patch([&](double u,
                     double v) { return Vec3d{1, 2, 3} + kAxis * (30 * v - 15) + radial(u) * 8.0; },
                 [&](double u, double) { return radial(u); });
}

/// Radius 12 about (-4, 0, 7), a cap from 0.1 to 1 rad off kAxis.
TriMesh spherePatch() {
    const auto [e1, e2] = frame(kAxis);
    const auto out = [&](double u, double v) {
        const double polar = 0.1 + 0.9 * v;
        return kAxis * std::cos(polar) +
               (e1 * std::cos(2 * kPi * u) + e2 * std::sin(2 * kPi * u)) * std::sin(polar);
    };
    return patch([&](double u, double v) { return Vec3d{-4, 0, 7} + out(u, v) * 12.0; }, out);
}

/// Apex (2, -1, 4), axis kAxis, half-angle 25 degrees, a full turn from 10 to 30 mm up the axis
/// (on half a turn the apex, 10 mm beyond the patch, is only found within 0.013 mm).
TriMesh conePatch() {
    const auto [e1, e2] = frame(kAxis);
    const double a = 25 * kDeg;
    const auto radial = [&](double u) {
        return e1 * std::cos(2 * kPi * u) + e2 * std::sin(2 * kPi * u);
    };
    return patch(
        [&](double u, double v) {
            const double h = 10 + 20 * v;
            return Vec3d{2, -1, 4} + kAxis * h + radial(u) * (h * std::tan(a));
        },
        [&](double u, double) { return radial(u) * std::cos(a) - kAxis * std::sin(a); });
}

PrimitiveFit fit(const TriMesh& mesh, std::optional<PrimitiveKind> kind) {
    const std::vector<std::uint32_t> faces = allFaces(mesh);
    const Result<PrimitiveFit> r = fitRegion(mesh, faces, {.kind = kind, .maxResidualMm = 0.1});
    REQUIRE(r.has_value());
    return *r;
}

double offAxis(const Primitive& s, const Vec3d& p) {
    const Vec3d v = p - s.point;
    return length(v - s.direction * dot(v, s.direction));
}

} // namespace

TEST_CASE("A plane is recovered from a noisy patch", "[stages][fit]") {
    const TriMesh mesh = planePatch();
    const PrimitiveFit f = fit(mesh, PrimitiveKind::Plane);
    CHECK(f.primitive.kind == PrimitiveKind::Plane);
    CHECK(dot(f.primitive.direction, kNormal) > std::cos(0.1 * kDeg)); // the faces' side
    CHECK(distanceTo(f.primitive, Vec3d{1, 2, 3}) < 0.01);
    CHECK(f.rmsMm < 0.013);
    // Noise up to 0.02 mm, uniform: 95% of the points are within 0.019 mm.
    CHECK(f.p95Mm > 0.017);
    CHECK(f.p95Mm <= f.maxMm);
    CHECK(f.maxMm < 0.025);
}

TEST_CASE("A cylinder is recovered from a noisy patch", "[stages][fit]") {
    const PrimitiveFit f = fit(cylinderPatch(), PrimitiveKind::Cylinder);
    CHECK(f.primitive.kind == PrimitiveKind::Cylinder);
    CHECK(std::abs(dot(f.primitive.direction, kAxis)) > std::cos(0.1 * kDeg));
    CHECK_THAT(f.primitive.radiusMm, WithinAbs(8.0, 0.01));
    CHECK(offAxis(f.primitive, Vec3d{1, 2, 3}) < 0.01);
    CHECK(f.p95Mm < 0.02);
}

TEST_CASE("A sphere is recovered from a noisy patch", "[stages][fit]") {
    const PrimitiveFit f = fit(spherePatch(), PrimitiveKind::Sphere);
    CHECK(f.primitive.kind == PrimitiveKind::Sphere);
    CHECK(length(f.primitive.point - Vec3d{-4, 0, 7}) < 0.01);
    CHECK_THAT(f.primitive.radiusMm, WithinAbs(12.0, 0.01));
    CHECK(f.p95Mm < 0.02);
}

TEST_CASE("A cone is recovered from a noisy patch", "[stages][fit]") {
    const PrimitiveFit f = fit(conePatch(), PrimitiveKind::Cone);
    CHECK(f.primitive.kind == PrimitiveKind::Cone);
    CHECK(length(f.primitive.point - Vec3d{2, -1, 4}) < 0.01);
    CHECK(dot(f.primitive.direction, kAxis) > std::cos(0.1 * kDeg)); // from the apex inwards
    CHECK_THAT(f.primitive.halfAngleRad, WithinAbs(25 * kDeg, 0.1 * kDeg));
    CHECK(f.p95Mm < 0.02);
}

TEST_CASE("Fitting with no kind picks the right one", "[stages][fit]") {
    CHECK(fit(planePatch(), std::nullopt).primitive.kind == PrimitiveKind::Plane);
    CHECK(fit(cylinderPatch(), std::nullopt).primitive.kind == PrimitiveKind::Cylinder);
    CHECK(fit(spherePatch(), std::nullopt).primitive.kind == PrimitiveKind::Sphere);
    CHECK(fit(conePatch(), std::nullopt).primitive.kind == PrimitiveKind::Cone);
}

TEST_CASE("A fit with its residual above the limit fails", "[stages][fit]") {
    const TriMesh mesh = cylinderPatch();
    const std::vector<std::uint32_t> faces = allFaces(mesh);
    const auto plane = fitRegion(mesh, faces, {.kind = PrimitiveKind::Plane, .maxResidualMm = 0.1});
    REQUIRE_FALSE(plane.has_value());
    CHECK(plane.error().code == ErrorCode::InvalidState);
    // The noise alone is above a limit of 0.01 mm, whatever the kind.
    CHECK_FALSE(fitRegion(mesh, faces, {.kind = std::nullopt, .maxResidualMm = 0.01}).has_value());
    CHECK(fitRegion(mesh, faces, {.kind = std::nullopt, .maxResidualMm = 0.1}).has_value());
}

TEST_CASE("A region fit rejects bad input", "[stages][fit]") {
    const TriMesh mesh = planePatch();
    const std::vector<std::uint32_t> two{0, 1};
    const std::vector<std::uint32_t> outside{0, 1, 2, static_cast<std::uint32_t>(mesh.faceCount())};
    CHECK(fitRegion(mesh, two, {}).error().code == ErrorCode::InvalidArgument);
    CHECK(fitRegion(mesh, outside, {}).error().code == ErrorCode::InvalidArgument);
    const std::vector<std::uint32_t> faces = allFaces(mesh);
    CHECK(fitRegion(mesh, faces, {.kind = std::nullopt, .maxResidualMm = 0.0}).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("Distances and normals of a cone", "[stages][fit]") {
    // Apex at the origin, opening up the z axis at 45 degrees.
    const Primitive cone{.kind = PrimitiveKind::Cone,
                         .point = Vec3d{0, 0, 0},
                         .direction = Vec3d{0, 0, 1},
                         .halfAngleRad = kPi / 4};
    CHECK_THAT(distanceTo(cone, {3, 0, 3}), WithinAbs(0.0, 1e-12));
    CHECK_THAT(distanceTo(cone, {0, 0, 2}), WithinAbs(std::sqrt(2.0), 1e-12));
    CHECK_THAT(distanceTo(cone, {0, 0, -2}), WithinAbs(2.0, 1e-12)); // behind the apex
    const Vec3d n = normalAt(cone, {0, 5, 1});
    CHECK_THAT(n.y, WithinAbs(std::sqrt(0.5), 1e-12));
    CHECK_THAT(n.z, WithinAbs(-std::sqrt(0.5), 1e-12));
    CHECK(std::string(primitiveKindName(PrimitiveKind::Cone)) == "cone");
}

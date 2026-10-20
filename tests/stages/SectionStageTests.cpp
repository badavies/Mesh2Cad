// SPDX-License-Identifier: GPL-3.0-or-later
// Section sketches: a mesh cut with a plane, and lines, arcs and splines fitted to the cut.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/pipeline/Section.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = std::numbers::pi;

/// Every curve ends where the next starts, and the last where the first starts.
void requireClosed(const SketchLoop& loop) {
    REQUIRE_FALSE(loop.curves.empty());
    for (std::size_t k = 0; k < loop.curves.size(); ++k) {
        const SketchCurve& next = loop.curves[(k + 1) % loop.curves.size()];
        REQUIRE(length(loop.curves[k].end() - next.start()) < 1e-9);
    }
}

std::size_t count(const SketchLoop& loop, CurveKind kind) {
    std::size_t n = 0;
    for (const SketchCurve& c : loop.curves) n += c.kind == kind ? 1 : 0;
    return n;
}

/// A rectangle 30 x 20 with corners rounded to radius 4, anticlockwise, a point every 0.25 mm.
std::vector<Vec2d> roundedRectangle() {
    std::vector<Vec2d> out;
    const double r = 4.0;
    const Vec2d centres[4] = {{26, 4}, {26, 16}, {4, 16}, {4, 4}};
    for (int corner = 0; corner < 4; ++corner) {
        const Vec2d c = centres[corner];
        const Vec2d next = centres[(corner + 1) % 4];
        const double a0 = (corner - 1) * kPi / 2;
        for (int k = 0; k < 25; ++k) {
            const double a = a0 + (kPi / 2) * k / 25;
            out.push_back(c + Vec2d{std::cos(a), std::sin(a)} * r);
        }
        // The straight side from this corner's end to the next corner's start.
        const Vec2d from = c + Vec2d{std::cos(a0 + kPi / 2), std::sin(a0 + kPi / 2)} * r;
        const Vec2d to = next + Vec2d{std::cos(a0 + kPi / 2), std::sin(a0 + kPi / 2)} * r;
        const int steps = static_cast<int>(std::lround(length(to - from) / 0.25));
        for (int k = 0; k < steps; ++k) out.push_back(from + (to - from) * (double(k) / steps));
    }
    return out;
}

} // namespace

TEST_CASE("planeThrough puts u along x and v along y on a z plane", "[stages][section]") {
    const SketchPlane plane = planeThrough({1, 2, 3}, {0, 0, 5});
    REQUIRE(plane.u == Vec3d{1, 0, 0});
    REQUIRE(plane.v == Vec3d{0, 1, 0});
    const SketchPlane tilted = planeThrough({0, 0, 0}, {1, 1, 1});
    REQUIRE_THAT(dot(tilted.u, tilted.normal()), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(length(tilted.normal()), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(dot(tilted.normal(), Vec3d{1, 1, 1}), WithinAbs(std::sqrt(3.0), 1e-12));
}

TEST_CASE("Sectioning a box gives four lines", "[stages][section]") {
    const TriMesh box = makeBox(20, 10, 6, 4);
    const auto section = sectionMesh(box, planeThrough({0, 0, 2.3}, {0, 0, 1}), {});
    REQUIRE(section.has_value());
    REQUIRE(section->openPieces == 0);
    REQUIRE(section->sketch.loops.size() == 1);
    const SketchLoop& loop = section->sketch.loops.front();
    requireClosed(loop);
    REQUIRE(loop.curves.size() == 4);
    REQUIRE(count(loop, CurveKind::Line) == 4);
    double perimeter = 0.0;
    for (const SketchCurve& c : loop.curves) perimeter += length(c.end() - c.start());
    REQUIRE_THAT(perimeter, WithinAbs(60.0, 1e-9));
    REQUIRE(section->maxErrorMm < 1e-9);

    // A slanted plane through the same box still gives four lines.
    const auto slanted = sectionMesh(box, planeThrough({10, 5, 3}, {0.2, -0.1, 1}), {});
    REQUIRE(slanted.has_value());
    REQUIRE(slanted->sketch.loops.size() == 1);
    REQUIRE(count(slanted->sketch.loops.front(), CurveKind::Line) == 4);
    REQUIRE(slanted->sketch.loops.front().curves.size() == 4);
}

TEST_CASE("Sectioning a cylinder gives one circle", "[stages][section]") {
    const TriMesh cylinder = makeCylinder(10.0, 20.0, 256, 4);
    const auto section = sectionMesh(cylinder, planeThrough({0, 0, 7.3}, {0, 0, 1}), {});
    REQUIRE(section.has_value());
    REQUIRE(section->sketch.loops.size() == 1);
    const SketchLoop& loop = section->sketch.loops.front();
    REQUIRE(loop.curves.size() == 1);
    const SketchCurve& circle = loop.curves.front();
    REQUIRE(circle.kind == CurveKind::Arc);
    REQUIRE_THAT(circle.radiusMm, WithinAbs(10.0, 0.01));
    REQUIRE(length(circle.centre) < 0.01);
    REQUIRE_THAT(std::abs(circle.sweepRad), WithinAbs(2.0 * kPi, 1e-12));
    requireClosed(loop);

    // A scanned cylinder, its vertices up to 0.02 mm off, still gives one circle.
    const auto scanned =
        sectionMesh(addNoise(cylinder, 0.02), planeThrough({0, 0, 7.3}, {0, 0, 1}), {});
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->sketch.loops.size() == 1);
    REQUIRE(scanned->sketch.loops.front().curves.size() == 1);
    REQUIRE_THAT(scanned->sketch.loops.front().curves.front().radiusMm, WithinAbs(10.0, 0.01));
}

TEST_CASE("A rounded rectangle gives four lines and four arcs", "[stages][section]") {
    const auto fit = fitLoop(roundedRectangle(), {});
    REQUIRE(fit.has_value());
    const SketchLoop& loop = fit->loop;
    requireClosed(loop);
    REQUIRE(loop.curves.size() == 8);
    REQUIRE(count(loop, CurveKind::Line) == 4);
    REQUIRE(count(loop, CurveKind::Arc) == 4);
    for (const SketchCurve& c : loop.curves) {
        if (c.kind != CurveKind::Arc) continue;
        REQUIRE_THAT(c.radiusMm, WithinAbs(4.0, 0.01));
        REQUIRE_THAT(c.sweepRad, WithinAbs(kPi / 2, 0.01));
    }
    REQUIRE(fit->maxErrorMm <= 0.05);

    // Points up to 0.02 mm off still give the same eight curves.
    std::vector<Vec2d> noisy = roundedRectangle();
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> noise(-0.02, 0.02);
    for (Vec2d& v : noisy) v = v + Vec2d{noise(rng), noise(rng)};
    const auto rough = fitLoop(noisy, {});
    REQUIRE(rough.has_value());
    requireClosed(rough->loop);
    REQUIRE(count(rough->loop, CurveKind::Line) == 4);
    REQUIRE(count(rough->loop, CurveKind::Arc) == 4);
    for (const SketchCurve& c : rough->loop.curves) {
        if (c.kind == CurveKind::Arc) REQUIRE_THAT(c.radiusMm, WithinAbs(4.0, 0.05));
    }
}

TEST_CASE("A freeform loop becomes a spline within the limit", "[stages][section]") {
    // An ellipse 40 x 16: no circle fits it, and arcs within the limit cover fewer than 100 points.
    std::vector<Vec2d> ellipse;
    for (int k = 0; k < 2000; ++k) {
        const double a = 2.0 * kPi * k / 2000;
        ellipse.push_back({20.0 * std::cos(a), 8.0 * std::sin(a)});
    }
    SectionParams params;
    params.maxErrorMm = 0.001;
    params.minCurvePoints = 100;
    const auto fit = fitLoop(ellipse, params);
    REQUIRE(fit.has_value());
    requireClosed(fit->loop);
    REQUIRE(count(fit->loop, CurveKind::Spline) >= 1);
    REQUIRE(fit->maxErrorMm <= 0.001);
    for (const SketchCurve& c : fit->loop.curves) {
        if (c.kind == CurveKind::Spline) REQUIRE(c.points.size() < 2000);
    }
}

TEST_CASE("A plane that misses the mesh or cuts only open pieces fails", "[stages][section]") {
    const TriMesh box = makeBox(20, 10, 6, 4);
    REQUIRE_FALSE(sectionMesh(box, planeThrough({0, 0, 9}, {0, 0, 1}), {}).has_value());

    // The box without its +y side (faces 96 to 127 of makeBox's 192) is open all the way up.
    std::vector<std::uint32_t> faces;
    for (std::size_t f = 0; f < box.faceCount(); ++f) {
        if (f >= 96 && f < 128) continue;
        const auto [a, b, c] = box.face(f);
        faces.insert(faces.end(), {a, b, c});
    }
    const TriMesh open = makeMesh(box.positions(), faces);
    const auto section = sectionMesh(open, planeThrough({0, 0, 2.3}, {0, 0, 1}), {});
    REQUIRE_FALSE(section.has_value());
    REQUIRE(section.error().code == ErrorCode::InvalidArgument);

    REQUIRE_FALSE(fitLoop(std::vector<Vec2d>{{0, 0}, {1, 0}}, {}).has_value());
}

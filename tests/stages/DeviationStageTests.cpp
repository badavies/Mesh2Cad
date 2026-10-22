// SPDX-License-Identifier: GPL-3.0-or-later
// Deviation hotspots: where a scan leaves its reference, as patches an agent can select and model.

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Primitive.h"
#include "m2c/pipeline/Boolean.h"
#include "m2c/pipeline/Deviation.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

constexpr double kPi = std::numbers::pi;

/// The top of a 40 x 40 x 10 block (z = 10) around (20, 20) out to radius 15, with a round
/// feature of radius 5 at its centre going `height` up (a boss) or down (a pocket, if negative):
/// annulus, wall and cap, welded, with outward normals. height 0 is a flat disc.
TriMesh topWithFeature(double height) {
    constexpr int n = 96;    // around
    constexpr int rings = 8; // across the annulus, up the wall and across the cap
    const Vec3d centre{20, 20, 10};
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    const auto at = [&](double r, double a, double z) {
        return Vec3d{centre.x + r * std::cos(a), centre.y + r * std::sin(a), z};
    };
    // Rows of n vertices from the outside in: annulus r 15 -> 5 at z 10, wall z 10 -> 10 +
    // height at r 5, cap r 5 -> 0 at z 10 + height; then the centre.
    std::vector<std::pair<double, double>> rows; // (r, z)
    for (int k = 0; k <= rings; ++k) rows.emplace_back(15.0 - 10.0 * k / rings, 10.0);
    if (height != 0.0) {
        for (int k = 1; k <= rings; ++k) rows.emplace_back(5.0, 10.0 + height * k / rings);
    }
    for (int k = 1; k < rings; ++k) rows.emplace_back(5.0 - 5.0 * k / rings, 10.0 + height);
    for (const auto& [r, z] : rows) {
        for (int j = 0; j < n; ++j) p.push_back(at(r, 2 * kPi * j / n, z));
    }
    p.push_back({centre.x, centre.y, 10.0 + height});
    const auto v = [&](std::size_t row, int j) {
        return static_cast<std::uint32_t>(row * n + static_cast<std::size_t>((j + n) % n));
    };
    // Counter-clockwise seen from where the surface faces: up for the flat parts, out from the
    // axis for a boss's wall, in towards it for a pocket's.
    for (std::size_t row = 0; row + 1 < rows.size(); ++row) {
        for (int j = 0; j < n; ++j) {
            idx.insert(idx.end(), {v(row, j), v(row, j + 1), v(row + 1, j + 1)});
            idx.insert(idx.end(), {v(row, j), v(row + 1, j + 1), v(row + 1, j)});
        }
    }
    const auto middle = static_cast<std::uint32_t>(p.size() - 1);
    for (int j = 0; j < n; ++j)
        idx.insert(idx.end(), {v(rows.size() - 1, j), v(rows.size() - 1, j + 1), middle});
    return makeMesh(std::move(p), std::move(idx));
}

Solid block() {
    auto solid = boxSolid(OrientedBox{.center = {20, 20, 5}, .sizeMm = {40, 40, 10}});
    REQUIRE(solid.has_value());
    return std::move(*solid);
}

double offAxis(const Vec3d& p) {
    return std::hypot(p.x - 20.0, p.y - 20.0);
}

} // namespace

TEST_CASE("A pocket the reference lacks is one inward hotspot", "[stages][deviation]") {
    const TriMesh scan = topWithFeature(-8.0);
    const auto field = deviationToSolid(scan, block(), 0.1);
    REQUIRE(field.has_value());
    const auto hotspots = deviationHotspots(scan, *field);
    REQUIRE(hotspots.size() == 1);
    const DeviationHotspot& h = hotspots[0];
    CHECK(h.sign == -1);
    CHECK(offAxis(h.centroid) < 1.0);
    const double wallAndFloor = 2 * kPi * 5 * 8 + kPi * 5 * 5;
    CHECK(std::abs(h.areaMm2 - wallAndFloor) < 0.1 * wallAndFloor);
    CHECK(h.maxSignedMm < -1.9); // the floor, 2 mm above the block's bottom
    CHECK(h.bounds.min.z > 1.99);
    CHECK(h.bounds.max.z < 10.01);
}

TEST_CASE("A boss the reference lacks is one outward hotspot", "[stages][deviation]") {
    const TriMesh scan = topWithFeature(6.0);
    const auto field = deviationToSolid(scan, block(), 0.1);
    REQUIRE(field.has_value());
    const auto hotspots = deviationHotspots(scan, *field);
    REQUIRE(hotspots.size() == 1);
    const DeviationHotspot& h = hotspots[0];
    CHECK(h.sign == 1);
    CHECK(offAxis(h.centroid) < 1.0);
    const double wallAndCap = 2 * kPi * 5 * 6 + kPi * 5 * 5;
    CHECK(std::abs(h.areaMm2 - wallAndCap) < 0.1 * wallAndCap);
    CHECK_THAT(h.maxSignedMm, Catch::Matchers::WithinAbs(6.0, 1e-6));
    CHECK(offAxis(scan.positions()[scan.face(h.seedFace)[0]]) < 5.01);
}

TEST_CASE("A scan within tolerance has no hotspots, and hotspots repeat", "[stages][deviation]") {
    const TriMesh flat = topWithFeature(0.0);
    const auto field = deviationToSolid(flat, block(), 0.1);
    REQUIRE(field.has_value());
    CHECK(deviationHotspots(flat, *field).empty());

    const TriMesh scan = topWithFeature(-8.0);
    const auto again = deviationToSolid(scan, block(), 0.1);
    REQUIRE(again.has_value());
    const auto a = deviationHotspots(scan, *again);
    const auto b = deviationHotspots(scan, *again);
    REQUIRE(a.size() == b.size());
    CHECK(a[0].faces == b[0].faces);
    CHECK(a[0].areaMm2 == b[0].areaMm2);
}

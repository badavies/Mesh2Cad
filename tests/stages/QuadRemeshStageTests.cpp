// SPDX-License-Identifier: GPL-3.0-or-later
// Quad layout stage tests (issue #16): synthetic closed shapes in, closed quad cages out, checked
// by topology and distance to the input.

#include <cmath>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/QuadLayout.h"
#include "m2c/pipeline/QuadRemesh.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

/// Unit cube as six quads, outward.
QuadLayout quadCube() {
    std::vector<Vec3d> p{
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    std::vector<std::uint32_t> v{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                                 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
    return *QuadLayout::create(std::move(p), {0, 4, 8, 12, 16, 20, 24}, std::move(v));
}

/// Builds a cage from per-face vertex lists.
QuadLayout cageOf(std::vector<Vec3d> p, const std::vector<std::vector<std::uint32_t>>& faces) {
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> v;
    for (const auto& f : faces) {
        v.insert(v.end(), f.begin(), f.end());
        starts.push_back(static_cast<std::uint32_t>(v.size()));
    }
    return *QuadLayout::create(std::move(p), std::move(starts), std::move(v));
}

std::vector<std::vector<std::uint32_t>> facesOf(const QuadLayout& cage) {
    std::vector<std::vector<std::uint32_t>> faces;
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        faces.emplace_back(cage.face(f).begin(), cage.face(f).end());
    }
    return faces;
}

} // namespace

TEST_CASE("A quad cube is a closed cage of genus zero", "[stages][quad]") {
    const QuadLayout cube = quadCube();
    const CageStats stats = cageStats(cube);
    CHECK(stats.closed());
    CHECK(stats.components == 1);
    CHECK(stats.edgeCount == 12);
    CHECK(stats.eulerCharacteristic() == 2);
    CHECK(stats.extraordinaryVertices == 8); // every corner has valence three
    CHECK(cube.extraordinaryVertices().size() == 8);
    CHECK_THAT(signedVolume(cube), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Cage creation rejects malformed faces", "[stages][quad]") {
    const std::vector<Vec3d> p{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    CHECK_FALSE(QuadLayout::create(p, {0, 2}, {0, 1}));    // two vertices
    CHECK_FALSE(QuadLayout::create(p, {0, 3}, {0, 1, 1})); // repeated vertex
    CHECK_FALSE(QuadLayout::create(p, {0, 3}, {0, 1, 5})); // out of range
    // An offset past the end of the vertex list, followed by one that is in range.
    CHECK_FALSE(QuadLayout::create(p, {0, 1000, 6}, {0, 1, 2, 0, 2, 3}));
    CHECK_FALSE(QuadLayout::create(p, {0, 4}, {0, 1, 2})); // offsets past the end
    CHECK_FALSE(QuadLayout::create({}, {0}, {}));          // empty
    CHECK(QuadLayout::create(p, {0, 3}, {0, 1, 2}));
}

TEST_CASE("Closing a cage fills an open loop and drops fragments", "[stages][quad]") {
    auto faces = facesOf(quadCube());
    faces.erase(faces.begin() + 1); // open the top
    std::vector<Vec3d> p = quadCube().positions();
    // A loose quad far away.
    for (const Vec3d& q : {Vec3d{5, 0, 0}, Vec3d{6, 0, 0}, Vec3d{6, 1, 0}, Vec3d{5, 1, 0}}) {
        p.push_back(q);
    }
    faces.push_back({8, 9, 10, 11});
    const QuadLayout open = cageOf(p, faces);
    CHECK_FALSE(cageStats(open).closed());

    CageCleanup report;
    const auto closed = closeCage(open, report);
    REQUIRE(closed);
    CHECK(report.filledLoops == 1);
    CHECK(report.largestLoop == 4);
    CHECK(report.droppedComponents == 1);
    CHECK(report.droppedComponentFaces == 1);
    const CageStats stats = cageStats(*closed);
    CHECK(stats.closed());
    CHECK(stats.faceCount == 6);
    CHECK(stats.vertexCount == 8);
    CHECK_THAT(signedVolume(*closed), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Closing a cage orients flipped faces outward", "[stages][quad]") {
    auto faces = facesOf(quadCube());
    for (auto& f : faces) std::reverse(f.begin(), f.end()); // all inward
    std::reverse(faces[2].begin(), faces[2].end());         // and one back again
    CageCleanup report;
    const auto closed = closeCage(cageOf(quadCube().positions(), faces), report);
    REQUIRE(closed);
    CHECK(report.flipped);
    CHECK(cageStats(*closed).closed());
    CHECK_THAT(signedVolume(*closed), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Closing a cage removes valence-two vertices", "[stages][quad]") {
    // Split the cube's front edge 0-1 with a vertex used only by the two faces beside it.
    std::vector<Vec3d> p = quadCube().positions();
    p.push_back({0.5, 0, 0});
    auto faces = facesOf(quadCube());
    faces[0] = {0, 3, 2, 1, 8}; // bottom: ... 1 -> 8 -> 0
    faces[2] = {0, 8, 1, 5, 4}; // front: 0 -> 8 -> 1 ...
    CageCleanup report;
    const auto closed = closeCage(cageOf(p, faces), report);
    REQUIRE(closed);
    CHECK(report.dissolvedVertices == 1);
    CHECK(closed->vertexCount() == 8);
    CHECK(cageStats(*closed).closed());
}

TEST_CASE("Remeshing a sphere gives a closed genus-zero cage near the surface", "[stages][quad]") {
    const TriMesh sphere = makeSphere(20.0, 4);
    QuadRemeshParams params;
    params.targetFaces = 600;
    const auto result = quadRemesh(sphere, params);
    REQUIRE(result);
    const CageStats& stats = result->stats;
    CHECK(stats.closed());
    CHECK(stats.components == 1);
    CHECK(stats.eulerCharacteristic() == 2);
    CHECK(stats.faceCount > params.targetFaces / 2);
    CHECK(stats.faceCount < params.targetFaces * 3 / 2);
    // Mostly quads.
    CHECK(stats.faceDegrees.at(4) * 10 >= stats.faceCount * 9);
    CHECK(signedVolume(result->cage) > 0.0);
    // Cage vertices sit on the scan, to within a small part of the edge length (about 2 mm).
    CHECK(result->maxDistanceMm < 0.5);
}

TEST_CASE("Remeshing a torus keeps its genus", "[stages][quad]") {
    const TriMesh torus = makeTorus(30.0, 10.0, 96, 48);
    QuadRemeshParams params;
    params.targetFaces = 1000;
    const auto result = quadRemesh(torus, params);
    REQUIRE(result);
    CHECK(result->stats.closed());
    CHECK(result->stats.components == 1);
    CHECK(result->stats.eulerCharacteristic() == 0);
    CHECK(result->stats.faceCount > params.targetFaces / 2);
    CHECK(result->stats.faceCount < params.targetFaces * 3 / 2);
    CHECK(result->maxDistanceMm < 1.0);
}

TEST_CASE("Remeshing is deterministic", "[stages][quad]") {
    const TriMesh sphere = makeSphere(10.0, 3);
    QuadRemeshParams params;
    params.targetFaces = 300;
    const auto a = quadRemesh(sphere, params);
    const auto b = quadRemesh(sphere, params);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->cage.positions() == b->cage.positions());
    CHECK(a->cage.faceVertices() == b->cage.faceVertices());
}

TEST_CASE("Remeshing rejects tiny targets", "[stages][quad]") {
    QuadRemeshParams params;
    params.targetFaces = 4;
    CHECK_FALSE(quadRemesh(makeSphere(), params));
}

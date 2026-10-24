// SPDX-License-Identifier: GPL-3.0-or-later
#include <random>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/DisplayProxy.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/Transform.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Test shapes are closed and outward", "[support]") {
    const MeshStats box = computeStats(makeBox(2, 3, 4, 3));
    CHECK(box.isWatertight());
    CHECK_THAT(box.signedVolume, WithinAbs(24.0, 1e-9));
    const MeshStats sphere = computeStats(makeSphere(1.0, 3));
    CHECK(sphere.isWatertight());
    CHECK(sphere.faceCount == 1280);
    CHECK(sphere.signedVolume > 4.0);
}

TEST_CASE("Ray casts hit the nearest face", "[core][spatial]") {
    const TriMesh box = makeBox(1, 1, 1, 4);
    const SpatialIndex& index = box.spatialIndex();
    const auto hit = index.raycast({0.3, 0.4, 5.0}, {0, 0, -1});
    REQUIRE(hit.has_value());
    CHECK_THAT(hit->distance, WithinAbs(4.0, 1e-12));
    CHECK_THAT(hit->point.z, WithinAbs(1.0, 1e-12));
    CHECK(faceNormal(box, hit->face).z > 0.99);

    CHECK_FALSE(index.raycast({3, 3, 3}, {0, 0, 1}).has_value());
    CHECK_FALSE(index.raycast({0.5, 0.5, 5.0}, {0, 0, -1}, 3.0).has_value()); // too short
    // From inside, the far side is hit.
    const auto inside = index.raycast({0.5, 0.5, 0.5}, {1, 0, 0});
    REQUIRE(inside.has_value());
    CHECK_THAT(inside->distance, WithinAbs(0.5, 1e-12));
    // The index is built once and shared by copies.
    const TriMesh copy = box;
    CHECK(&copy.spatialIndex() == &index);
}

TEST_CASE("Closest point and radius queries match brute force", "[core][spatial]") {
    const TriMesh sphere = addNoise(makeSphere(10.0, 3), 0.3);
    const SpatialIndex& index = sphere.spatialIndex();
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> d(-15.0, 15.0);
    for (int i = 0; i < 50; ++i) {
        const Vec3d q{d(rng), d(rng), d(rng)};
        double best = 1e300;
        for (std::size_t f = 0; f < sphere.faceCount(); ++f) {
            const auto [a, b, c] = sphere.face(f);
            const Vec3d p = closestPointOnTriangle(
                q, sphere.positions()[a], sphere.positions()[b], sphere.positions()[c]);
            best = std::min(best, length(p - q));
        }
        CHECK_THAT(index.closestPoint(q).distance, WithinAbs(best, 1e-9));

        const double r = 3.0;
        std::vector<std::uint32_t> expected;
        for (std::size_t f = 0; f < sphere.faceCount(); ++f) {
            const auto [a, b, c] = sphere.face(f);
            const Vec3d p = closestPointOnTriangle(
                q, sphere.positions()[a], sphere.positions()[b], sphere.positions()[c]);
            if (length(p - q) <= r) expected.push_back(static_cast<std::uint32_t>(f));
        }
        CHECK(index.facesWithin(q, r) == expected);
    }
}

TEST_CASE("MeshSelection set, count, invert and words", "[core][selection]") {
    MeshSelection s(130);
    s.set(0);
    s.set(64);
    s.set(129);
    CHECK(s.count() == 3);
    CHECK(s.indices() == std::vector<std::uint32_t>{0, 64, 129});
    s.invert();
    CHECK(s.count() == 127);
    CHECK_FALSE(s.test(64));
    const MeshSelection back = MeshSelection::fromWords(s.words(), 130);
    CHECK(back == s);
    s.set(1, false);
    CHECK(s.count() == 126);
}

TEST_CASE("Transforms compose and invert", "[core][transform]") {
    // 90 degrees about Z, then a shift.
    const Transform3d t{{0, -1, 0, 1, 0, 0, 0, 0, 1}, {1, 2, 3}};
    const Vec3d p{1, 0, 0};
    const Vec3d q = t.apply(p);
    CHECK(q == Vec3d{1, 3, 3});
    const Vec3d back = t.inverse().apply(q);
    CHECK_THAT(back.x, WithinAbs(1.0, 1e-15));
    CHECK_THAT(back.y, WithinAbs(0.0, 1e-15));
    const Vec3d twice = (t * t).apply(p);
    const Vec3d stepwise = t.apply(t.apply(p));
    CHECK(twice == stepwise);

    const TriMesh cube = makeCube();
    const TriMesh moved = *transformMesh(cube, t);
    CHECK(moved.sharedIndices() == cube.sharedIndices()); // faces are shared, not copied
    CHECK_THAT(computeStats(moved).signedVolume, WithinAbs(1.0, 1e-12));
}

TEST_CASE("Face components count separate shells", "[core][topology]") {
    const TriMesh two = combine({makeBox(1, 1, 1), makeBox(1, 1, 1, 2)});
    std::uint32_t count = 0;
    const auto comp = faceComponents(two, &count);
    CHECK(count == 2);
    CHECK(comp.front() == 0);
    CHECK(comp.back() == 1);

    std::vector<bool> keep(two.faceCount(), false);
    for (std::size_t f = 0; f < 12; ++f) keep[f] = true;
    const auto first = extractFaces(two, keep);
    REQUIRE(first.has_value());
    CHECK(first->faceCount() == 12);
    CHECK(first->vertexCount() == 8);
    CHECK_FALSE(extractFaces(two, std::vector<bool>(two.faceCount(), false)).has_value());
}

TEST_CASE("Face adjacency is built once and shared by copies", "[core][topology]") {
    const TriMesh sphere = makeSphere(10.0, 2);
    const FaceAdjacency& cached = sphere.faceAdjacency();
    const FaceAdjacency built = buildFaceAdjacency(sphere);
    CHECK(cached.offsets == built.offsets);
    CHECK(cached.neighbours == built.neighbours);
    CHECK(cached.neighbours.size() == 3 * sphere.faceCount()); // closed manifold: three each

    const TriMesh copy = sphere;
    CHECK(&copy.faceAdjacency() == &cached);
}

TEST_CASE("Display proxies shrink big meshes and keep their shape", "[core][proxy]") {
    const TriMesh sphere = makeSphere(10.0, 5); // 20480 faces
    const TriMesh proxy = buildDisplayProxy(sphere, 2000);
    CHECK(proxy.faceCount() <= 2000);
    CHECK(proxy.faceCount() > 500);
    for (const Vec3d& v : proxy.positions()) CHECK(std::abs(length(v) - 10.0) < 1.0);
    CHECK(buildDisplayProxy(sphere, 100000).faceCount() == sphere.faceCount());
}

TEST_CASE("Ray casts and closest points cope with -0.0 and flat triangles", "[core][spatial]") {
    // A direction of -0.0 across an axis on which the origin lies on the boxes' faces.
    const TriMesh cube = makeCube();
    const auto hit = cube.spatialIndex().raycast({0.0, 0.5, 5.0}, {-0.0, 0.0, -1.0});
    REQUIRE(hit.has_value());
    CHECK_THAT(hit->distance, WithinAbs(4.0, 1e-12));
    // A triangle with two equal corners is the segment between its two distinct corners.
    CHECK(closestPointOnTriangle({2, 1, 0}, {0, 0, 0}, {0, 0, 0}, {2, 0, 0}) == Vec3d{2, 0, 0});
    CHECK(closestPointOnTriangle({1, 1, 0}, {0, 0, 0}, {2, 0, 0}, {2, 0, 0}) == Vec3d{1, 0, 0});
}

TEST_CASE("A primitive is drawn clipped to its region's box", "[core][primitive]") {
    // A plane z = 5 over a 10 x 20 region: the box grown by 1% of its diagonal, all of it.
    const FittedPrimitive plane{
        .surface = {.kind = PrimitiveKind::Plane, .point = {3, 4, 5}, .direction = {0, 0, 1}},
        .extent = {.min = {0, 0, 5}, .max = {10, 20, 5}}};
    const auto flat = primitiveDisplay(plane);
    REQUIRE(flat.has_value());
    const double m = 0.01 * std::sqrt(500.0);
    CHECK_THAT(computeStats(*flat).surfaceArea, WithinAbs((10 + 2 * m) * (20 + 2 * m), 1e-6));
    for (std::size_t f = 0; f < flat->faceCount(); ++f) {
        const auto [a, b, c] = flat->face(f);
        const auto& p = flat->positions();
        CHECK(cross(p[b] - p[a], p[c] - p[a]).z > 0.0); // facing the plane's normal
    }

    // A cylinder of radius 4 along x through a 30 mm long region: all the way round, on it.
    const FittedPrimitive cylinder{.surface = {.kind = PrimitiveKind::Cylinder,
                                               .point = {0, 0, 0},
                                               .direction = {1, 0, 0},
                                               .radiusMm = 4.0},
                                   .extent = {.min = {0, -4, -4}, .max = {30, 4, 4}}};
    const auto round = primitiveDisplay(cylinder, 32);
    REQUIRE(round.has_value());
    const Aabb& box = round->bounds();
    const double grown = 0.01 * std::sqrt(30.0 * 30.0 + 8.0 * 8.0 + 8.0 * 8.0);
    CHECK_THAT(box.min.x, WithinAbs(-grown, 1e-6));
    CHECK_THAT(box.max.x, WithinAbs(30.0 + grown, 1e-6));
    CHECK_THAT(box.max.y - box.min.y, WithinAbs(8.0, 1e-9));
    for (std::size_t f = 0; f < round->faceCount(); ++f) {
        const auto [a, b, c] = round->face(f);
        const auto& p = round->positions();
        CHECK(distanceTo(cylinder.surface, p[a]) < 1e-9);
        const Vec3d centre = (p[a] + p[b] + p[c]) * (1.0 / 3.0);
        CHECK(dot(cross(p[b] - p[a], p[c] - p[a]), normalAt(cylinder.surface, centre)) > 0.0);
    }

    // A sphere entirely outside its box draws nothing.
    const FittedPrimitive away{
        .surface = {.kind = PrimitiveKind::Sphere, .point = {100, 0, 0}, .radiusMm = 1.0},
        .extent = {.min = {0, 0, 0}, .max = {1, 1, 1}}};
    CHECK_FALSE(primitiveDisplay(away).has_value());
}

TEST_CASE("Cage edges include an open sheet's boundary, each edge once", "[core][cage]") {
    // Two quads side by side: 0-1-2-3 and 1-4-5-2, so 7 edges, 6 of them on the boundary and
    // some of those running from a higher to a lower vertex.
    const auto sheet =
        QuadLayout::create({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}},
                           {0, 4, 8},
                           {0, 1, 2, 3, 1, 4, 5, 2});
    REQUIRE(sheet);
    const std::vector<std::uint32_t> edges = cageEdges(*sheet);
    CHECK(edges == std::vector<std::uint32_t>{0, 1, 0, 3, 1, 2, 1, 4, 2, 3, 2, 5, 4, 5});
}

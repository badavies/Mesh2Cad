// SPDX-License-Identifier: GPL-3.0-or-later
#include <random>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/DisplayProxy.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/MeshTopology.h"
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

TEST_CASE("Display proxies shrink big meshes and keep their shape", "[core][proxy]") {
    const TriMesh sphere = makeSphere(10.0, 5); // 20480 faces
    const TriMesh proxy = buildDisplayProxy(sphere, 2000);
    CHECK(proxy.faceCount() <= 2000);
    CHECK(proxy.faceCount() > 500);
    for (const Vec3d& v : proxy.positions()) CHECK(std::abs(length(v) - 10.0) < 1.0);
    CHECK(buildDisplayProxy(sphere, 100000).faceCount() == sphere.faceCount());
}

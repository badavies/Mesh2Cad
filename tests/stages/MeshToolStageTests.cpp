// SPDX-License-Identifier: GPL-3.0-or-later
// Stage tests: each tool on small synthetic shapes, checked by measured outcomes
// (ARCHITECTURE.md section 10).

#include <cmath>
#include <numbers>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/MeshStats.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/pipeline/MeshTools.h"
#include "m2c/pipeline/Shells.h"
#include "m2c/pipeline/Smooth.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// Removes the faces whose centroid is within `radius` of `center` (cuts a hole).
TriMesh cutHole(const TriMesh& mesh, const Vec3d& center, double radius) {
    std::vector<bool> keep(mesh.faceCount(), true);
    const auto& p = mesh.positions();
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        if (length((p[a] + p[b] + p[c]) * (1.0 / 3.0) - center) < radius) keep[f] = false;
    }
    return *extractFaces(mesh, keep);
}

double radiusSpread(const TriMesh& mesh, double r) {
    double sum2 = 0.0;
    for (const Vec3d& v : mesh.positions()) sum2 += (length(v) - r) * (length(v) - r);
    return std::sqrt(sum2 / static_cast<double>(mesh.vertexCount()));
}

} // namespace

TEST_CASE("Shell detection finds fragments and the main part", "[stages][shells]") {
    const TriMesh scan =
        combine({makeBox(1, 1, 1), makeSphere(5.0, 2, {20, 0, 0}), makeBox(0.1, 0.1, 0.1)});
    const ShellReport report = detectShells(scan);
    REQUIRE(report.shells.size() == 3);
    CHECK(report.shells[1].isMain);
    CHECK(report.shells[1].faceCount == 320);
    CHECK_FALSE(report.shells[0].isMain);
    CHECK_THAT(report.shells[0].areaMm2, WithinAbs(6.0, 1e-9));

    const MeshSelection small = smallShellFaces(report, 100, 0.0);
    CHECK(small.count() == 24);
    const auto cleaned = deleteFaces(scan, small);
    REQUIRE(cleaned.has_value());
    CHECK(detectShells(*cleaned).shells.size() == 1);
    CHECK(smallShellFaces(report, 0, 1.0).count() == 12); // only the tiny box by area
}

TEST_CASE("Taubin smoothing reduces noise without shrinking", "[stages][smooth]") {
    const TriMesh clean = makeSphere(10.0, 4);
    const TriMesh noisy = addNoise(clean, 0.3);
    const double volume = computeStats(clean).signedVolume;
    const auto smoothed = smoothTaubin(noisy, SmoothParams{20, 0.5, false, 40.0});
    REQUIRE(smoothed.has_value());
    CHECK(smoothed->sharedIndices() == noisy.sharedIndices());
    CHECK(radiusSpread(*smoothed, 10.0) < 0.5 * radiusSpread(noisy, 10.0));
    CHECK_THAT(computeStats(*smoothed).signedVolume, WithinRel(volume, 0.02));
}

TEST_CASE("Smoothing a selection leaves other vertices and sharp edges alone", "[stages][smooth]") {
    const TriMesh box = addNoise(makeBox(10, 10, 10, 6), 0.05);
    MeshSelection top(box.faceCount());
    for (std::size_t f = 0; f < box.faceCount(); ++f) {
        if (faceNormal(box, f).z > 0.9) top.set(f);
    }
    const auto smoothed = smoothTaubin(box, SmoothParams{10, 0.5, true, 30.0}, &top);
    REQUIRE(smoothed.has_value());
    std::size_t moved = 0;
    for (std::size_t v = 0; v < box.vertexCount(); ++v) {
        if (box.positions()[v] == smoothed->positions()[v]) continue;
        ++moved;
        CHECK(box.positions()[v].z > 9.5); // only top-face vertices move
    }
    CHECK(moved > 0);
    // The box edges (sharp) stay put: corners keep their position.
    CHECK(smoothed->positions()[0] == box.positions()[0]);
}

TEST_CASE("Repair cleans duplicates, degenerates and flipped faces", "[stages][repair]") {
    const TriMesh cube = makeBox(2, 2, 2, 2);
    std::vector<std::uint32_t> idx = cube.indices();
    std::swap(idx[1], idx[2]);                       // flip one face
    idx.insert(idx.end(), {idx[3], idx[4], idx[5]}); // duplicate a face
    idx.insert(idx.end(), {0, 0, 1});                // degenerate face
    const TriMesh broken = makeMesh(cube.positions(), idx);
    CHECK_FALSE(computeStats(broken).isWatertight());

    const auto repaired = repair(broken, {});
    REQUIRE(repaired.has_value());
    const MeshStats s = computeStats(repaired->mesh);
    CHECK(s.isWatertight());
    CHECK(s.faceCount == cube.faceCount());
    CHECK_THAT(s.signedVolume, WithinAbs(8.0, 1e-9)); // outward again
    CHECK(repaired->removedFaces == 2);
    CHECK_FALSE(repaired->selfIntersecting);
}

TEST_CASE("Hole filling closes small holes and leaves large ones", "[stages][holes]") {
    const TriMesh sphere = makeSphere(10.0, 4);
    const TriMesh holed = cutHole(cutHole(sphere, {10, 0, 0}, 1.5), {-10, 0, 0}, 5.0);
    CHECK(computeStats(holed).boundaryEdgeCount > 0);

    const auto limited = fillHoles(holed, HoleFillParams{20.0, 0, true});
    REQUIRE(limited.has_value());
    CHECK(limited->filled == 1);
    CHECK(limited->skipped == 1);
    CHECK(computeStats(limited->mesh).boundaryEdgeCount > 0);

    const auto all = fillHoles(holed, HoleFillParams{0.0, 0, true});
    REQUIRE(all.has_value());
    CHECK(all->filled == 2);
    const MeshStats s = computeStats(all->mesh);
    CHECK(s.isWatertight());
    // A faired patch follows the sphere: the volume comes back close to the original.
    CHECK_THAT(s.signedVolume, WithinRel(computeStats(sphere).signedVolume, 0.02));
}

TEST_CASE("Simplification reaches the target and stays closed", "[stages][simplify]") {
    const TriMesh sphere = makeSphere(10.0, 4); // 5120 faces
    const auto simplified = simplifyMesh(sphere, SimplifyParams{1000, true});
    REQUIRE(simplified.has_value());
    CHECK(simplified->faceCount() <= 1000);
    CHECK(simplified->faceCount() >= 900);
    const MeshStats s = computeStats(*simplified);
    CHECK(s.isWatertight());
    CHECK(radiusSpread(*simplified, 10.0) < 0.2);
    CHECK_FALSE(simplifyMesh(sphere, SimplifyParams{0, true}).has_value());
}

TEST_CASE("Simplification keeps open borders in place", "[stages][simplify]") {
    const TriMesh open = cutHole(makeSphere(10.0, 4), {0, 0, 10}, 4.0);
    const std::size_t borderBefore = computeStats(open).boundaryEdgeCount;
    const auto simplified = simplifyMesh(open, SimplifyParams{800, true});
    REQUIRE(simplified.has_value());
    CHECK(computeStats(*simplified).boundaryEdgeCount == borderBefore);
}

TEST_CASE("Remeshing evens out edge lengths and keeps box edges sharp", "[stages][remesh]") {
    const TriMesh box = makeBox(10, 10, 10, 1);
    const auto remeshed = remeshMesh(box, RemeshParams{1.0, 5, 60.0});
    REQUIRE(remeshed.has_value());
    const MeshStats s = computeStats(*remeshed);
    CHECK(s.isWatertight());
    CHECK_THAT(s.signedVolume, WithinRel(1000.0, 1e-6)); // flat faces and sharp edges kept
    double total = 0.0;
    std::size_t count = 0;
    for (std::size_t f = 0; f < remeshed->faceCount(); ++f) {
        const auto [a, b, c] = remeshed->face(f);
        const auto& p = remeshed->positions();
        total += length(p[a] - p[b]) + length(p[b] - p[c]) + length(p[c] - p[a]);
        count += 3;
    }
    CHECK_THAT(total / static_cast<double>(count), WithinRel(1.0, 0.3));
}

TEST_CASE("Shrink-wrap makes a closed mesh from broken input", "[stages][wrap]") {
    // An open box missing its top, plus a stray triangle: not repairable to a solid.
    TriMesh box = makeBox(10, 10, 10, 2);
    std::vector<bool> keep(box.faceCount(), true);
    for (std::size_t f = 0; f < box.faceCount(); ++f) keep[f] = faceNormal(box, f).z < 0.9;
    const TriMesh open =
        combine({*extractFaces(box, keep), makeMesh({{3, 3, 5}, {6, 3, 5}, {3, 6, 5}}, {0, 1, 2})});
    const auto wrapped = shrinkWrap(open, WrapParams{1.0, 0.1});
    REQUIRE(wrapped.has_value());
    const MeshStats s = computeStats(*wrapped);
    CHECK(s.isWatertight());
    CHECK(detectShells(*wrapped).shells.size() >= 1);
    CHECK(s.bounds.min.x < 0.0);
    CHECK(s.bounds.max.x > 10.0);
}

TEST_CASE("Deleting, keeping and flipping faces", "[stages][faces]") {
    const TriMesh cube = makeCube();
    MeshSelection two(12);
    two.set(0);
    two.set(1);
    CHECK(deleteFaces(cube, two)->faceCount() == 10);
    CHECK(keepFaces(cube, two)->faceCount() == 2);
    CHECK(keepFaces(cube, two)->vertexCount() == 4);
    CHECK_THAT(computeStats(*flipFaces(cube)).signedVolume, WithinAbs(-1.0, 1e-12));
    CHECK_FALSE(deleteFaces(cube, MeshSelection(5)).has_value());
}

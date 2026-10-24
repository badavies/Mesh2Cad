// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <vector>

#include "m2c/core/MeshTopology.h"
#include "m2c/pipeline/Defects.h"
#include "m2c/pipeline/MeshTools.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

/// A flat n x n grid of 1 mm squares on z = 0, facing +z, with vertex (i, j) at index i (n + 1) +
/// j. Squares listed in `skip` are left out.
struct Grid {
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::map<std::array<long long, 3>, std::uint32_t> lookup;

    std::uint32_t vertex(const Vec3d& q) {
        const std::array<long long, 3> key{
            std::llround(q.x * 1e6), std::llround(q.y * 1e6), std::llround(q.z * 1e6)};
        const auto [it, inserted] =
            lookup.try_emplace(key, static_cast<std::uint32_t>(positions.size()));
        if (inserted) positions.push_back(q);
        return it->second;
    }

    Grid(const int n, const std::vector<std::array<int, 2>>& skip = {}) {
        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j <= n; ++j) vertex({double(i), double(j), 0.0});
        }
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (std::ranges::contains(skip, std::array{i, j})) continue;
                const std::uint32_t a = vertex({double(i), double(j), 0.0});
                const std::uint32_t b = vertex({double(i + 1), double(j), 0.0});
                const std::uint32_t c = vertex({double(i + 1), double(j + 1), 0.0});
                const std::uint32_t d = vertex({double(i), double(j + 1), 0.0});
                indices.insert(indices.end(), {a, b, c, a, c, d});
            }
        }
    }

    [[nodiscard]] TriMesh mesh() const { return makeMesh(positions, indices); }
};

/// A 30 x 30 grid with a strap: a square tube 1 mm wide arching up out of square (10, 15) and
/// back down into square (14, 15), a handle like the small tunnels a scanner leaves.
TriMesh gridWithStrap() {
    Grid g(30, {{10, 15}, {14, 15}});
    const Vec3d centre{12.5, 15.5, 0.0};
    const double radius = 2.0;
    const int segments = 12;
    std::vector<std::array<std::uint32_t, 4>> rings;
    for (int k = 0; k <= segments; ++k) {
        const double t = std::numbers::pi * k / segments;
        const Vec3d out{-std::cos(t), 0.0, std::sin(t)}; // from the arch's centre to the path
        const Vec3d at = centre + out * radius;
        const Vec3d y{0.0, 0.5, 0.0};
        rings.push_back({g.vertex(at + out * 0.5 + y),
                         g.vertex(at - out * 0.5 + y),
                         g.vertex(at - out * 0.5 - y),
                         g.vertex(at + out * 0.5 - y)});
    }
    for (std::size_t k = 0; k < rings.size() - 1; ++k) {
        for (std::size_t s = 0; s < 4; ++s) {
            const std::uint32_t a = rings[k][s], b = rings[k][(s + 1) % 4];
            const std::uint32_t c = rings[k + 1][(s + 1) % 4], d = rings[k + 1][s];
            g.indices.insert(g.indices.end(), {a, b, c, a, c, d});
        }
    }
    // Orient the tube's faces to agree with the grid's.
    auto repaired = repair(g.mesh(), {});
    REQUIRE(repaired.has_value());
    return repaired->mesh;
}

/// The grid with vertices lifted to make spikes.
TriMesh gridWithSpikes(const std::vector<std::array<int, 2>>& tips, const double height) {
    Grid g(20);
    for (const auto& [i, j] : tips) g.positions[static_cast<std::size_t>(i * 21 + j)].z = height;
    return g.mesh();
}

ScanDefectReport find(const TriMesh& mesh, const ScanDefectParams& params = {}) {
    auto found = findScanDefects(mesh, params);
    REQUIRE(found.has_value());
    return std::move(*found);
}

double highest(const TriMesh& mesh) {
    double z = 0.0;
    for (const Vec3d& p : mesh.positions()) z = std::max(z, std::abs(p.z));
    return z;
}

} // namespace

TEST_CASE("Genus counts the handles of a set of faces", "[stages][defects]") {
    const TriMesh sphere = makeSphere(10.0, 3);
    const TriMesh torus = makeTorus();
    const auto all = [](const TriMesh& m) {
        MeshSelection s(m.faceCount());
        s.invert();
        return s;
    };
    CHECK(surfaceGenus(sphere, all(sphere)) == 0);
    CHECK(surfaceGenus(torus, all(torus)) == 1);
    const TriMesh two = combine({torus, makeTorus(3.0, 1.0, 32, 16)});
    CHECK(surfaceGenus(two, all(two)) == 2);
    CHECK(surfaceGenus(gridWithStrap(), all(gridWithStrap())) == 1);
    // A band around the torus's tube is an annulus: no handle.
    MeshSelection band(torus.faceCount());
    for (std::uint32_t f = 0; f < 48 * 2; ++f) band.set(f);
    CHECK(surfaceGenus(torus, band) == 0);
}

TEST_CASE("Clean surfaces have no spikes or tunnels", "[stages][defects]") {
    for (const TriMesh& mesh : {makeSphere(10.0, 4),
                                addNoise(makeSphere(10.0, 4), 0.05),
                                makeBox(10, 10, 10, 4),
                                makeRoundedCube(10.0, 4.0),
                                makeCylinder(5.0, 10.0),
                                makeTorus()}) {
        const ScanDefectReport found = find(mesh);
        CHECK(found.faces.count() == 0);
        CHECK(found.spikes == 0);
        CHECK(found.tunnels == 0);
    }
}

TEST_CASE("A spike is selected whole and a fill leaves the surface flat", "[stages][defects]") {
    const TriMesh spiky = gridWithSpikes({{10, 10}}, 4.0);
    const ScanDefectReport found = find(spiky);
    CHECK(found.spikes == 1);
    CHECK(found.tunnels == 0);
    CHECK(found.faces.count() == 6); // the faces around the tip
    for (const std::uint32_t f : found.faces.indices())
        CHECK(std::ranges::contains(spiky.face(f), 10u * 21u + 10u));

    auto kept = deleteFaces(spiky, found.faces);
    REQUIRE(kept.has_value());
    auto filled = fillHoles(*kept, {.maxEdges = 20});
    REQUIRE(filled.has_value());
    CHECK(highest(filled->mesh) < 1e-9);
    CHECK(find(filled->mesh).faces.count() == 0);

    // A short bump is no spike; a low tip angle limit lets a needle through.
    CHECK(find(gridWithSpikes({{10, 10}}, 0.5)).spikes == 0);
    CHECK(find(spiky, {.tipAngleDeg = 60.0}).spikes == 0);
    CHECK(find(spiky, {.spikes = false}).faces.count() == 0);
}

TEST_CASE("A blade of two lifted vertices is one spike with its steep sides", "[stages][defects]") {
    const TriMesh blade = gridWithSpikes({{10, 10}, {11, 10}}, 4.0);
    const ScanDefectReport found = find(blade);
    CHECK(found.spikes == 1);
    // Every face touching a lifted vertex is steep and goes with it.
    std::size_t touching = 0;
    for (std::uint32_t f = 0; f < blade.faceCount(); ++f) {
        const auto c = blade.face(f);
        if (std::ranges::any_of(c, [](const std::uint32_t v) { return v == 220 || v == 241; }))
            ++touching;
    }
    CHECK(found.faces.count() == touching);
    // Two spikes apart are counted apart.
    CHECK(find(gridWithSpikes({{5, 5}, {15, 15}}, 4.0)).spikes == 2);
    // Larger than max_faces: left alone.
    const ScanDefectReport small = find(blade, {.maxFaces = 1});
    CHECK(small.spikes == 0);
    CHECK(small.tooLarge == 1);
}

TEST_CASE("A strap over the surface is a tunnel that delete and fill removes",
          "[stages][defects]") {
    const TriMesh strapped = gridWithStrap();
    const ScanDefectReport found = find(strapped, {.spikes = false});
    CHECK(found.tunnels == 1);
    // The selection holds the handle: what is left has none.
    MeshSelection rest = found.faces;
    rest.invert();
    CHECK(surfaceGenus(strapped, rest) == 0);
    CHECK(surfaceGenus(strapped, found.faces) == 1);
    CHECK(found.faces.count() < 200);

    auto kept = deleteFaces(strapped, found.faces);
    REQUIRE(kept.has_value());
    auto filled = fillHoles(*kept, {.maxEdges = 100}); // the grid's own border stays open
    REQUIRE(filled.has_value());
    MeshSelection all(filled->mesh.faceCount());
    all.invert();
    CHECK(surfaceGenus(filled->mesh, all) == 0);
    CHECK(find(filled->mesh).tunnels == 0);

    // Too small a limit leaves it.
    CHECK(find(strapped, {.maxFaces = 10, .spikes = false}).tunnels == 0);
    CHECK(find(strapped, {.tunnels = false}).tunnels == 0);
}

TEST_CASE("A whole torus is one tunnel once the limit takes it in", "[stages][defects]") {
    const TriMesh torus = makeTorus(); // 2304 faces
    const ScanDefectReport found = find(torus, {.maxFaces = 2304});
    CHECK(found.tunnels == 1);
    CHECK(found.spikes == 0);
    CHECK(find(torus, {.maxFaces = 300}).tunnels == 0);
    CHECK_FALSE(findScanDefects(torus, {.maxFaces = 0}).has_value());
}

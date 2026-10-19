// SPDX-License-Identifier: GPL-3.0-or-later
// Quad layout stage tests (issue #16): synthetic closed shapes in, closed quad cages out, checked
// by topology and distance to the input.

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <utility>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/pipeline/MeshTools.h"
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

/// Appends the surface of an axis-aligned cube, n x n quads per side, faces outward.
void addGridCube(std::vector<Vec3d>& p,
                 std::vector<std::vector<std::uint32_t>>& faces,
                 int n,
                 Vec3d origin,
                 double size) {
    std::map<std::array<int, 3>, std::uint32_t> index;
    const auto vertex = [&](std::array<int, 3> c) {
        const auto [it, added] = index.try_emplace(c, static_cast<std::uint32_t>(p.size()));
        if (added) {
            p.push_back(origin + Vec3d{c[0] * size / n, c[1] * size / n, c[2] * size / n});
        }
        return it->second;
    };
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        for (const int side : {0, n}) {
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    const auto at = [&](int u, int v) {
                        std::array<int, 3> x{};
                        x[static_cast<std::size_t>(a)] = side;
                        x[static_cast<std::size_t>(b)] = u;
                        x[static_cast<std::size_t>(c)] = v;
                        return vertex(x);
                    };
                    std::vector<std::uint32_t> quad{
                        at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                    if (side == 0) std::swap(quad[1], quad[3]);
                    faces.push_back(std::move(quad));
                }
            }
        }
    }
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

TEST_CASE("Closing a cage joins a loose closed part with a tube", "[stages][quad]") {
    // A 30 mm body and, 6 mm above it, a 6 mm block: a post whose stem the remesher missed.
    std::vector<Vec3d> p;
    std::vector<std::vector<std::uint32_t>> faces;
    addGridCube(p, faces, 3, {0, 0, 0}, 30.0);
    addGridCube(p, faces, 2, {12, 12, 36}, 6.0);
    const QuadLayout raw = cageOf(p, faces);
    REQUIRE(cageStats(raw).components == 2);

    CageCleanup report;
    const auto joined = closeCage(raw, report);
    REQUIRE(joined);
    CHECK(report.joinedComponents == 1);
    CHECK(report.joinedComponentFaces == 24);
    CHECK(report.droppedComponents == 0);
    const CageStats stats = cageStats(*joined);
    CHECK(stats.closed());
    CHECK(stats.components == 1);
    CHECK(stats.eulerCharacteristic() == 2);
    // One quad of each part makes way for the four sides of a one-ring tube.
    CHECK(stats.faceCount == 53 + 23 + 4);
    CHECK(signedVolume(*joined) > 27000.0);

    // Left out when asked, and counted.
    CageCloseParams keepApart;
    keepApart.joinParts = false;
    const auto apart = closeCage(raw, report, keepApart);
    REQUIRE(apart);
    CHECK(report.joinedComponents == 0);
    CHECK(report.droppedComponents == 1);
    CHECK(report.droppedComponentFaces == 24);
    CHECK(apart->faceCount() == 54);
}

TEST_CASE("Closing a cage leaves out parts it cannot join", "[stages][quad]") {
    std::vector<Vec3d> p;
    std::vector<std::vector<std::uint32_t>> faces;
    addGridCube(p, faces, 3, {0, 0, 0}, 30.0);
    addGridCube(p, faces, 2, {12, 12, 12}, 6.0);    // inside the body: no quad faces it
    addGridCube(p, faces, 1, {12, 12, 40}, 2.0);    // six faces: noise
    addGridCube(p, faces, 2, {200, 200, 200}, 6.0); // far away
    CageCleanup report;
    const auto closed = closeCage(cageOf(p, faces), report);
    REQUIRE(closed);
    CHECK(report.joinedComponents == 0);
    CHECK(report.droppedComponents == 3);
    CHECK(report.droppedComponentFaces == 24 + 6 + 24);
    CHECK(closed->faceCount() == 54);
    CHECK(cageStats(*closed).closed());
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

TEST_CASE("Cage edges follow a guide drawn across the field", "[stages][quad]") {
    const double r = 20.0;
    const TriMesh sphere = makeSphere(r, 5);
    // A great circle tilted 35 degrees from the equator, so no axis-aligned field follows it.
    const double tilt = 35.0 * std::numbers::pi / 180.0;
    std::vector<Vec3d> circle;
    for (int i = 0; i <= 96; ++i) {
        const double a = 2.0 * std::numbers::pi * i / 96;
        circle.push_back(
            {r * std::cos(a), r * std::sin(a) * std::cos(tilt), r * std::sin(a) * std::sin(tilt)});
    }
    // Distance from each guide point to the nearest cage edge, as a share of the mean edge.
    const auto offsets = [&circle](const QuadLayout& cage) {
        std::vector<std::pair<Vec3d, Vec3d>> edges;
        double sum = 0.0;
        for (std::size_t f = 0; f < cage.faceCount(); ++f) {
            const auto face = cage.face(f);
            for (std::size_t i = 0; i < face.size(); ++i) {
                const Vec3d& a = cage.positions()[face[i]];
                const Vec3d& b = cage.positions()[face[(i + 1) % face.size()]];
                edges.emplace_back(a, b);
                sum += length(b - a);
            }
        }
        const double mean = sum / static_cast<double>(edges.size());
        std::vector<double> out;
        for (const Vec3d& q : circle) {
            double best = 1e300;
            for (const auto& [a, b] : edges) {
                const Vec3d ab = b - a;
                const double t = std::clamp(dot(q - a, ab) / dot(ab, ab), 0.0, 1.0);
                best = std::min(best, length(q - (a + ab * t)));
            }
            out.push_back(best / mean);
        }
        std::ranges::sort(out);
        return out;
    };
    QuadRemeshParams q;
    q.targetFaces = 600;
    const auto free = quadRemesh(sphere, q);
    REQUIRE(free);
    q.guides = {circle};
    const auto guided = quadRemesh(sphere, q);
    REQUIRE(guided);
    CHECK(guided->stats.closed());
    const auto before = offsets(free->cage);
    const auto after = offsets(guided->cage);
    // Measured: the median falls from 0.16 to 0.05 edge lengths, the 90th percentile from 0.36
    // to 0.12.
    CHECK(after[48] < 0.08);
    CHECK(after[48] < before[48] / 2);
    CHECK(after[86] < 0.15);
    CHECK(after[86] < before[86] / 2);

    q.guides = {{circle[0]}};
    CHECK(quadRemesh(sphere, q).error().code == ErrorCode::InvalidArgument);
    q.guides = {{Vec3d{0, 0, 0}, Vec3d{1, 0, 0}}}; // inside the sphere, off its surface
    CHECK(quadRemesh(sphere, q).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("An open sheet keeps its outline and faces the way the mesh does", "[stages][quad]") {
    const TriMesh cap = makeCap(20.0, std::numbers::pi / 3.0, 24);
    QuadRemeshParams q;
    q.targetFaces = 300;
    CageCloseParams close;
    close.keepOutline = true;
    // Area-weighted share of faces pointing away from the sphere's centre.
    const auto outward = [](const QuadLayout& cage) {
        double out = 0.0;
        double all = 0.0;
        for (std::size_t f = 0; f < cage.faceCount(); ++f) {
            const auto face = cage.face(f);
            Vec3d n{};
            Vec3d c{};
            for (std::size_t i = 0; i < face.size(); ++i) {
                const Vec3d& a = cage.positions()[face[i]];
                n = n + cross(a, cage.positions()[face[(i + 1) % face.size()]]);
                c = c + a;
            }
            all += length(n);
            if (dot(n, c) > 0.0) out += length(n);
        }
        return out / all;
    };
    const auto sheet = quadRemesh(cap, q, close);
    REQUIRE(sheet);
    CHECK(sheet->stats.orientedManifold());
    CHECK(sheet->stats.boundaryEdges > 0);
    CHECK(sheet->stats.components == 1);
    CHECK(sheet->cleanup.filledLoops == 0);
    CHECK(outward(sheet->cage) == 1.0);
    // The outline is the cap's rim at z = 10.
    for (const Vec3d& p : sheet->cage.positions()) CHECK(p.z > 9.9);

    const auto flipped = quadRemesh(*flipFaces(cap), q, close);
    REQUIRE(flipped);
    CHECK(outward(flipped->cage) == 0.0);

    // Without keepOutline the rim is filled and the cage closed.
    const auto closed = quadRemesh(cap, q);
    REQUIRE(closed);
    CHECK(closed->stats.closed());
    CHECK(closed->cleanup.filledLoops == 1);
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

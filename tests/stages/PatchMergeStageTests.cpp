// SPDX-License-Identifier: GPL-3.0-or-later
// Patch merging stage tests (issue #30): rectangles of cage quads become one face each, and the
// surface does not change.

#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <vector>

#include "m2c/pipeline/Deviation.h"
#include "m2c/pipeline/PatchMerge.h"
#include "m2c/pipeline/QuadRemesh.h"
#include "m2c/pipeline/Surface.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

/// The surface of a cube of side `size` as a cage of n x n quads per side, faces outward.
QuadLayout cubeGridCage(int n, double size) {
    std::map<std::array<int, 3>, std::uint32_t> index;
    std::vector<Vec3d> positions;
    const auto vertex = [&](std::array<int, 3> c) {
        const auto [it, added] = index.try_emplace(c, static_cast<std::uint32_t>(positions.size()));
        if (added) positions.push_back(Vec3d{c[0] * size / n, c[1] * size / n, c[2] * size / n});
        return it->second;
    };
    std::vector<std::uint32_t> starts{0}, vertices;
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3, c = (a + 2) % 3; // e_b x e_c = e_a
        for (const int side : {0, n}) {
            for (int p = 0; p < n; ++p) {
                for (int q = 0; q < n; ++q) {
                    const auto at = [&](int i, int j) {
                        std::array<int, 3> x{};
                        x[static_cast<std::size_t>(a)] = side;
                        x[static_cast<std::size_t>(b)] = i;
                        x[static_cast<std::size_t>(c)] = j;
                        return vertex(x);
                    };
                    std::array<std::uint32_t, 4> quad{
                        at(p, q), at(p + 1, q), at(p + 1, q + 1), at(p, q + 1)};
                    if (side == 0) std::swap(quad[1], quad[3]);
                    vertices.insert(vertices.end(), quad.begin(), quad.end());
                    starts.push_back(static_cast<std::uint32_t>(vertices.size()));
                }
            }
        }
    }
    return *QuadLayout::create(std::move(positions), std::move(starts), std::move(vertices));
}

} // namespace

TEST_CASE("The quads of a cube cage split into its six sides", "[stages][merge]") {
    const QuadLayout cage = cubeGridCage(3, 30.0);
    REQUIRE(cageStats(cage).closed());
    const QuadPartition partition = partitionQuads(cage);
    CHECK(partition.unmergedQuads == 0);
    CHECK(partition.lines == 12); // one along each edge of the cube, from corner to corner
    REQUIRE(partition.rectangles.size() == 6);
    std::vector<bool> seen(cage.faceCount(), false);
    for (const QuadRectangle& r : partition.rectangles) {
        CHECK(r.cellsU == 3);
        CHECK(r.cellsV == 3);
        REQUIRE(r.faces.size() == 9);
        for (const std::uint32_t f : r.faces) {
            CHECK_FALSE(seen[f]);
            seen[f] = true;
        }
    }
    // The same cage gives the same partition.
    const QuadPartition again = partitionQuads(cage);
    REQUIRE(again.rectangles.size() == 6);
    for (std::size_t k = 0; k < 6; ++k) {
        CHECK(again.rectangles[k].faces == partition.rectangles[k].faces);
        CHECK(again.rectangles[k].rotations == partition.rectangles[k].rotations);
    }
}

TEST_CASE("Merging faces leaves the surface as it was", "[stages][merge]") {
    const QuadLayout cage = cubeGridCage(3, 30.0);
    SurfaceBuildParams p;
    p.fit = false;
    p.merge = false;
    const auto single = buildSurface(cage, nullptr, p);
    REQUIRE(single);
    p.merge = true;
    const auto merged = buildSurface(cage, nullptr, p);
    REQUIRE(merged);

    const SolidSummary& a = single->solid.summary();
    const SolidSummary& b = merged->solid.summary();
    CHECK(a.faceCount == 54);
    CHECK(b.faceCount == 6);
    CHECK(merged->facesBeforeMerge == 54);
    CHECK(merged->mergedFaces == 6);
    CHECK(merged->unmergedQuads == 0);
    CHECK(b.valid);
    CHECK(b.closed);
    CHECK(b.solidCount == 1);
    CHECK(b.freeEdges == 0);
    CHECK(std::abs(b.volumeMm3 - a.volumeMm3) < 1e-6 * a.volumeMm3);
    CHECK(std::abs(b.areaMm2 - a.areaMm2) < 1e-6 * a.areaMm2);
    // No pole count check here: every merged side has a cap at each of its four corners, and a
    // cap's knot lines (where the surface is not C2) run across the whole merged face.
    for (const SurfacePatch& f : merged->solid.faces()) {
        CHECK(f.cellsU == 3);
        CHECK(f.cellsV == 3);
        CHECK(f.capPieces > 0); // the cube's corners are extraordinary vertices
    }
    // Points of the unmerged faces lie on the merged ones, and the other way round.
    const auto there = deviationToSolid(single->solid.display(), merged->solid, 0.1);
    REQUIRE(there);
    CHECK(std::max(std::abs(there->stats().minMm), there->stats().maxMm) < 1e-6);
    const auto back = deviationToSolid(merged->solid.display(), single->solid, 0.1);
    REQUIRE(back);
    CHECK(std::max(std::abs(back->stats().minMm), back->stats().maxMm) < 1e-6);
}

TEST_CASE("A merged sphere scan is a valid closed solid with far fewer faces", "[stages][merge]") {
    const double r = 20.0;
    const TriMesh sphere = makeSphere(r, 5);
    QuadRemeshParams q;
    q.targetFaces = 400;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    SurfaceBuildParams p;
    p.merge = false;
    const auto single = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(single);
    p.merge = true;
    const auto merged = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(merged);
    const SolidSummary& s = merged->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.shellCount == 1);
    CHECK(s.freeEdges == 0);
    CHECK(s.faceCount == merged->solid.faces().size());
    CHECK(s.faceCount < single->solid.summary().faceCount / 4);
    // Knots where the pieces join smoothly are gone: fewer control points.
    CHECK(s.poleCount < single->solid.summary().poleCount);
    CHECK(merged->facesBeforeMerge == single->solid.summary().faceCount);
    CHECK(std::abs(s.volumeMm3 - single->solid.summary().volumeMm3) <
          1e-6 * single->solid.summary().volumeMm3);
    // The deviation from the scan is the same.
    const auto a = deviationToSolid(sphere, single->solid, 0.1);
    const auto b = deviationToSolid(sphere, merged->solid, 0.1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(std::abs(a->stats().p95AbsMm - b->stats().p95AbsMm) < 1e-6);
    CHECK(std::abs(a->stats().maxMm - b->stats().maxMm) < 1e-6);
}

TEST_CASE("A cage with no irregular vertex is left unmerged", "[stages][merge]") {
    // A torus of 12 x 8 regular quads has no vertex to trace a line from: its one region is a
    // closed band with no corner, so every quad stays a face of its own.
    std::vector<Vec3d> p;
    const int n = 12, m = 8;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            const double u = 2.0 * std::numbers::pi * i / n, v = 2.0 * std::numbers::pi * j / m;
            p.push_back({(30.0 + 10.0 * std::cos(v)) * std::cos(u),
                         (30.0 + 10.0 * std::cos(v)) * std::sin(u),
                         10.0 * std::sin(v)});
        }
    }
    std::vector<std::uint32_t> starts{0}, verts;
    const auto at = [&](int i, int j) { return static_cast<std::uint32_t>((i % n) * m + j % m); };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            verts.insert(verts.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
            starts.push_back(static_cast<std::uint32_t>(verts.size()));
        }
    }
    const QuadLayout torus = *QuadLayout::create(std::move(p), std::move(starts), std::move(verts));
    const QuadPartition partition = partitionQuads(torus);
    CHECK(partition.lines == 0);
    CHECK(partition.unmergedQuads == torus.faceCount());
    CHECK(partition.rectangles.size() == torus.faceCount());
    SurfaceBuildParams params;
    params.fit = false;
    const auto built = buildSurface(torus, nullptr, params);
    REQUIRE(built);
    CHECK(built->mergedFaces == 0);
    CHECK(built->solid.summary().faceCount == torus.faceCount());
    CHECK(built->solid.summary().valid);
}

TEST_CASE("Merging refuses faces that do not match the cage", "[stages][merge]") {
    const QuadLayout cage = cubeGridCage(2, 10.0);
    auto patches = extractPatches(cage, PatchParams{});
    REQUIRE(patches);
    CHECK(mergeFaces(cage, patches->faces));
    auto wrong = patches->faces;
    wrong.pop_back(); // a quad with no face
    CHECK_FALSE(mergeFaces(cage, wrong));
    wrong = patches->faces;
    wrong.front().cageFace = static_cast<std::uint32_t>(cage.faceCount());
    CHECK_FALSE(mergeFaces(cage, wrong));
}

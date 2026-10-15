// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/MeshStats.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Stats of a closed cube", "[core][stats]") {
    const MeshStats s = computeStats(makeCube());
    CHECK(s.vertexCount == 8);
    CHECK(s.faceCount == 12);
    CHECK(s.edgeCount == 18);
    CHECK(s.boundaryEdgeCount == 0);
    CHECK(s.nonManifoldEdgeCount == 0);
    CHECK(s.degenerateFaceCount == 0);
    CHECK(s.isWatertight());
    CHECK_THAT(s.surfaceArea, WithinAbs(6.0, 1e-12));
    CHECK_THAT(s.signedVolume, WithinAbs(1.0, 1e-12)); // positive: faces point outward
}

TEST_CASE("Flipping all faces gives negative volume", "[core][stats]") {
    auto indices = cubeIndices();
    for (std::size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
    const MeshStats s = computeStats(makeMesh(cubePositions(), indices));
    CHECK_THAT(s.signedVolume, WithinAbs(-1.0, 1e-12));
}

TEST_CASE("Stats find a hole", "[core][stats]") {
    auto indices = cubeIndices();
    indices.erase(indices.begin() + 6, indices.begin() + 12); // remove the top two triangles
    const MeshStats s = computeStats(makeMesh(cubePositions(), indices));
    CHECK(s.faceCount == 10);
    CHECK(s.boundaryEdgeCount == 4);
    CHECK_FALSE(s.isWatertight());
}

TEST_CASE("Stats find a non-manifold edge", "[core][stats]") {
    auto positions = cubePositions();
    positions.push_back({0.5, -1.0, -1.0}); // vertex 8, outside the cube
    auto indices = cubeIndices();
    // Edge 0-1 already has two faces (bottom, front); this third face makes it non-manifold.
    // Its other two edges (1-8 and 8-0) are used once, so they are boundary edges.
    indices.insert(indices.end(), {0, 1, 8});
    const MeshStats s = computeStats(makeMesh(std::move(positions), std::move(indices)));
    CHECK(s.nonManifoldEdgeCount == 1);
    CHECK(s.boundaryEdgeCount == 2);
    CHECK_FALSE(s.isWatertight());
}

TEST_CASE("Stats count degenerate faces", "[core][stats]") {
    auto indices = cubeIndices();
    indices.insert(indices.end(), {0, 0, 1}); // repeated vertex
    indices.insert(indices.end(), {0, 1, 1});
    const MeshStats s = computeStats(makeMesh(cubePositions(), indices));
    CHECK(s.degenerateFaceCount == 2);
    CHECK(s.isWatertight()); // faces that repeat a vertex do not disturb the topology counts
    CHECK(s.faceCount == 14);
}

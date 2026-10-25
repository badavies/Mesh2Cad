// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <limits>

#include "m2c/core/SpatialIndex.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

TEST_CASE("TriMesh accepts a valid mesh", "[core][trimesh]") {
    const TriMesh cube = makeCube();
    CHECK(cube.vertexCount() == 8);
    CHECK(cube.faceCount() == 12);
    CHECK(cube.bounds().min == Vec3d{0, 0, 0});
    CHECK(cube.bounds().max == Vec3d{1, 1, 1});
    CHECK(cube.face(0) == std::array<std::uint32_t, 3>{0, 2, 1});
}

TEST_CASE("TriMesh copies share their buffers", "[core][trimesh]") {
    const TriMesh a = makeCube();
    const TriMesh b = a;
    CHECK(a.sharedPositions().get() == b.sharedPositions().get());
    CHECK(a.sharedIndices().get() == b.sharedIndices().get());
}

TEST_CASE("TriMesh hands out its spatial index only once built", "[core][trimesh]") {
    const TriMesh a = makeCube();
    const TriMesh b = a;
    CHECK(a.builtSpatialIndex() == nullptr); // asking does not build it
    CHECK(a.builtSpatialIndex() == nullptr);
    const SpatialIndex& index = b.spatialIndex();
    CHECK(a.builtSpatialIndex() == &index); // shared by the copies
}

TEST_CASE("TriMesh rejects invalid input", "[core][trimesh]") {
    SECTION("null buffers") {
        const auto mesh = TriMesh::create(TriMesh::PositionBuffer{}, TriMesh::IndexBuffer{});
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("no vertices") {
        CHECK_FALSE(
            TriMesh::create(std::vector<Vec3d>{}, std::vector<std::uint32_t>{0, 1, 2}).has_value());
    }
    SECTION("no faces") {
        CHECK_FALSE(TriMesh::create(cubePositions(), std::vector<std::uint32_t>{}).has_value());
    }
    SECTION("index count not a multiple of three") {
        CHECK_FALSE(
            TriMesh::create(cubePositions(), std::vector<std::uint32_t>{0, 1, 2, 3}).has_value());
    }
    SECTION("index out of range") {
        const auto mesh = TriMesh::create(cubePositions(), std::vector<std::uint32_t>{0, 1, 8});
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().message.find("face 0") != std::string::npos);
    }
    SECTION("non-finite coordinate") {
        auto positions = cubePositions();
        positions[3].y = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(
            TriMesh::create(std::move(positions), std::vector<std::uint32_t>{0, 1, 2}).has_value());
    }
}

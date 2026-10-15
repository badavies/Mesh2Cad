// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <limits>
#include <sstream>

#include "m2c/core/MeshStats.h"
#include "m2c/io/MeshIO.h"
#include "m2c/io/Stl.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

Result<TriMesh> readFrom(const std::string& bytes) {
    std::istringstream in(bytes, std::ios::in | std::ios::binary);
    return readStl(in);
}

std::string writeToString(const TriMesh& mesh) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    REQUIRE(writeStl(mesh, out).has_value());
    return out.str();
}

constexpr const char* kTwoTriangleQuad = R"(solid quad
  facet normal 0 0 1
    outer loop
      vertex 0 0 0
      vertex 1 0 0
      vertex 1 1 0
    endloop
  endfacet
  facet normal 0 0 1
    outer loop
      vertex 0 0 0
      vertex 1.0E+00 1 0
      vertex +0 1 -0
    endloop
  endfacet
endsolid quad
)";

} // namespace

TEST_CASE("Binary STL round trip welds the cube back to 8 vertices", "[io][stl]") {
    const std::string bytes = writeToString(makeCube());
    CHECK(bytes.size() == 84 + 50 * 12);

    const auto mesh = readFrom(bytes);
    REQUIRE(mesh.has_value());
    const MeshStats s = computeStats(*mesh);
    CHECK(s.vertexCount == 8);
    CHECK(s.faceCount == 12);
    CHECK(s.isWatertight());
    CHECK(s.signedVolume > 0.99);
}

TEST_CASE("ASCII STL is parsed and vertices are welded", "[io][stl]") {
    const auto mesh = readFrom(kTwoTriangleQuad);
    REQUIRE(mesh.has_value());
    CHECK(mesh->faceCount() == 2);
    CHECK(mesh->vertexCount() == 4); // -0 and +0 weld to the same vertex
    CHECK(mesh->face(1) == std::array<std::uint32_t, 3>{0, 2, 3});
}

TEST_CASE("STL reader reports malformed input", "[io][stl]") {
    SECTION("empty") {
        const auto mesh = readFrom("");
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::Parse);
    }
    SECTION("garbage") {
        CHECK(readFrom(std::string(200, 'x')).error().code == ErrorCode::Parse);
    }
    SECTION("truncated binary falls through to ASCII and fails") {
        std::string bytes = writeToString(makeCube());
        bytes.resize(bytes.size() - 7);
        CHECK_FALSE(readFrom(bytes).has_value());
    }
    SECTION("ASCII with a missing coordinate") {
        const auto mesh = readFrom("solid x\nvertex 0 0\nendsolid\n");
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::Parse);
    }
    SECTION("ASCII with a vertex count that is not a multiple of three") {
        CHECK_FALSE(readFrom("solid x\nvertex 0 0 0\nvertex 1 0 0\nendsolid\n").has_value());
    }
    SECTION("ASCII with a non-finite coordinate") {
        CHECK_FALSE(readFrom("solid x\nvertex 0 0 nan\nvertex 1 0 0\nvertex 0 1 0\nendsolid\n")
                        .has_value());
    }
    SECTION("binary with a NaN coordinate") {
        std::string bytes = writeToString(makeCube());
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(bytes.data() + 84 + 12, &nan, sizeof(nan));
        const auto mesh = readFrom(bytes);
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::Parse);
    }
    SECTION("binary header claiming zero triangles") {
        CHECK_FALSE(readFrom(std::string(84, '\0')).has_value());
    }
}

TEST_CASE("readMesh and writeMesh work on files", "[io][stl]") {
    const TempDir dir;
    const auto path = dir.path() / "cube.STL"; // extension matching is case-insensitive

    REQUIRE(writeMesh(makeCube(), path).has_value());
    const auto mesh = readMesh(path);
    REQUIRE(mesh.has_value());
    CHECK(computeStats(*mesh).isWatertight());

    CHECK(readMesh(dir.path() / "missing.stl").error().code == ErrorCode::Io);
    CHECK(readMesh(dir.path() / "missing.obj").error().code == ErrorCode::Io);
    CHECK(readMesh(dir.path() / "a.xyz").error().code == ErrorCode::Unsupported);
    CHECK(writeMesh(makeCube(), dir.path() / "a.xyz").error().code == ErrorCode::Unsupported);
}

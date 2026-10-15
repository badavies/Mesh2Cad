// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <sstream>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/MeshStats.h"
#include "m2c/io/MeshIO.h"
#include "m2c/io/Obj.h"
#include "m2c/io/Ply.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

Result<TriMesh> objFrom(const std::string& text) {
    std::istringstream in(text, std::ios::in | std::ios::binary);
    return readObj(in);
}

Result<TriMesh> plyFrom(const std::string& bytes) {
    std::istringstream in(bytes, std::ios::in | std::ios::binary);
    return readPly(in);
}

template <class T>
void appendRaw(std::string& s, T v, bool bigEndian = false) {
    std::array<char, sizeof(T)> raw{};
    std::memcpy(raw.data(), &v, sizeof(T));
    if (bigEndian) std::reverse(raw.begin(), raw.end());
    s.append(raw.data(), raw.size());
}

} // namespace

TEST_CASE("OBJ quads, slashes and negative indices give a closed cube", "[io][obj]") {
    const auto mesh = objFrom(R"(# cube with quads
o cube
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0 0 1
v 1 0 1
v 1 1 1
v 0 1 1
vt 0 0
vn 0 0 1
f 1//1 4//1 3//1 2//1
f 5/1/1 6/1/1 7/1/1 8/1/1
f 1 2 6 5
f 4 8 7 3
f 1 5 8 4
f -7 -6 -2 -3
)");
    REQUIRE(mesh.has_value());
    const MeshStats s = computeStats(*mesh);
    CHECK(s.vertexCount == 8);
    CHECK(s.faceCount == 12);
    CHECK(s.isWatertight());
    CHECK_THAT(s.signedVolume, Catch::Matchers::WithinAbs(1.0, 1e-12));
}

TEST_CASE("OBJ reader welds duplicates and drops unused vertices", "[io][obj]") {
    const auto mesh = objFrom("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 9 9 9\nv 1 0 0\nv 0 0 -0\n"
                              "f 1 2 3\nf 6 3 5\n");
    REQUIRE(mesh.has_value());
    CHECK(mesh->vertexCount() == 3);
    CHECK(mesh->faceCount() == 2);
}

TEST_CASE("OBJ reader rejects malformed input with a line number", "[io][obj]") {
    const auto badNumber = objFrom("v 0 0 0\nv 1 x 0\n");
    REQUIRE_FALSE(badNumber.has_value());
    CHECK(badNumber.error().code == ErrorCode::Parse);
    CHECK(badNumber.error().message.find("line 2") != std::string::npos);

    CHECK_FALSE(objFrom("v 0 0 0\nv 1 0 0\nf 1 2 3\n").has_value()); // index past the end
    CHECK_FALSE(objFrom("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2\n").has_value());
    CHECK_FALSE(objFrom("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n").has_value());
    CHECK_FALSE(objFrom("v 0 0 0\n").has_value()); // no faces
}

TEST_CASE("OBJ write and read back is bit-identical", "[io][obj]") {
    const TriMesh original =
        makeMesh({{0.1, 0.2, 0.3}, {1.0 / 3.0, -2.5e-7, 4}, {5, 6, 7.000000001}}, {0, 1, 2});
    std::ostringstream out(std::ios::out | std::ios::binary);
    REQUIRE(writeObj(original, out).has_value());
    const auto back = objFrom(out.str());
    REQUIRE(back.has_value());
    CHECK(back->positions() == original.positions());
    CHECK(back->indices() == original.indices());
}

TEST_CASE("ASCII PLY with extra properties and a polygon", "[io][ply]") {
    const auto mesh = plyFrom("ply\r\nformat ascii 1.0\r\ncomment made by hand\r\n"
                              "element vertex 4\r\nproperty float x\r\nproperty float y\r\n"
                              "property float z\r\nproperty uchar red\r\n"
                              "element face 1\r\nproperty list uchar int vertex_indices\r\n"
                              "end_header\r\n0 0 0 255\r\n1 0 0 0\r\n1 1 0 0\r\n0 1 0 0\r\n"
                              "4 0 1 2 3\r\n");
    REQUIRE(mesh.has_value());
    CHECK(mesh->vertexCount() == 4);
    CHECK(mesh->faceCount() == 2);
    CHECK(computeStats(*mesh).surfaceArea == 1.0);
}

TEST_CASE("Binary big-endian PLY with a skipped element", "[io][ply]") {
    std::string bytes = "ply\nformat binary_big_endian 1.0\nelement vertex 3\n"
                        "property double x\nproperty double y\nproperty double z\n"
                        "element material 1\nproperty list uchar uchar name\n"
                        "element face 1\nproperty list uchar uint vertex_index\nend_header\n";
    for (const double v : {0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 2.0, 0.0}) appendRaw(bytes, v, true);
    appendRaw<std::uint8_t>(bytes, 2);
    appendRaw<std::uint8_t>(bytes, 65);
    appendRaw<std::uint8_t>(bytes, 66);
    appendRaw<std::uint8_t>(bytes, 3);
    for (const std::uint32_t i : {0U, 1U, 2U}) appendRaw(bytes, i, true);

    const auto mesh = plyFrom(bytes);
    REQUIRE(mesh.has_value());
    CHECK(mesh->faceCount() == 1);
    CHECK(computeStats(*mesh).surfaceArea == 2.0);
}

TEST_CASE("Binary PLY write and read back is bit-identical", "[io][ply]") {
    const TriMesh cube = makeCube();
    std::ostringstream out(std::ios::out | std::ios::binary);
    REQUIRE(writePly(cube, out).has_value());
    const auto back = plyFrom(out.str());
    REQUIRE(back.has_value());
    CHECK(back->positions() == cube.positions());
    CHECK(back->indices() == cube.indices());
}

TEST_CASE("PLY reader rejects malformed input", "[io][ply]") {
    CHECK_FALSE(plyFrom("solid x\n").has_value());
    CHECK_FALSE(plyFrom("ply\nformat ascii 1.0\nelement vertex 1\n").has_value()); // no end
    CHECK_FALSE(plyFrom("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n"
                        "end_header\n0\n1\n2\n")
                    .has_value()); // no y, z
    CHECK_FALSE(plyFrom("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n"
                        "property float y\nproperty float z\nelement face 1\n"
                        "property list uchar int vertex_indices\nend_header\n"
                        "0 0 0\n1 0 0\n0 1 0\n3 0 1\n")
                    .has_value()); // truncated face
    const auto lying = plyFrom("ply\nformat binary_little_endian 1.0\nelement vertex 4000000000\n"
                               "property float x\nproperty float y\nproperty float z\n"
                               "end_header\n");
    REQUIRE_FALSE(lying.has_value());
}

TEST_CASE("readMesh and writeMesh choose the format by extension", "[io]") {
    const TempDir dir;
    const TriMesh cube = makeCube();
    for (const char* name : {"c.stl", "c.OBJ", "c.ply"}) {
        const auto path = dir.path() / name;
        REQUIRE(writeMesh(cube, path).has_value());
        const auto back = readMesh(path);
        REQUIRE(back.has_value());
        CHECK(computeStats(*back).isWatertight());
        CHECK(back->faceCount() == 12);
    }
    CHECK(readMesh(dir.path() / "x.3mf").error().code == ErrorCode::Unsupported);
}

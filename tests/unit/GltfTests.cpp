// SPDX-License-Identifier: GPL-3.0-or-later
// The glTF writer (issue #120): GLB and embedded glTF files a glTF reader accepts.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include "m2c/core/Error.h"
#include "m2c/io/Gltf.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;
using Json = nlohmann::json;

namespace {

std::uint32_t word(const std::string& bytes, const std::size_t at) {
    std::uint32_t w = 0;
    std::memcpy(&w, bytes.data() + at, sizeof(w));
    return w;
}

/// A GLB file's JSON and binary chunk, after checking its header and chunk layout.
struct Glb {
    Json json;
    std::string bin;
};

Glb parseGlb(const std::string& bytes) {
    REQUIRE(bytes.size() >= 28);
    CHECK(bytes.substr(0, 4) == "glTF");
    CHECK(word(bytes, 4) == 2);
    CHECK(word(bytes, 8) == bytes.size());
    const std::uint32_t jsonLength = word(bytes, 12);
    CHECK(bytes.substr(16, 4) == "JSON");
    CHECK(jsonLength % 4 == 0);
    const std::size_t binAt = 20 + jsonLength;
    REQUIRE(binAt + 8 <= bytes.size());
    const std::uint32_t binLength = word(bytes, binAt);
    CHECK(binLength % 4 == 0);
    CHECK(bytes.substr(binAt + 4, 4) == std::string("BIN\0", 4));
    CHECK(binAt + 8 + binLength == bytes.size());
    return {.json = Json::parse(bytes.substr(20, jsonLength)),
            .bin = bytes.substr(binAt + 8, binLength)};
}

std::string write(const TriMesh& mesh, const GltfOptions& options) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    REQUIRE(writeGltf(mesh, out, options).has_value());
    return out.str();
}

template <typename T>
std::vector<T> view(const Json& gltf, const std::string& bin, const std::size_t accessor) {
    const Json& a = gltf["accessors"][accessor];
    const Json& v = gltf["bufferViews"][a["bufferView"].get<std::size_t>()];
    const std::size_t components = a["type"] == "VEC3" ? 3 : 1;
    std::vector<T> values(a["count"].get<std::size_t>() * components);
    REQUIRE(v["byteLength"].get<std::size_t>() == values.size() * sizeof(T));
    std::memcpy(values.data(),
                bin.data() + v["byteOffset"].get<std::size_t>(),
                v["byteLength"].get<std::size_t>());
    return values;
}

std::string unbase64(const std::string& text) {
    const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        if (c == '=') break;
        bits = (bits << 6U) | static_cast<std::uint32_t>(digits.find(c));
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<char>((bits >> count) & 0xFFU));
        }
    }
    return out;
}

} // namespace

TEST_CASE("A GLB of a cube is in metres, Y up, with sharp edges", "[io][gltf]") {
    const TriMesh cube = makeBox(30, 20, 10);
    const Glb glb = parseGlb(write(cube, {.name = "block"}));
    const Json& g = glb.json;
    CHECK(g["asset"]["version"] == "2.0");
    CHECK(g["scenes"][0]["nodes"] == Json::array({0}));
    CHECK(g["nodes"][0]["name"] == "block");
    CHECK(g["meshes"][0]["name"] == "block");
    const Json& primitive = g["meshes"][0]["primitives"][0];
    CHECK(primitive["attributes"]["POSITION"] == 0);
    CHECK(primitive["attributes"]["NORMAL"] == 1);
    CHECK(primitive["indices"] == 2);
    CHECK(primitive["mode"] == 4);
    CHECK(g["buffers"][0]["byteLength"] == glb.bin.size());
    CHECK_FALSE(g["buffers"][0].contains("uri"));

    // Every corner of a box has three faces at right angles, so three normals each.
    CHECK(g["accessors"][0]["count"] == 24);
    CHECK(g["accessors"][2]["count"] == 36);
    // x stays, z becomes y, y becomes -z; millimetres become metres.
    const Json& lo = g["accessors"][0]["min"];
    const Json& hi = g["accessors"][0]["max"];
    CHECK_THAT(lo[0].get<double>(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(hi[0].get<double>(), WithinAbs(0.030, 1e-7));
    CHECK_THAT(hi[1].get<double>(), WithinAbs(0.010, 1e-7));
    CHECK_THAT(lo[2].get<double>(), WithinAbs(-0.020, 1e-7));
    CHECK_THAT(hi[2].get<double>(), WithinAbs(0.0, 1e-9));

    const auto positions = view<float>(g, glb.bin, 0);
    const auto normals = view<float>(g, glb.bin, 1);
    const auto indices = view<std::uint32_t>(g, glb.bin, 2);
    for (const std::uint32_t i : indices) CHECK(i < 24);
    for (std::size_t v = 0; v < 24; ++v) {
        const std::array n{normals[3 * v], normals[3 * v + 1], normals[3 * v + 2]};
        // Axis-aligned unit normals, and the triangles' winding agrees with them.
        CHECK_THAT(std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]), WithinAbs(1.0, 1e-6));
    }
    for (std::size_t t = 0; t < indices.size(); t += 3) {
        const auto p = [&](const std::size_t k) {
            const std::uint32_t i = indices[t + k];
            return Vec3d{positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]};
        };
        const Vec3d face = cross(p(1) - p(0), p(2) - p(0));
        const std::uint32_t i = indices[t];
        const Vec3d n{normals[3 * i], normals[3 * i + 1], normals[3 * i + 2]};
        CHECK(dot(face, n) > 0.0);
    }
    // The top face (+z here) faces +y in glTF.
    bool up = false;
    for (std::size_t v = 0; v < 24; ++v) up = up || normals[3 * v + 1] > 0.99F;
    CHECK(up);
}

TEST_CASE("A sphere shades smooth: one normal per vertex", "[io][gltf]") {
    const TriMesh ball = makeSphere(10.0, 3);
    const Glb glb = parseGlb(write(ball, {}));
    CHECK(glb.json["accessors"][0]["count"] == ball.vertexCount());
    const auto positions = view<float>(glb.json, glb.bin, 0);
    const auto normals = view<float>(glb.json, glb.bin, 1);
    for (std::size_t v = 0; v < ball.vertexCount(); ++v) {
        const Vec3d p{positions[3 * v], positions[3 * v + 1], positions[3 * v + 2]};
        const Vec3d n{normals[3 * v], normals[3 * v + 1], normals[3 * v + 2]};
        CHECK_THAT(length(n), WithinAbs(1.0, 1e-6));
        CHECK(dot(p, n) / length(p) > 0.99); // pointing out from the centre
    }
    // A crease angle of zero splits every vertex into one per triangle direction.
    const Glb flat = parseGlb(write(ball, {.creaseAngleDeg = 0.0}));
    CHECK(flat.json["accessors"][0]["count"] > ball.vertexCount());
}

TEST_CASE("A .gltf embeds its buffer as base64", "[io][gltf]") {
    const TriMesh cube = makeCube();
    const std::string text = write(cube, {.binary = false});
    const Json g = Json::parse(text);
    const std::string uri = g["buffers"][0]["uri"];
    const std::string prefix = "data:application/octet-stream;base64,";
    REQUIRE(uri.starts_with(prefix));
    const std::string bin = unbase64(uri.substr(prefix.size()));
    CHECK(bin.size() == g["buffers"][0]["byteLength"].get<std::size_t>());
    // The same bytes as the GLB's binary chunk.
    const Glb glb = parseGlb(write(cube, {}));
    CHECK(glb.bin.substr(0, bin.size()) == bin);
}

TEST_CASE("The glTF writer refuses a bad crease angle", "[io][gltf]") {
    std::ostringstream out;
    CHECK(writeGltf(makeCube(), out, {.creaseAngleDeg = -1.0}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(writeGltf(makeCube(), out, {.creaseAngleDeg = 181.0}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(out.str().empty());
}

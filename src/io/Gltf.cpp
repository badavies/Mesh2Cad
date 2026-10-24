// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Gltf.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ios>
#include <limits>
#include <numbers>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "m2c/core/Base64.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

static_assert(std::endian::native == std::endian::little,
              "glTF buffers are little-endian, and the writer copies floats as they are.");

constexpr std::uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr std::uint32_t kJsonChunk = 0x4E4F534A; // "JSON"
constexpr std::uint32_t kBinChunk = 0x004E4942;  // "BIN\0"
constexpr int kArrayBuffer = 34962;              // bufferView targets
constexpr int kElementArrayBuffer = 34963;
constexpr int kFloat = 5126; // accessor component types
constexpr int kUnsignedInt = 5125;
constexpr double kMetresPerMm = 0.001;

/// glTF's frame from ours: Z up becomes Y up, a quarter turn about x (x, y, z) -> (x, z, -y).
std::array<float, 3> toGltf(const Vec3d& v, const double scale) {
    return {static_cast<float>(v.x * scale),
            static_cast<float>(v.z * scale),
            static_cast<float>(-v.y * scale)};
}

/// The vertices written, with a normal per triangle corner (triangles of an index buffer).
struct Corners {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::uint32_t> indices;
};

/// One vertex per mesh vertex and distinct corner normal; see writeGltf().
Result<Corners> splitAtCreases(const TriMesh& mesh, const double creaseAngleDeg) {
    const auto& pos = mesh.positions();
    const std::size_t faces = mesh.faceCount();
    std::vector<Vec3d> weighted(faces), unit(faces); // area-weighted and unit face normals
    for (std::size_t f = 0; f < faces; ++f) {
        const auto [a, b, c] = mesh.face(f);
        weighted[f] = cross(pos[b] - pos[a], pos[c] - pos[a]);
        const double len = length(weighted[f]);
        unit[f] = len > 0.0 ? weighted[f] * (1.0 / len) : Vec3d{};
    }
    // The faces around each vertex, in face order.
    std::vector<std::uint32_t> start(mesh.vertexCount() + 1, 0);
    for (const std::uint32_t v : mesh.indices()) ++start[v + 1];
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) start[v + 1] += start[v];
    std::vector<std::uint32_t> around(mesh.indices().size());
    {
        std::vector next(start.begin(), start.end() - 1);
        for (std::size_t f = 0; f < faces; ++f) {
            for (const auto [a, b, c] = mesh.face(f); const std::uint32_t v : {a, b, c})
                around[next[v]++] = static_cast<std::uint32_t>(f);
        }
    }

    const double cosCrease = std::cos(creaseAngleDeg * std::numbers::pi / 180.0);
    Corners out;
    out.indices.reserve(mesh.indices().size());
    // Per mesh vertex, the normals already written for it and their vertex ids.
    std::vector<std::vector<std::pair<std::array<float, 3>, std::uint32_t>>> written(
        mesh.vertexCount());
    for (std::size_t f = 0; f < faces; ++f) {
        for (const auto [a, b, c] = mesh.face(f); const std::uint32_t v : {a, b, c}) {
            Vec3d sum{};
            for (std::uint32_t k = start[v]; k < start[v + 1]; ++k) {
                // A degenerate face has no direction of its own and takes all its neighbours'.
                if (const std::uint32_t g = around[k];
                    unit[f] == Vec3d{} || dot(unit[g], unit[f]) >= cosCrease)
                    sum = sum + weighted[g];
            }
            const double len = length(sum);
            const Vec3d n = len > 0.0 ? sum * (1.0 / len) : Vec3d{.x = 0, .y = 0, .z = 1};
            const std::array<float, 3> normal = toGltf(n, 1.0);
            auto& seen = written[v];
            const auto it = std::ranges::find(
                seen, normal, &std::pair<std::array<float, 3>, std::uint32_t>::first);
            if (it != seen.end()) {
                out.indices.push_back(it->second);
                continue;
            }
            if (out.positions.size() == std::numeric_limits<std::uint32_t>::max()) {
                return fail(ErrorCode::Unsupported, "glTF indices cannot pass 2^32-1 vertices");
            }
            const auto id = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(toGltf(pos[v], kMetresPerMm));
            out.normals.push_back(normal);
            seen.emplace_back(normal, id);
            out.indices.push_back(id);
        }
    }
    return out;
}

template <typename T>
void append(std::string& bytes, const std::vector<T>& values) {
    const std::size_t at = bytes.size();
    bytes.resize(at + values.size() * sizeof(T));
    std::memcpy(bytes.data() + at, values.data(), values.size() * sizeof(T));
}

void writeWord(std::ostream& out, const std::uint32_t word) {
    out.write(reinterpret_cast<const char*>(&word), sizeof(word));
}

} // namespace

Result<void> writeGltf(const TriMesh& mesh, std::ostream& out, const GltfOptions& options) {
    if (!(options.creaseAngleDeg >= 0.0) || !(options.creaseAngleDeg <= 180.0)) {
        return fail(ErrorCode::InvalidArgument, "the crease angle must be 0 to 180 degrees");
    }
    auto corners = splitAtCreases(mesh, options.creaseAngleDeg);
    if (!corners) return std::unexpected(corners.error());

    std::string buffer;
    append(buffer, corners->positions);
    const std::size_t normalsAt = buffer.size();
    append(buffer, corners->normals);
    const std::size_t indicesAt = buffer.size();
    append(buffer, corners->indices);
    if (buffer.size() > std::numeric_limits<std::uint32_t>::max() - 1024) {
        return fail(ErrorCode::Unsupported, "the mesh is too large for one glTF buffer");
    }

    std::array<float, 3> lo{}, hi{};
    lo.fill(std::numeric_limits<float>::max());
    hi.fill(std::numeric_limits<float>::lowest());
    for (const auto& p : corners->positions) {
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    const std::size_t vertices = corners->positions.size();
    using Json = nlohmann::ordered_json;
    Json bufferJson{{"byteLength", buffer.size()}};
    if (!options.binary)
        bufferJson["uri"] =
            "data:application/octet-stream;base64," +
            encodeBase64({reinterpret_cast<const std::uint8_t*>(buffer.data()), buffer.size()});
    const Json gltf{
        {"asset", {{"version", "2.0"}, {"generator", "Mesh2Cad"}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", {0}}}})},
        {"nodes", Json::array({{{"name", options.name}, {"mesh", 0}}})},
        {"meshes",
         Json::array({{{"name", options.name},
                       {"primitives",
                        Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}},
                                      {"indices", 2},
                                      {"material", 0},
                                      {"mode", 4}}})}}})},
        {"materials",
         Json::array({{{"name", "Mesh2Cad grey"},
                       {"pbrMetallicRoughness",
                        {{"baseColorFactor", {0.8, 0.8, 0.8, 1.0}},
                         {"metallicFactor", 0.0},
                         {"roughnessFactor", 0.5}}}}})},
        {"buffers", Json::array({bufferJson})},
        {"bufferViews",
         Json::array({{{"buffer", 0},
                       {"byteOffset", 0},
                       {"byteLength", normalsAt},
                       {"target", kArrayBuffer}},
                      {{"buffer", 0},
                       {"byteOffset", normalsAt},
                       {"byteLength", indicesAt - normalsAt},
                       {"target", kArrayBuffer}},
                      {{"buffer", 0},
                       {"byteOffset", indicesAt},
                       {"byteLength", buffer.size() - indicesAt},
                       {"target", kElementArrayBuffer}}})},
        {"accessors",
         Json::array(
             {{{"bufferView", 0},
               {"componentType", kFloat},
               {"count", vertices},
               {"type", "VEC3"},
               {"min", lo},
               {"max", hi}},
              {{"bufferView", 1}, {"componentType", kFloat}, {"count", vertices}, {"type", "VEC3"}},
              {{"bufferView", 2},
               {"componentType", kUnsignedInt},
               {"count", corners->indices.size()},
               {"type", "SCALAR"}}})}};

    if (!options.binary) {
        out << gltf.dump(1) << '\n';
    } else {
        // GLB: a 12-byte header, then the JSON chunk padded with spaces and the binary chunk
        // padded with zeros, each to a multiple of four bytes.
        std::string json = gltf.dump();
        json.resize((json.size() + 3) / 4 * 4, ' ');
        buffer.resize((buffer.size() + 3) / 4 * 4, '\0');
        writeWord(out, kGlbMagic);
        writeWord(out, 2);
        writeWord(out, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + buffer.size()));
        writeWord(out, static_cast<std::uint32_t>(json.size()));
        writeWord(out, kJsonChunk);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        writeWord(out, static_cast<std::uint32_t>(buffer.size()));
        writeWord(out, kBinChunk);
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }
    out.flush();
    if (!out) return fail(ErrorCode::Io, "failed while writing glTF data");
    return {};
}

} // namespace m2c

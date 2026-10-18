// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Stl.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

static_assert(std::endian::native == std::endian::little,
              "The binary STL code assumes a little-endian host.");

constexpr std::size_t kHeaderSize = 80;
constexpr std::size_t kPreambleSize = kHeaderSize + 4; // header + triangle count
constexpr std::size_t kTriangleSize = 50;              // normal + 3 vertices + attribute
constexpr std::size_t kChunkTriangles = 4096;

struct VertexKey {
    std::array<std::uint32_t, 3> bits;
    friend bool operator==(const VertexKey&, const VertexKey&) = default;
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ULL; // FNV-1a over the three 32-bit patterns
        for (const std::uint32_t b : k.bits) {
            h ^= b;
            h *= 1099511628211ULL;
        }
        h ^= h >> 32U;
        return h;
    }
};

/// Builds an indexed mesh from triangle soup. Vertices are welded on exact float equality.
/// Indices are assigned in first-seen order, so the result is deterministic.
class Welder {
public:
    /// Returns false (and adds nothing) if a coordinate is not finite.
    bool add(const float x, const float y, const float z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        // "+ 0.0f" turns -0.0 into +0.0 so both weld to the same vertex.
        const VertexKey key{{std::bit_cast<std::uint32_t>(x + 0.0f),
                             std::bit_cast<std::uint32_t>(y + 0.0f),
                             std::bit_cast<std::uint32_t>(z + 0.0f)}};
        const auto [it, inserted] =
            lookup_.try_emplace(key, static_cast<std::uint32_t>(positions_.size()));
        if (inserted) {
            positions_.push_back({.x = static_cast<double>(x),
                                  .y = static_cast<double>(y),
                                  .z = static_cast<double>(z)});
        }
        indices_.push_back(it->second);
        return true;
    }

    /// A closed mesh has about half as many vertices as triangles.
    void reserveTriangles(const std::size_t n) {
        indices_.reserve(n * 3);
        lookup_.reserve(n / 2);
        positions_.reserve(n / 2);
    }
    [[nodiscard]] std::size_t indexCount() const noexcept { return indices_.size(); }

    [[nodiscard]] Result<TriMesh> finish() {
        if (indices_.empty()) return fail(ErrorCode::Parse, "STL contains no triangles");
        return TriMesh::create(std::move(positions_), std::move(indices_));
    }

private:
    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> lookup_;
    std::vector<Vec3d> positions_;
    std::vector<std::uint32_t> indices_;
};

std::optional<float> parseFloat(std::string_view s) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    float value = 0.0f;
    const char* const end = s.data() + s.size();
    if (const auto [ptr, ec] = std::from_chars(s.data(), end, value);
        ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string lower(std::string s) {
    std::ranges::transform(
        s, s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Result<TriMesh> readBinary(std::istream& in, const std::uint32_t triangleCount) {
    in.clear();
    in.seekg(kPreambleSize, std::ios::beg);

    Welder welder;
    welder.reserveTriangles(triangleCount);
    std::vector<char> buffer;
    std::size_t done = 0;
    while (done < triangleCount) {
        const std::size_t chunk = std::min<std::size_t>(kChunkTriangles, triangleCount - done);
        buffer.resize(chunk * kTriangleSize);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!in) return fail(ErrorCode::Io, "unexpected end of STL file");

        for (std::size_t t = 0; t < chunk; ++t) {
            std::array<float, 9> v{};
            // Skip the 12-byte facet normal; it is recomputed from the vertices when needed.
            std::memcpy(v.data(), buffer.data() + t * kTriangleSize + 12, sizeof(v));
            for (std::size_t k = 0; k < 3; ++k) {
                if (!welder.add(v[3 * k], v[3 * k + 1], v[3 * k + 2])) {
                    return fail(ErrorCode::Parse,
                                "triangle " + std::to_string(done + t) +
                                    " has a non-finite coordinate");
                }
            }
        }
        done += chunk;
    }
    return welder.finish();
}

Result<TriMesh> readAscii(std::istream& in) {
    in.clear();
    in.seekg(0, std::ios::beg);

    std::string token;
    if (!(in >> token) || lower(token) != "solid") {
        return fail(ErrorCode::Parse,
                    "not an STL file: not a binary STL of the expected size and no 'solid' header");
    }

    Welder welder;
    while (in >> token) {
        if (token != "vertex") continue;
        std::array<float, 3> xyz{};
        for (float& c : xyz) {
            if (!(in >> token)) return fail(ErrorCode::Parse, "STL ends in the middle of a vertex");
            const auto value = parseFloat(token);
            if (!value) {
                return fail(ErrorCode::Parse, "invalid number '" + token + "' in STL vertex");
            }
            c = *value;
        }
        if (!welder.add(xyz[0], xyz[1], xyz[2])) {
            return fail(ErrorCode::Parse,
                        "STL vertex " + std::to_string(welder.indexCount()) +
                            " has a non-finite coordinate");
        }
    }
    if (welder.indexCount() % 3 != 0) {
        return fail(ErrorCode::Parse, "STL vertex count is not a multiple of 3 (truncated file?)");
    }
    return welder.finish();
}

} // namespace

Result<TriMesh> readStl(std::istream& in) {
    in.seekg(0, std::ios::end);
    const auto endPos = in.tellg();
    if (!in || endPos == std::istream::pos_type(-1)) {
        return fail(ErrorCode::Io, "cannot determine STL size (stream is not seekable)");
    }
    const auto size = static_cast<std::uint64_t>(static_cast<std::streamoff>(endPos));
    in.seekg(0, std::ios::beg);

    if (size >= kPreambleSize) {
        std::array<char, kPreambleSize> preamble{};
        in.read(preamble.data(), preamble.size());
        if (!in) return fail(ErrorCode::Io, "cannot read STL header");
        std::uint32_t count = 0;
        std::memcpy(&count, preamble.data() + kHeaderSize, sizeof(count));
        if (size == kPreambleSize + static_cast<std::uint64_t>(count) * kTriangleSize) {
            return readBinary(in, count);
        }
    }
    return readAscii(in);
}

Result<void> writeStl(const TriMesh& mesh, std::ostream& out) {
    if (mesh.faceCount() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::Unsupported, "binary STL cannot hold more than 2^32-1 triangles");
    }

    std::array<char, kHeaderSize> header{};
    constexpr std::string_view tag = "Mesh2Cad binary STL";
    std::ranges::copy(tag, header.begin());
    out.write(header.data(), header.size());

    const auto count = static_cast<std::uint32_t>(mesh.faceCount());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    const auto& pos = mesh.positions();
    std::vector<char> buffer;
    for (std::size_t first = 0; first < mesh.faceCount(); first += kChunkTriangles) {
        const std::size_t chunk = std::min<std::size_t>(kChunkTriangles, mesh.faceCount() - first);
        buffer.assign(chunk * kTriangleSize, 0);
        for (std::size_t t = 0; t < chunk; ++t) {
            const auto [a, b, c] = mesh.face(first + t);
            const Vec3d n = cross(pos[b] - pos[a], pos[c] - pos[a]);
            const double len = length(n);
            const auto [ux, uy, uz] = len > 0.0 ? n * (1.0 / len) : Vec3d{};

            const std::array values{static_cast<float>(ux),
                                    static_cast<float>(uy),
                                    static_cast<float>(uz),
                                    static_cast<float>(pos[a].x),
                                    static_cast<float>(pos[a].y),
                                    static_cast<float>(pos[a].z),
                                    static_cast<float>(pos[b].x),
                                    static_cast<float>(pos[b].y),
                                    static_cast<float>(pos[b].z),
                                    static_cast<float>(pos[c].x),
                                    static_cast<float>(pos[c].y),
                                    static_cast<float>(pos[c].z)};
            std::memcpy(buffer.data() + t * kTriangleSize, values.data(), sizeof(values));
            // The 2-byte attribute field stays zero.
        }
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }

    out.flush();
    if (!out) return fail(ErrorCode::Io, "failed while writing STL data");
    return {};
}

} // namespace m2c

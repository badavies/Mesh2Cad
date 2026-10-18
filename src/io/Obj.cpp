// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Obj.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

#include "detail/Parse.h"

namespace m2c {

using namespace io_detail;

Result<TriMesh> readObj(std::istream& in) {
    LineReader reader(in);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> poly;

    const auto where = [&reader] { return "OBJ line " + std::to_string(reader.lineNumber()); };

    while (const auto lineOpt = reader.next()) {
        std::string_view line = *lineOpt;
        if (const std::string_view keyword = nextToken(line); keyword == "v") {
            std::array<double, 3> xyz{};
            for (double& c : xyz) {
                const std::string_view token = nextToken(line);
                const auto value = parseDouble(token);
                if (!value) {
                    return fail(ErrorCode::Parse,
                                where() + ": invalid vertex coordinate '" + std::string(token) +
                                    "'");
                }
                c = *value;
            }
            // Optional w or vertex colour values after x y z are ignored.
            positions.push_back({.x = xyz[0], .y = xyz[1], .z = xyz[2]});
        } else if (keyword == "f") {
            poly.clear();
            for (std::string_view token = nextToken(line); !token.empty();
                 token = nextToken(line)) {
                const std::string_view vertexPart = token.substr(0, token.find('/'));
                const auto value = parseInt(vertexPart);
                if (!value || *value == 0) {
                    return fail(ErrorCode::Parse,
                                where() + ": invalid face index '" + std::string(token) + "'");
                }
                // OBJ indices are 1-based; negative ones count back from the last vertex so far.
                const auto count = static_cast<std::int64_t>(positions.size());
                const std::int64_t index = *value > 0 ? *value - 1 : count + *value;
                if (index < 0 || index >= count) {
                    return fail(ErrorCode::Parse,
                                where() + ": face index " + std::to_string(*value) +
                                    " refers to a vertex that is not defined yet");
                }
                poly.push_back(static_cast<std::uint32_t>(index));
            }
            if (poly.size() < 3) {
                return fail(ErrorCode::Parse, where() + ": face has fewer than three vertices");
            }
            appendFan(poly, indices);
        }
        // vt, vn, g, o, s, usemtl, mtllib, l, p, comments and blank lines are ignored.
        if (positions.size() > std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::Unsupported, "OBJ has more than 2^32-1 vertices");
        }
    }
    if (in.bad()) return fail(ErrorCode::Io, "error while reading OBJ data");
    return weldAndCompact(positions, indices, "OBJ");
}

Result<void> writeObj(const TriMesh& mesh, std::ostream& out) {
    std::string buffer;
    buffer.reserve(std::size_t{1} << 16U);
    const auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };
    std::array<char, 32> number{};
    const auto append = [&](const double v) {
        const auto [ptr, ec] = std::to_chars(number.data(), number.data() + number.size(), v);
        buffer.push_back(' ');
        buffer.append(number.data(), ptr);
    };

    buffer += "# Mesh2Cad OBJ\n";
    for (const auto& [x, y, z] : mesh.positions()) {
        buffer += 'v';
        append(x);
        append(y);
        append(z);
        buffer += '\n';
        if (buffer.size() > 60000) flush();
    }
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        buffer += 'f';
        for (const std::uint32_t v : mesh.face(f)) {
            const auto [ptr, ec] =
                std::to_chars(number.data(), number.data() + number.size(), std::uint64_t{v} + 1);
            buffer.push_back(' ');
            buffer.append(number.data(), ptr);
        }
        buffer += '\n';
        if (buffer.size() > 60000) flush();
    }
    flush();
    out.flush();
    if (!out) return fail(ErrorCode::Io, "failed while writing OBJ data");
    return {};
}

} // namespace m2c

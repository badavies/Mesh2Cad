// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Ply.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "detail/Parse.h"

namespace m2c {

using namespace io_detail;

namespace {

static_assert(std::endian::native == std::endian::little,
              "The binary PLY code assumes a little-endian host.");

enum class Format : std::uint8_t {
    Ascii,
    BinaryLittle,
    BinaryBig
};
enum class Type : std::uint8_t {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Float32,
    Float64
};

std::optional<Type> parseType(const std::string_view s) {
    if (s == "char" || s == "int8") return Type::Int8;
    if (s == "uchar" || s == "uint8") return Type::UInt8;
    if (s == "short" || s == "int16") return Type::Int16;
    if (s == "ushort" || s == "uint16") return Type::UInt16;
    if (s == "int" || s == "int32") return Type::Int32;
    if (s == "uint" || s == "uint32") return Type::UInt32;
    if (s == "float" || s == "float32") return Type::Float32;
    if (s == "double" || s == "float64") return Type::Float64;
    return std::nullopt;
}

std::size_t sizeOf(const Type t) {
    switch (t) {
    case Type::Int8:
    case Type::UInt8:
        return 1;
    case Type::Int16:
    case Type::UInt16:
        return 2;
    case Type::Int32:
    case Type::UInt32:
    case Type::Float32:
        return 4;
    case Type::Float64:
        return 8;
    }
    return 1;
}

struct Property {
    std::string name;
    Type type{Type::Float32};
    bool isList{false};
    Type countType{Type::UInt8};
};

struct Element {
    std::string name;
    std::uint64_t count{0};
    std::vector<Property> properties;
};

struct Header {
    Format format{Format::Ascii};
    std::vector<Element> elements;
};

Result<Header> readHeader(std::istream& in) {
    std::string line;
    const auto getLine = [&] -> bool {
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!getLine() || line != "ply")
        return fail(ErrorCode::Parse, "not a PLY file: no 'ply' magic");

    Header header;
    bool haveFormat = false;
    while (true) {
        if (!getLine()) return fail(ErrorCode::Parse, "PLY header has no 'end_header'");
        std::string_view rest = line;
        const std::string_view keyword = nextToken(rest);
        if (keyword == "end_header") break;
        if (keyword == "comment" || keyword == "obj_info" || keyword.empty()) continue;
        if (keyword == "format") {
            if (const std::string_view f = nextToken(rest); f == "ascii") {
                header.format = Format::Ascii;
            } else if (f == "binary_little_endian") {
                header.format = Format::BinaryLittle;
            } else if (f == "binary_big_endian") {
                header.format = Format::BinaryBig;
            } else {
                return fail(ErrorCode::Parse, "unknown PLY format '" + std::string(f) + "'");
            }
            haveFormat = true;
        } else if (keyword == "element") {
            Element e;
            e.name = std::string(nextToken(rest));
            const auto count = parseInt(nextToken(rest));
            if (e.name.empty() || !count || *count < 0) {
                return fail(ErrorCode::Parse, "invalid PLY element line '" + line + "'");
            }
            e.count = static_cast<std::uint64_t>(*count);
            header.elements.push_back(std::move(e));
        } else if (keyword == "property") {
            if (header.elements.empty()) {
                return fail(ErrorCode::Parse, "PLY property before any element");
            }
            Property p;
            std::string_view typeName = nextToken(rest);
            if (typeName == "list") {
                p.isList = true;
                const auto countType = parseType(nextToken(rest));
                typeName = nextToken(rest);
                if (!countType || *countType == Type::Float32 || *countType == Type::Float64) {
                    return fail(ErrorCode::Parse, "invalid PLY list count type in '" + line + "'");
                }
                p.countType = *countType;
            }
            const auto type = parseType(typeName);
            if (!type) return fail(ErrorCode::Parse, "unknown PLY property type in '" + line + "'");
            p.type = *type;
            p.name = std::string(nextToken(rest));
            header.elements.back().properties.push_back(std::move(p));
        } else {
            return fail(ErrorCode::Parse, "unknown PLY header line '" + line + "'");
        }
    }
    if (!haveFormat) return fail(ErrorCode::Parse, "PLY header has no 'format' line");
    return header;
}

/// Buffered reader for the binary body.
class ByteReader {
public:
    ByteReader(std::istream& in, const bool swap) : in_(in), swap_(swap) {}

    /// Reads one value of type `t` and converts it to double. False at end of data.
    bool read(const Type t, double& value) {
        std::array<unsigned char, 8> raw{};
        const std::size_t n = sizeOf(t);
        if (!take(raw.data(), n)) return false;
        if (swap_) std::reverse(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(n));
        switch (t) {
        case Type::Int8:
            value = static_cast<double>(std::bit_cast<std::int8_t>(raw[0]));
            break;
        case Type::UInt8:
            value = static_cast<double>(raw[0]);
            break;
        case Type::Int16:
            value = static_cast<double>(load<std::int16_t>(raw));
            break;
        case Type::UInt16:
            value = static_cast<double>(load<std::uint16_t>(raw));
            break;
        case Type::Int32:
            value = static_cast<double>(load<std::int32_t>(raw));
            break;
        case Type::UInt32:
            value = static_cast<double>(load<std::uint32_t>(raw));
            break;
        case Type::Float32:
            value = static_cast<double>(load<float>(raw));
            break;
        case Type::Float64:
            value = load<double>(raw);
            break;
        }
        return true;
    }

private:
    template <class T>
    static T load(const std::array<unsigned char, 8>& raw) {
        T v{};
        std::memcpy(&v, raw.data(), sizeof(T));
        return v;
    }

    bool take(unsigned char* dst, std::size_t n) {
        while (n > 0) {
            if (pos_ == size_) {
                in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
                size_ = static_cast<std::size_t>(in_.gcount());
                pos_ = 0;
                if (size_ == 0) return false;
            }
            const std::size_t k = std::min(n, size_ - pos_);
            std::memcpy(dst, buffer_.data() + pos_, k);
            pos_ += k;
            dst += k;
            n -= k;
        }
        return true;
    }

    std::istream& in_;
    bool swap_;
    std::array<char, 1U << 16U> buffer_{};
    std::size_t pos_{0};
    std::size_t size_{0};
};

/// Reads values from an ASCII body token by token, crossing line breaks as needed.
class AsciiReader {
public:
    explicit AsciiReader(std::istream& in) : lines_(in) {}

    bool read(Type, double& value) {
        while (true) {
            if (const std::string_view token = nextToken(rest_); !token.empty()) {
                const auto v = parseDouble(token);
                if (!v) return false;
                value = *v;
                return true;
            }
            const auto line = lines_.next();
            if (!line) return false;
            rest_ = *line;
        }
    }

private:
    LineReader lines_;
    std::string_view rest_;
};

constexpr std::uint64_t kMaxIndex = std::numeric_limits<std::uint32_t>::max();

template <class Reader>
Result<TriMesh> readBody(Reader& reader, const Header& header, const bool weld) {
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> poly;
    bool haveVertices = false;
    bool haveFaces = false;

    for (const auto& [elementName, rowCount, properties] : header.elements) {
        const bool isVertex = elementName == "vertex";
        const bool isFace = elementName == "face";
        std::array xyzSlot{-1, -1, -1};
        int indexSlot = -1;
        for (std::size_t i = 0; i < properties.size(); ++i) {
            const Property& p = properties[i];
            if (isVertex && !p.isList) {
                if (p.name == "x") xyzSlot[0] = static_cast<int>(i);
                if (p.name == "y") xyzSlot[1] = static_cast<int>(i);
                if (p.name == "z") xyzSlot[2] = static_cast<int>(i);
            }
            if (isFace && p.isList && (p.name == "vertex_indices" || p.name == "vertex_index")) {
                indexSlot = static_cast<int>(i);
            }
        }
        if (isVertex) {
            if (xyzSlot[0] < 0 || xyzSlot[1] < 0 || xyzSlot[2] < 0) {
                return fail(ErrorCode::Parse, "PLY vertex element has no x, y and z properties");
            }
            if (rowCount > kMaxIndex) {
                return fail(ErrorCode::Unsupported, "PLY has more than 2^32-1 vertices");
            }
            positions.reserve(safeReserve(rowCount));
            haveVertices = true;
        }
        if (isFace) {
            if (indexSlot < 0) {
                return fail(ErrorCode::Parse, "PLY face element has no vertex_indices list");
            }
            indices.reserve(safeReserve(rowCount * 3));
            haveFaces = true;
        }
        if (properties.empty()) continue; // nothing to read, however many rows

        for (std::uint64_t row = 0; row < rowCount; ++row) {
            std::array<double, 3> xyz{};
            for (std::size_t i = 0; i < properties.size(); ++i) {
                const Property& p = properties[i];
                const int slot = static_cast<int>(i);
                if (p.isList) {
                    double countValue = 0.0;
                    // The ASCII reader accepts any number here, so also refuse NaN and counts
                    // too large to convert.
                    if (!reader.read(p.countType, countValue) ||
                        !(countValue >= 0.0 && countValue <= 4294967295.0)) {
                        return fail(ErrorCode::Parse,
                                    "truncated or invalid PLY list in '" + elementName +
                                        "' element");
                    }
                    const auto count = static_cast<std::uint64_t>(countValue);
                    if (slot == indexSlot) poly.clear();
                    for (std::uint64_t k = 0; k < count; ++k) {
                        double v = 0.0;
                        if (!reader.read(p.type, v)) {
                            return fail(ErrorCode::Parse, "truncated PLY list data");
                        }
                        if (slot == indexSlot) {
                            if (!(v >= 0.0 &&
                                  v <= static_cast<double>(kMaxIndex))) { // also refuses NaN
                                return fail(ErrorCode::Parse, "invalid PLY vertex index");
                            }
                            poly.push_back(static_cast<std::uint32_t>(v));
                        }
                    }
                    if (slot == indexSlot) {
                        if (poly.size() < 3) {
                            return fail(ErrorCode::Parse,
                                        "PLY face " + std::to_string(row) +
                                            " has fewer than three vertices");
                        }
                        appendFan(poly, indices);
                    }
                } else {
                    double v = 0.0;
                    if (!reader.read(p.type, v)) {
                        return fail(ErrorCode::Parse,
                                    "truncated or invalid PLY data in '" + elementName +
                                        "' element");
                    }
                    for (std::size_t c = 0; c < 3; ++c) {
                        if (slot == xyzSlot[c]) xyz[c] = v;
                    }
                }
            }
            if (isVertex) positions.push_back({.x = xyz[0], .y = xyz[1], .z = xyz[2]});
        }
    }
    if (!haveVertices) return fail(ErrorCode::Parse, "PLY has no vertex element");
    if (!haveFaces)
        return fail(ErrorCode::Parse, "PLY has no face element (point clouds are not supported)");
    if (!weld) return TriMesh::create(std::move(positions), std::move(indices));
    return weldAndCompact(positions, indices, "PLY");
}

Result<TriMesh> readMesh(std::istream& in, const bool weld) {
    auto header = readHeader(in);
    if (!header) return std::unexpected(header.error());
    if (header->format == Format::Ascii) {
        AsciiReader reader(in);
        return readBody(reader, *header, weld);
    }
    ByteReader reader(in, header->format == Format::BinaryBig);
    return readBody(reader, *header, weld);
}

} // namespace

Result<TriMesh> readPly(std::istream& in) {
    return readMesh(in, true);
}

Result<TriMesh> readPlyUnwelded(std::istream& in) {
    return readMesh(in, false);
}

Result<void> writePly(const TriMesh& mesh, std::ostream& out) {
    out << "ply\nformat binary_little_endian 1.0\ncomment Mesh2Cad\n"
        << "element vertex " << mesh.vertexCount() << "\n"
        << "property double x\nproperty double y\nproperty double z\n"
        << "element face " << mesh.faceCount() << "\n"
        << "property list uchar uint vertex_indices\nend_header\n";

    std::vector<char> buffer;
    buffer.reserve(std::size_t{1} << 16U);
    const auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };
    const auto put = [&](const void* data, const std::size_t n) {
        const auto* bytes = static_cast<const char*>(data);
        buffer.insert(buffer.end(), bytes, bytes + n);
        if (buffer.size() > 60000) flush();
    };
    for (const auto& [x, y, z] : mesh.positions()) {
        const std::array xyz{x, y, z};
        put(xyz.data(), sizeof(xyz));
    }
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        constexpr unsigned char three = 3;
        const auto corners = mesh.face(f);
        put(&three, 1);
        put(corners.data(), sizeof(corners));
    }
    flush();
    out.flush();
    if (!out) return fail(ErrorCode::Io, "failed while writing PLY data");
    return {};
}

} // namespace m2c

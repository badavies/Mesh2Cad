// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Project.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <miniz.h>
#include <sstream>
#include <system_error>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/io/Ply.h"

namespace m2c {

namespace {

using Json = nlohmann::json;

constexpr const char* kFormatName = "mesh2cad-project";

Json vec(const Vec3d& v) {
    return Json::array({v.x, v.y, v.z});
}
Vec3d vec(const Json& j) {
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

Json pickedJson(const PickedPoint& p) {
    return Json{{"point", vec(p.position)},
                {"object_id", p.objectId},
                {"snap", std::string(toString(p.snap))},
                {"element", p.element}};
}

PickedPoint pickedFrom(const Json& j) {
    PickedPoint p;
    p.position = vec(j.at("point"));
    p.objectId = j.at("object_id").get<std::uint64_t>();
    p.element = j.at("element").get<std::uint32_t>();
    const std::string snap = j.at("snap").get<std::string>();
    for (const SnapKind k : {SnapKind::Free, SnapKind::Surface, SnapKind::Edge, SnapKind::Vertex}) {
        if (snap == toString(k)) p.snap = k;
    }
    return p;
}

// Binary arrays are stored in the host's byte order, which is little-endian on every platform
// Mesh2Cad builds for (x86-64, ARM64), as the selection bitsets always were.
static_assert(sizeof(Vec3d) == 3 * sizeof(double));

template <class T>
void append(std::string& out, const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    const std::uint64_t count = values.size();
    out.append(reinterpret_cast<const char*>(&count), sizeof count);
    if (!values.empty()) {
        out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
    }
}

/// Reads back what append() wrote, checking every length against the data.
class ByteReader {
public:
    explicit ByteReader(const std::string& data) : data_(data) {}

    template <class T>
    Result<std::vector<T>> take() {
        std::uint64_t count = 0;
        if (data_.size() - pos_ < sizeof count) return truncated();
        std::memcpy(&count, data_.data() + pos_, sizeof count);
        pos_ += sizeof count;
        if (count > (data_.size() - pos_) / sizeof(T)) return truncated();
        std::vector<T> values(static_cast<std::size_t>(count));
        if (count > 0) std::memcpy(values.data(), data_.data() + pos_, values.size() * sizeof(T));
        pos_ += values.size() * sizeof(T);
        return values;
    }
    [[nodiscard]] bool done() const noexcept { return pos_ == data_.size(); }

private:
    static std::unexpected<Error> truncated() {
        return fail(ErrorCode::Parse, "truncated data in project archive");
    }
    const std::string& data_;
    std::size_t pos_{0};
};

std::string encode(const QuadLayout& cage) {
    std::string out;
    append(out, cage.positions());
    append(out, cage.faceStarts());
    append(out, cage.faceVertices());
    return out;
}

Result<QuadLayout> decodeQuadLayout(const std::string& data) {
    ByteReader in(data);
    auto positions = in.take<Vec3d>();
    if (!positions) return std::unexpected(positions.error());
    auto starts = in.take<std::uint32_t>();
    if (!starts) return std::unexpected(starts.error());
    auto vertices = in.take<std::uint32_t>();
    if (!vertices) return std::unexpected(vertices.error());
    return QuadLayout::create(std::move(*positions), std::move(*starts), std::move(*vertices));
}

/// Per face: {cage face, quads along u and v, cap pieces}, its breaks along u and v, then its
/// poles (format 3). Format 2 stored {cage face, spans, cap pieces} and the poles of a face of
/// spans x spans equal pieces.
std::string encode(const std::vector<SurfacePatch>& faces) {
    std::string out;
    append(out, std::vector<std::uint64_t>{faces.size()});
    for (const SurfacePatch& f : faces) {
        append(out, std::vector<std::uint64_t>{f.cageFace, f.cellsU, f.cellsV, f.capPieces});
        append(out, f.uBreaks);
        append(out, f.vBreaks);
        append(out, f.poles);
    }
    return out;
}

bool validBreaks(const std::vector<double>& b) {
    if (b.size() < 2 || b.size() > 4097 || b.front() != 0.0 || b.back() != 1.0) return false;
    for (std::size_t i = 1; i < b.size(); ++i) {
        if (!(b[i] > b[i - 1])) return false;
    }
    return true;
}

Result<std::vector<SurfacePatch>> decodeFaces(const std::string& data, int version) {
    ByteReader in(data);
    auto count = in.take<std::uint64_t>();
    if (!count) return std::unexpected(count.error());
    if (count->size() != 1) return fail(ErrorCode::Parse, "bad face list in project archive");
    const auto bad = [] { return fail(ErrorCode::Parse, "bad face in project archive"); };
    std::vector<SurfacePatch> faces;
    for (std::uint64_t i = 0; i < count->front(); ++i) {
        auto header = in.take<std::uint64_t>();
        if (!header) return std::unexpected(header.error());
        SurfacePatch f;
        if (version < 3) {
            if (header->size() != 3 || (*header)[1] < 1 || (*header)[1] > 1024) return bad();
            f.uBreaks = uniformBreaks(static_cast<std::size_t>((*header)[1]));
            f.vBreaks = f.uBreaks;
            f.capPieces = static_cast<std::size_t>((*header)[2]);
        } else {
            if (header->size() != 4 || (*header)[1] < 1 || (*header)[1] > 4096 ||
                (*header)[2] < 1 || (*header)[2] > 4096) {
                return bad();
            }
            auto uBreaks = in.take<double>();
            if (!uBreaks) return std::unexpected(uBreaks.error());
            auto vBreaks = in.take<double>();
            if (!vBreaks) return std::unexpected(vBreaks.error());
            if (!validBreaks(*uBreaks) || !validBreaks(*vBreaks)) return bad();
            f.cellsU = static_cast<std::uint32_t>((*header)[1]);
            f.cellsV = static_cast<std::uint32_t>((*header)[2]);
            f.capPieces = static_cast<std::size_t>((*header)[3]);
            f.uBreaks = std::move(*uBreaks);
            f.vBreaks = std::move(*vBreaks);
        }
        f.cageFace = static_cast<std::uint32_t>((*header)[0]);
        auto poles = in.take<Vec3d>();
        if (!poles) return std::unexpected(poles.error());
        f.poles = std::move(*poles);
        if (f.poles.size() != f.polesPerRow() * f.poleRows()) return bad();
        faces.push_back(std::move(f));
    }
    return faces;
}

/// Owns a miniz writer or reader and always ends it.
struct ZipArchive {
    mz_zip_archive zip{};
    bool writer{false};
    bool open{false};
    ~ZipArchive() {
        if (!open) return;
        if (writer) {
            mz_zip_writer_end(&zip);
        } else {
            mz_zip_reader_end(&zip);
        }
    }
};

Result<void>
addEntry(ZipArchive& archive, const std::string& name, const std::string& data, mz_uint level) {
    if (!mz_zip_writer_add_mem(&archive.zip, name.c_str(), data.data(), data.size(), level)) {
        return fail(ErrorCode::Io,
                    "cannot add '" + name + "' to the project archive: " +
                        mz_zip_get_error_string(mz_zip_get_last_error(&archive.zip)));
    }
    return {};
}

Result<std::string> readEntry(ZipArchive& archive, const std::string& name) {
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&archive.zip, name.c_str(), &size, 0);
    if (data == nullptr) {
        return fail(ErrorCode::Parse, "project archive has no readable '" + name + "'");
    }
    std::string out(static_cast<const char*>(data), size);
    mz_free(data);
    return out;
}

} // namespace

Result<void> writeProject(const ProjectData& project, const std::filesystem::path& path) {
    ZipArchive archive;
    archive.writer = true;
    // Built in memory and written with std::ofstream, so non-ASCII paths work on Windows.
    if (!mz_zip_writer_init_heap(&archive.zip, 0, 1 << 20)) {
        return fail(ErrorCode::Internal, "cannot start the project archive");
    }
    archive.open = true;

    Json objects = Json::array();
    for (const ProjectObject& o : project.objects) {
        Json j{{"id", o.id},
               {"name", o.name},
               {"visible", o.visible},
               {"made_by", o.madeBy},
               {"made_with", o.madeWith},
               {"inputs", o.inputs}};
        if (const auto* m = std::get_if<ProjectMesh>(&o.data)) {
            const std::string file = "meshes/" + std::to_string(o.id) + ".ply";
            std::ostringstream ply(std::ios::out | std::ios::binary);
            if (auto ok = writePly(*m->mesh, ply); !ok) return ok;
            if (auto ok = addEntry(archive, file, ply.str(), MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "mesh";
            j["file"] = file;
            j["alignment"] = Json{{"rotation", m->alignment.rotation},
                                  {"translation", vec(m->alignment.translation)}};
        } else if (const auto* s = std::get_if<ProjectSelection>(&o.data)) {
            const std::string file = "selections/" + std::to_string(o.id) + ".bits";
            const auto& words = s->faces->words();
            std::string bytes(words.size() * sizeof(std::uint64_t), '\0');
            if (!words.empty()) std::memcpy(bytes.data(), words.data(), bytes.size());
            if (auto ok = addEntry(archive, file, bytes, MZ_DEFAULT_LEVEL); !ok) return ok;
            j["kind"] = "selection";
            j["file"] = file;
            j["mesh_id"] = s->meshId;
            j["face_count"] = s->faces->size();
        } else if (const auto* measurement = std::get_if<Measurement>(&o.data)) {
            j["kind"] = "measurement";
            j["a"] = pickedJson(measurement->a);
            j["b"] = pickedJson(measurement->b);
        } else if (const auto* cage = std::get_if<QuadLayout>(&o.data)) {
            const std::string file = "quad_layouts/" + std::to_string(o.id) + ".bin";
            if (auto ok = addEntry(archive, file, encode(*cage), MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "quad_layout";
            j["file"] = file;
        } else if (const auto* solid = std::get_if<Solid>(&o.data)) {
            const std::string base = "solids/" + std::to_string(o.id);
            auto brep = occt::writeBrep(*solid);
            if (!brep) return std::unexpected(brep.error());
            if (auto ok = addEntry(archive, base + ".brep", *brep, MZ_BEST_SPEED); !ok) return ok;
            const std::string faces = encode(solid->faces());
            if (auto ok = addEntry(archive, base + ".faces", faces, MZ_BEST_SPEED); !ok) return ok;
            // The display mesh is tessellate(faces, n), with 2 n^2 triangles per cage quad, so n
            // is all it takes to rebuild it.
            std::size_t quads = 0;
            for (const SurfacePatch& f : solid->faces()) quads += std::size_t{f.cellsU} * f.cellsV;
            const double perQuad =
                double(solid->display().faceCount()) / double(std::max<std::size_t>(quads, 1));
            j["kind"] = "solid";
            j["brep_file"] = base + ".brep";
            j["brep_format"] = "binary";
            j["faces_file"] = base + ".faces";
            j["display_grid"] = std::lround(std::sqrt(perQuad / 2.0));
        } else {
            const auto& d = std::get<ProjectDeviation>(o.data);
            const std::string file = "deviations/" + std::to_string(o.id) + ".bin";
            std::string bytes;
            append(bytes, d.field.distances());
            if (auto ok = addEntry(archive, file, bytes, MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "deviation";
            j["file"] = file;
            j["mesh_id"] = d.meshId;
            j["reference_id"] = d.referenceId;
            j["tolerance_mm"] = d.field.stats().toleranceMm;
        }
        objects.push_back(std::move(j));
    }
    const Json manifest{{"format", kFormatName},
                        {"version", kProjectFormatVersion},
                        {"next_id", project.nextId},
                        {"objects", std::move(objects)}};
    if (auto ok = addEntry(archive, "manifest.json", manifest.dump(2), MZ_DEFAULT_LEVEL); !ok) {
        return ok;
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive.zip, &buffer, &size)) {
        return fail(ErrorCode::Internal, "cannot finish the project archive");
    }
    // The finished buffer is ours now; miniz no longer frees it in mz_zip_writer_end().
    const std::unique_ptr<void, decltype(&mz_free)> ownedBuffer(buffer, &mz_free);

    std::filesystem::path temp = path;
    temp += ".partial";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return fail(ErrorCode::Io, "cannot open '" + temp.string() + "' for writing");
        file.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
        file.close();
        if (!file) return fail(ErrorCode::Io, "failed while writing '" + temp.string() + "'");
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return fail(ErrorCode::Io, "cannot replace '" + path.string() + "'");
    }
    return {};
}

Result<ProjectData> readProject(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for reading");
    std::string bytes(static_cast<std::size_t>(std::max<std::streamoff>(file.tellg(), 0)), '\0');
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) return fail(ErrorCode::Io, "cannot read '" + path.string() + "'");

    ZipArchive archive;
    if (!mz_zip_reader_init_mem(&archive.zip, bytes.data(), bytes.size(), 0)) {
        return fail(ErrorCode::Parse,
                    "'" + path.string() + "' is not a Mesh2Cad project (not a zip)");
    }
    archive.open = true;

    auto manifestText = readEntry(archive, "manifest.json");
    if (!manifestText) return std::unexpected(manifestText.error());
    const Json manifest = Json::parse(*manifestText, nullptr, false);
    if (manifest.is_discarded() || manifest.value("format", "") != kFormatName) {
        return fail(ErrorCode::Parse, "'" + path.string() + "' is not a Mesh2Cad project");
    }
    const int version = manifest.value("version", 0);
    if (version > kProjectFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "project was saved by a newer Mesh2Cad (format " + std::to_string(version) +
                        ")");
    }
    if (version < 1) return fail(ErrorCode::Parse, "project has an invalid format version");
    // Version 1 holds meshes, selections and measurements, stored as in version 2, so it needs no
    // migration; the object kinds version 2 added are refused in a version 1 file. Version 2
    // faces are read as faces of one cage quad (decodeFaces).

    try {
        ProjectData project;
        project.nextId = manifest.at("next_id").get<std::uint64_t>();
        for (const Json& j : manifest.at("objects")) {
            ProjectObject o;
            o.id = j.at("id").get<std::uint64_t>();
            o.name = j.at("name").get<std::string>();
            o.visible = j.value("visible", true);
            o.madeBy = j.value("made_by", "");
            o.madeWith = j.value("made_with", "");
            o.inputs = j.value("inputs", std::vector<std::uint64_t>{});
            const std::string kind = j.at("kind").get<std::string>();
            if (kind == "mesh") {
                auto text = readEntry(archive, j.at("file").get<std::string>());
                if (!text) return std::unexpected(text.error());
                std::istringstream in(*text, std::ios::in | std::ios::binary);
                auto mesh = readPlyUnwelded(in);
                if (!mesh) return std::unexpected(mesh.error());
                ProjectMesh m{std::make_shared<const TriMesh>(std::move(*mesh)), {}};
                const Json& a = j.at("alignment");
                const auto rotation = a.at("rotation").get<std::vector<double>>();
                if (rotation.size() != 9) return fail(ErrorCode::Parse, "bad alignment in project");
                std::copy(rotation.begin(), rotation.end(), m.alignment.rotation.begin());
                m.alignment.translation = vec(a.at("translation"));
                o.data = std::move(m);
            } else if (kind == "selection") {
                auto raw = readEntry(archive, j.at("file").get<std::string>());
                if (!raw) return std::unexpected(raw.error());
                const auto faceCount = j.at("face_count").get<std::size_t>();
                std::vector<std::uint64_t> words(raw->size() / sizeof(std::uint64_t));
                if (words.size() != faceCount / 64 + (faceCount % 64 != 0 ? 1 : 0)) {
                    return fail(ErrorCode::Parse, "bad selection in project archive");
                }
                if (!words.empty()) std::memcpy(words.data(), raw->data(), words.size() * 8);
                o.data =
                    ProjectSelection{j.at("mesh_id").get<std::uint64_t>(),
                                     std::make_shared<const MeshSelection>(
                                         MeshSelection::fromWords(std::move(words), faceCount))};
            } else if (kind == "measurement") {
                o.data = Measurement{pickedFrom(j.at("a")), pickedFrom(j.at("b"))};
            } else if (version >= 2 && kind == "quad_layout") {
                auto raw = readEntry(archive, j.at("file").get<std::string>());
                if (!raw) return std::unexpected(raw.error());
                auto cage = decodeQuadLayout(*raw);
                if (!cage) return std::unexpected(cage.error());
                o.data = std::move(*cage);
            } else if (version >= 2 && kind == "solid") {
                auto brep = readEntry(archive, j.at("brep_file").get<std::string>());
                if (!brep) return std::unexpected(brep.error());
                auto raw = readEntry(archive, j.at("faces_file").get<std::string>());
                if (!raw) return std::unexpected(raw.error());
                auto faces = decodeFaces(*raw, version);
                if (!faces) return std::unexpected(faces.error());
                auto display = tessellate(*faces, j.at("display_grid").get<int>());
                if (!display) return std::unexpected(display.error());
                auto solid = occt::readBrep(
                    *brep,
                    j.value("brep_format", "text") == "binary" ? occt::BrepFormat::Binary
                                                               : occt::BrepFormat::Text,
                    std::move(*faces),
                    std::make_shared<const FaceTessellation>(std::move(*display)));
                if (!solid) return std::unexpected(solid.error());
                o.data = std::move(*solid);
            } else if (version >= 2 && kind == "deviation") {
                auto raw = readEntry(archive, j.at("file").get<std::string>());
                if (!raw) return std::unexpected(raw.error());
                ByteReader in(*raw);
                auto distances = in.take<double>();
                if (!distances) return std::unexpected(distances.error());
                auto field = DeviationField::create(std::move(*distances),
                                                    j.at("tolerance_mm").get<double>());
                if (!field) return std::unexpected(field.error());
                o.data = ProjectDeviation{j.at("mesh_id").get<std::uint64_t>(),
                                          j.at("reference_id").get<std::uint64_t>(),
                                          std::move(*field)};
            } else {
                return fail(ErrorCode::Parse, "unknown object kind '" + kind + "' in project");
            }
            project.objects.push_back(std::move(o));
        }
        return project;
    } catch (const Json::exception& e) {
        return fail(ErrorCode::Parse, std::string("malformed project manifest: ") + e.what());
    }
}

} // namespace m2c

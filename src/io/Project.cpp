// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <miniz.h>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/Vec3.h"
#include "m2c/io/Ply.h"

namespace m2c {

namespace {

using Json = nlohmann::json;

constexpr auto kFormatName = "mesh2cad-project";

Json vec(const Vec3d& v) {
    return Json::array({v.x, v.y, v.z});
}
Vec3d vec(const Json& j) {
    return {.x = j.at(0).get<double>(), .y = j.at(1).get<double>(), .z = j.at(2).get<double>()};
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

Json sketchJson(const Sketch& sketch) {
    const auto point = [](const Vec2d& p) { return Json::array({p.x, p.y}); };
    Json loops = Json::array();
    for (const SketchLoop& loop : sketch.loops) {
        Json curves = Json::array();
        for (const SketchCurve& c : loop.curves) {
            Json j{{"type", curveKindName(c.kind)}};
            if (c.kind == CurveKind::Arc) {
                j["centre"] = point(c.centre);
                j["radius_mm"] = c.radiusMm;
                j["start_rad"] = c.startRad;
                j["sweep_rad"] = c.sweepRad;
            } else {
                j["points"] = Json::array();
                for (const Vec2d& p : c.points) j["points"].push_back(point(p));
            }
            curves.push_back(std::move(j));
        }
        loops.push_back(std::move(curves));
    }
    const SketchPlane& plane = sketch.plane;
    return Json{
        {"plane", {{"origin", vec(plane.origin)}, {"u", vec(plane.u)}, {"v", vec(plane.v)}}},
        {"loops", std::move(loops)}};
}

Result<Sketch> sketchFrom(const Json& j) {
    const auto point = [](const Json& p) {
        return Vec2d{.x = p.at(0).get<double>(), .y = p.at(1).get<double>()};
    };
    Sketch sketch;
    const Json& plane = j.at("plane");
    sketch.plane = {
        .origin = vec(plane.at("origin")), .u = vec(plane.at("u")), .v = vec(plane.at("v"))};
    for (const Json& loop : j.at("loops")) {
        SketchLoop& out = sketch.loops.emplace_back();
        for (const Json& c : loop) {
            const auto kind = curveKindFromName(c.at("type").get<std::string>());
            if (!kind) return fail(ErrorCode::Parse, "unknown sketch curve type in project");
            SketchCurve& curve = out.curves.emplace_back();
            curve.kind = *kind;
            if (*kind == CurveKind::Arc) {
                curve.centre = point(c.at("centre"));
                curve.radiusMm = c.at("radius_mm").get<double>();
                curve.startRad = c.at("start_rad").get<double>();
                curve.sweepRad = c.at("sweep_rad").get<double>();
            } else {
                for (const Json& p : c.at("points")) curve.points.push_back(point(p));
            }
        }
    }
    return sketch;
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
        std::vector<T> values(count);
        if (count > 0) std::memcpy(values.data(), data_.data() + pos_, values.size() * sizeof(T));
        pos_ += values.size() * sizeof(T);
        return values;
    }

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
    for (const auto& [cageFace, cellsU, cellsV, uBreaks, vBreaks, poles, capPieces] : faces) {
        append(out, std::vector<std::uint64_t>{cageFace, cellsU, cellsV, capPieces});
        append(out, uBreaks);
        append(out, vBreaks);
        append(out, poles);
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
            f.uBreaks = uniformBreaks((*header)[1]);
            f.vBreaks = f.uBreaks;
            f.capPieces = (*header)[2];
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
            f.capPieces = (*header)[3];
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

Result<void> addEntry(ZipArchive& archive,
                      const std::string& name,
                      const std::string& data,
                      const mz_uint level) {
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
    if (!mz_zip_writer_init_heap(&archive.zip, 0, 1U << 20U)) {
        return fail(ErrorCode::Internal, "cannot start the project archive");
    }
    archive.open = true;

    Json objects = Json::array();
    for (const auto& [id, name, visible, data, madeBy, madeWith, inputs] : project.objects) {
        Json j{{"id", id},
               {"name", name},
               {"visible", visible},
               {"made_by", madeBy},
               {"made_with", madeWith},
               {"inputs", inputs}};
        if (const auto* m = std::get_if<ProjectMesh>(&data)) {
            const std::string file = "meshes/" + std::to_string(id) + ".ply";
            std::ostringstream ply(std::ios::out | std::ios::binary);
            if (auto ok = writePly(*m->mesh, ply); !ok) return ok;
            if (auto ok = addEntry(archive, file, ply.str(), MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "mesh";
            j["file"] = file;
            j["alignment"] = Json{{"rotation", m->alignment.rotation},
                                  {"translation", vec(m->alignment.translation)}};
        } else if (const auto* s = std::get_if<ProjectSelection>(&data)) {
            const std::string file = "selections/" + std::to_string(id) + ".bits";
            const auto& words = s->faces->words();
            std::string bytes(words.size() * sizeof(std::uint64_t), '\0');
            if (!words.empty()) std::memcpy(bytes.data(), words.data(), bytes.size());
            if (auto ok = addEntry(archive, file, bytes, MZ_DEFAULT_LEVEL); !ok) return ok;
            j["kind"] = "selection";
            j["file"] = file;
            j["mesh_id"] = s->meshId;
            j["face_count"] = s->faces->size();
        } else if (const auto* measurement = std::get_if<Measurement>(&data)) {
            j["kind"] = "measurement";
            j["a"] = pickedJson(measurement->a);
            j["b"] = pickedJson(measurement->b);
        } else if (const auto* cage = std::get_if<QuadLayout>(&data)) {
            const std::string file = "quad_layouts/" + std::to_string(id) + ".bin";
            if (auto ok = addEntry(archive, file, encode(*cage), MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "quad_layout";
            j["file"] = file;
        } else if (const auto* solid = std::get_if<Solid>(&data)) {
            const std::string base = "solids/" + std::to_string(id);
            auto brep = occt::writeBrep(*solid);
            if (!brep) return std::unexpected(brep.error());
            if (auto ok = addEntry(archive, base + ".brep", *brep, MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "solid";
            j["brep_file"] = base + ".brep";
            j["brep_format"] = "binary";
            if (solid->faces().empty()) { // trimmed primitives: the kernel's faces only
                objects.push_back(std::move(j));
                continue;
            }
            const std::string faces = encode(solid->faces());
            if (auto ok = addEntry(archive, base + ".faces", faces, MZ_BEST_SPEED); !ok) return ok;
            // The display mesh is tessellate(faces, n), with 2 n^2 triangles per cage quad, so n
            // is all it takes to rebuild it.
            std::size_t quads = 0;
            for (const SurfacePatch& f : solid->faces()) quads += std::size_t{f.cellsU} * f.cellsV;
            const double perQuad = static_cast<double>(solid->display().faceCount()) /
                                   static_cast<double>(std::max<std::size_t>(quads, 1));
            j["faces_file"] = base + ".faces";
            j["display_grid"] = std::lround(std::sqrt(perQuad / 2.0));
        } else if (const auto* primitive = std::get_if<FittedPrimitive>(&data)) {
            const Primitive& surface = primitive->surface;
            j["kind"] = "primitive";
            j["surface"] = Json{{"type", primitiveKindName(surface.kind)},
                                {"point", vec(surface.point)},
                                {"direction", vec(surface.direction)},
                                {"radius_mm", surface.radiusMm},
                                {"half_angle_rad", surface.halfAngleRad}};
            j["extent"] =
                Json{{"min", vec(primitive->extent.min)}, {"max", vec(primitive->extent.max)}};
            j["rms_mm"] = primitive->rmsMm;
            j["max_mm"] = primitive->maxMm;
            j["p95_mm"] = primitive->p95Mm;
        } else if (const auto* sketch = std::get_if<Sketch>(&data)) {
            j["kind"] = "sketch";
            j["sketch"] = sketchJson(*sketch);
        } else {
            const auto& [meshId, referenceId, field] = std::get<ProjectDeviation>(data);
            const std::string file = "deviations/" + std::to_string(id) + ".bin";
            std::string bytes;
            append(bytes, field.distances());
            if (auto ok = addEntry(archive, file, bytes, MZ_BEST_SPEED); !ok) return ok;
            j["kind"] = "deviation";
            j["file"] = file;
            j["mesh_id"] = meshId;
            j["reference_id"] = referenceId;
            j["tolerance_mm"] = field.stats().toleranceMm;
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
            if (const std::string kind = j.at("kind").get<std::string>(); kind == "mesh") {
                auto text = readEntry(archive, j.at("file").get<std::string>());
                if (!text) return std::unexpected(text.error());
                std::istringstream in(*text, std::ios::in | std::ios::binary);
                auto mesh = readPlyUnwelded(in);
                if (!mesh) return std::unexpected(mesh.error());
                ProjectMesh m{.mesh = std::make_shared<const TriMesh>(std::move(*mesh)),
                              .alignment = {}};
                const Json& a = j.at("alignment");
                const auto rotation = a.at("rotation").get<std::vector<double>>();
                if (rotation.size() != 9) return fail(ErrorCode::Parse, "bad alignment in project");
                std::ranges::copy(rotation, m.alignment.rotation.begin());
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
                    ProjectSelection{.meshId = j.at("mesh_id").get<std::uint64_t>(),
                                     .faces = std::make_shared<const MeshSelection>(
                                         MeshSelection::fromWords(std::move(words), faceCount))};
            } else if (kind == "measurement") {
                o.data = Measurement{.a = pickedFrom(j.at("a")), .b = pickedFrom(j.at("b"))};
            } else if (version >= 2 && kind == "quad_layout") {
                auto raw = readEntry(archive, j.at("file").get<std::string>());
                if (!raw) return std::unexpected(raw.error());
                auto cage = decodeQuadLayout(*raw);
                if (!cage) return std::unexpected(cage.error());
                o.data = std::move(*cage);
            } else if (version >= 2 && kind == "solid") {
                auto brep = readEntry(archive, j.at("brep_file").get<std::string>());
                if (!brep) return std::unexpected(brep.error());
                if (version >= 5 && !j.contains("faces_file")) {
                    auto solid = occt::readBrep(*brep, occt::BrepFormat::Binary, {}, nullptr);
                    if (!solid) return std::unexpected(solid.error());
                    o.data = std::move(*solid);
                    project.objects.push_back(std::move(o));
                    continue;
                }
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
                o.data = ProjectDeviation{.meshId = j.at("mesh_id").get<std::uint64_t>(),
                                          .referenceId = j.at("reference_id").get<std::uint64_t>(),
                                          .field = std::move(*field)};
            } else if (version >= 4 && kind == "primitive") {
                const Json& s = j.at("surface");
                const auto type = primitiveKindFromName(s.at("type").get<std::string>());
                if (!type) return fail(ErrorCode::Parse, "unknown primitive type in project");
                o.data = FittedPrimitive{
                    .surface = {.kind = *type,
                                .point = vec(s.at("point")),
                                .direction = vec(s.at("direction")),
                                .radiusMm = s.at("radius_mm").get<double>(),
                                .halfAngleRad = s.at("half_angle_rad").get<double>()},
                    .extent = {.min = vec(j.at("extent").at("min")),
                               .max = vec(j.at("extent").at("max"))},
                    .rmsMm = j.at("rms_mm").get<double>(),
                    .maxMm = j.at("max_mm").get<double>(),
                    .p95Mm = j.at("p95_mm").get<double>()};
            } else if (version >= 6 && kind == "sketch") {
                auto sketch = sketchFrom(j.at("sketch"));
                if (!sketch) return std::unexpected(sketch.error());
                o.data = std::move(*sketch);
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

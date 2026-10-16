// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/io/Project.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <miniz.h>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

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
        } else {
            const auto& measurement = std::get<Measurement>(o.data);
            j["kind"] = "measurement";
            j["a"] = pickedJson(measurement.a);
            j["b"] = pickedJson(measurement.b);
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
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(ErrorCode::Io, "cannot open '" + path.string() + "' for reading");
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

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
    // Version 1 is the first format, so there is nothing to migrate yet.

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
                auto mesh = readPly(in);
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
                if (!words.empty()) std::memcpy(words.data(), raw->data(), words.size() * 8);
                o.data =
                    ProjectSelection{j.at("mesh_id").get<std::uint64_t>(),
                                     std::make_shared<const MeshSelection>(
                                         MeshSelection::fromWords(std::move(words), faceCount))};
            } else if (kind == "measurement") {
                o.data = Measurement{pickedFrom(j.at("a")), pickedFrom(j.at("b"))};
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

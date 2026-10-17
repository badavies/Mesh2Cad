// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <miniz.h>
#include <sstream>
#include <string>

#include "m2c/io/Ply.h"
#include "m2c/io/Project.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

TEST_CASE("Project files round-trip every object kind", "[io][project]") {
    const TempDir dir;
    ProjectData project;
    project.nextId = 9;
    const auto cube = std::make_shared<const TriMesh>(makeBox(1.5, 2, 3, 2));
    Transform3d aligned{{0, -1, 0, 1, 0, 0, 0, 0, 1}, {1, 2, 3}};
    project.objects.push_back(
        {1, "Scan", true, ProjectMesh{cube, aligned}, "mesh.import", R"({"path":"scan.stl"})", {}});
    auto faces = std::make_shared<MeshSelection>(cube->faceCount());
    faces->set(0);
    faces->set(47);
    project.objects.push_back(
        {4, "Base", false, ProjectSelection{1, faces}, "select.crease", "{}", {1}});
    const Measurement m{{{0, 0, 0}, 1, SnapKind::Vertex, 0}, {{1.5, 2, 3}, 0, SnapKind::Free, 0}};
    project.objects.push_back({7, "Diagonal", true, m, "measure.distance", "{}", {1}});

    const auto path = dir.path() / "test project ü.m2c";
    REQUIRE(writeProject(project, path).has_value());
    const auto back = readProject(path);
    REQUIRE(back.has_value());
    CHECK(back->nextId == 9);
    REQUIRE(back->objects.size() == 3);

    const auto& mesh = std::get<ProjectMesh>(back->objects[0].data);
    CHECK(mesh.mesh->positions() == cube->positions());
    CHECK(mesh.mesh->indices() == cube->indices());
    CHECK(mesh.alignment.rotation == aligned.rotation);
    CHECK(mesh.alignment.translation == aligned.translation);
    CHECK(back->objects[0].madeWith == R"({"path":"scan.stl"})");

    const auto& sel = std::get<ProjectSelection>(back->objects[1].data);
    CHECK(sel.meshId == 1);
    CHECK(*sel.faces == *faces);
    CHECK_FALSE(back->objects[1].visible);
    CHECK(back->objects[1].inputs == std::vector<std::uint64_t>{1});

    const auto& meas = std::get<Measurement>(back->objects[2].data);
    CHECK(meas.a.snap == SnapKind::Vertex);
    CHECK(meas.b.position == Vec3d{1.5, 2, 3});
}

TEST_CASE("Reading a broken project fails cleanly", "[io][project]") {
    const TempDir dir;
    CHECK(readProject(dir.path() / "missing.m2c").error().code == ErrorCode::Io);
    const auto junk = dir.path() / "junk.m2c";
    std::ofstream(junk) << "not a zip";
    CHECK(readProject(junk).error().code == ErrorCode::Parse);
}

TEST_CASE("Project files keep quad layouts and deviations", "[io][project]") {
    const TempDir dir;
    ProjectData project;
    project.nextId = 4;
    const auto mesh = std::make_shared<const TriMesh>(makeCube());
    project.objects.push_back({1, "Scan", true, ProjectMesh{mesh, {}}, "", "", {}});
    // A cube cage with one triangle face, so faces have different sizes.
    auto cage = QuadLayout::create({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5, 0.5, 1}},
                                   {0, 4, 7, 10, 13, 16},
                                   {0, 3, 2, 1, 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4});
    REQUIRE(cage);
    project.objects.push_back({2, "Cage", true, *cage, "quad.remesh", "{}", {1}});
    std::vector<double> d(mesh->vertexCount());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = 0.01 * double(i) - 0.03;
    project.objects.push_back(
        {3, "Dev", true, ProjectDeviation{1, 1, *DeviationField::create(d, 0.02)}, "", "", {1}});

    const auto path = dir.path() / "v2.m2c";
    REQUIRE(writeProject(project, path).has_value());
    const auto back = readProject(path);
    REQUIRE(back.has_value());
    REQUIRE(back->objects.size() == 3);
    const auto& c = std::get<QuadLayout>(back->objects[1].data);
    CHECK(c.positions() == cage->positions());
    CHECK(c.faceStarts() == cage->faceStarts());
    CHECK(c.faceVertices() == cage->faceVertices());
    const auto& dev = std::get<ProjectDeviation>(back->objects[2].data);
    CHECK(dev.meshId == 1);
    CHECK(dev.referenceId == 1);
    CHECK(dev.field.distances() == d);
    CHECK(dev.field.stats().toleranceMm == 0.02);
    CHECK(dev.field.stats().withinTolerance ==
          DeviationField::create(d, 0.02)->stats().withinTolerance);
}

namespace {

/// A project file as version 1 wrote it, with the given objects in its manifest.
void writeVersion1(const std::filesystem::path& path, const std::string& objects) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    std::ostringstream ply(std::ios::out | std::ios::binary);
    REQUIRE(writePly(makeCube(), ply));
    const std::string mesh = ply.str();
    REQUIRE(mz_zip_writer_add_mem(&zip, "meshes/1.ply", mesh.data(), mesh.size(), 0));
    const std::string manifest = R"({"format":"mesh2cad-project","version":1,"next_id":3,)"
                                 R"("objects":)" +
                                 objects + "}";
    REQUIRE(mz_zip_writer_add_mem(&zip, "manifest.json", manifest.data(), manifest.size(), 0));
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::ofstream(path, std::ios::binary)
        .write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
}

} // namespace

TEST_CASE("Version 1 project files still open", "[io][project]") {
    const TempDir dir;
    const auto path = dir.path() / "v1.m2c";
    writeVersion1(path,
                  R"([{"id":1,"name":"Scan","kind":"mesh","file":"meshes/1.ply",)"
                  R"("alignment":{"rotation":[1,0,0,0,1,0,0,0,1],"translation":[0,0,0]}},)"
                  R"({"id":2,"name":"D","kind":"measurement",)"
                  R"("a":{"point":[0,0,0],"object_id":1,"snap":"vertex","element":0},)"
                  R"("b":{"point":[1,0,0],"object_id":1,"snap":"free","element":0}}])");
    const auto back = readProject(path);
    REQUIRE(back.has_value());
    REQUIRE(back->objects.size() == 2);
    CHECK(std::get<ProjectMesh>(back->objects[0].data).mesh->faceCount() == 12);
    CHECK(std::get<Measurement>(back->objects[1].data).b.position == Vec3d{1, 0, 0});

    // Kinds added in version 2 cannot appear in a version 1 file.
    writeVersion1(path, R"([{"id":1,"name":"C","kind":"quad_layout","file":"meshes/1.ply"}])");
    CHECK(readProject(path).error().code == ErrorCode::Parse);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>

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

// SPDX-License-Identifier: GPL-3.0-or-later
// The M2 commands (scan to CAD) run end to end through the registry.

#include <algorithm>
#include <fstream>
#include <miniz.h>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/commands/Command.h"
#include "m2c/io/MeshIO.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

struct Session {
    CommandRegistry registry;
    Document document;
    CommandContext context{document};

    Session() { REQUIRE(registerBuiltinCommands(registry).has_value()); }

    Json run(std::string_view name, const Json& params = Json::object()) {
        const auto result = registry.execute(context, name, params);
        INFO(name << " " << params.dump());
        if (!result) INFO(result.error().message);
        REQUIRE(result.has_value());
        return *result;
    }

    Result<Json> tryRun(std::string_view name, const Json& params = Json::object()) {
        return registry.execute(context, name, params);
    }

    ObjectId import(const TempDir& dir, const TriMesh& mesh, const std::string& name) {
        const auto path = dir.path() / (name + ".ply");
        REQUIRE(writeMesh(mesh, path).has_value());
        return run("mesh.import", {{"path", path.string()}})["id"].get<ObjectId>();
    }
};

} // namespace

TEST_CASE("quad.remesh joins a loose part or leaves it out on request", "[commands][quad]") {
    // A ball with a smaller ball 4 mm above it: two closed parts, as a post whose stem is too
    // thin for the cage comes out of the remesher.
    Session s;
    const TempDir dir;
    const ObjectId mesh =
        s.import(dir, combine({makeSphere(20.0, 4), makeSphere(6.0, 3, {0, 0, 30})}), "balls");
    const Json joined = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 400}});
    CHECK(joined["cleanup"]["joined_components"] == 1);
    CHECK(joined["cleanup"]["dropped_components"] == 0);
    CHECK(joined["stats"]["components"] == 1);
    CHECK(joined["stats"]["closed"] == true);
    const Json apart =
        s.run("quad.remesh", {{"id", mesh}, {"target_faces", 400}, {"join_parts", false}});
    CHECK(apart["cleanup"]["joined_components"] == 0);
    CHECK(apart["cleanup"]["dropped_components"] == 1);
    CHECK(apart["cleanup"]["dropped_component_faces"] ==
          joined["cleanup"]["joined_component_faces"]);
    CHECK(apart["stats"]["face_count"].get<int>() < joined["stats"]["face_count"].get<int>());
}

TEST_CASE("quad.remesh adds a closed cage and undoes as one step", "[commands][quad]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");

    const Json result = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 400}});
    const auto cage = result["id"].get<ObjectId>();
    CHECK(result["mesh_id"] == mesh);
    CHECK(result["stats"]["closed"] == true);
    CHECK(result["stats"]["euler_characteristic"] == 2);
    CHECK(result["stats"]["face_count"].get<int>() > 200);
    CHECK(result["stats"]["face_count"].get<int>() < 600);
    CHECK(result["max_distance_mm"].get<double>() < 0.5);
    CHECK(result.contains("cleanup"));

    const auto object = s.document.find(cage);
    REQUIRE(object);
    REQUIRE(object->quadLayout() != nullptr);
    CHECK(std::string(object->kind()) == "quad_layout");
    CHECK(object->name == "ball (quads)");
    CHECK(object->provenance.command == "quad.remesh");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{mesh});
    CHECK(s.document.findMesh(mesh)); // the mesh is unchanged

    const Json listed = s.run("doc.list");
    CHECK(listed["objects"][1]["kind"] == "quad_layout");
    CHECK(listed["objects"][1]["face_count"] == result["stats"]["face_count"]);

    CHECK(s.document.undoHistory().back().label == "quad.remesh");
    REQUIRE(s.document.undo());
    CHECK_FALSE(s.document.find(cage));
}

TEST_CASE("quad.remesh validates its input", "[commands][quad]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(), "ball");
    CHECK_FALSE(s.tryRun("quad.remesh", {{"id", mesh}, {"target_faces", 2}}));
    CHECK_FALSE(s.tryRun("quad.remesh", {{"id", 99}}));
    const Json cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 100}});
    // A cage is not a mesh.
    CHECK_FALSE(s.tryRun("quad.remesh", {{"id", cage["id"]}}));
}

TEST_CASE("surface.build adds a valid solid from a quad layout", "[commands][surface]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];

    // The mesh is needed for fitting.
    CHECK_FALSE(s.tryRun("surface.build", {{"id", cage}}));
    CHECK_FALSE(s.tryRun("surface.build", {{"id", mesh}, {"mesh_id", mesh}})); // not a cage

    const Json result = s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}});
    const auto solid = result["id"].get<ObjectId>();
    CHECK(result["summary"]["valid"] == true);
    CHECK(result["summary"]["free_edges"] == 0);
    CHECK(result["summary"]["closed"] == true);
    CHECK(result["summary"]["solid_count"] == 1);
    CHECK_THAT(result["summary"]["bounds"]["max"][0].get<double>(),
               Catch::Matchers::WithinAbs(20.0, 0.1));
    CHECK(result["seams"]["max_gap_mm"].get<double>() < 1e-6);
    CHECK(result["seams"]["max_normal_angle_deg"].get<double>() < 1.0);
    CHECK(result["seams"]["unmatched_samples"] == 0);
    CHECK(result["summary"]["volume_mm3"].get<double>() > 33000.0);
    CHECK(result["summary"]["volume_mm3"].get<double>() < 33600.0);
    CHECK(result["fit"]["rms_distance_mm"].get<double>() < 0.05);
    CHECK(result["fit"]["fits"] == 1);
    CHECK(result["fit"]["smoothing"] == 0.05);
    CHECK(result["cap_pieces"].get<int>() > 0);

    const auto object = s.document.find(solid);
    REQUIRE(object);
    REQUIRE(object->solid() != nullptr);
    CHECK(std::string(object->kind()) == "solid");
    CHECK(object->name == "ball (quads) (solid)");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{cage.get<ObjectId>(), mesh});
    CHECK(s.document.undoHistory().back().label == "surface.build");

    // solid.info reads the same summary back, and only for solids.
    const Json info = s.run("solid.info", {{"id", solid}});
    CHECK(info["summary"] == result["summary"]);
    CHECK(info["name"] == "ball (quads) (solid)");
    CHECK_FALSE(s.tryRun("solid.info", {{"id", mesh}}));

    // Without fitting, no mesh is needed.
    const Json unfitted = s.run("surface.build", {{"id", cage}, {"fit", false}});
    CHECK(unfitted["summary"]["valid"] == true);
    CHECK_FALSE(unfitted.contains("fit"));
}

TEST_CASE("solid.export writes a STEP file that reads back as the same solid",
          "[commands][surface]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];
    const auto solid = s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}})["id"];
    const auto path = (dir.path() / "ball.step").string();

    const Json result = s.run("solid.export", {{"id", solid}, {"path", path}});
    CHECK(result["check"]["passed"] == true);
    CHECK(result["check"]["solid_count"] == 1);
    CHECK(result["check"]["valid"] == true);
    CHECK(result["check"]["closed"] == true);
    CHECK(result["check"]["volume_error"].get<double>() < 1e-3);
    CHECK(std::filesystem::file_size(path) > 1000);
    CHECK_FALSE(s.document.canRedo());

    CHECK_FALSE(s.tryRun("solid.export", {{"id", mesh}, {"path", path}}));
    const auto stl = (dir.path() / "ball.stl").string();
    CHECK_FALSE(s.tryRun("solid.export", {{"id", solid}, {"path", stl}}));
    CHECK_FALSE(std::filesystem::exists(stl));
    CHECK(s.run("solid.export",
                {{"id", solid}, {"path", (dir.path() / "b.STP").string()}})["check"]["passed"] ==
          true);
    CHECK_FALSE(s.tryRun("solid.export",
                         {{"id", solid}, {"path", (dir.path() / "no" / "x.step").string()}}));
}

TEST_CASE("deviation.compute measures a mesh against a solid or a mesh", "[commands][deviation]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    const ObjectId bigger = s.import(dir, makeSphere(20.4, 4), "bigger");
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];
    const auto solid = s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}})["id"];

    const Json toSolid = s.run("deviation.compute",
                               {{"id", bigger}, {"reference_id", solid}, {"tolerance_mm", 0.5}});
    CHECK_THAT(toSolid["stats"]["mean_mm"].get<double>(), Catch::Matchers::WithinAbs(0.4, 0.02));
    CHECK(toSolid["stats"]["within_tolerance"] == 1.0);
    CHECK(toSolid["stats"]["count"] == makeSphere(20.4, 4).vertexCount());
    const auto dev = toSolid["id"].get<ObjectId>();
    const auto object = s.document.find(dev);
    REQUIRE(object);
    CHECK(std::string(object->kind()) == "deviation");
    CHECK(object->name == "bigger (deviation)");
    CHECK(object->deviation()->meshId == bigger);

    const Json toMesh = s.run("deviation.compute", {{"id", bigger}, {"reference_id", mesh}});
    CHECK_THAT(toMesh["stats"]["mean_mm"].get<double>(), Catch::Matchers::WithinAbs(0.4, 0.01));
    CHECK(toMesh["stats"]["tolerance_mm"] == 0.1);

    CHECK_FALSE(s.tryRun("deviation.compute", {{"id", bigger}, {"reference_id", bigger}}));
    CHECK_FALSE(s.tryRun("deviation.compute", {{"id", bigger}, {"reference_id", cage}}));
    CHECK_FALSE(s.tryRun("deviation.compute", {{"id", solid}, {"reference_id", mesh}}));

    // The report: statistics, then the histogram.
    const auto csv = dir.path() / "dev.csv";
    s.run("deviation.report", {{"id", dev}, {"path", csv.string()}, {"bins", 4}});
    std::ifstream in(csv);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    REQUIRE(lines.size() == 9 + 1 + 1 + 4);
    CHECK(lines[0] == "statistic,value");
    CHECK(lines[1].starts_with("count,"));
    CHECK(lines[8] == "within_tolerance,1");
    CHECK(lines[9].empty());
    CHECK(lines[10] == "bin_low_mm,bin_high_mm,count");
    CHECK_FALSE(
        s.tryRun("deviation.report", {{"id", dev}, {"path", (dir.path() / "d.txt").string()}}));
    CHECK_FALSE(s.tryRun("deviation.report", {{"id", mesh}, {"path", csv.string()}}));

    // Removing the reference removes the deviation.
    s.run("object.delete", {{"id", solid}});
    CHECK_FALSE(s.document.find(dev));
}

namespace {

/// Rewrites a saved project as format 2 wrote it: the manifest's version, and each solid's faces
/// as {cage face, spans, cap pieces} and poles (format 2 faces covered one cage quad each).
void rewriteAsVersion2(const std::string& path, const std::vector<SurfacePatch>& faces) {
    mz_zip_archive in{};
    REQUIRE(mz_zip_reader_init_file(&in, path.c_str(), 0));
    std::vector<std::pair<std::string, std::string>> entries;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
        char name[256];
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&in, i, &size, 0);
        REQUIRE(data != nullptr);
        entries.emplace_back(name, std::string(static_cast<const char*>(data), size));
        mz_free(data);
    }
    mz_zip_reader_end(&in);
    const auto append = [](std::string& out, const auto& values) {
        const std::uint64_t count = values.size();
        out.append(reinterpret_cast<const char*>(&count), sizeof count);
        out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof values[0]);
    };
    for (auto& [name, data] : entries) {
        if (name == "manifest.json") {
            Json manifest = Json::parse(data);
            manifest["version"] = 2;
            data = manifest.dump();
        } else if (name.ends_with(".faces")) {
            data.clear();
            append(data, std::vector<std::uint64_t>{faces.size()});
            for (const SurfacePatch& f : faces) {
                REQUIRE(f.cellsU == 1);
                append(data, std::vector<std::uint64_t>{f.cageFace, f.piecesU(), f.capPieces});
                append(data, f.poles);
            }
        }
    }
    mz_zip_archive out{};
    REQUIRE(mz_zip_writer_init_file(&out, path.c_str(), 0));
    for (const auto& [name, data] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&out, name.c_str(), data.data(), data.size(), 0));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&out));
    mz_zip_writer_end(&out);
}

} // namespace

TEST_CASE("Project files keep quad layouts, solids and deviations", "[commands][project]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];
    const Json built = s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}});
    const auto solid = built["id"].get<ObjectId>();
    const auto dev = s.run("deviation.compute", {{"id", mesh}, {"reference_id", solid}})["id"];
    const auto path = (dir.path() / "m2.m2c").string();
    CHECK(s.run("project.save", {{"path", path}})["object_count"] == 4);

    Session t;
    t.run("project.open", {{"path", path}});
    const auto a = s.document.find(cage.get<ObjectId>());
    const auto b = t.document.find(cage.get<ObjectId>());
    REQUIRE(b);
    REQUIRE(b->quadLayout() != nullptr);
    CHECK(b->quadLayout()->cage.positions() == a->quadLayout()->cage.positions());
    CHECK(b->quadLayout()->cage.faceVertices() == a->quadLayout()->cage.faceVertices());

    const auto opened = t.document.find(solid);
    REQUIRE(opened);
    REQUIRE(opened->solid() != nullptr);
    const Solid before = s.document.find(solid)->solid()->solid; // find() returns a copy
    const Solid& after = opened->solid()->solid;
    CHECK(after.summary().valid);
    CHECK(after.summary().faceCount == before.summary().faceCount);
    CHECK(after.summary().volumeMm3 == before.summary().volumeMm3);
    CHECK(after.display().positions() == before.display().positions());
    CHECK(after.faces().size() == before.faces().size());
    CHECK(after.faces().front().poles == before.faces().front().poles);
    CHECK(opened->provenance.command == "surface.build");

    const auto field = t.document.find(dev.get<ObjectId>());
    REQUIRE(field);
    REQUIRE(field->deviation() != nullptr);
    CHECK(field->deviation()->field.distances() ==
          s.document.find(dev.get<ObjectId>())->deviation()->field.distances());

    // The opened solid works as before.
    const auto step = (dir.path() / "again.step").string();
    CHECK(t.run("solid.export", {{"id", solid}, {"path", step}})["check"]["passed"] == true);

    // Merged faces cover more than one cage quad each.
    CHECK(built["merge"]["merged_faces"].get<int>() > 0);
    CHECK(std::any_of(before.faces().begin(), before.faces().end(), [](const SurfacePatch& f) {
        return f.cellsU * f.cellsV > 1;
    }));
}

TEST_CASE("Project files of format 2 open", "[commands][project]") {
    // Format 2 faces covered one cage quad each, as an unmerged solid's do.
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];
    const auto solid =
        s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}, {"merge", false}})["id"]
            .get<ObjectId>();
    const auto path = (dir.path() / "m2.m2c").string();
    s.run("project.save", {{"path", path}});
    const Solid before = s.document.find(solid)->solid()->solid;
    rewriteAsVersion2(path, before.faces());
    Session u;
    u.run("project.open", {{"path", path}});
    const auto old = u.document.find(solid);
    REQUIRE(old);
    REQUIRE(old->solid() != nullptr);
    CHECK(old->solid()->solid.summary().faceCount == before.summary().faceCount);
    CHECK(old->solid()->solid.display().positions() == before.display().positions());
    CHECK(old->solid()->solid.faces().back().uBreaks == before.faces().back().uBreaks);
    CHECK(old->solid()->solid.faces().back().poles == before.faces().back().poles);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The M2 commands (scan to CAD) run end to end through the registry.

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
    CHECK(result["cap_pieces"].get<int>() > 0);

    const auto object = s.document.find(solid);
    REQUIRE(object);
    REQUIRE(object->solid() != nullptr);
    CHECK(std::string(object->kind()) == "solid");
    CHECK(object->name == "ball (quads) (solid)");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{cage.get<ObjectId>(), mesh});
    CHECK(s.document.undoHistory().back().label == "surface.build");

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

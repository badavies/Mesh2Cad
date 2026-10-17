// SPDX-License-Identifier: GPL-3.0-or-later
// Every M1 command run end to end through the registry, as the UI, CLI, JSON channel and MCP
// server all do.

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/commands/Command.h"
#include "m2c/io/MeshIO.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

struct Session {
    CommandRegistry registry;
    Document document;
    CommandContext context{document};
    std::vector<Json> log;

    Session() {
        REQUIRE(registerBuiltinCommands(registry).has_value());
        context.sessionLog = [this](const Json& line) { log.push_back(line); };
    }

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

TEST_CASE("Mesh tools edit in place and undo as one step", "[commands][tools]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, addNoise(makeSphere(10.0, 3), 0.2), "ball");

    const Json smoothed = s.run("mesh.smooth", {{"id", id}, {"iterations", 5}});
    CHECK(smoothed["id"] == id);
    CHECK(s.document.undoHistory().back().label == "mesh.smooth");

    const Json simplified = s.run("mesh.simplify", {{"id", id}, {"target_ratio", 0.5}});
    CHECK(simplified["report"]["faces_after"].get<int>() <= 640);
    CHECK(simplified["stats"]["is_watertight"] == true);

    const Json repaired = s.run("mesh.repair", {{"id", id}});
    CHECK(repaired["stats"]["is_watertight"] == true);

    const Json remeshed = s.run("mesh.remesh", {{"id", id}, {"target_edge_mm", 2.0}});
    CHECK(remeshed["stats"]["is_watertight"] == true);

    const Json filled = s.run("holes.fill", {{"id", id}});
    CHECK(filled["report"]["filled"] == 0);

    const Json wrapped = s.run("mesh.wrap", {{"id", id}, {"alpha_mm", 2.0}});
    CHECK(wrapped["stats"]["is_watertight"] == true);
    CHECK(wrapped["id"] != id);

    s.run("mesh.flip", {{"id", id}});
    CHECK(s.run("mesh.info", {{"id", id}})["stats"]["signed_volume_mm3"].get<double>() < 0.0);
    s.run("doc.undo");
    CHECK(s.run("mesh.info", {{"id", id}})["stats"]["signed_volume_mm3"].get<double>() > 0.0);

    // A failing command changes nothing and adds no undo step.
    const auto steps = s.document.undoHistory().size();
    CHECK(s.tryRun("mesh.simplify", {{"id", id}, {"target_faces", 0}}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(s.tryRun("mesh.remesh", {{"id", id}}).error().code == ErrorCode::InvalidArgument);
    CHECK(s.document.undoHistory().size() == steps);
}

TEST_CASE("mesh.simplify stops at max_error_mm", "[commands][tools]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeSphere(10.0, 4), "ball"); // 5120 faces

    // With only an error limit there is no count target: it goes as far as the limit allows.
    const Json loose = s.run("mesh.simplify", {{"id", id}, {"max_error_mm", 1.0}});
    CHECK(loose["report"]["faces_after"].get<int>() < 1000);
    CHECK(loose["stats"]["is_watertight"] == true);
    s.run("doc.undo");

    // A target count as well: whichever comes first.
    const Json counted =
        s.run("mesh.simplify", {{"id", id}, {"max_error_mm", 1.0}, {"target_faces", 3000}});
    CHECK(counted["report"]["faces_after"].get<int>() < 3000);
    CHECK(counted["report"]["faces_after"].get<int>() >= 2998);

    CHECK(s.tryRun("mesh.simplify", {{"id", id}, {"max_error_mm", 0}}).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("Shell commands find, split and clean fragments", "[commands][shells]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(
        dir, combine({makeSphere(10.0, 2), makeBox(1, 1, 1, 1), makeBox(0.2, 0.2, 0.2)}), "scan");

    const Json shells = s.run("shells.detect", {{"id", id}});
    REQUIRE(shells["shells"].size() == 3);
    CHECK(shells["shells"][0]["is_main"] == true);

    const Json sel = s.run("shells.select", {{"id", id}, {"shells", {1, 2}}});
    CHECK(sel["face_count"] == 24);

    const Json cleaned = s.run("shells.clean", {{"id", id}, {"min_area_mm2", 1.0}});
    CHECK(cleaned["deleted_shells"] == 1);
    CHECK(s.run("shells.detect", {{"id", id}})["shells"].size() == 2);
    // The shell selection was on the old faces, so it went with them.
    CHECK_FALSE(s.document.find(sel["selection_id"].get<ObjectId>()).has_value());

    const Json split = s.run("shells.split", {{"id", id}});
    CHECK(split["created"].size() == 1);
    CHECK(s.run("shells.detect", {{"id", id}})["shells"].size() == 1);
    s.run("doc.undo");
    CHECK(s.run("shells.keep", {{"id", id}})["deleted_shells"] == 1);
}

TEST_CASE("Selections, face deletion and datum alignment through commands", "[commands][align]") {
    Session s;
    const TempDir dir;
    const TriMesh box = makeBox(40, 30, 20, 2);
    const ObjectId id = s.import(dir, box, "bracket");

    // Rotate it away from the axes first.
    const double c = std::cos(0.5), sn = std::sin(0.5);
    s.run("align.transform",
          {{"id", id}, {"rotation", {c, -sn, 0, sn, c, 0, 0, 0, 1}}, {"translation", {5, 6, 7}}});

    // Pick the bottom by casting a ray up from below and flooding across flat edges.
    const Json hit = s.run("mesh.pick", {{"origin", {10, 20, -100}}, {"direction", {0, 0, 1}}});
    REQUIRE(hit["hit"] == true);
    const Json bottom = s.run("select.crease", {{"id", id}, {"face", hit["face"]}});
    CHECK(bottom["face_count"] == 8);
    const Json grown = s.run("select.grow", {{"selection_id", bottom["selection_id"]}});
    CHECK(grown["face_count"].get<int>() > 8);
    s.run("select.shrink", {{"selection_id", bottom["selection_id"]}});

    const Json front = s.run("mesh.pick", {{"origin", {0, -100, 10}}, {"direction", {0, 1, 0}}});
    const Json frontSel = s.run("select.crease", {{"id", id}, {"face", front["face"]}});
    const Json plane = s.run("plane.fit", {{"selection_id", bottom["selection_id"]}});
    CHECK_THAT(std::abs(plane["normal"][2].get<double>()), WithinAbs(1.0, 1e-9));

    const Json aligned = s.run(
        "align.datum",
        {{"id", id}, {"primary", bottom["selection_id"]}, {"secondary", frontSel["selection_id"]}});
    CHECK_THAT(aligned["stats"]["bounds"]["min"][2].get<double>(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(aligned["stats"]["bounds"]["min"][1].get<double>(), WithinAbs(0.0, 1e-9));
    // The selections survive alignment (same faces).
    CHECK(s.document.find(bottom["selection_id"].get<ObjectId>()).has_value());

    // align.reset goes back to the original pose.
    const Json reset = s.run("align.reset", {{"id", id}});
    CHECK_THAT(reset["stats"]["bounds"]["max"][0].get<double>(), WithinAbs(40.0, 1e-9));
    CHECK_THAT(reset["stats"]["bounds"]["min"][1].get<double>(), WithinAbs(0.0, 1e-9));

    const Json principal = s.run("align.principal", {{"id", id}, {"origin", "bounds_center"}});
    CHECK_THAT(principal["stats"]["bounds"]["max"][0].get<double>(), WithinAbs(20.0, 1e-9));

    // Delete the bottom faces: the mesh opens and the used selection is gone.
    const Json all = s.run("select.all", {{"id", id}});
    s.run("select.invert", {{"selection_id", all["selection_id"]}});
    const Json brush = s.run("select.brush",
                             {{"id", id},
                              {"center", {0, 0, -10}},
                              {"radius_mm", 5.0},
                              {"selection_id", all["selection_id"]}});
    CHECK(brush["face_count"].get<int>() > 0);
    const Json deleted = s.run("faces.delete", {{"selection_id", all["selection_id"]}});
    CHECK(deleted["report"]["deleted_faces"] == brush["face_count"]);
    CHECK(deleted["stats"]["is_watertight"] == false);
    CHECK(s.tryRun("select.invert", {{"selection_id", all["selection_id"]}}).error().code ==
          ErrorCode::NotFound);
}

TEST_CASE("align.symmetry puts the mirror plane on X = 0 and reports the fit",
          "[commands][align]") {
    Session s;
    const TempDir dir;
    // Symmetric about y = 10 only; the import sees it turned and moved.
    const TriMesh part = combine({makeBox(40, 20, 10, 4), makeBox(10, 20, 15, 2)});
    const Transform3d pose{{0, -1, 0, 1, 0, 0, 0, 0, 1}, {5, 6, 7}}; // 90 degrees about Z
    const ObjectId id = s.import(dir, *transformMesh(part, pose), "part");

    const Json aligned = s.run("align.symmetry", {{"id", id}, {"origin", "bounds_bottom_center"}});
    CHECK(aligned["report"]["residual_mm"].get<double>() < 1e-6);
    const Json& bounds = aligned["stats"]["bounds"];
    CHECK_THAT(bounds["min"][0].get<double>(), WithinAbs(-10.0, 1e-6)); // 20 wide, centred
    CHECK_THAT(bounds["max"][0].get<double>(), WithinAbs(10.0, 1e-6));
    CHECK_THAT(bounds["min"][2].get<double>(), WithinAbs(0.0, 1e-9));
    CHECK(s.document.undoHistory().back().label == "align.symmetry");

    CHECK(s.tryRun("align.symmetry", {{"id", id}, {"origin", "corner"}}).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("A brush stroke is one command and one undo step", "[commands][select]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeBox(40, 10, 10, 8), "bar");
    const auto steps = s.document.undoHistory().size();
    const Json one =
        s.run("select.brush", {{"id", id}, {"center", {0, 5, 10}}, {"radius_mm", 3.0}});
    const Json stroke = s.run(
        "select.brush",
        {{"id", id}, {"centers", {{0, 5, 10}, {20, 5, 10}, {40, 5, 10}}}, {"radius_mm", 3.0}});
    CHECK(stroke["face_count"].get<int>() > 2 * one["face_count"].get<int>());
    CHECK(s.document.undoHistory().size() == steps + 2);
    CHECK(s.tryRun("select.brush", {{"id", id}, {"centers", Json::array()}, {"radius_mm", 3.0}})
              .error()
              .code == ErrorCode::InvalidArgument);
}

TEST_CASE("Measurements snap to meshes and are saved objects", "[commands][measure]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeBox(10, 10, 10, 1), "cube");
    const Json m = s.run("measure.distance",
                         {{"a", {{"id", id}, {"point", {0.1, -0.1, 0.05}}}},
                          {"b", {{"point", {10, 10, 10}}}},
                          {"snap_mm", 0.5}});
    CHECK(m["a"]["snap"] == "vertex");
    CHECK(m["b"]["snap"] == "free");
    CHECK_THAT(m["distance_mm"].get<double>(), WithinAbs(std::sqrt(300.0), 1e-9));
    const auto listed = s.run("doc.list")["objects"];
    REQUIRE(listed.size() == 2);
    CHECK(listed[1]["kind"] == "measurement");

    // Moving the mesh moves the measured point that sits on it.
    s.run("align.transform", {{"id", id}, {"translation", {1, 0, 0}}});
    const auto moved = s.run("doc.list")["objects"][1]["measurement"];
    CHECK(moved["a"]["point"][0] == 1.0);
    CHECK(moved["b"]["point"][0] == 10.0);

    // Deleting the mesh deletes its measurement.
    s.run("object.delete", {{"id", id}});
    CHECK(s.run("doc.list")["objects"].empty());
}

TEST_CASE("Projects save and reopen the whole document", "[commands][project]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeBox(10, 10, 10, 2), "cube");
    s.run("align.transform", {{"id", id}, {"translation", {1, 2, 3}}});
    s.run("select.crease", {{"id", id}, {"face", 0}, {"name", "Bottom"}});
    s.run("object.rename", {{"id", id}, {"name", "Housing"}});
    s.run("object.show", {{"id", id}, {"visible", false}});
    const auto path = (dir.path() / "work.m2c").string();
    s.run("project.save", {{"path", path}});
    const Json before = s.run("doc.list");

    Session t;
    t.run("project.open", {{"path", path}});
    const Json after = t.run("doc.list");
    CHECK(after["objects"] == before["objects"]);
    CHECK_FALSE(t.document.canUndo());
    // Ids continue after the saved ones.
    const ObjectId next = t.import(dir, makeCube(), "next");
    CHECK(next > 2);
    CHECK(t.run("mesh.info", {{"id", id}})["alignment"]["translation"] ==
          Json::array({1.0, 2.0, 3.0}));

    t.run("project.new");
    CHECK(t.run("doc.list")["objects"].empty());
}

TEST_CASE("Replaying the session log reproduces the document", "[commands][replay]") {
    Session s;
    s.context.source = "test";
    const TempDir dir;
    const ObjectId id = s.import(dir, addNoise(makeSphere(10.0, 3), 0.2), "ball");
    s.run("mesh.smooth", {{"id", id}});
    s.run("mesh.simplify", {{"id", id}, {"target_faces", 500}});
    s.run("select.brush", {{"id", id}, {"center", {10, 0, 0}}, {"radius_mm", 4.0}});
    s.run("align.principal", {{"id", id}});
    s.run("mesh.info", {{"id", id}}); // queries are not logged
    REQUIRE(s.log.size() == 5);
    CHECK(s.log[1]["source"] == "test");
    CHECK(s.document.undoHistory().back().source == "test");

    Session replay;
    for (const Json& line : s.log) {
        replay.run(line["command"].get<std::string>(), line["params"]);
    }
    const auto a = s.document.findMesh(id);
    const auto b = replay.document.findMesh(id);
    REQUIRE((a && b));
    CHECK(a->mesh->positions() == b->mesh->positions()); // bit-identical on one platform
    CHECK(a->mesh->indices() == b->mesh->indices());
    CHECK(replay.run("doc.list") == s.run("doc.list"));
}

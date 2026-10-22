// SPDX-License-Identifier: GPL-3.0-or-later
// The M2 commands (scan to CAD) run end to end through the registry.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <miniz.h>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "m2c/commands/Command.h"
#include "m2c/core/Sketch.h"
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

TEST_CASE("quad.remesh lays out cage edges along guides", "[commands][quad]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeSphere(20.0, 4), "ball");
    // Half the equator, as drawn on the surface.
    Json half = Json::array();
    for (int i = 0; i <= 16; ++i) {
        const double a = std::numbers::pi * i / 16;
        half.push_back({20.0 * std::cos(a), 20.0 * std::sin(a), 0.0});
    }
    const Json guided = s.run(
        "quad.remesh", {{"id", mesh}, {"target_faces", 400}, {"guides", Json::array({half})}});
    CHECK(guided["stats"]["closed"] == true);
    CHECK(guided["stats"]["euler_characteristic"] == 2);
    // Not a list of polylines; a polyline of one point; a point that is not three numbers; a
    // guide off the surface.
    const auto rejects = [&](const Json& lines) {
        return !s.tryRun("quad.remesh", {{"id", mesh}, {"guides", lines}});
    };
    const auto point = [](double x, double y, double z) { return Json::array({x, y, z}); };
    CHECK(rejects(half));
    CHECK(rejects(Json::array({Json::array({point(20, 0, 0)})})));
    CHECK(rejects(Json::array({Json::array({Json::array({20.0, 0.0}), point(0, 20, 0)})})));
    CHECK(rejects(Json::array({Json::array({point(0, 0, 0), point(1, 0, 0)})})));
}

TEST_CASE("quad.remesh keeps the outline of a scanned sheet", "[commands][quad]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeCap(20.0, std::numbers::pi / 3.0, 24), "lid");
    const Json sheet =
        s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}, {"keep_outline", true}});
    CHECK(sheet["stats"]["closed"] == false);
    CHECK(sheet["stats"]["euler_characteristic"] == 1); // a disc
    CHECK(sheet["cleanup"]["filled_loops"] == 0);
    // Without it the rim is filled.
    const Json closed = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}});
    CHECK(closed["stats"]["closed"] == true);
    // A surface needs a closed cage.
    CHECK_FALSE(s.tryRun("surface.build", {{"id", sheet["id"]}, {"mesh_id", mesh}}));
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
    // Both long commands say what stage they are at, so a front end can show it.
    std::vector<std::string> stages;
    s.context.progress = [&stages](std::string_view stage, double) { stages.emplace_back(stage); };
    const auto cage = s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"];
    REQUIRE(stages.size() == 3);
    CHECK(stages.front() == "remeshing into quads");
    stages.clear();

    // The mesh is needed for fitting.
    CHECK_FALSE(s.tryRun("surface.build", {{"id", cage}}));
    CHECK_FALSE(s.tryRun("surface.build", {{"id", mesh}, {"mesh_id", mesh}})); // not a cage

    const Json result = s.run("surface.build", {{"id", cage}, {"mesh_id", mesh}});
    const auto solid = result["id"].get<ObjectId>();
    REQUIRE(stages.size() == 5);
    CHECK(stages[0].starts_with("fitting the cage ("));
    CHECK(stages[1] == "making the faces");
    CHECK(stages[3] == "sewing the solid");
    CHECK(stages.back() == "checking that no faces cross");
    CHECK(result["summary"]["valid"] == true);
    CHECK(result["summary"]["free_edges"] == 0);
    CHECK(result["summary"]["closed"] == true);
    CHECK(result["summary"]["solid_count"] == 1);
    CHECK_THAT(result["summary"]["bounds"]["max"][0].get<double>(),
               Catch::Matchers::WithinAbs(20.0, 0.1));
    CHECK(result["seams"]["max_gap_mm"].get<double>() < 1e-6);
    CHECK(result["seams"]["max_normal_angle_deg"].get<double>() < 1.0);
    CHECK(result["seams"]["max_cap_normal_angle_deg"] == result["seams"]["max_normal_angle_deg"]);
    CHECK(result["seams"]["max_regular_normal_angle_deg"].get<double>() < 1e-3);
    CHECK(result["seams"]["unmatched_samples"] == 0);
    CHECK(result["summary"]["volume_mm3"].get<double>() > 33000.0);
    CHECK(result["summary"]["volume_mm3"].get<double>() < 33600.0);
    CHECK(result["fit"]["rms_distance_mm"].get<double>() < 0.05);
    CHECK(result["fit"]["fits"] == 1);
    CHECK(result["fit"]["smoothing"] == 0.05);
    CHECK(result["fit"]["refinements"] == 0);
    CHECK(result["fit"]["p95_distance_mm"].get<double>() <=
          result["fit"]["max_distance_mm"].get<double>());
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

    // Refining to a target the first fit misses subdivides the cage once and fits again.
    const double p95 = result["fit"]["p95_distance_mm"].get<double>();
    const Json refined = s.run(
        "surface.build",
        {{"id", cage}, {"mesh_id", mesh}, {"refine_to_mm", p95 / 2.0}, {"max_refinements", 1}});
    CHECK(refined["summary"]["valid"] == true);
    CHECK(refined["fit"]["refinements"] == 1);
    CHECK(refined["fit"]["p95_before_refinement_mm"] == Json::array({p95}));
    CHECK(refined["fit"]["p95_distance_mm"].get<double>() < p95);
    CHECK_FALSE(
        s.tryRun("surface.build", {{"id", cage}, {"mesh_id", mesh}, {"max_refinements", 4}}));

    // Without fitting, no mesh is needed.
    const Json unfitted = s.run("surface.build", {{"id", cage}, {"fit", false}});
    CHECK(unfitted["summary"]["valid"] == true);
    CHECK_FALSE(unfitted.contains("fit"));
}

TEST_CASE("surface.build holds a selected flat area on its fitted plane", "[commands][surface]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, addNoise(makeRoundedCube(10.0, 8.0, 4), 0.03), "box");
    const ObjectId ball = s.import(dir, makeSphere(5.0, 3), "ball");
    const auto cage = s.run("quad.remesh", {{"id", box}, {"target_faces", 300}})["id"];
    const auto top = s.run(
        "select.brush",
        {{"id", box}, {"centers", Json::array({{0, 0, 10}})}, {"radius_mm", 5.0}})["selection_id"];
    const auto plane = s.run("primitive.fit", {{"selection_id", top}, {"kind", "plane"}})["id"];
    const Json flat = Json::array({{{"selection_id", top}, {"plane_id", plane}}});

    const Json built = s.run("surface.build", {{"id", cage}, {"mesh_id", box}, {"flat", flat}});
    CHECK(built["summary"]["valid"] == true);
    REQUIRE(built["fit"]["held_vertices"].size() == 1);
    CHECK(built["fit"]["held_vertices"][0].get<int>() > 0);
    const auto object = s.document.find(built["id"].get<ObjectId>());
    REQUIRE(object);
    CHECK(object->provenance.inputs ==
          std::vector<ObjectId>{
              cage.get<ObjectId>(), box, top.get<ObjectId>(), plane.get<ObjectId>()});
    CHECK(s.run("surface.build", {{"id", cage}, {"mesh_id", box}})["fit"]["held_vertices"] ==
          Json::array());

    // The area must be a selection of the mesh fitted to, held by a plane, while fitting.
    const auto everything = s.run("select.all", {{"id", ball}})["selection_id"];
    const auto sphere =
        s.run("primitive.fit", {{"selection_id", everything}, {"kind", "sphere"}})["id"];
    const std::size_t objects = s.document.objects().size();
    const auto fails = [&](const Json& params) {
        const auto r = s.tryRun("surface.build", params);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    };
    fails({{"id", cage},
           {"mesh_id", box},
           {"flat", Json::array({{{"selection_id", everything}, {"plane_id", plane}}})}});
    fails({{"id", cage},
           {"mesh_id", box},
           {"flat", Json::array({{{"selection_id", top}, {"plane_id", sphere}}})}});
    fails({{"id", cage}, {"mesh_id", box}, {"fit", false}, {"flat", flat}});
    fails({{"id", cage}, {"fit", false}, {"flat", flat}});
    fails({{"id", cage}, {"mesh_id", box}, {"flat", Json::array({{{"selection_id", top}}})}});
    // A list of bare ids, or one area without its list, says what to give.
    for (const Json& flatWrong :
         {Json::array({top, plane}), Json{{"selection_id", top}, {"plane_id", plane}}}) {
        const auto r =
            s.tryRun("surface.build", {{"id", cage}, {"mesh_id", box}, {"flat", flatWrong}});
        REQUIRE_FALSE(r);
        CHECK_THAT(r.error().message,
                   Catch::Matchers::ContainsSubstring(R"([{"selection_id": 5, "plane_id": 12})"));
    }
    CHECK(s.document.objects().size() == objects);
}

TEST_CASE("A scanned sheet becomes a solid wall", "[commands][surface]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeCap(20.0, std::numbers::pi / 3.0, 24), "lid");
    const Json sheet =
        s.run("quad.remesh", {{"id", mesh}, {"target_faces", 300}, {"keep_outline", true}});
    CHECK(sheet["stats"]["closed"] == false);
    CHECK(sheet["stats"]["euler_characteristic"] == 1); // a disc
    // An open cage needs a thickness.
    CHECK_FALSE(s.tryRun("surface.build", {{"id", sheet["id"]}, {"mesh_id", mesh}}));
    const Json wall =
        s.run("surface.build", {{"id", sheet["id"]}, {"mesh_id", mesh}, {"thickness_mm", 2.0}});
    CHECK(wall["summary"]["valid"] == true);
    CHECK(wall["summary"]["closed"] == true);
    CHECK(wall["summary"]["solid_count"] == 1);
    CHECK(wall["fit"]["p95_distance_mm"].get<double>() < 0.05);
    CHECK_FALSE(s.tryRun("surface.build",
                         {{"id", sheet["id"]}, {"mesh_id", mesh}, {"thickness_mm", -1.0}}));
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

TEST_CASE("solid.export writes sketches and planes beside the solid", "[commands][surface]") {
    Session s;
    const TempDir dir;
    const ObjectId can = s.import(dir, makeCylinder(10.0, 30.0, 256, 6), "can");
    const Json section =
        s.run("sketch.section", {{"id", can}, {"plane", "xy"}, {"offset_mm", 12}, {"name", "Mid"}});
    const auto solid = s.run("solid.extrude", {{"id", section["id"]}, {"distance_mm", 5}})["id"];
    const Json regions = s.run("mesh.segment", {{"id", can}})["regions"];
    REQUIRE(regions[0]["kind"] == "plane");
    const Json cap = s.run("primitive.fit", {{"selection_id", regions[0]["selection_id"]}});
    const auto path = (dir.path() / "can.step").string();

    const Json result = s.run("solid.export",
                              {{"id", solid},
                               {"sketch_ids", {section["id"]}},
                               {"plane_ids", {cap["id"]}},
                               {"path", path}});
    CHECK(result["id"] == solid);
    CHECK(result["check"]["passed"] == true);
    CHECK(result["check"]["solid_count"] == 1);
    CHECK(result["check"]["volume_error"].get<double>() < 1e-3);
    CHECK(result["check"]["sketch_edge_count"] == 1); // the section is one circle
    CHECK(result["check"]["plane_face_count"] == 1);

    // Sketches alone: no solid in the file.
    const auto sketches = (dir.path() / "sketch.step").string();
    const Json alone = s.run("solid.export", {{"sketch_ids", {section["id"]}}, {"path", sketches}});
    CHECK(alone["check"]["passed"] == true);
    CHECK(alone["check"]["solid_count"] == 0);
    CHECK_FALSE(alone.contains("id"));

    // Nothing to write, or a primitive that is not a plane: an error and no file.
    const auto bad = (dir.path() / "bad.step").string();
    CHECK_FALSE(s.tryRun("solid.export", {{"path", bad}}));
    std::optional<Json> wall;
    for (const Json& r : regions) {
        if (r["kind"] == "cylinder") {
            wall = s.run("primitive.fit", {{"selection_id", r["selection_id"]}});
        }
    }
    REQUIRE(wall.has_value());
    CHECK_FALSE(s.tryRun("solid.export", {{"plane_ids", {(*wall)["id"]}}, {"path", bad}}));
    CHECK_FALSE(s.tryRun("solid.export", {{"sketch_ids", {solid}}, {"path", bad}}));
    CHECK_FALSE(std::filesystem::exists(bad));
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

    // Info returns the statistics compute returned, and takes only deviations.
    const Json info = s.run("deviation.info", {{"id", dev}});
    CHECK(info["stats"] == toSolid["stats"]);
    CHECK(info["mesh_id"] == bigger);
    CHECK(info["reference_id"] == solid);
    const auto wrongKind = s.tryRun("deviation.info", {{"id", mesh}});
    REQUIRE_FALSE(wrongKind);
    CHECK(wrongKind.error().code == ErrorCode::InvalidArgument);
    const auto unknown = s.tryRun("deviation.info", {{"id", 9999}});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == ErrorCode::NotFound);
    const Command* infoCommand = s.registry.find("deviation.info");
    REQUIRE(infoCommand != nullptr);
    CHECK(infoCommand->paramsSchema["required"] == Json::array({"id"}));
    CHECK_FALSE(infoCommand->mutatesDocument);
    CHECK_FALSE(infoCommand->writesFiles);

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

TEST_CASE("deviation.compute reports hotspots and selects them", "[commands][deviation]") {
    // A ball moved 1 mm along x against itself: outside on +x, inside on -x, in tolerance between.
    Session s;
    const TempDir dir;
    const ObjectId reference = s.import(dir, makeSphere(20.0, 4), "reference");
    const ObjectId moved = s.import(dir, makeSphere(20.0, 4, {1, 0, 0}), "moved");
    const std::size_t objects = s.document.objects().size();
    const Json result =
        s.run("deviation.compute",
              {{"id", moved}, {"reference_id", reference}, {"select_hotspots", true}});
    const Json& hotspots = result["hotspots"];
    REQUIRE(hotspots.size() == 2);
    CHECK(hotspots[0]["area_mm2"].get<double>() >= hotspots[1]["area_mm2"].get<double>());
    CHECK(std::set<std::string>{hotspots[0]["sign"], hotspots[1]["sign"]} ==
          std::set<std::string>{"+", "-"});
    for (const Json& h : hotspots) {
        CAPTURE(h.dump());
        const auto selection = s.document.find(h["selection_id"].get<ObjectId>());
        REQUIRE(selection);
        REQUIRE(selection->selection() != nullptr);
        CHECK(selection->selection()->meshId == moved);
        CHECK(selection->selection()->faces->count() == h["face_count"].get<std::size_t>());
        CHECK(selection->name == "Deviation " + h["sign"].get<std::string>() + "1");
        const bool outside = h["sign"] == "+";
        CHECK((h["centroid"][0].get<double>() > 10.0) == outside);
        CHECK((h["max_signed_mm"].get<double>() > 0.9) == outside);
        CHECK((h["max_signed_mm"].get<double>() < -0.9) == !outside);
    }
    // deviation.info reports the same hotspots.
    Json plain = hotspots;
    for (Json& h : plain) h.erase("selection_id");
    CHECK(s.run("deviation.info", {{"id", result["id"]}})["hotspots"] == plain);

    // One undo takes the deviation and its selections.
    CHECK(s.document.objects().size() == objects + 3);
    REQUIRE(s.document.undo());
    CHECK(s.document.objects().size() == objects);

    // A limit, none, and none to find.
    CHECK(s.run("deviation.compute",
                {{"id", moved}, {"reference_id", reference}, {"max_hotspots", 1}})["hotspots"]
              .size() == 1);
    CHECK(s.run("deviation.compute",
                {{"id", moved}, {"reference_id", reference}, {"max_hotspots", 0}})["hotspots"]
              .empty());
    CHECK(s.run("deviation.compute",
                {{"id", moved}, {"reference_id", reference}, {"tolerance_mm", 2.0}})["hotspots"]
              .empty());
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

TEST_CASE("primitive.trim trims fitted planes into a box", "[commands][trim]") {
    Session s;
    const TempDir dir;
    const ObjectId mesh = s.import(dir, makeBox(30, 20, 10, 8), "block");
    const Json regions = s.run("mesh.segment", {{"id", mesh}})["regions"];
    REQUIRE(regions.size() == 6);
    Json primitives = Json::array();
    for (const Json& r : regions) {
        primitives.push_back(s.run("primitive.fit", {{"selection_id", r["selection_id"]}})["id"]);
    }

    // One plane short, the faces leave the box open and nothing is added.
    const std::size_t before = s.document.objects().size();
    Json five = primitives;
    five.erase(five.size() - 1);
    const auto open = s.tryRun("primitive.trim", {{"primitive_ids", five}, {"mesh_id", mesh}});
    REQUIRE_FALSE(open.has_value());
    CHECK(open.error().code == ErrorCode::InvalidState);
    CHECK(open.error().message.find("open between") != std::string::npos);
    CHECK(s.document.objects().size() == before);
    CHECK_FALSE(s.tryRun("primitive.trim", {{"primitive_ids", {mesh}}, {"mesh_id", mesh}}));
    CHECK_FALSE(s.tryRun("primitive.trim", {{"mesh_id", mesh}}));

    const Json trimmed =
        s.run("primitive.trim", {{"primitive_ids", primitives}, {"mesh_id", mesh}});
    CHECK(trimmed["summary"]["face_count"] == 6);
    CHECK(trimmed["summary"]["edge_count"] == 12);
    CHECK(trimmed["summary"]["valid"] == true);
    CHECK(trimmed["summary"]["pole_count"] == 0);
    CHECK_THAT(trimmed["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(6000.0, 1e-9));
    CHECK(trimmed["kept_faces"] == 6);
    CHECK(trimmed["max_gap_mm"].get<double>() < 1e-3);
    const auto solid = trimmed["id"].get<ObjectId>();
    const auto object = s.document.find(solid);
    REQUIRE(object);
    CHECK(object->name == "block (trimmed solid)");
    CHECK(object->provenance.inputs.size() == 7);

    // The scan lies on the solid's faces.
    const Json deviation = s.run("deviation.compute", {{"id", mesh}, {"reference_id", solid}});
    CHECK(deviation["stats"]["max_mm"].get<double>() < 1e-6);
    CHECK(deviation["stats"]["min_mm"].get<double>() > -1e-6);
    std::vector<Vec3d> bigger = makeBox(32, 22, 12, 8).positions(); // 1 mm all round
    for (Vec3d& v : bigger) v = v - Vec3d{1, 1, 1};
    const ObjectId around =
        s.import(dir, makeMesh(std::move(bigger), makeBox(32, 22, 12, 8).indices()), "around");
    const Json outside = s.run("deviation.compute", {{"id", around}, {"reference_id", solid}});
    CHECK_THAT(outside["stats"]["min_mm"].get<double>(), Catch::Matchers::WithinAbs(1.0, 1e-9));

    const auto step = (dir.path() / "block.step").string();
    CHECK(s.run("solid.export", {{"id", solid}, {"path", step}})["check"]["passed"] == true);

    // A project keeps the trimmed solid as its kernel shape alone.
    const auto path = (dir.path() / "block.m2c").string();
    s.run("project.save", {{"path", path}});
    Session t;
    t.run("project.open", {{"path", path}});
    const auto opened = t.document.find(solid);
    REQUIRE(opened);
    REQUIRE(opened->solid() != nullptr);
    const Solid& after = opened->solid()->solid;
    CHECK(after.summary().faceCount == 6);
    CHECK(after.summary().volumeMm3 == object->solid()->solid.summary().volumeMm3);
    CHECK(after.faces().empty());
    CHECK(after.display().positions() == object->solid()->solid.display().positions());
}

namespace {

/// A box scan from `at` to `at + size`, trimmed from its fitted planes.
ObjectId trimmedBox(
    Session& s, const TempDir& dir, const Vec3d& at, const Vec3d& size, const std::string& name) {
    const TriMesh scan = makeBox(size.x, size.y, size.z, 8);
    std::vector<Vec3d> p = scan.positions();
    for (Vec3d& v : p) v = v + at;
    const ObjectId mesh = s.import(dir, makeMesh(std::move(p), scan.indices()), name);
    const Json regions = s.run("mesh.segment", {{"id", mesh}})["regions"];
    Json primitives = Json::array();
    for (const Json& r : regions) {
        primitives.push_back(s.run("primitive.fit", {{"selection_id", r["selection_id"]}})["id"]);
    }
    return s
        .run("primitive.trim",
             {{"primitive_ids", primitives}, {"mesh_id", mesh}, {"name", name}})["id"]
        .get<ObjectId>();
}

} // namespace

TEST_CASE("solid.boolean fuses, cuts and intersects trimmed boxes", "[commands][boolean]") {
    Session s;
    const TempDir dir;
    const ObjectId a = trimmedBox(s, dir, {0, 0, 0}, {30, 20, 10}, "a");
    const ObjectId b = trimmedBox(s, dir, {10, 5, 5}, {30, 20, 10}, "b");
    const ObjectId far = trimmedBox(s, dir, {100, 0, 0}, {30, 20, 10}, "far");
    const double overlap = 20.0 * 15.0 * 5.0;

    const Json fused = s.run("solid.boolean", {{"id", a}, {"tool_id", b}, {"operation", "fuse"}});
    CHECK(fused["summary"]["valid"] == true);
    CHECK(fused["summary"]["solid_count"] == 1);
    CHECK_THAT(fused["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(12000.0 - overlap, 1e-9));
    const auto object = s.document.find(fused["id"].get<ObjectId>());
    REQUIRE(object);
    CHECK(object->name == "a fuse b");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{a, b});
    CHECK_THAT(s.run("solid.info", {{"id", a}})["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(6000.0, 1e-9));

    const Json cut = s.run("solid.boolean",
                           {{"id", a}, {"tool_id", b}, {"operation", "cut"}, {"name", "notched"}});
    CHECK_THAT(cut["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(6000.0 - overlap, 1e-9));
    const auto step = (dir.path() / "notched.step").string();
    CHECK(s.run("solid.export", {{"id", cut["id"]}, {"path", step}})["check"]["passed"] == true);

    // Disjoint solids share nothing; nothing is added for a failure or a bad request.
    const std::size_t before = s.document.objects().size();
    const auto none =
        s.tryRun("solid.boolean", {{"id", a}, {"tool_id", far}, {"operation", "common"}});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::InvalidState);
    CHECK_FALSE(s.tryRun("solid.boolean", {{"id", a}, {"tool_id", a}, {"operation", "fuse"}}));
    CHECK_FALSE(s.tryRun("solid.boolean", {{"id", a}, {"tool_id", b}, {"operation", "glue"}}));
    CHECK(s.document.objects().size() == before);
}

TEST_CASE("primitive.solid makes a fitted cylinder a solid that cuts a true hole",
          "[commands][boolean]") {
    Session s;
    const TempDir dir;
    const ObjectId can = s.import(dir, makeCylinder(5.0, 30.0, 96, 30), "pin");
    const Json regions = s.run("mesh.segment", {{"id", can}})["regions"];
    REQUIRE(regions.size() == 3);
    REQUIRE(regions[2]["kind"] == "cylinder");
    const Json fit = s.run("primitive.fit", {{"selection_id", regions[2]["selection_id"]}});
    const double r = fit["radius_mm"].get<double>();
    const auto primitive = fit["id"].get<ObjectId>();

    const Json pin = s.run("primitive.solid", {{"id", primitive}});
    CHECK(pin["summary"]["valid"] == true);
    CHECK(pin["summary"]["face_count"] == 3);
    CHECK_THAT(pin["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * r * r * 30.0, 1e-9));
    const auto object = s.document.find(pin["id"].get<ObjectId>());
    REQUIRE(object);
    CHECK(object->name == "Cylinder fit (solid)");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{primitive});
    const Json longer = s.run("primitive.solid", {{"id", primitive}, {"length_mm", 40}});
    CHECK_THAT(longer["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * r * r * 40.0, 1e-9));

    // Through a plate across the middle of the pin, it leaves one exact cylindrical face.
    const ObjectId plate = trimmedBox(s, dir, {-15, -15, 10}, {30, 30, 10}, "plate");
    const Json holed =
        s.run("solid.boolean", {{"id", plate}, {"tool_id", pin["id"]}, {"operation", "cut"}});
    CHECK(holed["summary"]["face_count"] == 7);
    CHECK_THAT(holed["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(9000.0 - std::numbers::pi * r * r * 10.0, 1e-9));

    // A plane does not make a solid, and nothing is added.
    const std::size_t before = s.document.objects().size();
    const Json cap = s.run("primitive.fit", {{"selection_id", regions[0]["selection_id"]}});
    const auto flat = s.tryRun("primitive.solid", {{"id", cap["id"]}});
    REQUIRE_FALSE(flat.has_value());
    CHECK(flat.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(s.tryRun("primitive.solid", {{"id", plate}}));
    CHECK(s.document.objects().size() == before + 1);
}

TEST_CASE("primitive.box fits a box to a selection or makes one from its size", "[commands][box]") {
    Session s;
    const TempDir dir;
    const ObjectId scan = s.import(dir, addNoise(makeBox(30, 20, 10, 8), 0.02), "block");
    const auto all = s.run("select.all", {{"id", scan}})["selection_id"];
    const Json fitted = s.run("primitive.box", {{"selection_id", all}});
    CHECK(fitted["summary"]["valid"] == true);
    CHECK(fitted["summary"]["face_count"] == 6);
    CHECK_THAT(fitted["size_mm"][0].get<double>(), Catch::Matchers::WithinAbs(30.0, 0.05));
    CHECK_THAT(fitted["size_mm"][1].get<double>(), Catch::Matchers::WithinAbs(20.0, 0.05));
    CHECK_THAT(fitted["size_mm"][2].get<double>(), Catch::Matchers::WithinAbs(10.0, 0.05));
    CHECK_THAT(fitted["center"][2].get<double>(), Catch::Matchers::WithinAbs(5.0, 0.05));
    CHECK(fitted["fit"]["p95_mm"].get<double>() < 0.05);
    CHECK(fitted["fit"]["size_mm"] == fitted["size_mm"]);
    const auto object = s.document.find(fitted["id"].get<ObjectId>());
    REQUIRE(object);
    CHECK(object->name == "Box fit");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{all.get<ObjectId>(), scan});

    // Sizes snapped to nominal values keep the fitted centre and axes.
    const Json snapped = s.run("primitive.box", {{"selection_id", all}, {"size_mm", {30, 20, 10}}});
    CHECK_THAT(snapped["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinAbs(6000.0, 1e-6));
    CHECK(snapped["center"] == fitted["center"]);
    CHECK(snapped["fit"]["size_mm"] == fitted["size_mm"]);

    // Without a selection: a size, a centre and axes turned 45 degrees about z.
    const Json made = s.run("primitive.box",
                            {{"size_mm", {10, 10, 10}},
                             {"center", {0, 0, 10}},
                             {"axes", {{1, 1, 0}, {-1, 1, 0}, {0, 0, 1}}},
                             {"name", "die"}});
    CHECK_FALSE(made.contains("fit"));
    CHECK_THAT(made["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinAbs(1000.0, 1e-6));
    CHECK_THAT(made["summary"]["bounds"]["max"][0].get<double>(),
               Catch::Matchers::WithinAbs(5.0 * std::sqrt(2.0), 1e-6));
    CHECK_THAT(made["summary"]["bounds"]["min"][2].get<double>(),
               Catch::Matchers::WithinAbs(5.0, 1e-6));
    CHECK(s.document.find(made["id"].get<ObjectId>())->name == "die");
    // The box is a solid like any other: cut a corner out of the block with it.
    const Json cut = s.run("solid.boolean",
                           {{"id", snapped["id"]}, {"tool_id", made["id"]}, {"operation", "cut"}});
    CHECK(cut["summary"]["valid"] == true);
    CHECK_THAT(cut["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinAbs(6000.0 - 100.0 * 5.0 / 4.0,
                                          2.0)); // the fitted centre is off by up to 0.05 mm

    // Failures add nothing.
    const ObjectId ball = s.import(dir, makeSphere(10.0, 3), "ball");
    const auto round = s.run("select.all", {{"id", ball}})["selection_id"];
    const std::size_t before = s.document.objects().size();
    const auto fails = [&](const Json& params, ErrorCode code) {
        const auto r = s.tryRun("primitive.box", params);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code);
    };
    fails(Json::object(), ErrorCode::InvalidArgument);
    fails({{"selection_id", round}}, ErrorCode::InvalidState);
    fails({{"size_mm", {10, 0, 10}}}, ErrorCode::InvalidArgument);
    fails({{"size_mm", {10, 10, 10}}, {"axes", {{1, 0, 0}, {1, 1, 0}, {0, 0, 1}}}},
          ErrorCode::InvalidArgument);
    fails({{"size_mm", {10, 10, 10}}, {"axes", {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}}},
          ErrorCode::InvalidArgument);
    fails({{"size_mm", {10, 10, 10}}, {"axes", {{1, 0, 0}, {0, 1, 0}}}},
          ErrorCode::InvalidArgument);
    CHECK(s.document.objects().size() == before);
}

TEST_CASE("fillet.fit rounds a box's edge at the scanned fillet's radius", "[commands][fillet]") {
    Session s;
    const TempDir dir;
    // The scan's fillet band: a quarter arc of radius 3 along the top back edge of a 30 x 20 x
    // 10 box, as a strip of triangles.
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    for (int i = 0; i <= 26; ++i) {
        for (int j = 0; j <= 12; ++j) {
            const double a = std::numbers::pi / 2.0 * j / 12.0;
            p.push_back({2.0 + i, 17.0 + 3.0 * std::cos(a), 7.0 + 3.0 * std::sin(a)});
            if (i > 0 && j > 0) {
                const auto at = [](int u, int v) { return static_cast<std::uint32_t>(u * 13 + v); };
                idx.insert(idx.end(),
                           {at(i - 1, j - 1),
                            at(i, j - 1),
                            at(i, j),
                            at(i - 1, j - 1),
                            at(i, j),
                            at(i - 1, j)});
            }
        }
    }
    const ObjectId scan = s.import(dir, addNoise(makeMesh(p, idx), 0.02), "band");
    const auto band = s.run("select.all", {{"id", scan}})["selection_id"];
    const auto block =
        s.run("primitive.box", {{"size_mm", {30, 20, 10}}, {"center", {15, 10, 5}}})["id"];
    const Json edge = Json::array({{15, 20, 10}});

    const Json fit =
        s.run("fillet.fit", {{"solid_id", block}, {"edge_points", edge}, {"selection_id", band}});
    CHECK_THAT(fit["radius_mm"].get<double>(), Catch::Matchers::WithinAbs(3.0, 0.05));
    CHECK(fit["p95_mm"].get<double>() < 0.05);
    CHECK(fit["builds"].get<int>() > 1);
    CHECK(fit["summary"]["valid"] == true);
    CHECK(fit["summary"]["face_count"] == 7);
    const auto object = s.document.find(fit["id"].get<ObjectId>());
    REQUIRE(object);
    CHECK(object->name == "Box (filleted)");
    CHECK(object->provenance.inputs ==
          std::vector<ObjectId>{block.get<ObjectId>(), band.get<ObjectId>(), scan});

    const Json typed = s.run(
        "fillet.fit",
        {{"solid_id", block}, {"edge_points", edge}, {"selection_id", band}, {"radius_mm", 3.0}});
    CHECK(typed["radius_mm"] == 3.0);
    CHECK(typed["builds"] == 1);

    // Failures add nothing: a typed radius far from the scan's, one too large for the box, no
    // edge points, and a mesh instead of a solid.
    const std::size_t before = s.document.objects().size();
    const auto fails = [&](const Json& params, ErrorCode code) {
        const auto r = s.tryRun("fillet.fit", params);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code);
    };
    fails({{"solid_id", block}, {"edge_points", edge}, {"selection_id", band}, {"radius_mm", 1.0}},
          ErrorCode::InvalidState);
    fails({{"solid_id", block}, {"edge_points", edge}, {"selection_id", band}, {"radius_mm", 12.0}},
          ErrorCode::InvalidState);
    fails({{"solid_id", block}, {"edge_points", Json::array()}, {"selection_id", band}},
          ErrorCode::InvalidArgument);
    fails({{"solid_id", scan}, {"edge_points", edge}, {"selection_id", band}},
          ErrorCode::InvalidArgument);
    CHECK(s.document.objects().size() == before);
}

TEST_CASE("A fuzzy rounded cube is drilled true for a scanned dowel", "[commands][boolean]") {
    Session s;
    const TempDir dir;
    // A 30 mm cube with rounded edges, and a 9 mm dowel 40 mm long through its middle, their skins
    // roughened by up to 0.15 mm as a printer's fuzzy skin does: no plane or cylinder fits them
    // within 0.1 mm.
    const ObjectId cube =
        s.import(dir, addNoise(makeRoundedCube(15, 4, 4, {15, 15, 15}), 0.15), "cube");
    std::vector<Vec3d> rod = makeCylinder(4.5, 40, 96, 40).positions();
    for (Vec3d& v : rod) v = v + Vec3d{15, 15, -5};
    const ObjectId dowel = s.import(
        dir,
        addNoise(makeMesh(std::move(rod), makeCylinder(4.5, 40, 96, 40).indices()), 0.15, 11),
        "dowel");
    const Json cage = s.run("quad.remesh", {{"id", cube}, {"target_faces", 400}});
    const Json body = s.run("surface.build", {{"id", cage["id"]}, {"mesh_id", cube}});
    REQUIRE(body["summary"]["valid"] == true);
    REQUIRE(body["summary"]["solid_count"] == 1);

    // Brush the dowel's side in the middle, and fit a cylinder to it.
    Json centers = Json::array();
    for (int k = 0; k < 16; ++k) {
        const double a = 2.0 * std::numbers::pi * k / 16;
        centers.push_back({15 + 4.5 * std::cos(a), 15 + 4.5 * std::sin(a), 15.0});
    }
    const Json side =
        s.run("select.brush", {{"id", dowel}, {"centers", centers}, {"radius_mm", 8.0}});
    const Json fit = s.run(
        "primitive.fit",
        {{"selection_id", side["selection_id"]}, {"kind", "cylinder"}, {"max_residual_mm", 0.3}});
    const double r = fit["radius_mm"].get<double>();
    CHECK_THAT(r, Catch::Matchers::WithinAbs(4.5, 0.05));
    CHECK_THAT(fit["axis_point"][0].get<double>(), Catch::Matchers::WithinAbs(15.0, 0.05));
    CHECK_THAT(fit["axis_point"][1].get<double>(), Catch::Matchers::WithinAbs(15.0, 0.05));

    // Drill a hole with 0.1 mm clearance for the dowel all the way through the cube.
    const double drill = r + 0.1;
    const Json bit =
        s.run("primitive.solid", {{"id", fit["id"]}, {"length_mm", 40.0}, {"radius_mm", drill}});
    CHECK_THAT(bit["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * drill * drill * 40.0, 1e-9));
    const Json drilled =
        s.run("solid.boolean", {{"id", body["id"]}, {"tool_id", bit["id"]}, {"operation", "cut"}});
    CHECK(drilled["summary"]["valid"] == true);
    CHECK(drilled["summary"]["closed"] == true);
    CHECK(drilled["summary"]["solid_count"] == 1);
    const double removed = body["summary"]["volume_mm3"].get<double>() -
                           drilled["summary"]["volume_mm3"].get<double>();
    CHECK_THAT(removed, Catch::Matchers::WithinRel(std::numbers::pi * drill * drill * 30.0, 0.02));

    // The hole's wall is the exact cylinder: points on it from z = 2 to 28 lie on the solid.
    Vec3d axis{fit["axis_direction"][0].get<double>(),
               fit["axis_direction"][1].get<double>(),
               fit["axis_direction"][2].get<double>()};
    if (axis.z < 0) axis = axis * -1.0;
    const Vec3d point{fit["axis_point"][0].get<double>(),
                      fit["axis_point"][1].get<double>(),
                      fit["axis_point"][2].get<double>()};
    const Vec3d across = cross(axis, Vec3d{1, 0, 0});
    const Vec3d u = across * (1.0 / length(across));
    const Vec3d v = cross(axis, u);
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    constexpr int around = 64, up = 13;
    const auto at = [&](const int j, const int i) {
        return static_cast<std::uint32_t>(j * around + i % around);
    };
    for (int j = 0; j <= up; ++j) {
        const Vec3d centre = point + axis * ((2.0 + 2.0 * j) - point.z);
        for (int i = 0; i < around; ++i) {
            const double a = 2.0 * std::numbers::pi * i / around;
            p.push_back(centre + (u * std::cos(a) + v * std::sin(a)) * drill);
        }
    }
    for (int j = 0; j < up; ++j) {
        for (int i = 0; i < around; ++i) {
            idx.insert(idx.end(), {at(j, i), at(j + 1, i + 1), at(j, i + 1)});
            idx.insert(idx.end(), {at(j, i), at(j + 1, i), at(j + 1, i + 1)});
        }
    }
    const ObjectId wall = s.import(dir, makeMesh(std::move(p), std::move(idx)), "hole wall");
    const Json onWall = s.run("deviation.compute", {{"id", wall}, {"reference_id", drilled["id"]}});
    CHECK(onWall["stats"]["max_mm"].get<double>() < 1e-4);
    CHECK(onWall["stats"]["min_mm"].get<double>() > -1e-4);

    const auto step = (dir.path() / "drilled.step").string();
    CHECK(s.run("solid.export", {{"id", drilled["id"]}, {"path", step}})["check"]["passed"] ==
          true);
}

TEST_CASE("sketch.section adds a sketch of a mesh cut by a plane", "[commands][sketch]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, makeBox(20, 10, 6, 4), "box");
    const std::size_t before = s.document.objects().size();

    const Json level = s.run("sketch.section", {{"id", box}, {"plane", "xy"}, {"offset_mm", 2.3}});
    REQUIRE(level["loops"].size() == 1);
    CHECK(level["loops"][0] == Json{{"lines", 4}, {"arcs", 0}, {"splines", 0}});
    CHECK(level["open_pieces"] == 0);
    CHECK(level["max_error_mm"].get<double>() < 1e-9);
    CHECK(level["plane"]["origin"] == Json::array({0.0, 0.0, 2.3}));
    const auto object = s.document.find(level["id"].get<ObjectId>());
    REQUIRE(object.has_value());
    REQUIRE(object->sketch() != nullptr);
    CHECK(std::string(object->kind()) == "sketch");
    CHECK(object->name == "Section");
    CHECK(object->provenance.inputs == std::vector<ObjectId>{box});
    CHECK(object->sketch()->sketch.loops.front().curves.size() == 4);
    CHECK(s.document.undoHistory().back().label == "sketch.section");

    // The zx plane has u along z and v along x, so the cut at y = 5 is 6 x 20 in the sketch.
    const Json side = s.run("sketch.section", {{"id", box}, {"plane", "zx"}, {"offset_mm", 5}});
    CHECK(side["plane"]["u"] == Json::array({0.0, 0.0, 1.0}));
    const auto sideSketch = s.document.find(side["id"].get<ObjectId>());
    REQUIRE(sideSketch.has_value());
    for (const SketchCurve& c : sideSketch->sketch()->sketch.loops.front().curves) {
        CHECK(c.start().x >= -1e-9);
        CHECK(c.start().x <= 6.0 + 1e-9);
        CHECK(c.start().y <= 20.0 + 1e-9);
    }

    // A cylinder cut by a point and a normal is one circle.
    const ObjectId can = s.import(dir, makeCylinder(10.0, 30.0, 256, 6), "can");
    const Json round = s.run(
        "sketch.section",
        {{"id", can}, {"point", {0, 0, 12.3}}, {"normal", {0, 0, 2}}, {"name", "Can section"}});
    CHECK(round["loops"][0] == Json{{"lines", 0}, {"arcs", 1}, {"splines", 0}});
    CHECK(s.document.find(round["id"].get<ObjectId>())->name == "Can section");

    // A fitted plane: the can's end cap, moved halfway down the can.
    const Json regions = s.run("mesh.segment", {{"id", can}})["regions"];
    REQUIRE(regions[0]["kind"] == "plane");
    const Json cap = s.run("primitive.fit", {{"selection_id", regions[0]["selection_id"]}});
    const double offset = (15.0 - cap["point"][2].get<double>()) / cap["normal"][2].get<double>();
    const Json datum =
        s.run("sketch.section", {{"id", can}, {"plane_id", cap["id"]}, {"offset_mm", offset}});
    CHECK(datum["loops"][0]["arcs"] == 1);
    CHECK_THAT(datum["plane"]["origin"][2].get<double>(), Catch::Matchers::WithinAbs(15.0, 1e-9));
    CHECK(s.document.find(datum["id"].get<ObjectId>())->provenance.inputs ==
          std::vector<ObjectId>{can, cap["id"].get<ObjectId>()});

    // Each failure adds nothing.
    const std::size_t made = s.document.objects().size();
    CHECK(made == before + 5 + regions.size() + 1); // 4 sketches, the can, its regions, the cap
    CHECK_FALSE(s.tryRun("sketch.section", {{"id", box}}).has_value());
    CHECK_FALSE(
        s.tryRun("sketch.section",
                 {{"id", box}, {"plane", "xy"}, {"point", {0, 0, 1}}, {"normal", {0, 0, 1}}})
            .has_value());
    CHECK_FALSE(s.tryRun("sketch.section", {{"id", box}, {"point", {0, 0, 1}}}).has_value());
    CHECK_FALSE(
        s.tryRun("sketch.section", {{"id", box}, {"point", {0, 0, 1}}, {"normal", {0, 0, 0}}})
            .has_value());
    CHECK_FALSE(
        s.tryRun("sketch.section", {{"id", box}, {"plane", "xy"}, {"offset_mm", 50}}).has_value());
    const Json wall = s.run("primitive.fit", {{"selection_id", regions[2]["selection_id"]}});
    CHECK_FALSE(s.tryRun("sketch.section", {{"id", can}, {"plane_id", wall["id"]}}).has_value());
    CHECK(s.document.objects().size() == made + 1); // just the wall's fit
}

TEST_CASE("solid.extrude and solid.revolve make solids from a sketch", "[commands][sketch]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, makeBox(20, 10, 6, 4), "box");
    const Json level = s.run("sketch.section", {{"id", box}, {"plane", "xy"}, {"offset_mm", 2.3}});
    const ObjectId sketch = level["id"].get<ObjectId>();

    // The 20 x 10 cut extruded 5 mm up, 4 mm about the plane, and 3 mm up with 1 mm down.
    const Json up = s.run("solid.extrude", {{"id", sketch}, {"distance_mm", 5}});
    CHECK(up["summary"]["valid"] == true);
    CHECK(up["summary"]["solid_count"] == 1);
    CHECK_THAT(up["summary"]["volume_mm3"].get<double>(), Catch::Matchers::WithinRel(1000.0, 1e-9));
    const auto solid = s.document.find(up["id"].get<ObjectId>());
    REQUIRE(solid.has_value());
    CHECK(std::string(solid->kind()) == "solid");
    CHECK(solid->name == "Section extrusion");
    CHECK(solid->provenance.inputs == std::vector<ObjectId>{sketch});
    CHECK(s.document.undoHistory().back().label == "solid.extrude");
    const Json down = s.run("solid.extrude", {{"id", sketch}, {"distance_mm", -5}});
    CHECK_THAT(down["summary"]["bounds"]["min"][2].get<double>(),
               Catch::Matchers::WithinAbs(-2.7, 1e-7));
    const Json mid =
        s.run("solid.extrude", {{"id", sketch}, {"distance_mm", 4}, {"extent", "symmetric"}});
    CHECK_THAT(mid["summary"]["bounds"]["min"][2].get<double>(),
               Catch::Matchers::WithinAbs(0.3, 1e-7));
    CHECK_THAT(mid["summary"]["volume_mm3"].get<double>(), Catch::Matchers::WithinRel(800.0, 1e-9));
    const Json both = s.run(
        "solid.extrude",
        {{"id", sketch}, {"distance_mm", 3}, {"extent", "two_sided"}, {"back_distance_mm", 1}});
    CHECK_THAT(both["summary"]["bounds"]["max"][2].get<double>(),
               Catch::Matchers::WithinAbs(5.3, 1e-7));
    CHECK_THAT(both["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(800.0, 1e-9));

    // The zx cut at y = 5 is z in [0, 6] by x in [0, 20]: about a line 5 mm below it, a tube.
    const Json side = s.run("sketch.section", {{"id", box}, {"plane", "zx"}, {"offset_mm", 5}});
    const ObjectId profile = side["id"].get<ObjectId>();
    const Json tube = s.run(
        "solid.revolve",
        {{"id", profile}, {"axis_point", {0, -5}}, {"axis_direction", {1, 0}}, {"name", "Tube"}});
    CHECK(tube["summary"]["valid"] == true);
    CHECK_THAT(tube["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * (625.0 - 25.0) * 6.0, 1e-9));
    CHECK(s.document.find(tube["id"].get<ObjectId>())->name == "Tube");
    const Json half = s.run(
        "solid.revolve",
        {{"id", profile}, {"axis_point", {0, -5}}, {"axis_direction", {1, 0}}, {"angle_deg", 180}});
    CHECK_THAT(half["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * (625.0 - 25.0) * 3.0, 1e-9));
    // About the u axis, which runs along an edge of the profile: a full cylinder of radius 20.
    const Json disc = s.run("solid.revolve", {{"id", profile}, {"axis", "u"}});
    CHECK_THAT(disc["summary"]["volume_mm3"].get<double>(),
               Catch::Matchers::WithinRel(std::numbers::pi * 400.0 * 6.0, 1e-9));

    // Each failure adds nothing.
    const std::size_t made = s.document.objects().size();
    CHECK_FALSE(s.tryRun("solid.extrude", {{"id", box}, {"distance_mm", 5}}).has_value());
    CHECK_FALSE(s.tryRun("solid.extrude", {{"id", sketch}, {"distance_mm", 0}}).has_value());
    CHECK_FALSE(s.tryRun("solid.extrude", {{"id", sketch}}).has_value());
    CHECK_FALSE(s.tryRun("solid.revolve", {{"id", profile}, {"axis", "v"}, {"axis_point", {0, 0}}})
                    .has_value());
    CHECK_FALSE(s.tryRun("solid.revolve", {{"id", profile}}).has_value());
    CHECK_FALSE(s.tryRun("solid.revolve",
                         {{"id", profile}, {"axis_point", {0, 10}}, {"axis_direction", {1, 0}}})
                    .has_value());
    CHECK(s.document.objects().size() == made);
}

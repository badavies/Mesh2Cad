// SPDX-License-Identifier: GPL-3.0-or-later
// Every M1 command run end to end through the registry, as the UI, CLI, JSON channel and MCP
// server all do.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/commands/Command.h"
#include "m2c/core/Base64.h"
#include "m2c/core/SelectionColors.h"
#include "m2c/io/MeshIO.h"

#include "TestMeshes.h"
#include "TestPng.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kDegree = std::numbers::pi / 180.0;

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

TEST_CASE("mesh.segment stores each primitive region as a selection", "[commands][segment]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeCylinder(10.0, 30.0, 96, 30), "can");

    const Json result = s.run("mesh.segment", {{"id", id}});
    const Json& regions = result["regions"];
    REQUIRE(regions.size() == 3);
    CHECK(regions[0]["kind"] == "plane");
    CHECK(regions[1]["kind"] == "plane");
    CHECK(regions[2]["kind"] == "cylinder");
    CHECK_THAT(regions[2]["radius_mm"].get<double>(), WithinAbs(10.0, 1e-3));
    CHECK(result["organic_face_count"] == 0);
    CHECK_FALSE(result.contains("organic_selection_id"));
    std::size_t faces = 0;
    for (const Json& r : regions) {
        const auto selection = s.document.find(r["selection_id"].get<ObjectId>());
        REQUIRE(selection.has_value());
        faces += r["face_count"].get<std::size_t>();
    }
    CHECK(faces == 96 * 30 * 2 + 2 * 96);
    CHECK(s.document.undoHistory().back().label == "mesh.segment");

    // Only cylinders: the ends are organic.
    const Json side = s.run("mesh.segment", {{"id", id}, {"kinds", {"cylinder"}}});
    CHECK(side["regions"].size() == 1);
    CHECK(side["organic_face_count"] == 2 * 96);
    CHECK(side.contains("organic_selection_id"));
    CHECK_FALSE(s.tryRun("mesh.segment", {{"id", id}, {"kinds", {"cone"}}}).has_value());
}

TEST_CASE("mesh.segment creates the fitted primitives and reports coverage",
          "[commands][segment]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, makeBox(30, 20, 10, 6), "box");

    // Without the option: selections only, as before.
    const std::size_t before = s.document.objects().size();
    const Json plain = s.run("mesh.segment", {{"id", box}});
    CHECK(s.document.objects().size() == before + 6);
    for (const Json& r : plain["regions"]) CHECK_FALSE(r.contains("primitive_id"));
    CHECK(plain["unclassified_face_count"] == 0);
    CHECK(plain["covered_area_fraction"].get<double>() >= 0.98);
    REQUIRE(s.document.undo());

    // One undo takes the selections and the primitives.
    s.run("mesh.segment", {{"id", box}, {"create_primitives", true}});
    CHECK(s.document.objects().size() == before + 12);
    REQUIRE(s.document.undo());
    CHECK(s.document.objects().size() == before);

    const Json result = s.run("mesh.segment", {{"id", box}, {"create_primitives", true}});
    const Json& regions = result["regions"];
    REQUIRE(regions.size() == 6);
    for (const Json& r : regions) {
        CAPTURE(r.dump());
        const auto primitive = s.document.find(r["primitive_id"].get<ObjectId>());
        REQUIRE(primitive);
        REQUIRE(primitive->primitive() != nullptr);
        CHECK(primitive->name == "Plane fit");
        CHECK(primitive->provenance.command == "mesh.segment");
        CHECK(primitive->provenance.inputs ==
              std::vector<ObjectId>{r["selection_id"].get<ObjectId>(), box});
        // The same plane primitive.fit finds on the selection.
        const Primitive& made = primitive->primitive()->fit.surface;
        const Json fit = s.run("primitive.fit", {{"selection_id", r["selection_id"]}});
        REQUIRE(fit["kind"] == "plane");
        const Vec3d normal{fit["normal"][0].get<double>(),
                           fit["normal"][1].get<double>(),
                           fit["normal"][2].get<double>()};
        const Vec3d point{fit["point"][0].get<double>(),
                          fit["point"][1].get<double>(),
                          fit["point"][2].get<double>()};
        CHECK(length(made.direction - normal) < 1e-6);
        CHECK(std::abs(dot(made.direction, made.point) - dot(normal, point)) < 1e-6);
    }

    // A bump on the top is organic: coverage drops.
    TriMesh bumpy = makeBox(30, 20, 10, 12);
    std::vector<Vec3d> p = bumpy.positions();
    for (Vec3d& v : p) {
        const double r2 = (v.x - 15) * (v.x - 15) + (v.y - 10) * (v.y - 10);
        if (v.z > 9.99) v.z += 2.0 * std::exp(-r2 / 8.0);
    }
    const ObjectId bump = s.import(dir, makeMesh(std::move(p), bumpy.indices()), "bump");
    const Json organic = s.run("mesh.segment", {{"id", bump}});
    CHECK(organic["unclassified_face_count"].get<int>() > 0);
    CHECK(organic["covered_area_fraction"].get<double>() <
          plain["covered_area_fraction"].get<double>());
}

TEST_CASE("primitive.fit adds an exact primitive fitted to a selection", "[commands][fit]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeCylinder(10.0, 30.0, 96, 30), "can");
    const Json regions = s.run("mesh.segment", {{"id", id}})["regions"];
    REQUIRE(regions.size() == 3);
    const Json& side = regions[2];
    REQUIRE(side["kind"] == "cylinder");

    const std::size_t before = s.document.objects().size();
    const Json fit = s.run("primitive.fit", {{"selection_id", side["selection_id"]}});
    CHECK(fit["kind"] == "cylinder");
    CHECK_THAT(fit["radius_mm"].get<double>(), WithinAbs(10.0, 1e-3));
    CHECK(fit["p95_mm"].get<double>() < 1e-3);
    CHECK_THAT(fit["extent"]["max"][2].get<double>() - fit["extent"]["min"][2].get<double>(),
               WithinAbs(30.0, 1e-9));
    const auto object = s.document.find(fit["id"].get<ObjectId>());
    REQUIRE(object.has_value());
    REQUIRE(object->primitive() != nullptr);
    CHECK(std::string(object->kind()) == "primitive");
    CHECK(object->name == "Cylinder fit");
    CHECK(object->provenance.inputs.front() == side["selection_id"].get<ObjectId>());
    CHECK(s.document.objects().size() == before + 1);

    // An end cap is a plane; a sphere does not fit the side within 0.1 mm and adds nothing.
    const Json cap = s.run("primitive.fit", {{"selection_id", regions[0]["selection_id"]}});
    CHECK(cap["kind"] == "plane");
    CHECK_THAT(std::abs(cap["normal"][2].get<double>()), WithinAbs(1.0, 1e-9));
    const auto sphere =
        s.tryRun("primitive.fit", {{"selection_id", side["selection_id"]}, {"kind", "sphere"}});
    REQUIRE_FALSE(sphere.has_value());
    CHECK(sphere.error().code == ErrorCode::InvalidState);
    CHECK(s.document.objects().size() == before + 2);
    CHECK_FALSE(
        s.tryRun("primitive.fit", {{"selection_id", side["selection_id"]}, {"kind", "torus"}})
            .has_value());
    CHECK_FALSE(s.tryRun("primitive.fit", {{"selection_id", id}}).has_value()); // a mesh

    // The primitive is saved in the project and read back.
    const auto path = (dir.path() / "fit.m2c").string();
    s.run("project.save", {{"path", path}});
    Session t;
    t.run("project.open", {{"path", path}});
    const auto back = t.document.find(fit["id"].get<ObjectId>());
    REQUIRE(back.has_value());
    REQUIRE(back->primitive() != nullptr);
    CHECK(back->primitive()->fit.surface.radiusMm == object->primitive()->fit.surface.radiusMm);
    CHECK(t.run("doc.list")["objects"].back()["primitive"]["kind"] == "plane");

    s.run("doc.undo");
    CHECK(s.document.objects().size() == before + 1);
}

TEST_CASE("primitive.align makes fitted planes parallel and refits them", "[commands][fit]") {
    Session s;
    const TempDir dir;
    // A block whose top leans 0.573 degrees along x.
    const TriMesh box = makeBox(30, 20, 10, 8);
    std::vector<Vec3d> p = box.positions();
    for (Vec3d& q : p) {
        if (q.z == 10.0) q.z += 0.01 * (q.x - 15.0);
    }
    const ObjectId mesh = s.import(dir, makeMesh(p, box.indices()), "block");
    const Json regions = s.run("mesh.segment", {{"id", mesh}})["regions"];
    REQUIRE(regions.size() == 6);
    ObjectId top = 0, bottom = 0, side = 0;
    for (const Json& r : regions) {
        const Json fit = s.run("primitive.fit", {{"selection_id", r["selection_id"]}});
        const double nz = fit["normal"][2].get<double>();
        const double nx = fit["normal"][0].get<double>();
        if (nz > 0.9) top = fit["id"];
        if (nz < -0.9) bottom = fit["id"];
        if (nx > 0.9) side = fit["id"];
    }
    REQUIRE(top != 0);
    REQUIRE(bottom != 0);
    REQUIRE(side != 0);
    const auto normal = [&](ObjectId id) {
        return s.document.find(id)->primitive()->fit.surface.direction;
    };
    const std::size_t steps = s.document.undoHistory().size();

    // Too far from its region for 0.05 mm: nothing changes.
    const Vec3d leaning = normal(top);
    const auto strict =
        s.tryRun("primitive.align",
                 {{"ids", {top}}, {"reference_ids", {bottom}}, {"max_residual_mm", 0.05}});
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().code == ErrorCode::InvalidState);
    CHECK(normal(top) == leaning);

    const Json aligned = s.run("primitive.align", {{"ids", {top}}, {"reference_ids", {bottom}}});
    REQUIRE(aligned["primitives"].size() == 1);
    const Json& line = aligned["primitives"][0];
    CHECK_THAT(line["turned_deg"].get<double>(), WithinAbs(0.573, 0.01));
    CHECK(line["p95_before_mm"].get<double>() < 1e-6);
    CHECK(line["p95_mm"].get<double>() > 0.1);
    CHECK(normal(top) == normal(bottom) * -1.0);
    CHECK_THAT(s.document.find(top)->primitive()->fit.surface.point.z, WithinAbs(10.0, 1e-9));
    CHECK(s.document.find(top)->primitive()->display != nullptr);
    CHECK(s.document.undoHistory().size() == steps + 1);
    CHECK(s.document.undoHistory().back().label == "primitive.align");
    s.run("doc.undo");
    CHECK(normal(top) == leaning);

    // With no reference, both take their mean and end exactly parallel.
    s.run("primitive.align", {{"ids", {top, bottom}}});
    CHECK_THAT(dot(normal(top), normal(bottom)), WithinAbs(-1.0, 1e-15));
    CHECK_THAT(dot(normal(top), Vec3d{0, 0, 1}), WithinAbs(std::cos(0.2865 * kDegree), 1e-6));

    // A world plane, a perpendicular and a set angle.
    s.run("primitive.align", {{"ids", {top}}, {"plane", "xy"}});
    CHECK(normal(top) == Vec3d{0, 0, 1});
    const Json square =
        s.run("primitive.align",
              {{"ids", {side}}, {"reference_ids", {top}}, {"relation", "perpendicular"}});
    CHECK(square["primitives"][0]["turned_deg"].get<double>() < 1e-6);
    s.run("primitive.align",
          {{"ids", {side}}, {"axis", "z"}, {"relation", "angle"}, {"angle_deg", 60}});
    CHECK_THAT(dot(normal(side), Vec3d{0, 0, 1}), WithinAbs(std::cos(30 * kDegree), 1e-12));

    CHECK_FALSE(s.tryRun("primitive.align", {{"ids", {top}}, {"relation", "perpendicular"}}));
    CHECK_FALSE(
        s.tryRun("primitive.align", {{"ids", {top}}, {"axis", "z"}, {"relation", "angle"}}));
    CHECK_FALSE(s.tryRun("primitive.align", {{"ids", {top}}, {"axis", "z"}, {"plane", "xy"}}));
    CHECK_FALSE(s.tryRun("primitive.align", {{"ids", {mesh}}, {"axis", "z"}}));
    CHECK_FALSE(s.tryRun("primitive.align", {{"ids", Json::array()}}));
}

TEST_CASE("primitive.align holds a cylinder's axis along a plane's normal", "[commands][fit]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeCylinder(10.0, 30.0, 96, 30), "can");
    const Json regions = s.run("mesh.segment", {{"id", id}})["regions"];
    REQUIRE(regions.size() == 3);
    const ObjectId cap =
        s.run("primitive.fit", {{"selection_id", regions[0]["selection_id"]}})["id"];
    const Json side = s.run("primitive.fit", {{"selection_id", regions[2]["selection_id"]}});
    REQUIRE(side["kind"] == "cylinder");
    const ObjectId can = side["id"];

    // Perpendicular to the cap's plane: the axis along its normal.
    const Json aligned =
        s.run("primitive.align",
              {{"ids", {can}}, {"reference_ids", {cap}}, {"relation", "perpendicular"}});
    const Primitive axis = s.document.find(can)->primitive()->fit.surface;
    CHECK(std::abs(axis.direction.z) == 1.0);
    CHECK_THAT(axis.radiusMm, WithinAbs(10.0, 1e-3));
    CHECK(aligned["primitives"][0]["p95_mm"].get<double>() < 1e-3);
    // Parallel to the plane would lay it on its side, far from its region.
    CHECK_FALSE(s.tryRun("primitive.align",
                         {{"ids", {can}}, {"reference_ids", {cap}}, {"max_residual_mm", 1.0}}));
    // Mixed references, and a region that is gone.
    CHECK_FALSE(s.tryRun("primitive.align", {{"ids", {cap}}, {"reference_ids", {cap, can}}}));
    s.run("object.delete", {{"id", regions[2]["selection_id"]}});
    const auto gone = s.tryRun("primitive.align", {{"ids", {can}}, {"axis", "z"}});
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().message.find("is gone") != std::string::npos);
}

TEST_CASE("view.render draws the visible objects to a PNG", "[commands][render]") {
    Session s;
    const TempDir dir;
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    const auto path = dir.path() / "view.png";
    CHECK_FALSE(s.tryRun("view.render", {{"path", path.string()}})); // nothing to draw

    const ObjectId box = s.import(dir, makeBox(20, 10, 6, 4), "box");
    const Json regions = s.run("mesh.segment", {{"id", box}})["regions"];
    // The bottom is fitted (out of sight from iso); the +x side, in sight, is selected later.
    Json bottom, side;
    for (const Json& r : regions) {
        if (r["normal"][2].get<double>() < -0.9) bottom = r["selection_id"];
        if (r["normal"][0].get<double>() > 0.9) side = r["selection_id"];
    }
    REQUIRE(bottom.is_number());
    REQUIRE(side.is_number());
    const ObjectId plane = s.run("primitive.fit", {{"selection_id", bottom}})["id"];
    const ObjectId sketch =
        s.run("sketch.section", {{"id", box}, {"plane", "xy"}, {"offset_mm", 2.3}})["id"];
    const Json view =
        s.run("view.render", {{"path", path.string()}, {"width", 64}, {"height", 48}});
    CHECK(view["drawn_ids"] == Json{box, plane, sketch});
    CHECK(view["image"]["mime_type"] == "image/png");
    const std::string first = bytes(path);
    CHECK(first.starts_with("\x89PNG"));
    CHECK(s.document.undoHistory().back().label != "view.render"); // changes nothing

    // The same document gives the same picture; a selection, a highlight or a view changes it.
    s.run("view.render", {{"path", path.string()}, {"width", 64}, {"height", 48}});
    CHECK(bytes(path) == first);
    const auto changes = [&](const Json& extra) {
        Json params{{"path", path.string()}, {"width", 64}, {"height", 48}};
        params.update(extra);
        s.run("view.render", params);
        return bytes(path) != first;
    };
    CHECK(changes({{"selection_id", side}}));
    CHECK(changes({{"highlight_id", box}}));
    CHECK(changes({{"view", "top"}}));
    CHECK(changes({{"direction", {0, 0, -1}}}));
    s.run("object.show", {{"id", sketch}, {"visible", false}});
    CHECK(s.run("view.render", {{"path", path.string()}})["drawn_ids"] == Json{box, plane});

    CHECK_FALSE(s.tryRun("view.render", {{"path", path.string()}, {"view", "sideways"}}));
    CHECK_FALSE(s.tryRun("view.render", {{"path", path.string()}, {"direction", {0, 0, 0}}}));
    CHECK_FALSE(s.tryRun("view.render", {{"path", path.string()}, {"width", 8}}));
    CHECK_FALSE(s.tryRun("view.render", {{"path", (dir.path() / "no" / "x.png").string()}}));
}

TEST_CASE("view.render colours each listed selection as the app does", "[commands][render]") {
    Session s;
    const TempDir dir;
    const auto path = dir.path() / "regions.png";
    const ObjectId box = s.import(dir, makeBox(20, 10, 6, 4), "box");
    const Json regions = s.run("mesh.segment", {{"id", box}})["regions"];
    REQUIRE(regions.size() == 6);
    Json ids = Json::array();
    for (const Json& r : regions) ids.push_back(r["selection_id"]);

    const Json view = s.run("view.render", {{"path", path.string()}, {"selection_ids", ids}});
    const Json& legend = view["color_legend"];
    REQUIRE(legend.size() == 6);
    for (std::size_t k = 0; k < 6; ++k) {
        CHECK(legend[k]["id"] == ids[k]);
        const Rgb c = selectionColor(ids[k].get<ObjectId>());
        CHECK(legend[k]["rgb"] == Json::array({c.r, c.g, c.b}));
        CHECK(legend[k]["name"].is_string());
    }

    // The three faces in sight from iso meet at a corner; each shows in its own colour, on the
    // grey mesh and shaded (shading scales the channels alike, so compare their ratios).
    const auto image = readPng(path);
    REQUIRE(image.has_value());
    const auto chroma = [](const Rgb& c) {
        const double top = std::max({c.r, c.g, c.b, 1e-9});
        return std::array<double, 3>{c.r / top, c.g / top, c.b / top};
    };
    std::map<ObjectId, int> seen;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            const auto p = chroma(image->at(x, y));
            for (const Json& id : ids) {
                const Rgb c = selectionColor(id.get<ObjectId>());
                const auto e = chroma(
                    {0.2 * 0.80 + 0.8 * c.r, 0.2 * 0.82 + 0.8 * c.g, 0.2 * 0.86 + 0.8 * c.b});
                if (std::abs(p[0] - e[0]) + std::abs(p[1] - e[1]) + std::abs(p[2] - e[2]) < 0.03)
                    ++seen[id.get<ObjectId>()];
            }
        }
    }
    int shown = 0;
    for (const auto& [id, pixels] : seen) shown += pixels > 2000 ? 1 : 0;
    CHECK(shown == 3);

    // selection_id alone draws as before, orange, and is listed in orange.
    const Json single = s.run("view.render", {{"path", path.string()}, {"selection_id", ids[0]}});
    REQUIRE(single["color_legend"].size() == 1);
    CHECK(single["color_legend"][0]["rgb"] == Json::array({1.0, 0.5, 0.1}));
    CHECK(s.run("view.render", {{"path", path.string()}})["color_legend"].empty());
    CHECK_FALSE(s.tryRun("view.render", {{"path", path.string()}, {"selection_ids", {box}}}));
}

namespace {

/// The bounds, in pixels, of the faces marked orange: {min x, min y, max x, max y}.
std::array<int, 4> orangeBounds(const Image& image) {
    std::array<int, 4> b{image.width, image.height, -1, -1};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const Rgb p = image.at(x, y);
            if (p.r > 0.3 && p.r > p.b + 0.25 && p.g < 0.75 * p.r) {
                b = {std::min(b[0], x), std::min(b[1], y), std::max(b[2], x), std::max(b[3], y)};
            }
        }
    }
    return b;
}

} // namespace

TEST_CASE("view.render frames an object and draws axes", "[commands][render]") {
    Session s;
    const TempDir dir;
    const auto path = dir.path() / "focus.png";
    const ObjectId box = s.import(dir, makeBox(200, 200, 200, 20), "big");
    // A small patch near the middle of the top face.
    const Json patch =
        s.run("select.brush", {{"id", box}, {"center", {100, 100, 200}}, {"radius_mm", 6}});
    const Json sel = patch["selection_id"];
    const auto cover = [&](const Json& extra) {
        Json params{
            {"path", path.string()}, {"width", 400}, {"height", 300}, {"selection_id", sel}};
        params.update(extra);
        s.run("view.render", params);
        const auto image = readPng(path);
        REQUIRE(image.has_value());
        const auto b = orangeBounds(*image);
        REQUIRE(b[2] >= b[0]);
        return std::max(static_cast<double>(b[2] - b[0] + 1) / image->width,
                        static_cast<double>(b[3] - b[1] + 1) / image->height);
    };
    CHECK(cover(Json::object()) < 0.10);
    CHECK(cover({{"focus_id", sel}}) >= 0.40);

    // An unknown object fails and writes nothing.
    const auto other = dir.path() / "none.png";
    const auto missing = s.tryRun("view.render", {{"path", other.string()}, {"focus_id", 999}});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(other));

    // The triad: red, green and blue in the bottom-left corner; without it, the picture as before.
    const auto bytes = [](const std::filesystem::path& p) {
        std::ifstream file(p, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    s.run("view.render", {{"path", path.string()}});
    const std::string plain = bytes(path);
    s.run("view.render", {{"path", path.string()}, {"axes", false}});
    CHECK(bytes(path) == plain);
    s.run("view.render", {{"path", path.string()}, {"axes", true}});
    const auto image = readPng(path);
    REQUIRE(image.has_value());
    bool red = false, green = false, blue = false;
    for (int y = image->height * 85 / 100; y < image->height; ++y) {
        for (int x = 0; x < image->width * 15 / 100; ++x) {
            const Rgb p = image->at(x, y);
            red = red || (p.r > 0.8 && p.g < 0.4 && p.b < 0.4);
            green = green || (p.g > 0.7 && p.r < 0.5 && p.b < 0.5);
            blue = blue || (p.b > 0.85 && p.r < 0.45 && p.g < 0.65);
        }
    }
    CHECK(red);
    CHECK(green);
    CHECK(blue);
}

TEST_CASE("view.render quad is four views at one scale", "[commands][render]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, makeBox(30, 20, 10, 4), "box");
    s.run("sketch.section", {{"id", box}, {"plane", "xy"}, {"offset_mm", 5}});
    const auto quadPath = dir.path() / "quad.png";
    const Json quad =
        s.run("view.render",
              {{"path", quadPath.string()}, {"view", "quad"}, {"width", 200}, {"height", 160}});
    REQUIRE(quad["panels"].size() == 4);
    const auto sheet = readPng(quadPath);
    REQUIRE(sheet.has_value());
    REQUIRE(sheet->width == 200);
    REQUIRE(sheet->height == 160);
    // Each panel is the single view at half the size with the same (sphere) framing, apart from
    // its label in the top-left corner.
    const std::array<const char*, 4> views{"front", "right", "top", "iso"};
    for (std::size_t k = 0; k < views.size(); ++k) {
        CAPTURE(views[k]);
        const Json& panel = quad["panels"][k];
        CHECK(panel["view"] == views[k]);
        const auto single = dir.path() / "single.png";
        s.run("view.render",
              {{"path", single.string()},
               {"view", views[k]},
               {"framing", "sphere"},
               {"width", 100},
               {"height", 80}});
        const auto view = readPng(single);
        REQUIRE(view.has_value());
        const int left = panel["x"].get<int>(), top = panel["y"].get<int>();
        int differing = 0, labelled = 0;
        for (int y = 0; y < 80; ++y) {
            for (int x = 0; x < 100; ++x) {
                const Rgb a = sheet->at(left + x, top + y), b = view->at(x, y);
                const bool same = a.r == b.r && a.g == b.g && a.b == b.b;
                if (x < 4 + 6 * 5 && y < 4 + 7) {
                    labelled += same ? 0 : 1;
                } else {
                    differing += same ? 0 : 1;
                }
            }
        }
        CHECK(differing == 0);
        CHECK(labelled > 0);
    }

    // Odd sizes are refused rather than rounded.
    const auto odd =
        s.tryRun("view.render", {{"path", quadPath.string()}, {"view", "quad"}, {"width", 201}});
    REQUIRE_FALSE(odd);
    CHECK(odd.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(s.tryRun(
        "view.render", {{"path", quadPath.string()}, {"view", "quad"}, {"direction", {1, 0, 0}}}));
}

TEST_CASE("view.render without a path returns the picture", "[commands][render]") {
    Session s;
    const TempDir dir;
    s.import(dir, makeBox(30, 20, 10, 4), "box");
    const auto path = dir.path() / "view.png";
    const Json written =
        s.run("view.render", {{"path", path.string()}, {"width", 64}, {"height", 48}});
    CHECK(written["path"] == path.string());
    CHECK(written["image"] == Json{{"path", path.string()}, {"mime_type", "image/png"}});
    const Json returned = s.run("view.render", {{"width", 64}, {"height", 48}});
    CHECK_FALSE(returned.contains("path"));
    CHECK(returned["image"]["mime_type"] == "image/png");
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    CHECK(returned["image"]["data_base64"] == encodeBase64(bytes));
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

TEST_CASE("object.show and object.delete take several objects as one step", "[commands]") {
    Session s;
    const TempDir dir;
    const ObjectId box = s.import(dir, makeBox(30, 20, 10, 4), "box");
    const ObjectId ball = s.import(dir, makeSphere(5.0, 2), "ball");
    const Json regions = s.run("mesh.segment", {{"id", box}})["regions"];
    const auto steps = s.document.undoHistory().size();
    s.run("object.show", {{"ids", {box, ball}}, {"visible", false}});
    CHECK(s.document.undoHistory().size() == steps + 1);
    CHECK_FALSE(s.document.find(box)->visible);
    CHECK_FALSE(s.document.find(ball)->visible);
    CHECK_FALSE(s.tryRun("object.show", {{"visible", true}}));
    CHECK_FALSE(s.tryRun("object.show", {{"id", box}, {"ids", {ball}}, {"visible", true}}));
    const auto missing = s.tryRun("object.show", {{"ids", {box, 999}}, {"visible", true}});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(s.document.find(box)->visible); // nothing changed

    // The box takes its selections with it, so listing one after it is fine.
    s.run("object.delete", {{"ids", {box, regions[0]["selection_id"], ball}}});
    CHECK(s.document.objects().empty());
    CHECK(s.document.undoHistory().size() == steps + 2);
    REQUIRE(s.document.undo());
    CHECK(s.document.objects().size() == 2 + regions.size());
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

TEST_CASE("Edits that change nothing add no undo step and keep selections", "[commands][tools]") {
    Session s;
    const TempDir dir;
    const ObjectId id = s.import(dir, makeCube(), "cube");
    const Json all = s.run("select.all", {{"id", id}});
    s.run("select.invert", {{"selection_id", all["selection_id"]}}); // now empty
    const auto steps = s.document.undoHistory().size();

    CHECK(s.run("holes.fill", {{"id", id}})["report"]["filled"] == 0);
    CHECK(s.run("faces.delete",
                {{"selection_id", all["selection_id"]}})["report"]["deleted_faces"] == 0);
    CHECK(s.run("shells.keep", {{"id", id}})["deleted_shells"] == 0);
    CHECK(s.document.undoHistory().size() == steps);
    CHECK(s.document.find(all["selection_id"].get<ObjectId>()).has_value());

    // A rotation with a non-finite entry is not a rotation.
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(s.tryRun("align.transform", {{"id", id}, {"rotation", {inf, 0, 0, 0, 1, 0, 0, 0, 1}}})
              .error()
              .code == ErrorCode::InvalidArgument);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <regex>
#include <set>
#include <string>

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

    Result<Json> run(std::string_view name, const Json& params = Json::object()) {
        return registry.execute(context, name, params);
    }
};

std::filesystem::path writeCube(const TempDir& dir) {
    const auto path = dir.path() / "cube.stl";
    REQUIRE(writeMesh(makeCube(), path).has_value());
    return path;
}

} // namespace

TEST_CASE("Every command is well formed", "[commands]") {
    Session s;
    const std::regex dotted("^[a-z]+(\\.[a-z]+)+$");
    const auto commands = s.registry.list();
    REQUIRE_FALSE(commands.empty());
    for (const Command* c : commands) {
        CAPTURE(c->name);
        CHECK(std::regex_match(c->name, dotted));
        CHECK_FALSE(c->summary.empty());
        CHECK(c->paramsSchema.is_object());
        CHECK(c->paramsSchema.value("type", "") == "object");
        // A command that writes a file is told where by "path", required except by view.render,
        // which returns the picture instead when it has no path (ADR-031).
        if (c->writesFiles) {
            const Json& required = c->paramsSchema.value("required", Json::array());
            CHECK(c->paramsSchema["properties"].contains("path"));
            CHECK((std::ranges::find(required, Json("path")) != required.end()) ==
                  (c->name != "view.render"));
        }
    }
}

TEST_CASE("Every path parameter names the file types it takes", "[commands]") {
    // The app's file dialogs filter on x-file-types, so a path without it offers every file.
    Session s;
    const std::regex extension("^[a-z0-9]+$");
    for (const Command* c : s.registry.list()) {
        const Json properties = c->paramsSchema.value("properties", Json::object());
        if (!properties.contains("path")) continue;
        CAPTURE(c->name);
        const Json& types = properties["path"].value("x-file-types", Json::array());
        CHECK_FALSE(types.empty());
        for (const Json& t : types) CHECK(std::regex_match(t.get<std::string>(), extension));
    }
    const Json& step = s.registry.find("solid.export")->paramsSchema["properties"]["path"];
    CHECK(step["x-file-types"] == Json::array({"step", "stp"}));
}

TEST_CASE("Commands that write files say so", "[commands]") {
    Session s;
    const std::set<std::string> writers{"deviation.report",
                                        "mesh.export",
                                        "project.save",
                                        "solid.export",
                                        "solid.export.mesh",
                                        "view.render"};
    for (const Command* c : s.registry.list()) {
        CAPTURE(c->name);
        CHECK(c->writesFiles == writers.contains(c->name));
    }
    const auto listed = s.run("commands.list");
    REQUIRE(listed.has_value());
    for (const Json& c : (*listed)["commands"]) {
        CHECK(c["writes_files"] == writers.contains(c["name"].get<std::string>()));
    }
}

TEST_CASE("Every object id parameter names the kinds it accepts", "[commands]") {
    // The app builds its object pickers from x-object, so an id without it would be a bare number.
    Session s;
    const std::set<std::string> kinds{"mesh",
                                      "selection",
                                      "quad_layout",
                                      "solid",
                                      "deviation",
                                      "primitive",
                                      "sketch",
                                      "measurement",
                                      "any"};
    std::size_t annotated = 0;
    for (const Command* c : s.registry.list()) {
        const Json properties = c->paramsSchema.value("properties", Json::object());
        for (const auto& [key, prop] : properties.items()) {
            CAPTURE(c->name, key);
            if (key == "id" || key.ends_with("_id")) CHECK(prop.contains("x-object"));
            if (!prop.contains("x-object")) continue;
            ++annotated;
            const Json& x = prop["x-object"];
            for (const Json& kind : x.is_array() ? x : Json::array({x})) {
                CHECK(kinds.contains(kind.get<std::string>()));
            }
        }
    }
    CHECK(annotated >= 50);
}

TEST_CASE("Registry rejects bad registrations", "[commands]") {
    Session s;
    const auto handler = [](CommandContext&, const Json&) -> Result<Json> {
        return Json::object();
    };
    CHECK(s.registry.add({"", "x", Json::object(), false, handler}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(s.registry.add({"a.b", "x", Json::object(), false, nullptr}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(s.registry.add({"mesh.info", "dup", Json::object(), false, handler}).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("Unknown commands and malformed parameters are reported", "[commands]") {
    Session s;
    CHECK(s.run("no.such.command").error().code == ErrorCode::NotFound);
    CHECK(s.registry.execute(s.context, "doc.list", Json::array()).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(s.registry.execute(s.context, "doc.list", Json()).has_value()); // null means {}
    CHECK(s.run("mesh.info").error().code == ErrorCode::InvalidArgument);
    CHECK(s.run("mesh.info", {{"id", "one"}}).error().code == ErrorCode::InvalidArgument);
    CHECK(s.run("mesh.info", {{"id", 99}}).error().code == ErrorCode::NotFound);
    CHECK(s.run("mesh.import", {{"path", 5}}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("commands.list describes the registry", "[commands]") {
    Session s;
    const auto result = s.run("commands.list");
    REQUIRE(result.has_value());
    CHECK((*result)["commands"].size() == s.registry.list().size());
    CHECK((*result)["api_version"] == "1.0");
}

TEST_CASE("Import, inspect, export and undo through commands", "[commands]") {
    Session s;
    const TempDir dir;
    const auto cube = writeCube(dir);

    const auto imported = s.run("mesh.import", {{"path", cube.string()}});
    REQUIRE(imported.has_value());
    CHECK((*imported)["name"] == "cube");
    CHECK((*imported)["stats"]["face_count"] == 12);
    CHECK((*imported)["stats"]["is_watertight"] == true);
    const auto id = (*imported)["id"].get<ObjectId>();

    const auto info = s.run("mesh.info", {{"id", id}});
    REQUIRE(info.has_value());
    CHECK((*info)["stats"]["vertex_count"] == 8);
    CHECK((*info)["stats"]["bounds"]["max"] == Json::array({1.0, 1.0, 1.0}));

    const auto listed = s.run("doc.list");
    REQUIRE(listed.has_value());
    CHECK((*listed)["objects"].size() == 1);

    const auto out = dir.path() / "copy.stl";
    REQUIRE(s.run("mesh.export", {{"id", id}, {"path", out.string()}}).has_value());
    CHECK(readMesh(out).has_value());

    REQUIRE(s.run("doc.undo").has_value());
    CHECK((*s.run("doc.list"))["objects"].empty());
    CHECK(s.run("doc.undo").error().code == ErrorCode::InvalidState);
    REQUIRE(s.run("doc.redo").has_value());
    CHECK((*s.run("doc.list"))["objects"].size() == 1);
}

TEST_CASE("Import honours a custom name and reports file errors", "[commands]") {
    Session s;
    const TempDir dir;
    const auto cube = writeCube(dir);

    const auto imported = s.run("mesh.import", {{"path", cube.string()}, {"name", "Housing"}});
    REQUIRE(imported.has_value());
    CHECK((*imported)["name"] == "Housing");

    const auto missing = s.run("mesh.import", {{"path", (dir.path() / "nope.stl").string()}});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::Io);
    CHECK(s.document.objects().size() == 1); // a failed import changes nothing
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <regex>

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
    }
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

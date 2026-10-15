// SPDX-License-Identifier: GPL-3.0-or-later
// The JSON-RPC channel and the MCP server, and the M1 gate rule that the CLI, JSON and MCP front
// ends give the same results (ARCHITECTURE.md sections 8 and 12).

#include "m2c/io/MeshIO.h"
#include "m2c/rpc/RpcServer.h"
#include "m2c/rpc/Session.h"

#include "McpServer.h"
#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

struct Engine {
    CommandRegistry registry;
    Document document;
    CommandContext context{document};
    Engine() { REQUIRE(registerBuiltinCommands(registry).has_value()); }
};

Json request(int id, const std::string& method, Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

/// The same small session for every front end.
std::vector<std::pair<std::string, Json>> script(const std::string& path) {
    return {{"mesh.import", {{"path", path}}},
            {"mesh.smooth", {{"id", 1}, {"iterations", 3}}},
            {"mesh.simplify", {{"id", 1}, {"target_faces", 400}}},
            {"select.crease", {{"id", 1}, {"face", 0}, {"max_angle_deg", 30}}},
            {"align.principal", {{"id", 1}, {"origin", "bounds_bottom_center"}}},
            {"measure.distance",
             {{"a", {{"id", 1}, {"point", {0, 0, 0}}}}, {"b", {{"point", {1, 2, 3}}}}}}};
}

} // namespace

TEST_CASE("JSON-RPC requests, errors, notifications and batches", "[rpc]") {
    Engine e;
    std::vector<Json> events;
    RpcServer server(
        e.registry, e.context, std::nullopt, [&](const Json& event) { events.push_back(event); });

    const Json hello = *server.handle(request(1, "rpc.hello"));
    CHECK(hello["result"]["server"] == "mesh2cad");
    CHECK(hello["result"]["auth_required"] == false);

    const Json list = *server.handle(request(2, "doc.list"));
    CHECK(list["id"] == 2);
    CHECK(list["result"]["objects"].empty());

    CHECK((*server.handle(request(3, "no.such")))["error"]["code"] == RpcServer::kMethodNotFound);
    CHECK((*server.handle(request(4, "mesh.info", {{"id", "x"}})))["error"]["code"] ==
          RpcServer::kInvalidParams);
    const Json missing = *server.handle(request(5, "mesh.info", {{"id", 7}}));
    CHECK(missing["error"]["code"] == RpcServer::kCommandFailed);
    CHECK(missing["error"]["data"]["code"] == "not_found");

    CHECK(server.handleLine("{oops").find("-32700") != std::string::npos);
    CHECK(server.handleLine(R"({"jsonrpc":"2.0","method":"doc.list"})").empty()); // notification
    CHECK((*server.handle(Json{{"id", 9}, {"method", "doc.list"}}))["error"]["code"] ==
          RpcServer::kInvalidRequest);

    const Json batch =
        *server.handle(Json::array({request(10, "doc.list"), request(11, "doc.history")}));
    REQUIRE(batch.size() == 2);
    CHECK(batch[1]["id"] == 11);

    const TempDir dir;
    const auto path = dir.path() / "cube.stl";
    REQUIRE(writeMesh(makeCube(), path).has_value());
    CHECK(events.empty());
    REQUIRE(
        (*server.handle(request(12, "mesh.import", {{"path", path.string()}}))).contains("result"));
    REQUIRE(events.size() == 1);
    CHECK(events[0]["method"] == "doc.changed");
    CHECK(events[0]["params"]["command"] == "mesh.import");
}

TEST_CASE("JSON-RPC sessions with a token refuse commands until authenticated", "[rpc]") {
    Engine e;
    RpcServer server(e.registry, e.context, std::string("s3cret"));
    CHECK((*server.handle(request(1, "rpc.hello")))["result"]["auth_required"] == true);
    CHECK((*server.handle(request(2, "doc.list")))["error"]["code"] == RpcServer::kUnauthorized);
    CHECK((*server.handle(request(3, "rpc.auth", {{"token", "guess"}})))["error"]["code"] ==
          RpcServer::kUnauthorized);
    CHECK((*server.handle(
              request(4, "rpc.auth", {{"token", "s3cret"}})))["result"]["authenticated"] == true);
    CHECK((*server.handle(request(5, "doc.list"))).contains("result"));
    CHECK(e.context.source == "api");
    CHECK((*server.handle(request(
              6, "rpc.auth", {{"token", "s3cret"}, {"source", "mcp"}})))["result"]["source"] ==
          "mcp");
    CHECK(e.context.source == "mcp");
}

TEST_CASE("MCP server lists commands as tools and calls them", "[mcp]") {
    Engine e;
    e.context.source = "mcp";
    RpcServer rpc(e.registry, e.context);
    int next = 1;
    McpServer mcp([&](const std::string& method, const Json& params) -> Result<Json> {
        return *rpc.handle(request(next++, method, params));
    });

    const Json init = *mcp.handle(request(1, "initialize", {{"protocolVersion", "2025-06-18"}}));
    CHECK(init["result"]["serverInfo"]["name"] == "mesh2cad");
    CHECK(init["result"]["capabilities"].contains("tools"));
    CHECK_FALSE(mcp.handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));

    const Json tools = *mcp.handle(request(2, "tools/list"));
    const auto& list = tools["result"]["tools"];
    CHECK(list.size() == e.registry.list().size());
    bool sawImport = false;
    for (const Json& t : list) {
        const std::string name = t["name"].get<std::string>();
        CHECK(name.find('.') == std::string::npos);
        if (name == "mesh_import") {
            sawImport = true;
            CHECK(t["annotations"]["readOnlyHint"] == false);
            CHECK(t["inputSchema"]["required"][0] == "path");
        }
        if (name == "doc_list") CHECK(t["annotations"]["readOnlyHint"] == true);
    }
    CHECK(sawImport);

    const TempDir dir;
    const auto path = dir.path() / "cube.obj";
    REQUIRE(writeMesh(makeCube(), path).has_value());
    const Json called = *mcp.handle(request(
        3, "tools/call", {{"name", "mesh_import"}, {"arguments", {{"path", path.string()}}}}));
    CHECK(called["result"]["isError"] == false);
    CHECK(called["result"]["structuredContent"]["stats"]["face_count"] == 12);
    CHECK(e.document.undoHistory().back().source == "mcp");

    const Json failed =
        *mcp.handle(request(4, "tools/call", {{"name", "mesh_info"}, {"arguments", {{"id", 99}}}}));
    CHECK(failed["result"]["isError"] == true);
    CHECK((*mcp.handle(request(5, "resources/list")))["error"]["code"] == -32601);
}

TEST_CASE("CLI, JSON-RPC and MCP give identical results", "[rpc][mcp][gate]") {
    const TempDir dir;
    const auto path = (dir.path() / "ball.ply").string();
    REQUIRE(writeMesh(addNoise(makeSphere(10.0, 3), 0.2), path).has_value());
    const auto steps = script(path);

    // CLI path: straight through the registry.
    Engine cli;
    for (const auto& [name, params] : steps) {
        REQUIRE(cli.registry.execute(cli.context, name, params).has_value());
    }

    // JSON-RPC path: text lines in, text lines out.
    Engine json;
    RpcServer rpc(json.registry, json.context);
    int id = 1;
    for (const auto& [name, params] : steps) {
        const Json reply = Json::parse(rpc.handleLine(request(id++, name, params).dump()));
        REQUIRE(reply.contains("result"));
    }

    // MCP path: tools/call through an in-process JSON-RPC engine.
    Engine agent;
    RpcServer agentRpc(agent.registry, agent.context);
    McpServer mcp([&](const std::string& method, const Json& params) -> Result<Json> {
        return *agentRpc.handle(request(id++, method, params));
    });
    for (const auto& [name, params] : steps) {
        const Json reply = *mcp.handle(request(
            id++, "tools/call", {{"name", McpServer::toolName(name)}, {"arguments", params}}));
        REQUIRE(reply["result"]["isError"] == false);
    }

    const auto a = cli.document.findMesh(1);
    const auto b = json.document.findMesh(1);
    const auto c = agent.document.findMesh(1);
    REQUIRE((a && b && c));
    CHECK(a->mesh->positions() == b->mesh->positions());
    CHECK(a->mesh->positions() == c->mesh->positions());
    CHECK(a->mesh->indices() == c->mesh->indices());
    const auto listOf = [](Engine& e) {
        return (*e.registry.execute(e.context, "doc.list", Json::object()))["objects"];
    };
    CHECK(listOf(cli) == listOf(json));
    CHECK(listOf(cli) == listOf(agent));
}

TEST_CASE("Session files round-trip and tokens are random", "[rpc]") {
    const TempDir dir;
    const auto file = dir.path() / "sub" / "session.json";
    REQUIRE(writeSessionFile({"/tmp/mesh2cad.sock", "abc123", 42}, file).has_value());
    const auto info = readSessionFile(file);
    REQUIRE(info.has_value());
    CHECK(info->endpoint == "/tmp/mesh2cad.sock");
    CHECK(info->token == "abc123");
    CHECK(info->processId == 42);
    CHECK(readSessionFile(dir.path() / "none.json").error().code == ErrorCode::NotFound);
    const std::string t1 = makeSessionToken();
    CHECK(t1.size() == 32);
    CHECK(t1 != makeSessionToken());
}

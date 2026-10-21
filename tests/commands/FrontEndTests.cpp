// SPDX-License-Identifier: GPL-3.0-or-later
// The JSON-RPC channel and the MCP server, and the M1 gate rule that the CLI, JSON and MCP front
// ends give the same results (ARCHITECTURE.md sections 8 and 12).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        if (name == "solid_export") { // leaves the document alone but writes a file
            CHECK(t["annotations"]["readOnlyHint"] == false);
            CHECK(t["annotations"]["destructiveHint"] == true);
        }
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

    // A picture comes back as an image the agent can see, after the text.
    const auto png = (dir.path() / "view.png").string();
    const Json view = *mcp.handle(request(
        6,
        "tools/call",
        {{"name", "view_render"}, {"arguments", {{"path", png}, {"width", 32}, {"height", 32}}}}));
    const Json& content = view["result"]["content"];
    REQUIRE(content.size() == 2);
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["mimeType"] == "image/png");
    CHECK(content[1]["data"].get<std::string>().starts_with("iVBORw0KGgo"));

    // Without a path the picture comes back in the result, as an image block only, and no file
    // is written anywhere near.
    const auto listing = [] {
        std::vector<std::filesystem::path> names;
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::current_path()))
            names.push_back(entry.path());
        std::ranges::sort(names);
        return names;
    };
    const auto before = listing();
    const Json inline_ = *mcp.handle(
        request(9,
                "tools/call",
                {{"name", "view_render"}, {"arguments", {{"width", 32}, {"height", 32}}}}));
    const Json& parts = inline_["result"]["content"];
    REQUIRE(parts.size() == 2);
    CHECK(parts[1]["type"] == "image");
    CHECK(parts[1]["mimeType"] == "image/png");
    CHECK(parts[1]["data"].get<std::string>().starts_with("iVBORw0KGgo")); // \x89PNG\r\n\x1a\n
    CHECK_FALSE(inline_["result"]["structuredContent"]["image"].contains("data_base64"));
    CHECK(parts[0]["text"].get<std::string>().find("data_base64") == std::string::npos);
    CHECK_FALSE(inline_["result"]["structuredContent"].contains("path"));
    CHECK(listing() == before);

    // A tool without parameters may be sent null arguments.
    const Json listed =
        *mcp.handle(request(7, "tools/call", {{"name", "doc_list"}, {"arguments", nullptr}}));
    CHECK(listed["result"]["isError"] == false);
    CHECK(listed["result"]["structuredContent"]["objects"].size() == 1);
    const Json wrong =
        *mcp.handle(request(8, "tools/call", {{"name", "doc_list"}, {"arguments", 3}}));
    CHECK(wrong["result"]["isError"] == true);

    const Json failed =
        *mcp.handle(request(4, "tools/call", {{"name", "mesh_info"}, {"arguments", {{"id", 99}}}}));
    CHECK(failed["result"]["isError"] == true);
    CHECK((*mcp.handle(request(5, "resources/list")))["error"]["code"] == -32601);
}

TEST_CASE("MCP server reports progress while a slow tool runs", "[mcp]") {
    Engine e;
    RpcServer rpc(e.registry, e.context);
    int next = 1;
    std::vector<Json> sent;
    std::atomic<std::size_t> heard{0};
    bool wantProgress = true; // whether the slow call waits for three heartbeats
    McpServer mcp(
        [&](const std::string& method, const Json& params) -> Result<Json> {
            // doc.list stands in for a long command. It runs until three heartbeats have gone
            // out (or 200 ms without a token), so a busy machine cannot starve them.
            if (method == "doc.list") {
                const std::size_t before = heard;
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                while (wantProgress && heard < before + 3 &&
                       std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return *rpc.handle(request(next++, method, params));
        },
        [&](const Json& notification) {
            sent.push_back(notification);
            ++heard;
        },
        std::chrono::milliseconds(20));

    const Json called = *mcp.handle(
        request(1, "tools/call", {{"name", "doc_list"}, {"_meta", {{"progressToken", "job-1"}}}}));
    CHECK(called["result"]["isError"] == false);
    const std::size_t count = sent.size();
    REQUIRE(count >= 3);
    double last = 0.0;
    for (const Json& n : sent) {
        CHECK(n["method"] == "notifications/progress");
        CHECK(n["params"]["progressToken"] == "job-1");
        CHECK(n["params"]["progress"].get<double>() > last); // must only grow
        last = n["params"]["progress"].get<double>();
        CHECK(n["params"]["message"].get<std::string>().starts_with("doc.list running"));
    }
    // Nothing follows the response, and a call without a token gets no progress.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(sent.size() == count);
    wantProgress = false;
    (void)mcp.handle(request(2, "tools/call", {{"name", "doc_list"}}));
    CHECK(sent.size() == count);
    wantProgress = true;
    (void)mcp.handle(
        request(3, "tools/call", {{"name", "doc_list"}, {"_meta", {{"progressToken", 7}}}}));
    REQUIRE(sent.size() > count);
    CHECK(sent.back()["params"]["progressToken"] == 7);
}

TEST_CASE("Long commands report progress over JSON-RPC and MCP", "[rpc][mcp]") {
    Engine e;
    // Over MCP, the command stays on each stage until a progress message has named it, so a busy
    // machine cannot starve the heartbeat.
    std::atomic<int> named{0}; // 1: "starting" was named, 2: "half" was too
    bool overMcp = false;
    const auto waitFor = [&named, &overMcp](int stage) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (overMcp && named < stage && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    REQUIRE(e.registry
                .add(Command{
                    .name = "test.progress",
                    .summary = "Reports two stages.",
                    .paramsSchema = {{"type", "object"}},
                    .handler = [&waitFor](CommandContext& context, const Json&) -> Result<Json> {
                        report(context.progress, "starting");
                        waitFor(1);
                        report(context.progress, "half", 0.5);
                        waitFor(2);
                        return Json{{"done", true}};
                    }})
                .has_value());
    std::vector<std::string> outer;
    e.context.progress = [&outer](std::string_view stage, double) { outer.emplace_back(stage); };
    std::vector<Json> events;
    McpServer* mcpServer = nullptr;
    RpcServer rpc(e.registry, e.context, std::nullopt, [&](const Json& event) {
        events.push_back(event);
        if (mcpServer != nullptr) mcpServer->engineEvent(event);
    });

    // JSON-RPC: each stage is a notification for the request, sent before its response.
    CHECK((*rpc.handle(request(7, "test.progress")))["result"]["done"] == true);
    REQUIRE(events.size() == 2);
    CHECK(events[0]["method"] == "rpc.progress");
    CHECK(events[0]["params"] ==
          Json{{"id", 7}, {"command", "test.progress"}, {"stage", "starting"}});
    CHECK(events[1]["params"]["fraction"] == 0.5);
    CHECK(outer == std::vector<std::string>{"starting", "half"}); // the context's own listener too
    REQUIRE(e.context.progress);

    // MCP: the progress messages name the latest stage.
    std::mutex mutex;
    std::vector<std::string> messages;
    int next = 100;
    McpServer mcp(
        [&](const std::string& method, const Json& params) -> Result<Json> {
            return *rpc.handle(request(next++, method, params));
        },
        [&](const Json& n) {
            const std::scoped_lock lock(mutex);
            const std::string message = n["params"]["message"].get<std::string>();
            if (message.starts_with("test.progress: starting, ")) named = std::max(named.load(), 1);
            if (message.starts_with("test.progress: half (50%), ")) named = 2;
            messages.push_back(message);
        },
        std::chrono::milliseconds(20));
    mcpServer = &mcp;
    overMcp = true;
    const Json called = *mcp.handle(
        request(1, "tools/call", {{"name", "test_progress"}, {"_meta", {{"progressToken", "t"}}}}));
    CHECK(called["result"]["isError"] == false);
    const std::scoped_lock lock(mutex);
    REQUIRE(messages.size() >= 2);
    CHECK(std::ranges::any_of(
        messages, [](const std::string& m) { return m.starts_with("test.progress: starting, "); }));
    CHECK(messages.back().starts_with("test.progress: half (50%), "));
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

TEST_CASE("Front ends refuse fields of the wrong type instead of throwing", "[rpc][mcp]") {
    Engine e;
    RpcServer server(e.registry, e.context, std::string("s3cret"));
    const Json numeric = *server.handle(Json{{"jsonrpc", 2}, {"id", 1}, {"method", "doc.list"}});
    CHECK(numeric["error"]["code"] == RpcServer::kInvalidRequest);
    CHECK((*server.handle(request(2, "rpc.auth", {{"token", 42}})))["error"]["code"] ==
          RpcServer::kUnauthorized);

    RpcServer rpc(e.registry, e.context);
    std::vector<std::string> called;
    McpServer mcp([&](const std::string& method, const Json& params) -> Result<Json> {
        called.push_back(method);
        return *rpc.handle(request(9, method, params));
    });
    // Only listed tools are called: internal methods are not tools.
    const Json unknown = *mcp.handle(request(3, "tools/call", {{"name", "rpc.auth"}}));
    CHECK(unknown["result"]["isError"] == true);
    CHECK((*mcp.handle(request(4, "tools/call", {{"name", 7}})))["result"]["isError"] == true);
    const Json badMethod = *mcp.handle(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", 5}});
    CHECK(badMethod["error"]["code"] == -32600);
    CHECK(called == std::vector<std::string>{"commands.list"});
}

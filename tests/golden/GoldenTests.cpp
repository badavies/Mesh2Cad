// SPDX-License-Identifier: GPL-3.0-or-later
// Golden scans (docs/ARCHITECTURE.md section 10): every folder under the golden directory holds a
// session log, a metrics file and the scan it uses. The session is replayed through the CLI path
// (the registry), the JSON channel and the MCP server; all three must give identical documents,
// and the stored metrics must hold. See tests/golden/README.md for the file formats.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

#include "m2c/rpc/RpcServer.h"

#include "McpServer.h"
#include "TestMeshes.h"

using namespace m2c;

namespace {

namespace fs = std::filesystem;

struct Engine {
    CommandRegistry registry;
    Document document;
    CommandContext context{document};
    Engine() { REQUIRE(registerBuiltinCommands(registry).has_value()); }
};

struct Step {
    std::string command;
    Json params;
};

fs::path goldenRoot() {
    if (const char* dir = std::getenv("M2C_GOLDEN_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    return M2C_GOLDEN_DIR;
}

std::vector<fs::path> goldenScans() {
    std::vector<fs::path> scans;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(goldenRoot(), ec)) {
        if (entry.is_directory() && fs::exists(entry.path() / "metrics.json")) {
            scans.push_back(entry.path());
        }
    }
    std::ranges::sort(scans);
    return scans;
}

Json readJson(const fs::path& file) {
    std::ifstream in(file);
    REQUIRE(in.good());
    return Json::parse(in);
}

/// Session lines in the format of the app's session log and `mesh2cad batch`. Relative paths are
/// resolved against the scan folder; "source" is ignored (each front end records its own).
std::vector<Step> readSession(const fs::path& folder, const std::string& name) {
    std::ifstream in(folder / name);
    REQUIRE(in.good());
    std::vector<Step> steps;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const Json request = Json::parse(line);
        Json params = request.value("params", Json::object());
        if (params.contains("path") && fs::path(params["path"].get<std::string>()).is_relative())
            params["path"] = (folder / params["path"].get<std::string>()).string();
        steps.push_back({request.at("command").get<std::string>(), std::move(params)});
    }
    return steps;
}

Json rpcRequest(int id, const std::string& method, Json params) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

/// One expectation: a plain value must match exactly; {"min": a, "max": b} is an inclusive range.
void checkMetric(const Json& actual, const std::string& pointer, const Json& expected) {
    INFO("metric " << pointer);
    const Json::json_pointer at(pointer);
    REQUIRE(actual.contains(at));
    const Json& value = actual.at(at);
    if (expected.is_object()) {
        REQUIRE(value.is_number());
        if (expected.contains("min")) CHECK(value.get<double>() >= expected["min"].get<double>());
        if (expected.contains("max")) CHECK(value.get<double>() <= expected["max"].get<double>());
    } else {
        CHECK(value == expected);
    }
}

void runGolden(const fs::path& folder) {
    const Json metrics = readJson(folder / "metrics.json");
    const auto steps = readSession(folder, metrics.value("session", "session.jsonl"));
    REQUIRE_FALSE(steps.empty());

    // CLI path, timed against the stored budget.
    Engine cli;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& step : steps) {
        INFO(step.command << " " << step.params.dump());
        const auto result = cli.registry.execute(cli.context, step.command, step.params);
        REQUIRE(result.has_value());
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (metrics.contains("max_seconds")) CHECK(seconds <= metrics["max_seconds"].get<double>());

    // JSON channel and MCP server.
    Engine json;
    RpcServer rpc(json.registry, json.context);
    Engine agent;
    RpcServer agentRpc(agent.registry, agent.context);
    int id = 1;
    McpServer mcp([&](const std::string& method, const Json& params) -> Result<Json> {
        return *agentRpc.handle(rpcRequest(id++, method, params));
    });
    for (const auto& step : steps) {
        INFO(step.command << " " << step.params.dump());
        const Json reply =
            Json::parse(rpc.handleLine(rpcRequest(id++, step.command, step.params).dump()));
        REQUIRE(reply.contains("result"));
        const Json called = *mcp.handle(
            rpcRequest(id++,
                       "tools/call",
                       {{"name", McpServer::toolName(step.command)}, {"arguments", step.params}}));
        REQUIRE(called["result"]["isError"] == false);
    }

    // Identical documents: same objects, and bit-identical meshes (same platform and build).
    const auto listOf = [](Engine& e) {
        return (*e.registry.execute(e.context, "doc.list", Json::object()))["objects"];
    };
    const Json objects = listOf(cli);
    CHECK(objects == listOf(json));
    CHECK(objects == listOf(agent));
    for (const Json& object : objects) {
        const auto objectId = object["id"].get<std::uint64_t>();
        const auto a = cli.document.findMesh(objectId);
        if (!a) continue;
        const auto b = json.document.findMesh(objectId);
        const auto c = agent.document.findMesh(objectId);
        REQUIRE((b && c));
        CHECK(a->mesh->positions() == b->mesh->positions());
        CHECK(a->mesh->positions() == c->mesh->positions());
        CHECK(a->mesh->indices() == b->mesh->indices());
        CHECK(a->mesh->indices() == c->mesh->indices());
    }

    // Stored metrics, read through query commands on the final document.
    for (const Json& check : metrics.value("checks", Json::array())) {
        const std::string command = check.at("command").get<std::string>();
        INFO("check " << check.dump());
        const auto result =
            cli.registry.execute(cli.context, command, check.value("params", Json::object()));
        REQUIRE(result.has_value());
        for (const auto& [pointer, expected] : check.at("expect").items()) {
            checkMetric(*result, pointer, expected);
        }
    }
}

} // namespace

TEST_CASE("Golden scans meet their stored metrics through every front end", "[golden]") {
    const auto scans = goldenScans();
    if (scans.empty()) {
        SKIP("no golden scans in " << goldenRoot().string()
                                   << "; see tests/golden/README.md or set M2C_GOLDEN_DIR");
    }
    for (const auto& folder : scans) {
        DYNAMIC_SECTION(folder.filename().string()) {
            runGolden(folder);
        }
    }
}

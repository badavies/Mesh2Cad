// SPDX-License-Identifier: GPL-3.0-or-later
// Golden scans (docs/ARCHITECTURE.md section 10): every folder under the golden directory holds a
// session log, a metrics file and the scan it uses. The session is replayed through the CLI path
// (the registry), the JSON channel and the MCP server; all three must give identical documents,
// and the stored metrics must hold. See tests/golden/README.md for the file formats.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

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

/// Relative paths in a session or a check are resolved against the scan folder.
Json resolvePaths(const fs::path& folder, Json params) {
    if (params.contains("path") && fs::path(params["path"].get<std::string>()).is_relative())
        params["path"] = (folder / params["path"].get<std::string>()).string();
    return params;
}

/// Session lines in the format of the app's session log and `mesh2cad batch`. "source" is
/// ignored (each front end records its own).
std::vector<Step> readSession(const fs::path& folder, const std::string& name) {
    std::ifstream in(folder / name);
    REQUIRE(in.good());
    std::vector<Step> steps;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const Json request = Json::parse(line);
        steps.push_back({request.at("command").get<std::string>(),
                         resolvePaths(folder, request.value("params", Json::object()))});
    }
    return steps;
}

/// SHA-256 of a file as lowercase hex (FIPS 180-4), so a metrics file names exactly the scan it
/// was measured on. Compare with `sha256sum` or `certutil -hashfile <file> SHA256`.
std::string sha256File(const fs::path& file) {
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667,
                                   0xbb67ae85,
                                   0x3c6ef372,
                                   0xa54ff53a,
                                   0x510e527f,
                                   0x9b05688c,
                                   0x1f83d9ab,
                                   0x5be0cd19};
    const auto block = [&](const unsigned char* p) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
                   (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, x] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t t1 = x + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                                     ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                                     ((a & b) ^ (a & c) ^ (b & c));
            x = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        const std::array<std::uint32_t, 8> add{a, b, c, d, e, f, g, x};
        for (std::size_t i = 0; i < 8; ++i) h[i] += add[i];
    };

    std::ifstream in(file, std::ios::binary);
    REQUIRE(in.good());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    const std::uint64_t bits = std::uint64_t{data.size()} * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    for (std::size_t i = 0; i < data.size(); i += 64) block(data.data() + i);

    std::string hex;
    for (const std::uint32_t v : h) {
        static constexpr char digits[] = "0123456789abcdef";
        for (int i = 28; i >= 0; i -= 4) hex += digits[(v >> i) & 0xf];
    }
    return hex;
}

/// The scan a metrics file was measured on. Scans are large and stay out of Git (copies in each
/// folder are gitignored), so a folder whose copy is missing is skipped, not failed.
struct ScanFile {
    fs::path path;
    std::string sha256;
};

std::optional<ScanFile> scanOf(const fs::path& folder) {
    const Json metrics = readJson(folder / "metrics.json");
    if (!metrics.contains("scan")) return ScanFile{};
    const Json& scan = metrics["scan"];
    ScanFile file{folder / scan.at("file").get<std::string>(), scan.value("sha256", "")};
    if (!fs::exists(file.path)) return std::nullopt;
    return file;
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

    // The scan must be the exact file the limits were measured on.
    const auto scan = scanOf(folder);
    REQUIRE(scan.has_value());
    if (!scan->sha256.empty()) {
        INFO("scan " << scan->path.string() << " is not the file in metrics.json");
        REQUIRE(sha256File(scan->path) == scan->sha256);
    }

    // CLI path, timed against the stored budget.
    Engine cli;
    std::vector<Json> results;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& step : steps) {
        INFO(step.command << " " << step.params.dump());
        const auto result = cli.registry.execute(cli.context, step.command, step.params);
        REQUIRE(result.has_value());
        results.push_back(*result);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // Time budgets hold for optimised builds; a debug build of CGAL is many times slower.
#ifdef NDEBUG
    if (metrics.contains("max_seconds")) CHECK(seconds <= metrics["max_seconds"].get<double>());
#else
    (void)seconds;
#endif

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

    // Stored metrics, read through query commands on the final document or, for a check that
    // names a "step" (1 = the session's first line), from that step's own result.
    for (const Json& check : metrics.value("checks", Json::array())) {
        INFO("check " << check.dump());
        Json result;
        if (check.contains("step")) {
            const auto step = check.at("step").get<std::size_t>();
            REQUIRE((step >= 1 && step <= results.size()));
            result = results[step - 1];
        } else {
            const std::string command = check.at("command").get<std::string>();
            auto queried = cli.registry.execute(
                cli.context, command, resolvePaths(folder, check.value("params", Json::object())));
            REQUIRE(queried.has_value());
            result = std::move(*queried);
        }
        for (const auto& [pointer, expected] : check.at("expect").items()) {
            checkMetric(result, pointer, expected);
        }
    }
}

} // namespace

TEST_CASE("Golden scans meet their stored metrics through every front end", "[golden]") {
    std::vector<fs::path> scans;
    for (const auto& folder : goldenScans()) {
        if (scanOf(folder)) {
            scans.push_back(folder);
        } else {
            WARN("skipping " << folder.filename().string()
                             << ": its scan is not there (see tests/golden/README.md)");
        }
    }
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

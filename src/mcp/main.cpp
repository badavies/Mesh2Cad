// SPDX-License-Identifier: GPL-3.0-or-later
//
// mesh2cad-mcp: Model Context Protocol server over stdio, for AI agents (Claude Desktop, Claude
// Code, ...).
//
//   mesh2cad-mcp                 work on the running desktop app's document if it is open,
//                                otherwise on a private headless document
//   mesh2cad-mcp --headless      always use a private headless document
//   mesh2cad-mcp --connect       only connect to the running app; fail if it is not open
//   mesh2cad-mcp --session <file> session file to use (default: the app's per-user location)

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/document/Document.h"
#include "m2c/rpc/RpcServer.h"
#include "m2c/rpc/Session.h"

#include "McpServer.h"

namespace {

using m2c::Json;
using m2c::Result;

/// Engine in this process, reached through the JSON-RPC layer like any other front end.
struct HeadlessEngine {
    m2c::CommandRegistry registry;
    m2c::Document document;
    m2c::CommandContext context{.document = document};
    std::unique_ptr<m2c::RpcServer> server;

    HeadlessEngine() {
        (void)m2c::registerBuiltinCommands(registry);
        context.source = "mcp";
        server = std::make_unique<m2c::RpcServer>(registry, context);
    }

    Result<Json> call(const std::string& method, const Json& params) const {
        static long long next = 1;
        const auto reply = server->handle(
            Json{{"jsonrpc", "2.0"}, {"id", next++}, {"method", method}, {"params", params}});
        return reply.value_or(Json::object());
    }
};

/// The desktop app's engine over its local socket.
struct RemoteEngine {
    std::unique_ptr<m2c::LocalClient> client;
    long long next{1};

    Result<Json> call(const std::string& method, const Json& params) {
        const long long id = next++;
        const Json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        if (auto ok = client->writeLine(request.dump()); !ok) return std::unexpected(ok.error());
        while (true) {
            auto line = client->readLine();
            if (!line) return std::unexpected(line.error());
            // Skip doc.changed notifications and anything that is not our answer.
            if (Json reply = Json::parse(*line, nullptr, false);
                reply.is_object() && reply.value("id", Json()) == Json(id))
                return reply;
        }
    }

    static Result<std::unique_ptr<RemoteEngine>> open(const std::filesystem::path& sessionFile) {
        auto info = m2c::readSessionFile(sessionFile);
        if (!info) return std::unexpected(info.error());
        auto client = m2c::LocalClient::connect(info->endpoint);
        if (!client) return std::unexpected(client.error());
        auto engine = std::make_unique<RemoteEngine>();
        engine->client = std::move(*client);
        const auto auth = engine->call("rpc.auth", Json{{"token", info->token}, {"source", "mcp"}});
        if (!auth) return std::unexpected(auth.error());
        if (auth->contains("error")) {
            return m2c::fail(m2c::ErrorCode::InvalidState,
                             "the Mesh2Cad app refused the session token");
        }
        return engine;
    }
};

} // namespace

int main(int argc, char** argv) {
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    enum class Mode : std::uint8_t {
        Auto,
        Headless,
        Connect
    } mode = Mode::Auto;
    std::filesystem::path sessionFile = m2c::defaultSessionFile();
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (const std::string_view arg = args[i]; arg == "--headless") {
            mode = Mode::Headless;
        } else if (arg == "--connect") {
            mode = Mode::Connect;
        } else if (arg == "--session" && i + 1 < args.size()) {
            sessionFile = args[++i];
        } else if (arg == "--version") {
            std::cout << "Mesh2Cad " << MESH2CAD_VERSION << '\n';
            return 0;
        } else {
            std::cerr << "usage: mesh2cad-mcp [--headless | --connect] [--session <file>]\n";
            return 2;
        }
    }

    std::unique_ptr<HeadlessEngine> headless;
    std::unique_ptr<RemoteEngine> remote;
    if (mode != Mode::Headless) {
        if (auto opened = RemoteEngine::open(sessionFile)) {
            remote = std::move(*opened);
            std::cerr << "mesh2cad-mcp: connected to the running Mesh2Cad app\n";
        } else if (mode == Mode::Connect) {
            std::cerr << "mesh2cad-mcp: " << opened.error().message << '\n';
            return 1;
        }
    }
    if (!remote) {
        headless = std::make_unique<HeadlessEngine>();
        std::cerr << "mesh2cad-mcp: using a headless document\n";
    }

    m2c::McpServer server([&](const std::string& method, const Json& params) -> Result<Json> {
        return remote ? remote->call(method, params) : headless->call(method, params);
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const Json message = Json::parse(line, nullptr, false);
        if (message.is_discarded()) {
            std::cout << Json{{"jsonrpc", "2.0"},
                              {"id", nullptr},
                              {"error", {{"code", -32700}, {"message", "parse error"}}}}
                             .dump()
                      << '\n'
                      << std::flush;
            continue;
        }
        if (const auto reply = server.handle(message)) {
            std::cout << reply->dump(-1, ' ', false, Json::error_handler_t::replace) << '\n'
                      << std::flush;
        }
    }
    return 0;
}

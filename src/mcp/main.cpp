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

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

/// Where an engine sends the notifications it writes while a call runs (rpc.progress, ...).
using EventSink = std::function<void(const Json&)>;

/// Engine in this process, reached through the JSON-RPC layer like any other front end.
struct HeadlessEngine {
    m2c::CommandRegistry registry;
    m2c::Document document;
    m2c::CommandContext context{.document = document};
    std::unique_ptr<m2c::RpcServer> server;
    EventSink onEvent;

    HeadlessEngine() {
        (void)m2c::registerBuiltinCommands(registry);
        context.source = "mcp";
        server = std::make_unique<m2c::RpcServer>(
            registry, context, std::nullopt, [this](const Json& n) {
                if (onEvent) onEvent(n);
            });
    }

    std::atomic<long long> current{0}; ///< The id of the latest call.

    Result<Json> call(const std::string& method, const Json& params) {
        const long long id = ++current;
        const auto reply = server->handle(
            Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        return reply.value_or(Json::object());
    }

    /// Stops the call in progress; called on another thread.
    void cancel() const { (void)server->cancel(Json(current.load())); }
};

/// The desktop app's engine over its local socket.
struct RemoteEngine {
    std::unique_ptr<m2c::LocalClient> client;
    std::atomic<long long> current{0}; ///< The id of the latest call.
    std::mutex writeMutex;             ///< A cancel is written while a call reads.
    EventSink onEvent;

    Result<Json> call(const std::string& method, const Json& params) {
        const long long id = ++current;
        const Json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        if (auto ok = write(request); !ok) return std::unexpected(ok.error());
        while (true) {
            auto line = client->readLine();
            if (!line) return std::unexpected(line.error());
            // Notifications (doc.changed, rpc.progress) go to the sink; other answers are skipped.
            Json reply = Json::parse(*line, nullptr, false);
            if (!reply.is_object()) continue;
            if (reply.value("id", Json()) == Json(id)) return reply;
            if (reply.contains("method") && !reply.contains("id") && onEvent) onEvent(reply);
        }
    }

    /// Stops the call in progress; called on another thread. On Windows the pipe serialises
    /// this write behind the call's pending read, so it goes out with the next line the app sends.
    void cancel() {
        (void)write(Json{
            {"jsonrpc", "2.0"}, {"method", "rpc.cancel"}, {"params", {{"id", current.load()}}}});
    }

    Result<void> write(const Json& message) {
        const std::scoped_lock lock(writeMutex);
        return client->writeLine(message.dump());
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

    // Messages go to standard output through a stream of their own: a headless quad.remesh
    // points std::cout at nothing while Instant Meshes runs, and messages must still get out.
    std::ostream out(std::cout.rdbuf());
    std::cin.tie(nullptr); // reading would flush std::cout, which the remesh may be swapping
    std::mutex outMutex;
    const auto send = [&out, &outMutex](const Json& message) {
        const std::scoped_lock lock(outMutex);
        out << message.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
    };
    // Progress notifications go out from another thread while a tool runs.
    m2c::McpServer server(
        [&](const std::string& method, const Json& params) -> Result<Json> {
            return remote ? remote->call(method, params) : headless->call(method, params);
        },
        send);
    // The stages a long command reports go into the progress messages.
    const EventSink forward = [&server](const Json& n) { server.engineEvent(n); };
    if (remote) remote->onEvent = forward;
    if (headless) headless->onEvent = forward;
    server.setCancel([&] { remote ? remote->cancel() : headless->cancel(); });

    // Messages are handled in order on a worker thread while this one reads, so a
    // notifications/cancelled reaches the tool call it names while that runs.
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Json> queue;
    bool done = false;
    std::thread worker([&] {
        for (;;) {
            Json message;
            {
                std::unique_lock lock(queueMutex);
                wake.wait(lock, [&] { return done || !queue.empty(); });
                if (queue.empty()) return; // input ended and every message was handled
                message = std::move(queue.front());
                queue.pop_front();
            }
            if (const auto reply = server.handle(message)) send(*reply);
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        Json message = Json::parse(line, nullptr, false);
        if (message.is_discarded()) {
            send(Json{{"jsonrpc", "2.0"},
                      {"id", nullptr},
                      {"error", {{"code", -32700}, {"message", "parse error"}}}});
            continue;
        }
        if (m2c::McpServer::isCancel(message)) {
            (void)server.handle(message);
            continue;
        }
        {
            const std::scoped_lock lock(queueMutex);
            queue.push_back(std::move(message));
        }
        wake.notify_one();
    }
    {
        const std::scoped_lock lock(queueMutex);
        done = true;
    }
    wake.notify_one();
    worker.join();
    return 0;
}

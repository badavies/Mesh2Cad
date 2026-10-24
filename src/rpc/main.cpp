// SPDX-License-Identifier: GPL-3.0-or-later
//
// mesh2cad-rpc: a headless engine driven by JSON-RPC 2.0 over stdin/stdout, one message per line
// (ARCHITECTURE.md section 8). Scripts and the MCP server use it; the desktop app offers the same
// protocol on a local socket.
//
//   mesh2cad-rpc [--log <session.jsonl>]
//
// Responses go to stdout, doc.changed notifications too; diagnostics go to stderr. Requests run in
// order; rpc.cancel {"id"} is answered at once and stops the running request with that id.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "m2c/commands/Command.h"
#include "m2c/document/Document.h"
#include "m2c/rpc/RpcServer.h"

int main(int argc, char** argv) {
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    std::ofstream logFile;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (const std::string_view arg = args[i]; arg == "--log" && i + 1 < args.size()) {
            logFile.open(args[++i], std::ios::app);
            if (!logFile) {
                std::cerr << "cannot open session log '" << args[i] << "'\n";
                return 2;
            }
        } else if (arg == "--version") {
            std::cout << "Mesh2Cad " << MESH2CAD_VERSION << '\n';
            return 0;
        } else {
            std::cerr << "usage: mesh2cad-rpc [--log <session.jsonl>] [--version]\n";
            return 2;
        }
    }

    m2c::CommandRegistry registry;
    if (const auto ok = m2c::registerBuiltinCommands(registry); !ok) {
        std::cerr << "internal error: " << ok.error().message << '\n';
        return 1;
    }
    m2c::Document document;
    m2c::CommandContext context{.document = document};
    context.source = "json";
    if (logFile.is_open()) {
        context.sessionLog = [&logFile](const m2c::Json& line) {
            logFile << line.dump() << '\n' << std::flush;
        };
    }
    // The engine runs requests in order on a worker thread while this one reads, so an
    // rpc.cancel can reach the request it names while that runs. Both threads write lines.
    // Lines go to standard output through a stream of their own: quad.remesh points std::cout
    // at nothing while Instant Meshes runs, and replies and events must still get out.
    std::ostream out(std::cout.rdbuf());
    std::cin.tie(nullptr); // reading would flush std::cout, which the remesh may be swapping
    std::mutex outMutex;
    const auto send = [&out, &outMutex](const std::string& line) {
        if (line.empty()) return;
        const std::scoped_lock lock(outMutex);
        out << line << '\n' << std::flush;
    };
    m2c::RpcServer server(
        registry, context, std::nullopt, [&send](const m2c::Json& event) { send(event.dump()); });

    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    bool done = false;
    std::thread worker([&] {
        for (;;) {
            std::string line;
            {
                std::unique_lock lock(queueMutex);
                wake.wait(lock, [&] { return done || !queue.empty(); });
                if (queue.empty()) return; // input ended and every request has run
                line = std::move(queue.front());
                queue.pop_front();
            }
            send(server.handleLine(line));
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (const m2c::Json message = m2c::Json::parse(line, nullptr, false);
            m2c::RpcServer::isCancel(message)) {
            if (const auto response = server.handle(message)) send(response->dump());
            continue;
        }
        {
            const std::scoped_lock lock(queueMutex);
            queue.push_back(std::move(line));
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

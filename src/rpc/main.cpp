// SPDX-License-Identifier: GPL-3.0-or-later
//
// mesh2cad-rpc: a headless engine driven by JSON-RPC 2.0 over stdin/stdout, one message per line
// (ARCHITECTURE.md section 8). Scripts and the MCP server use it; the desktop app offers the same
// protocol on a local socket.
//
//   mesh2cad-rpc [--log <session.jsonl>]
//
// Responses go to stdout, doc.changed notifications too; diagnostics go to stderr.

#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "m2c/commands/Command.h"
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
    m2c::RpcServer server(registry, context, std::nullopt, [](const m2c::Json& event) {
        std::cout << event.dump() << '\n' << std::flush;
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (const std::string response = server.handleLine(line); !response.empty())
            std::cout << response << '\n' << std::flush;
    }
    return 0;
}

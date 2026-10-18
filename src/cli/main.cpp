// SPDX-License-Identifier: GPL-3.0-or-later
//
// Command-line front end. It adds no capability of its own: every action is a registered command.
//
//   mesh2cad list                          list commands and their parameter schemas
//   mesh2cad run <command> [params-json]   run one command on an empty document
//   mesh2cad batch [file]                  run JSON lines {"command": "...", "params": {...}}
//                                          from a file (or stdin) on one shared document
//
// Each command prints one line: {"ok":true,"result":...} or {"ok":false,"error":{...}}.
// Exit status: 0 if every command succeeded, 1 if a command failed, 2 for a usage error.
// A batch stops at the first failing command.

#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/document/Document.h"

namespace {

using m2c::Json;

constexpr int kExitOk = 0;
constexpr int kExitCommandFailed = 1;
constexpr int kExitUsage = 2;

void usage(std::ostream& out) {
    out << "Mesh2Cad " << MESH2CAD_VERSION << "\n"
        << "usage:\n"
        << "  mesh2cad list\n"
        << "  mesh2cad run <command> [params-json]\n"
        << "  mesh2cad batch [file]        (reads stdin when no file is given)\n"
        << "  mesh2cad --version | --help\n";
}

void print(const Json& envelope) {
    std::cout << envelope.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
}

bool runOne(const m2c::CommandRegistry& registry,
            m2c::CommandContext& context,
            const std::string_view name,
            const Json& params) {
    const auto result = registry.execute(context, name, params);
    if (result) {
        print(Json{{"ok", true}, {"result", *result}});
    } else {
        print(Json{{"ok", false}, {"error", m2c::toJson(result.error())}});
    }
    return result.has_value();
}

int runBatch(const m2c::CommandRegistry& registry,
             m2c::CommandContext& context,
             std::istream& input) {
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

        const Json request = Json::parse(line, nullptr, false);
        const auto command = request.is_object() ? request.find("command") : request.end();
        if (!request.is_object() || command == request.end() || !command->is_string()) {
            print(Json{{"ok", false},
                       {"error",
                        m2c::toJson(m2c::Error{
                            .code = m2c::ErrorCode::InvalidArgument,
                            .message = "line " + std::to_string(lineNumber) +
                                       ": expected a JSON object with a string 'command'"})}});
            return kExitCommandFailed;
        }
        if (const auto params = request.find("params");
            !runOne(registry,
                    context,
                    command->get<std::string>(),
                    params == request.end() ? Json::object() : *params)) {
            return kExitCommandFailed;
        }
    }
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char* const> raw(argv, static_cast<std::size_t>(argc));
    const auto rest = raw.subspan(raw.empty() ? 0 : 1);
    const std::vector<std::string_view> args(rest.begin(), rest.end());

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage(args.empty() ? std::cerr : std::cout);
        return args.empty() ? kExitUsage : kExitOk;
    }
    if (args[0] == "--version") {
        std::cout << "Mesh2Cad " << MESH2CAD_VERSION << '\n';
        return kExitOk;
    }

    m2c::CommandRegistry registry;
    if (const auto registered = m2c::registerBuiltinCommands(registry); !registered) {
        std::cerr << "internal error: " << registered.error().message << '\n';
        return kExitCommandFailed;
    }
    m2c::Document document;
    m2c::CommandContext context{.document = document};
    context.source = "cli";

    if (args[0] == "list" && args.size() == 1) {
        return runOne(registry, context, "commands.list", Json::object()) ? kExitOk
                                                                          : kExitCommandFailed;
    }
    if (args[0] == "run" && (args.size() == 2 || args.size() == 3)) {
        Json params = Json::object();
        if (args.size() == 3) {
            params = Json::parse(args[2], nullptr, false);
            if (params.is_discarded()) {
                std::cerr << "parameters are not valid JSON\n";
                return kExitUsage;
            }
        }
        return runOne(registry, context, args[1], params) ? kExitOk : kExitCommandFailed;
    }
    if (args[0] == "batch" && args.size() <= 2) {
        if (args.size() == 2) {
            std::ifstream file{std::string(args[1])};
            if (!file) {
                std::cerr << "cannot open '" << args[1] << "'\n";
                return kExitUsage;
            }
            return runBatch(registry, context, file);
        }
        return runBatch(registry, context, std::cin);
    }

    usage(std::cerr);
    return kExitUsage;
}

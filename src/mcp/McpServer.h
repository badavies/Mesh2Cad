// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"

namespace m2c {

/// Model Context Protocol server (ARCHITECTURE.md section 8): exposes every engine command as an
/// MCP tool. It holds no engine of its own; `call` sends one JSON-RPC request to an engine, either
/// in-process (headless) or over the desktop app's local socket, so an AI agent and a person work
/// on the same document with the same commands.
class McpServer {
public:
    /// Sends {method, params} to the engine and returns the JSON-RPC response object.
    using EngineCall = std::function<Result<Json>(const std::string& method, const Json& params)>;

    explicit McpServer(EngineCall call);

    /// Handles one MCP message; returns the response, or nothing for notifications.
    [[nodiscard]] std::optional<Json> handle(const Json& message);

    /// "mesh.import" -> "mesh_import": MCP clients only accept [A-Za-z0-9_-] in tool names.
    [[nodiscard]] static std::string toolName(const std::string& command);

private:
    [[nodiscard]] Result<Json> listTools();
    [[nodiscard]] Json callTool(const Json& params);

    EngineCall call_;
    std::map<std::string, std::string> commandForTool_;
};

} // namespace m2c

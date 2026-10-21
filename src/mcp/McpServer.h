// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
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
    /// Sends a notification to the client. Called from another thread while a tool runs, never
    /// at the same time as `handle` returns a response, so it needs no lock of its own.
    using Notify = std::function<void(const Json& notification)>;

    /// While a tools/call whose request carries `_meta.progressToken` runs, `notify` gets a
    /// notifications/progress every `progressEvery`, so a client can show that a long command is
    /// alive and keep waiting for it. Without `notify` no progress is sent.
    explicit McpServer(EngineCall call,
                       Notify notify = {},
                       std::chrono::milliseconds progressEvery = std::chrono::seconds(2));

    /// Handles one MCP message; returns the response, or nothing for notifications.
    [[nodiscard]] std::optional<Json> handle(const Json& message);

    /// A notification from the engine while `call` runs. The latest rpc.progress stage goes into
    /// the progress messages ("surface.build: making the faces, 12 s"); the rest is dropped.
    void engineEvent(const Json& notification);

    /// "mesh.import" -> "mesh_import": MCP clients only accept [A-Za-z0-9_-] in tool names.
    [[nodiscard]] static std::string toolName(const std::string& command);

private:
    [[nodiscard]] Result<Json> listTools();
    [[nodiscard]] Json callTool(const Json& params);
    /// call_(command, arguments), reporting progress against `token` (null: none) meanwhile.
    [[nodiscard]] Result<Json>
    callReporting(const std::string& command, const Json& arguments, const Json& token);

    EngineCall call_;
    Notify notify_;
    std::chrono::milliseconds progressEvery_;
    std::mutex stageMutex_; ///< Guards stage_: written by engineEvent, read by the heartbeat.
    std::string stage_;     ///< The running command's latest stage, or empty.
    std::map<std::string, std::string> commandForTool_;
};

} // namespace m2c

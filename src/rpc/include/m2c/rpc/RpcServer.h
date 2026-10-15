// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "m2c/commands/Command.h"

namespace m2c {

/// JSON-RPC 2.0 front end over the command registry (ARCHITECTURE.md section 8). Transport-free:
/// a transport (stdio, the desktop app's local socket) feeds it one message per line and writes
/// back what it returns.
///
/// - Every registered command is a method with the same name and an object of parameters.
/// - `rpc.hello` returns the server name, version and whether authentication is needed.
/// - When constructed with a token, every other method fails until `rpc.auth` {"token": ...}
///   succeeds, so only processes that can read the session file can drive the app. `rpc.auth`
///   may also name the client ({"source": "mcp"}); undo steps and the session log record it.
/// - After a request that changes the document, a `doc.changed` notification with the new
///   revision is sent through the event callback.
class RpcServer {
public:
    using EventSink = std::function<void(const Json& notification)>;

    RpcServer(const CommandRegistry& registry,
              CommandContext& context,
              std::optional<std::string> token = std::nullopt,
              EventSink events = {});

    /// Handles one parsed message (a request, a notification or a batch). Returns the response,
    /// or nothing for notifications.
    [[nodiscard]] std::optional<Json> handle(const Json& message);

    /// Parses and handles one line of text; returns the response line (without newline), or an
    /// empty string when there is nothing to send.
    [[nodiscard]] std::string handleLine(std::string_view line);

    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }

    // JSON-RPC error codes.
    static constexpr int kParseError = -32700;
    static constexpr int kInvalidRequest = -32600;
    static constexpr int kMethodNotFound = -32601;
    static constexpr int kInvalidParams = -32602;
    static constexpr int kInternalError = -32603;
    static constexpr int kCommandFailed = -32000; ///< A command ran and reported an Error.
    static constexpr int kUnauthorized = -32001;

private:
    std::optional<Json> handleOne(const Json& request);

    const CommandRegistry& registry_;
    CommandContext& context_;
    std::optional<std::string> token_;
    EventSink events_;
    bool authenticated_{false};
};

/// Maps an Error to a JSON-RPC error object: InvalidArgument -> -32602, NotFound for an unknown
/// command -> -32601, everything else -32000, with the Mesh2Cad code in `data`.
[[nodiscard]] Json rpcError(const Error& error);

} // namespace m2c

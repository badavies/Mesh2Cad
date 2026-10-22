// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "m2c/commands/Command.h"
#include "m2c/core/Progress.h"

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
/// - While a request runs, each stage its command reports goes out through the event callback
///   as `rpc.progress` {"id", "command", "stage", "fraction"?} before the response.
/// - A request's id is its job id: `rpc.cancel` {"id"} asks the running request with that id to
///   stop, and returns {"cancelled": whether one was running}. A long command stops at its next
///   checkpoint and fails with code -32800 ("cancelled"), leaving the document as it was.
///   rpc.cancel can be handled on another thread while a request runs (isCancel()); a transport
///   that reads while the engine works sends it there instead of queueing it behind the request.
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

    /// Asks the running request with this id to stop; false when none is running. Thread-safe.
    bool cancel(const Json& id);
    /// Asks every running request to stop (the app's Cancel button). Thread-safe.
    void cancelAll();
    /// Whether the message is an rpc.cancel, which handle() may take on any thread.
    [[nodiscard]] static bool isCancel(const Json& message);

    // JSON-RPC error codes.
    static constexpr int kParseError = -32700;
    static constexpr int kInvalidRequest = -32600;
    static constexpr int kMethodNotFound = -32601;
    static constexpr int kInvalidParams = -32602;
    static constexpr int kInternalError = -32603;
    static constexpr int kCommandFailed = -32000; ///< A command ran and reported an Error.
    static constexpr int kUnauthorized = -32001;
    static constexpr int kCancelled = -32800; ///< As LSP's RequestCancelled.

private:
    std::optional<Json> handleOne(const Json& request);

    const CommandRegistry& registry_;
    CommandContext& context_;
    std::optional<std::string> token_;
    EventSink events_;
    std::atomic<bool> authenticated_{false};
    std::mutex runningMutex_;
    std::optional<std::pair<Json, CancelToken>> running_; ///< The request running, by id.
};

/// Maps an Error to a JSON-RPC error object: InvalidArgument -> -32602, NotFound for an unknown
/// command -> -32601, Cancelled -> -32800, everything else -32000, with the Mesh2Cad code in
/// `data`.
[[nodiscard]] Json rpcError(const Error& error);

} // namespace m2c

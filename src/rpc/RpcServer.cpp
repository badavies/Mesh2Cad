// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/rpc/RpcServer.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"

namespace m2c {

namespace {

Json errorResponse(const Json& id, int code, const std::string& message, Json data = nullptr) {
    Json error{{"code", code}, {"message", message}};
    if (!data.is_null()) error["data"] = std::move(data);
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

/// Constant-time comparison, so the token cannot be guessed byte by byte from timing.
bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(static_cast<unsigned char>(a[i]) ^
                                           static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

} // namespace

Json rpcError(const Error& error) {
    int code = RpcServer::kCommandFailed;
    if (error.code == ErrorCode::InvalidArgument) code = RpcServer::kInvalidParams;
    if (error.code == ErrorCode::Cancelled) code = RpcServer::kCancelled;
    if (error.code == ErrorCode::NotFound && error.message.starts_with("unknown command")) {
        code = RpcServer::kMethodNotFound;
    }
    return Json{{"code", code}, {"message", error.message}, {"data", toJson(error)}};
}

RpcServer::RpcServer(const CommandRegistry& registry,
                     CommandContext& context,
                     std::optional<std::string> token,
                     EventSink events)
    : registry_(registry), context_(context), token_(std::move(token)), events_(std::move(events)),
      authenticated_(!token_) {}

bool RpcServer::cancel(const Json& id) {
    const std::scoped_lock lock(runningMutex_);
    if (!running_ || running_->first != id) return false;
    running_->second.cancel();
    return true;
}

void RpcServer::cancelAll() {
    const std::scoped_lock lock(runningMutex_);
    if (running_) running_->second.cancel();
}

bool RpcServer::isCancel(const Json& message) {
    return message.is_object() && message.contains("method") && message["method"] == "rpc.cancel";
}

std::optional<Json> RpcServer::handle(const Json& message) {
    if (message.is_array()) {
        if (message.empty())
            return std::make_optional(errorResponse(nullptr, kInvalidRequest, "empty batch"));
        Json responses = Json::array();
        for (const Json& request : message) {
            if (auto r = handleOne(request)) responses.push_back(std::move(*r));
        }
        if (responses.empty()) return std::nullopt;
        return std::make_optional(std::move(responses));
    }
    return handleOne(message);
}

std::string RpcServer::handleLine(std::string_view line) {
    if (line.find_first_not_of(" \t\r\n") == std::string_view::npos) return {};
    const Json message = Json::parse(line, nullptr, false);
    if (message.is_discarded()) {
        return errorResponse(nullptr, kParseError, "message is not valid JSON").dump();
    }
    const auto response = handle(message);
    if (!response) return {};
    return response->dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<Json> RpcServer::handleOne(const Json& request) {
    // value() throws when a key holds the wrong type, so check types before reading.
    if (!request.is_object() || !request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
        !request.contains("method") || !request["method"].is_string()) {
        return std::make_optional(
            errorResponse(request.is_object() ? request.value("id", Json()) : Json(),
                          kInvalidRequest,
                          "not a JSON-RPC 2.0 request"));
    }
    const bool isNotification = !request.contains("id");
    const Json id = request.value("id", Json());
    const std::string method = request["method"].get<std::string>();
    const Json params = request.value("params", Json::object());
    const auto reply = [&](Json response) -> std::optional<Json> {
        if (isNotification) return std::nullopt;
        return std::make_optional(std::move(response));
    };
    const auto ok = [&](Json result) {
        return reply(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };

    if (method == "rpc.hello") {
        return ok(Json{{"server", "mesh2cad"},
                       {"version", MESH2CAD_VERSION},
                       {"auth_required", !authenticated_},
                       {"source", context_.source}});
    }
    if (method == "rpc.auth") {
        const std::string token =
            params.is_object() && params.contains("token") && params["token"].is_string()
                ? params["token"].get<std::string>()
                : std::string();
        if (token_ && sameToken(token, *token_)) {
            authenticated_ = true;
            // A client may say who it is ("mcp", "script", ...); undo steps and the session log
            // then show that instead of "json".
            if (params.contains("source") && params["source"].is_string()) {
                if (const std::string source = params["source"].get<std::string>();
                    !source.empty() && source.size() <= 32)
                    context_.source = source;
            }
            return ok(Json{{"authenticated", true}, {"source", context_.source}});
        }
        return reply(errorResponse(id, kUnauthorized, "wrong session token"));
    }
    if (!authenticated_) {
        return reply(
            errorResponse(id, kUnauthorized, "call rpc.auth with the session token first"));
    }
    if (!params.is_object() && !params.is_null()) {
        return reply(errorResponse(id, kInvalidParams, "params must be an object"));
    }
    if (method == "rpc.cancel") {
        if (!params.is_object() || !params.contains("id") ||
            !(params["id"].is_string() || params["id"].is_number_integer())) {
            return reply(errorResponse(id, kInvalidParams, "rpc.cancel needs the request's id"));
        }
        return ok(Json{{"cancelled", cancel(params["id"])}});
    }

    const std::uint64_t before = context_.document.revision();
    // While the command runs, its stages go out as rpc.progress notifications for this request,
    // and rpc.cancel with its id stops it (the context's own token is set aside meanwhile).
    const Progress outer = context_.progress;
    const CancelToken token = CancelToken::make();
    context_.progress = Progress(
        [this, &id, &method, &outer](std::string_view stage, double fraction) {
            if (events_) {
                Json event{{"id", id}, {"command", method}, {"stage", std::string(stage)}};
                if (fraction >= 0.0) event["fraction"] = fraction;
                events_(Json{{"jsonrpc", "2.0"}, {"method", "rpc.progress"}, {"params", event}});
            }
            report(outer, stage, fraction);
        },
        token);
    if (!isNotification) {
        const std::scoped_lock lock(runningMutex_);
        running_.emplace(id, token);
    }
    const auto result = registry_.execute(context_, method, params);
    {
        const std::scoped_lock lock(runningMutex_);
        running_.reset();
    }
    context_.progress = outer;
    if (events_ && context_.document.revision() != before) {
        events_(
            Json{{"jsonrpc", "2.0"},
                 {"method", "doc.changed"},
                 {"params", {{"revision", context_.document.revision()}, {"command", method}}}});
    }
    if (!result) {
        Json error = rpcError(result.error());
        return reply(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
    }
    return ok(*result);
}

} // namespace m2c

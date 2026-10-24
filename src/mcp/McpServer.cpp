// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpServer.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Base64.h"
#include "m2c/core/Error.h"

namespace m2c {

namespace {

constexpr auto kProtocolVersion = "2025-06-18";

Json response(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json errorResponse(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

/// A result carrying a picture ({"image": {"mime_type", and "path" or "data_base64"}}, as
/// view.render's) also gets it as an image block, so the agent sees it: read from the file it
/// wrote, or moved out of the result (the text and structured copies then leave the data out).
/// Nothing is added when the file cannot be read.
void attachImage(Json& result, Json& content) {
    if (!result.is_object() || !result.contains("image") || !result["image"].is_object()) return;
    Json& image = result["image"];
    if (!image.contains("mime_type") || !image["mime_type"].is_string()) return;
    if (image.contains("data_base64") && image["data_base64"].is_string()) {
        content.push_back(Json{{"type", "image"},
                               {"data", std::move(image["data_base64"])},
                               {"mimeType", image["mime_type"]}});
        image.erase("data_base64");
        return;
    }
    if (!image.contains("path") || !image["path"].is_string()) return;
    std::ifstream file(image["path"].get<std::string>(), std::ios::binary);
    if (!file) return;
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    content.push_back(
        Json{{"type", "image"}, {"data", encodeBase64(bytes)}, {"mimeType", image["mime_type"]}});
}

} // namespace

McpServer::McpServer(EngineCall call, Notify notify, const std::chrono::milliseconds progressEvery)
    : call_(std::move(call)), notify_(std::move(notify)), progressEvery_(progressEvery) {}

Result<Json>
McpServer::callReporting(const std::string& command, const Json& arguments, const Json& token) {
    if (!notify_ || token.is_null()) return call_(command, arguments);
    // The engine call blocks this thread; a second one reports the seconds spent until it returns,
    // with the latest stage the command reported (engineEvent). There is no total.
    {
        const std::scoped_lock lock(stageMutex_);
        stage_.clear();
    }
    std::mutex mutex;
    std::condition_variable_any tick;
    std::jthread const heartbeat([&](const std::stop_token& stop) {
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        for (;;) {
            (void)tick.wait_for(lock, stop, progressEvery_, [] { return false; });
            if (stop.stop_requested()) return;
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::string stage;
            {
                const std::scoped_lock stageLock(stageMutex_);
                stage = stage_;
            }
            notify_(Json{{"jsonrpc", "2.0"},
                         {"method", "notifications/progress"},
                         {"params",
                          {{"progressToken", token},
                           {"progress", seconds},
                           {"message",
                            std::format("{}{}{}, {} s",
                                        command,
                                        stage.empty() ? " running" : ": ",
                                        stage,
                                        static_cast<long long>(seconds))}}}});
        }
    });
    return call_(command, arguments); // the heartbeat stops and is joined on return
}

void McpServer::engineEvent(const Json& notification) {
    if (!notification.is_object() || notification.value("method", Json()) != "rpc.progress" ||
        !notification.contains("params") || !notification["params"].is_object()) {
        return;
    }
    const Json& p = notification["params"];
    std::string stage = p.value("stage", std::string());
    if (p.contains("fraction") && p["fraction"].is_number()) {
        stage += " (" + std::to_string(std::lround(p["fraction"].get<double>() * 100.0)) + "%)";
    }
    const std::scoped_lock lock(stageMutex_);
    stage_ = std::move(stage);
}

std::string McpServer::toolName(const std::string& command) {
    std::string name = command;
    for (char& c : name) {
        if (c == '.') c = '_';
    }
    return name;
}

Result<Json> McpServer::listTools() {
    auto reply = call_("commands.list", Json::object());
    if (!reply) return std::unexpected(reply.error());
    if (!reply->contains("result")) {
        return fail(ErrorCode::Internal, "engine did not list its commands");
    }
    Json tools = Json::array();
    commandForTool_.clear();
    for (const Json& c : (*reply)["result"]["commands"]) {
        const std::string command = c["name"].get<std::string>();
        const bool mutates = c.value("mutates_document", false);
        // A file it writes may replace one that was there, so it is neither read-only nor safe.
        const bool writes = c.value("writes_files", false);
        const std::string name = toolName(command);
        commandForTool_[name] = command;
        std::string description = c.value("summary", "");
        if (mutates) description += " Changes the document (undoable with doc_undo).";
        if (writes) description += " Writes the file at path, replacing any file already there.";
        tools.push_back(Json{{"name", name},
                             {"title", command},
                             {"description", description},
                             {"inputSchema", c["params_schema"]},
                             {"annotations",
                              {{"readOnlyHint", !mutates && !writes},
                               {"destructiveHint", mutates || writes},
                               {"openWorldHint", false}}}});
    }
    return Json{{"tools", std::move(tools)}};
}

Json McpServer::callTool(const Json& params) {
    const auto failed = [](const std::string& text) {
        return Json{{"content", {{{"type", "text"}, {"text", text}}}}, {"isError", true}};
    };
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return failed("tools/call needs the tool's name");
    }
    const std::string name = params["name"].get<std::string>();
    if (commandForTool_.empty()) (void)listTools();
    // Only listed tools: internal methods such as rpc.auth are not tools.
    const auto it = commandForTool_.find(name);
    if (it == commandForTool_.end()) return failed("unknown tool '" + name + "'");
    const std::string& command = it->second;
    // Clients send a tool without parameters no arguments, {} or null.
    const Json arguments = params.contains("arguments") && !params["arguments"].is_null()
                               ? params["arguments"]
                               : Json::object();
    Json token;
    if (params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].contains("progressToken")) {
        const Json& t = params["_meta"]["progressToken"];
        if (t.is_string() || t.is_number_integer()) token = t;
    }

    auto reply = callReporting(command, arguments, token);
    if (!reply) return failed(reply.error().message);
    if (reply->contains("error")) {
        const Json& e = (*reply)["error"];
        return Json{
            {"content", {{{"type", "text"}, {"text", e.value("message", "command failed")}}}},
            {"structuredContent", e.value("data", Json::object())},
            {"isError", true}};
    }
    Json result = std::move((*reply)["result"]);
    Json images = Json::array();
    attachImage(result, images);
    Json out{{"content", {{{"type", "text"}, {"text", result.dump()}}}}, {"isError", false}};
    for (Json& image : images) out["content"].push_back(std::move(image));
    if (result.is_object()) out["structuredContent"] = std::move(result);
    return out;
}

bool McpServer::isCancel(const Json& message) {
    return message.is_object() && message.contains("method") &&
           message["method"] == "notifications/cancelled";
}

std::optional<Json> McpServer::handle(const Json& message) {
    if (isCancel(message)) {
        const Json params = message.value("params", Json::object());
        // The engine is asked under the lock, so the call cannot finish and another start
        // meanwhile.
        const std::scoped_lock lock(callMutex_);
        if (params.is_object() && params.contains("requestId") && callId_ &&
            *callId_ == params["requestId"] && !callCancelled_) {
            callCancelled_ = true;
            if (cancel_) cancel_();
        }
        return std::nullopt;
    }
    if (!message.is_object() || !message.contains("method")) {
        if (message.is_object() && message.contains("id")) {
            return std::make_optional(errorResponse(message["id"], -32600, "invalid request"));
        }
        return std::nullopt; // a response or junk: nothing to answer
    }
    if (!message.contains("id")) return std::nullopt; // other notifications (initialized)
    const Json& id = message["id"];
    if (!message["method"].is_string()) {
        return std::make_optional(errorResponse(id, -32600, "invalid request"));
    }
    const std::string method = message["method"].get<std::string>();
    const Json params = message.contains("params") ? message["params"] : Json::object();

    if (method == "initialize") {
        return std::make_optional(
            response(id,
                     Json{{"protocolVersion", kProtocolVersion},
                          {"capabilities", {{"tools", {{"listChanged", false}}}}},
                          {"serverInfo", {{"name", "mesh2cad"}, {"version", MESH2CAD_VERSION}}},
                          {"instructions",
                           "Mesh2Cad scan-to-CAD engine. Start with doc_list to see the "
                           "objects. Meshes, selections and measurements are referred to "
                           "by id. Every change is one undo step (doc_undo)."}}));
    }
    if (method == "ping") return std::make_optional(response(id, Json::object()));
    if (method == "tools/list") {
        auto tools = listTools();
        if (!tools) return std::make_optional(errorResponse(id, -32603, tools.error().message));
        return std::make_optional(response(id, *tools));
    }
    if (method == "tools/call") {
        {
            const std::scoped_lock lock(callMutex_);
            callId_ = id;
            callCancelled_ = false;
        }
        Json result = callTool(params);
        const std::scoped_lock lock(callMutex_);
        callId_.reset();
        if (callCancelled_) return std::nullopt;
        return std::make_optional(response(id, std::move(result)));
    }
    return std::make_optional(errorResponse(id, -32601, "method not found: " + method));
}

} // namespace m2c

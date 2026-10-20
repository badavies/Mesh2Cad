// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpServer.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "m2c/commands/Command.h"
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

std::string base64(const std::string& bytes) {
    static constexpr char kDigits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        const auto at = [&](std::size_t k) {
            return k < left ? static_cast<unsigned>(static_cast<unsigned char>(bytes[i + k])) : 0u;
        };
        const unsigned triple = at(0) << 16 | at(1) << 8 | at(2);
        out += kDigits[triple >> 18 & 63];
        out += kDigits[triple >> 12 & 63];
        out += left > 1 ? kDigits[triple >> 6 & 63] : '=';
        out += left > 2 ? kDigits[triple & 63] : '=';
    }
    return out;
}

/// A result naming a picture it wrote ({"image": {"path", "mime_type"}}, as view.render's) also
/// carries the picture, so the agent sees it. Nothing is added when the file cannot be read.
void attachImage(const Json& result, Json& content) {
    if (!result.is_object() || !result.contains("image") || !result["image"].is_object()) return;
    const Json& image = result["image"];
    if (!image.contains("path") || !image["path"].is_string() || !image.contains("mime_type") ||
        !image["mime_type"].is_string()) {
        return;
    }
    std::ifstream file(image["path"].get<std::string>(), std::ios::binary);
    if (!file) return;
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    content.push_back(
        Json{{"type", "image"}, {"data", base64(bytes)}, {"mimeType", image["mime_type"]}});
}

} // namespace

McpServer::McpServer(EngineCall call, Notify notify, const std::chrono::milliseconds progressEvery)
    : call_(std::move(call)), notify_(std::move(notify)), progressEvery_(progressEvery) {}

Result<Json>
McpServer::callReporting(const std::string& command, const Json& arguments, const Json& token) {
    if (!notify_ || token.is_null()) return call_(command, arguments);
    // The engine call blocks this thread; a second one reports the seconds spent until it returns.
    // Commands cannot say how far along they are yet (that comes with long jobs), so the
    // progress is the elapsed time and there is no total.
    std::mutex mutex;
    std::condition_variable_any tick;
    std::jthread heartbeat([&](const std::stop_token& stop) {
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        for (;;) {
            (void)tick.wait_for(lock, stop, progressEvery_, [] { return false; });
            if (stop.stop_requested()) return;
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            notify_(Json{{"jsonrpc", "2.0"},
                         {"method", "notifications/progress"},
                         {"params",
                          {{"progressToken", token},
                           {"progress", seconds},
                           {"message",
                            command + " running, " +
                                std::to_string(static_cast<long long>(seconds)) + " s"}}}});
        }
    });
    return call_(command, arguments); // the heartbeat stops and is joined on return
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
    const Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
    Json token;
    if (params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].contains("progressToken")) {
        const Json& t = params["_meta"]["progressToken"];
        if (t.is_string() || t.is_number_integer()) token = t;
    }

    const auto reply = callReporting(command, arguments, token);
    if (!reply) return failed(reply.error().message);
    if (reply->contains("error")) {
        const Json& e = (*reply)["error"];
        return Json{
            {"content", {{{"type", "text"}, {"text", e.value("message", "command failed")}}}},
            {"structuredContent", e.value("data", Json::object())},
            {"isError", true}};
    }
    const Json& result = (*reply)["result"];
    Json out{{"content", {{{"type", "text"}, {"text", result.dump()}}}}, {"isError", false}};
    attachImage(result, out["content"]);
    if (result.is_object()) out["structuredContent"] = result;
    return out;
}

std::optional<Json> McpServer::handle(const Json& message) {
    if (!message.is_object() || !message.contains("method")) {
        if (message.is_object() && message.contains("id")) {
            return std::make_optional(errorResponse(message["id"], -32600, "invalid request"));
        }
        return std::nullopt; // a response or junk: nothing to answer
    }
    if (!message.contains("id")) return std::nullopt; // notifications (initialized, cancelled)
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
    if (method == "tools/call") return std::make_optional(response(id, callTool(params)));
    return std::make_optional(errorResponse(id, -32601, "method not found: " + method));
}

} // namespace m2c

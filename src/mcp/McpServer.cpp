// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpServer.h"

namespace m2c {

namespace {

constexpr const char* kProtocolVersion = "2025-06-18";

Json response(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json errorResponse(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

} // namespace

McpServer::McpServer(EngineCall call) : call_(std::move(call)) {}

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
        const std::string name = toolName(command);
        commandForTool_[name] = command;
        tools.push_back(
            Json{{"name", name},
                 {"title", command},
                 {"description",
                  c.value("summary", "") +
                      (mutates ? " Changes the document (undoable with doc_undo)." : "")},
                 {"inputSchema", c["params_schema"]},
                 {"annotations",
                  {{"readOnlyHint", !mutates},
                   {"destructiveHint", mutates},
                   {"openWorldHint", false}}}});
    }
    return Json{{"tools", std::move(tools)}};
}

Json McpServer::callTool(const Json& params) {
    const std::string name = params.value("name", "");
    if (commandForTool_.empty()) (void)listTools();
    const auto it = commandForTool_.find(name);
    const std::string command = it != commandForTool_.end() ? it->second : name;
    const Json arguments = params.value("arguments", Json::object());

    const auto reply = call_(command, arguments);
    if (!reply) {
        return Json{{"content", {{{"type", "text"}, {"text", reply.error().message}}}},
                    {"isError", true}};
    }
    if (reply->contains("error")) {
        const Json& e = (*reply)["error"];
        return Json{
            {"content", {{{"type", "text"}, {"text", e.value("message", "command failed")}}}},
            {"structuredContent", e.value("data", Json::object())},
            {"isError", true}};
    }
    const Json& result = (*reply)["result"];
    Json out{{"content", {{{"type", "text"}, {"text", result.dump()}}}}, {"isError", false}};
    if (result.is_object()) out["structuredContent"] = result;
    return out;
}

std::optional<Json> McpServer::handle(const Json& message) {
    if (!message.is_object() || !message.contains("method")) {
        if (message.is_object() && message.contains("id")) {
            return errorResponse(message["id"], -32600, "invalid request");
        }
        return std::nullopt; // a response or junk: nothing to answer
    }
    const std::string method = message["method"].get<std::string>();
    if (!message.contains("id")) return std::nullopt; // notifications (initialized, cancelled)
    const Json id = message["id"];
    const Json params = message.value("params", Json::object());

    if (method == "initialize") {
        return response(id,
                        Json{{"protocolVersion", kProtocolVersion},
                             {"capabilities", {{"tools", {{"listChanged", false}}}}},
                             {"serverInfo", {{"name", "mesh2cad"}, {"version", MESH2CAD_VERSION}}},
                             {"instructions",
                              "Mesh2Cad scan-to-CAD engine. Start with doc_list to see the "
                              "objects. Meshes, selections and measurements are referred to "
                              "by id. Every change is one undo step (doc_undo)."}});
    }
    if (method == "ping") return response(id, Json::object());
    if (method == "tools/list") {
        auto tools = listTools();
        if (!tools) return errorResponse(id, -32603, tools.error().message);
        return response(id, *tools);
    }
    if (method == "tools/call") return response(id, callTool(params));
    return errorResponse(id, -32601, "method not found: " + method);
}

} // namespace m2c

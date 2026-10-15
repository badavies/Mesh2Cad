// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/MeshStats.h"
#include "m2c/io/MeshIO.h"

namespace m2c {

namespace {

// ---- Parameter helpers ------------------------------------------------------------------------

Result<std::string> requireString(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' is required and must be a string");
    }
    return it->get<std::string>();
}

Result<std::optional<std::string>> optionalString(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    if (it == params.end()) return std::optional<std::string>{};
    if (!it->is_string()) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be a string");
    }
    return std::optional<std::string>{it->get<std::string>()};
}

Result<MeshObject> requireMeshObject(const CommandContext& context, const Json& params) {
    const auto it = params.find("id");
    // Parsed JSON stores non-negative integers as unsigned, but a Json built in code from an int
    // is signed, so accept both.
    const bool isId =
        it != params.end() &&
        (it->is_number_unsigned() || (it->is_number_integer() && it->get<std::int64_t>() >= 0));
    if (!isId) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter 'id' is required and must be a non-negative integer");
    }
    const auto id = it->get<ObjectId>();
    auto object = context.document.find(id);
    if (!object) return fail(ErrorCode::NotFound, "no object with id " + std::to_string(id));
    return *object;
}

Json describe(const MeshObject& object) {
    return Json{
        {"id", object.id}, {"name", object.name}, {"stats", toJson(computeStats(*object.mesh))}};
}

// ---- Command definitions ----------------------------------------------------------------------

Command commandsList(const CommandRegistry& registry) {
    return Command{"commands.list",
                   "List every registered command with its summary and parameter schema.",
                   Json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
                   false,
                   [&registry](CommandContext&, const Json&) -> Result<Json> {
                       Json commands = Json::array();
                       for (const Command* c : registry.list()) {
                           commands.push_back(Json{{"name", c->name},
                                                   {"summary", c->summary},
                                                   {"mutates_document", c->mutatesDocument},
                                                   {"params_schema", c->paramsSchema}});
                       }
                       return Json{{"commands", std::move(commands)}};
                   }};
}

Command meshImport() {
    return Command{"mesh.import",
                   "Read a mesh file (STL) into the document as a new object.",
                   Json::parse(R"({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Path of the mesh file to read."},
                "name":{"type":"string","description":"Object name. Defaults to the file name without extension."}
            },
            "required":["path"],
            "additionalProperties":false})"),
                   true,
                   [](CommandContext& context, const Json& params) -> Result<Json> {
                       const auto path = requireString(params, "path");
                       if (!path) return std::unexpected(path.error());
                       const auto name = optionalString(params, "name");
                       if (!name) return std::unexpected(name.error());

                       const std::filesystem::path file(*path);
                       auto mesh = readMesh(file);
                       if (!mesh) return std::unexpected(mesh.error());

                       const std::string objectName = name->value_or(file.stem().string());
                       const auto id = context.document.addMesh(
                           objectName, std::make_shared<const TriMesh>(std::move(*mesh)));
                       if (!id) return std::unexpected(id.error());
                       return describe(*context.document.find(*id));
                   }};
}

Command meshInfo() {
    return Command{"mesh.info",
                   "Report counts, bounds, area, volume and topology problems (holes, non-manifold "
                   "edges) of a mesh.",
                   Json::parse(R"({
            "type":"object",
            "properties":{"id":{"type":"integer","minimum":0,"description":"Object id."}},
            "required":["id"],
            "additionalProperties":false})"),
                   false,
                   [](CommandContext& context, const Json& params) -> Result<Json> {
                       const auto object = requireMeshObject(context, params);
                       if (!object) return std::unexpected(object.error());
                       return describe(*object);
                   }};
}

Command meshExport() {
    return Command{"mesh.export",
                   "Write a mesh object to a file (binary STL).",
                   Json::parse(R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0,"description":"Object id."},
                "path":{"type":"string","description":"Destination file; the extension selects the format."}
            },
            "required":["id","path"],
            "additionalProperties":false})"),
                   false,
                   [](CommandContext& context, const Json& params) -> Result<Json> {
                       const auto object = requireMeshObject(context, params);
                       if (!object) return std::unexpected(object.error());
                       const auto path = requireString(params, "path");
                       if (!path) return std::unexpected(path.error());
                       if (auto written = writeMesh(*object->mesh, std::filesystem::path(*path));
                           !written) {
                           return std::unexpected(written.error());
                       }
                       return Json{{"id", object->id}, {"path", *path}};
                   }};
}

Command docList() {
    return Command{"doc.list",
                   "List the objects in the document.",
                   Json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
                   false,
                   [](CommandContext& context, const Json&) -> Result<Json> {
                       Json objects = Json::array();
                       for (const MeshObject& o : context.document.objects()) {
                           objects.push_back(Json{{"id", o.id},
                                                  {"name", o.name},
                                                  {"vertex_count", o.mesh->vertexCount()},
                                                  {"face_count", o.mesh->faceCount()}});
                       }
                       return Json{{"objects", std::move(objects)}};
                   }};
}

Command docUndo() {
    return Command{"doc.undo",
                   "Undo the last document change.",
                   Json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
                   true,
                   [](CommandContext& context, const Json&) -> Result<Json> {
                       if (auto undone = context.document.undo(); !undone) {
                           return std::unexpected(undone.error());
                       }
                       return Json::object();
                   }};
}

Command docRedo() {
    return Command{"doc.redo",
                   "Redo the last undone document change.",
                   Json::parse(R"({"type":"object","properties":{},"additionalProperties":false})"),
                   true,
                   [](CommandContext& context, const Json&) -> Result<Json> {
                       if (auto redone = context.document.redo(); !redone) {
                           return std::unexpected(redone.error());
                       }
                       return Json::object();
                   }};
}

} // namespace

Result<void> registerBuiltinCommands(CommandRegistry& registry) {
    std::vector<Command> commands;
    commands.push_back(commandsList(registry));
    commands.push_back(meshImport());
    commands.push_back(meshInfo());
    commands.push_back(meshExport());
    commands.push_back(docList());
    commands.push_back(docUndo());
    commands.push_back(docRedo());
    for (Command& command : commands) {
        if (auto added = registry.add(std::move(command)); !added) return added;
    }
    return {};
}

} // namespace m2c

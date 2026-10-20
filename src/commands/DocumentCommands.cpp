// SPDX-License-Identifier: GPL-3.0-or-later
// commands.list, doc.*, object.*, mesh.import, mesh.info, mesh.export.

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/document/Document.h"
#include "m2c/io/MeshIO.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

constexpr auto kNoParams = R"({"type":"object","properties":{},"additionalProperties":false})";
constexpr auto kIdOnly = R"({
    "type":"object",
    "properties":{"id":{"type":"integer","x-object":"any","minimum":0,"description":"Object id."}},
    "required":["id"],"additionalProperties":false})";
constexpr auto kMeshIdOnly = R"({
    "type":"object",
    "properties":{"id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh."}},
    "required":["id"],"additionalProperties":false})";

Json historyJson(const std::vector<HistoryStep>& steps) {
    Json out = Json::array();
    for (const auto& [label, source] : steps)
        out.push_back(Json{{"label", label}, {"source", source}});
    return out;
}

} // namespace

void addDocumentCommands(const CommandRegistry& registry, std::vector<Command>& out) {
    out.push_back(
        makeCommand("commands.list",
                    "List every registered command with its summary and parameter schema.",
                    kNoParams,
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
                    }));

    out.push_back(makeCommand(
        "doc.list",
        "List the objects in the document: meshes, selections, measurements and quad layouts.",
        kNoParams,
        false,
        [](const CommandContext& context, const Json&) -> Result<Json> {
            Json objects = Json::array();
            for (const DocumentObject& o : context.document.objects()) {
                objects.push_back(describe(o));
            }
            return Json{{"objects", std::move(objects)}, {"revision", context.document.revision()}};
        }));

    out.push_back(
        makeCommand("doc.history",
                    "List the undo and redo steps, with the source (ui, cli, json, mcp) of each.",
                    kNoParams,
                    false,
                    [](const CommandContext& context, const Json&) -> Result<Json> {
                        return Json{{"undo", historyJson(context.document.undoHistory())},
                                    {"redo", historyJson(context.document.redoHistory())}};
                    }));

    Command undo = makeCommand("doc.undo",
                               "Undo the last document change.",
                               kNoParams,
                               true,
                               [](CommandContext& context, const Json&) -> Result<Json> {
                                   M2C_TRY(done, context.document.undo());
                                   return Json::object();
                               });
    undo.opensUndoStep = false;
    out.push_back(std::move(undo));

    Command redo = makeCommand("doc.redo",
                               "Redo the last undone document change.",
                               kNoParams,
                               true,
                               [](CommandContext& context, const Json&) -> Result<Json> {
                                   M2C_TRY(done, context.document.redo());
                                   return Json::object();
                               });
    redo.opensUndoStep = false;
    out.push_back(std::move(redo));

    out.push_back(makeCommand(
        "object.delete",
        "Delete an object. Deleting a mesh also deletes its selections and measurements.",
        kIdOnly,
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(id, requireId(params, "id"));
            M2C_TRY(done, context.document.remove(*id));
            return Json{{"id", *id}};
        }));

    out.push_back(makeCommand("object.rename",
                              "Rename an object.",
                              R"({
            "type":"object",
            "properties":{"id":{"type":"integer","x-object":"any","minimum":0},"name":{"type":"string"}},
            "required":["id","name"],"additionalProperties":false})",
                              true,
                              [](CommandContext& context, const Json& params) -> Result<Json> {
                                  M2C_TRY(id, requireId(params, "id"));
                                  M2C_TRY(name, requireString(params, "name"));
                                  M2C_TRY(done, context.document.rename(*id, *name));
                                  return Json{{"id", *id}, {"name", *name}};
                              }));

    out.push_back(makeCommand("object.show",
                              "Show or hide an object in the viewport.",
                              R"({
            "type":"object",
            "properties":{"id":{"type":"integer","x-object":"any","minimum":0},"visible":{"type":"boolean"}},
            "required":["id","visible"],"additionalProperties":false})",
                              true,
                              [](CommandContext& context, const Json& params) -> Result<Json> {
                                  M2C_TRY(id, requireId(params, "id"));
                                  M2C_TRY(visible, optionalBool(params, "visible", true));
                                  M2C_TRY(done, context.document.setVisible(*id, *visible));
                                  return Json{{"id", *id}, {"visible", *visible}};
                              }));

    out.push_back(makeCommand(
        "mesh.import",
        "Read a mesh file (STL, OBJ or PLY) into the document as a new object.",
        R"({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Path of the mesh file to read (.stl, .obj, .ply)."},
                "name":{"type":"string","description":"Object name. Defaults to the file name without extension."}
            },
            "required":["path"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(path, requireString(params, "path"));
            M2C_TRY(name, optionalString(params, "name"));
            const std::filesystem::path file(*path);
            M2C_TRY(mesh, readMesh(file));
            M2C_TRY(id,
                    context.document.addMesh(name->value_or(file.stem().string()),
                                             std::make_shared<const TriMesh>(std::move(*mesh)),
                                             context.provenance()));
            return describeMesh(context.document, *id);
        }));

    out.push_back(makeCommand(
        "mesh.info",
        "Report counts, bounds, area, volume and topology problems (holes, non-manifold edges) of "
        "a mesh.",
        kMeshIdOnly,
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            return describeMesh(context.document, object->id);
        }));

    out.push_back(
        makeCommand("mesh.export",
                    "Write a mesh object to a file: binary STL, OBJ, or binary PLY.",
                    R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0,"description":"Object id."},
                "path":{"type":"string","description":"Destination file; the extension (.stl, .obj, .ply) selects the format."}
            },
            "required":["id","path"],"additionalProperties":false})",
                    false,
                    [](const CommandContext& context, const Json& params) -> Result<Json> {
                        M2C_TRY(object, requireMesh(context, params));
                        M2C_TRY(path, requireString(params, "path"));
                        M2C_TRY(written, writeMesh(*object->mesh, std::filesystem::path(*path)));
                        return Json{{"id", object->id}, {"path", *path}};
                    }));
}

} // namespace m2c::cmd

namespace m2c {

Result<void> registerBuiltinCommands(CommandRegistry& registry) {
    std::vector<Command> commands;
    cmd::addDocumentCommands(registry, commands);
    cmd::addMeshToolCommands(commands);
    cmd::addShellCommands(commands);
    cmd::addSelectionCommands(commands);
    cmd::addSegmentCommands(commands);
    cmd::addPrimitiveCommands(commands);
    cmd::addAlignMeasureCommands(commands);
    cmd::addProjectCommands(commands);
    cmd::addQuadCommands(commands);
    cmd::addSurfaceCommands(commands);
    cmd::addSketchCommands(commands);
    cmd::addDeviationCommands(commands);
    for (Command& command : commands) {
        if (auto added = registry.add(std::move(command)); !added) return added;
    }
    return {};
}

} // namespace m2c

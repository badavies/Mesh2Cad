// SPDX-License-Identifier: GPL-3.0-or-later
// Shell detection: find, select, split, keep or delete unconnected parts.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Transform.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/MeshTools.h"
#include "m2c/pipeline/Shells.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json shellJson(const ShellInfo& s) {
    return Json{{"index", s.index},
                {"face_count", s.faceCount},
                {"area_mm2", s.areaMm2},
                {"bounds", toJson(s.bounds)},
                {"is_main", s.isMain}};
}

constexpr auto kSmallSchema = R"({
    "type":"object",
    "properties":{
        "id":{"type":"integer","minimum":0},
        "min_faces":{"type":"integer","minimum":0,"default":0,"description":"Shells with fewer faces count as small."},
        "min_area_mm2":{"type":"number","minimum":0,"default":0,"description":"Shells with less area count as small."}
    },
    "required":["id"],"additionalProperties":false})";

} // namespace

void addShellCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "shells.detect",
        "List the unconnected parts (shells) of a mesh with face count, area and bounds; the "
        "largest is marked as the main part.",
        R"({"type":"object","properties":{"id":{"type":"integer","minimum":0}},
            "required":["id"],"additionalProperties":false})",
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const ShellReport report = detectShells(*object->mesh);
            Json shells = Json::array();
            for (const ShellInfo& s : report.shells) shells.push_back(shellJson(s));
            return Json{{"id", object->id}, {"shells", std::move(shells)}};
        }));

    out.push_back(makeCommand(
        "shells.select",
        "Create a selection holding the faces of the listed shells.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "shells":{"type":"array","items":{"type":"integer","minimum":0},"description":"Shell indices from shells.detect."}
            },
            "required":["id","shells"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const auto& list = params.at("shells");
            if (!list.is_array()) {
                return fail(ErrorCode::InvalidArgument, "parameter 'shells' must be an array");
            }
            const ShellReport report = detectShells(*object->mesh);
            std::vector<std::uint32_t> wanted;
            for (const auto& v : list) {
                const auto index = v.get<std::int64_t>();
                if (index < 0 || static_cast<std::size_t>(index) >= report.shells.size()) {
                    return fail(ErrorCode::InvalidArgument,
                                "shell " + std::to_string(index) + " does not exist");
                }
                wanted.push_back(static_cast<std::uint32_t>(index));
            }
            const auto faces = std::make_shared<const MeshSelection>(shellFaces(report, wanted));
            const std::size_t count = faces->count();
            M2C_TRY(id,
                    context.document.add("Shell selection",
                                         SelectionData{.meshId = object->id, .faces = faces},
                                         context.provenance({object->id})));
            return Json{{"selection_id", *id}, {"mesh_id", object->id}, {"face_count", count}};
        }));

    out.push_back(makeCommand(
        "shells.clean",
        "Delete every shell below a size limit (never the main part).",
        kSmallSchema,
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(minFaces, optionalInteger(params, "min_faces", 0, 0, INT64_MAX));
            M2C_TRY(minArea, optionalNumber(params, "min_area_mm2", 0.0, 0.0, 1e18));
            if (*minFaces == 0 && *minArea == 0.0) {
                return fail(ErrorCode::InvalidArgument, "give min_faces or min_area_mm2");
            }
            const ShellReport report = detectShells(*object->mesh);
            const MeshSelection small =
                smallShellFaces(report, static_cast<std::size_t>(*minFaces), *minArea);
            const std::size_t removedFaces = small.count();
            if (removedFaces == 0) {
                return Json{{"id", object->id}, {"deleted_shells", 0}, {"deleted_faces", 0}};
            }
            std::vector hit(report.shells.size(), false);
            for (const std::uint32_t f : small.indices()) hit[report.faceShell[f]] = true;
            const auto deletedShells = static_cast<std::size_t>(std::ranges::count(hit, true));
            M2C_TRY(kept, deleteFaces(*object->mesh, small));
            M2C_TRY(done, commitMesh(context, *object, std::move(*kept)));
            return Json{{"id", object->id},
                        {"deleted_shells", deletedShells},
                        {"deleted_faces", removedFaces}};
        }));

    out.push_back(makeCommand("shells.keep",
                              "Delete every shell except the main (largest) part.",
                              R"({
            "type":"object","properties":{"id":{"type":"integer","minimum":0}},
            "required":["id"],"additionalProperties":false})",
                              true,
                              [](CommandContext& context, const Json& params) -> Result<Json> {
                                  M2C_TRY(object, requireMesh(context, params));
                                  const ShellReport report = detectShells(*object->mesh);
                                  std::vector<std::uint32_t> main;
                                  for (const ShellInfo& s : report.shells) {
                                      if (s.isMain) main.push_back(s.index);
                                  }
                                  const std::size_t deleted = report.shells.size() - 1;
                                  if (deleted > 0) {
                                      M2C_TRY(kept,
                                              keepFaces(*object->mesh, shellFaces(report, main)));
                                      M2C_TRY(done, commitMesh(context, *object, std::move(*kept)));
                                  }
                                  return Json{{"id", object->id}, {"deleted_shells", deleted}};
                              }));

    out.push_back(makeCommand(
        "shells.split",
        "Split every shell except the main part into its own mesh object.",
        R"({
            "type":"object","properties":{"id":{"type":"integer","minimum":0}},
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const ShellReport report = detectShells(*object->mesh);
            // Split shells keep the alignment record of the mesh they came from.
            const Transform3d alignment = context.document.find(object->id)->mesh()->alignment;
            Json created = Json::array();
            std::vector<std::uint32_t> main;
            for (const ShellInfo& s : report.shells) {
                if (s.isMain) {
                    main.push_back(s.index);
                    continue;
                }
                M2C_TRY(part, keepFaces(*object->mesh, shellFaces(report, {s.index})));
                M2C_TRY(id,
                        context.document.add(
                            object->name + " shell " + std::to_string(s.index),
                            MeshData{.mesh = std::make_shared<const TriMesh>(std::move(*part)),
                                     .alignment = alignment},
                            context.provenance({object->id})));
                created.push_back(*id);
            }
            if (!created.empty()) {
                M2C_TRY(kept, keepFaces(*object->mesh, shellFaces(report, main)));
                M2C_TRY(done, commitMesh(context, *object, std::move(*kept)));
            }
            return Json{{"id", object->id}, {"created", std::move(created)}};
        }));
}

} // namespace m2c::cmd

// SPDX-License-Identifier: GPL-3.0-or-later
// Selection tools. A selection is a document object; tools either create a new one or combine
// their result into an existing one (selection_id + mode), so the UI and an AI agent build up
// selections the same way and no command ever carries face index lists.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/Select.h"
#include "m2c/pipeline/Shells.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

/// Shared "where does the result go" properties, spliced into each tool's schema.
#define M2C_SELECTION_TARGET                                                                       \
    R"("selection_id":{"type":"integer","minimum":0,"description":"Combine into this selection instead of creating a new one."},
    "mode":{"type":"string","enum":["replace","add","subtract"],"description":"How to combine with selection_id (default add)."},
    "name":{"type":"string","description":"Name of a new selection."})"

/// Stores `faces` as a new selection on `mesh`, or combines it into params.selection_id.
Result<Json> storeSelection(CommandContext& context,
                            const MeshObject& mesh,
                            MeshSelection faces,
                            const Json& params) {
    M2C_TRY(target, optionalSelection(context, params, "selection_id", mesh.id));
    M2C_TRY(mode, optionalString(params, "mode"));
    const std::string m = mode->value_or(*target ? "add" : "replace");
    if (m != "replace" && m != "add" && m != "subtract") {
        return fail(ErrorCode::InvalidArgument, "mode must be replace, add or subtract");
    }
    if (*target) {
        const MeshSelection& old = *(*target)->faces;
        if (m == "add" || m == "subtract") {
            MeshSelection combined = old;
            for (const std::uint32_t f : faces.indices()) combined.set(f, m == "add");
            faces = std::move(combined);
        }
        const std::size_t count = faces.count();
        M2C_TRY(done,
                context.document.replaceSelection(
                    (*target)->id, std::make_shared<const MeshSelection>(std::move(faces))));
        return Json{{"selection_id", (*target)->id}, {"mesh_id", mesh.id}, {"face_count", count}};
    }
    if (m == "subtract") {
        return fail(ErrorCode::InvalidArgument, "mode 'subtract' needs a selection_id");
    }
    M2C_TRY(name, optionalString(params, "name"));
    const std::size_t count = faces.count();
    M2C_TRY(id,
            context.document.add(
                name->value_or("Selection"),
                SelectionData{.meshId = mesh.id,
                              .faces = std::make_shared<const MeshSelection>(std::move(faces))},
                context.provenance({mesh.id})));
    return Json{{"selection_id", *id}, {"mesh_id", mesh.id}, {"face_count", count}};
}

/// Runs a tool that transforms an existing selection in place.
Command selectionEdit(std::string name,
                      std::string summary,
                      const char* schema,
                      MeshSelection (*edit)(const TriMesh&, const MeshSelection&, const Json&)) {
    return makeCommand(
        std::move(name),
        std::move(summary),
        schema,
        true,
        [edit](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(sel, requireSelection(context, params, "selection_id"));
            M2C_TRY(mesh, requireMesh(context, Json{{"id", sel->meshId}}));
            MeshSelection result = edit(*mesh->mesh, *sel->faces, params);
            const std::size_t count = result.count();
            M2C_TRY(done,
                    context.document.replaceSelection(
                        sel->id, std::make_shared<const MeshSelection>(std::move(result))));
            return Json{{"selection_id", sel->id}, {"mesh_id", mesh->id}, {"face_count", count}};
        });
}

} // namespace

void addSelectionCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "select.all",
        "Select every face of a mesh.",
        R"({"type":"object","properties":{"id":{"type":"integer","minimum":0},)" M2C_SELECTION_TARGET
        R"(},"required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            return storeSelection(context, *mesh, allFaces(*mesh->mesh), params);
        }));

    out.push_back(makeCommand(
        "select.brush",
        "Select the faces within a radius of a point on a mesh (a brush stroke is a series of "
        "these with mode add).",
        R"({"type":"object","properties":{
            "id":{"type":"integer","minimum":0},
            "center":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
            "centers":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"minItems":1,"description":"A whole stroke: the union of a brush at each point, as one undo step. Use instead of center."},
            "radius_mm":{"type":"number","exclusiveMinimum":0},
            "connected":{"type":"boolean","default":true,"description":"Only faces connected to the one under the centre."},)" M2C_SELECTION_TARGET
        R"(},"required":["id","radius_mm"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            if (!params.contains("radius_mm")) {
                return fail(ErrorCode::InvalidArgument, "parameter 'radius_mm' is required");
            }
            M2C_TRY(radius, optionalNumber(params, "radius_mm", 1.0, 0.0, 1e12));
            M2C_TRY(connected, optionalBool(params, "connected", true));
            std::vector<Vec3d> centers;
            if (params.contains("centers")) {
                const Json& list = params.at("centers");
                if (!list.is_array() || list.empty() || params.contains("center")) {
                    return fail(ErrorCode::InvalidArgument,
                                "give either 'center' or a non-empty 'centers' list");
                }
                for (const auto& i : list) {
                    M2C_TRY(c, requireVec3(Json{{"p", i}}, "p"));
                    centers.push_back(*c);
                }
            } else {
                M2C_TRY(center, requireVec3(params, "center"));
                centers.push_back(*center);
            }
            MeshSelection faces(mesh->mesh->faceCount());
            for (const Vec3d& c : centers) {
                for (const MeshSelection dab = brushSelect(*mesh->mesh, c, *radius, *connected);
                     const std::uint32_t f : dab.indices())
                    faces.set(f);
            }
            return storeSelection(context, *mesh, std::move(faces), params);
        }));

    out.push_back(makeCommand(
        "select.lasso",
        "Select the faces whose centres fall inside a screen polygon, given the view-projection "
        "matrix the polygon was drawn with.",
        R"({"type":"object","properties":{
            "id":{"type":"integer","minimum":0},
            "view_projection":{"type":"array","items":{"type":"number"},"minItems":16,"maxItems":16,"description":"Column-major 4x4 matrix (OpenGL)."},
            "polygon":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"minItems":3,"description":"Points in normalised device coordinates."},
            "eye":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Camera position; skips faces turned away."},
            "visible_only":{"type":"boolean","default":false,"description":"Skip hidden faces (needs eye)."},)" M2C_SELECTION_TARGET
        R"(},"required":["id","view_projection","polygon"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            LassoParams p;
            const Json& vp = params.at("view_projection");
            if (!vp.is_array() || vp.size() != 16) {
                return fail(ErrorCode::InvalidArgument, "view_projection must have 16 numbers");
            }
            for (std::size_t i = 0; i < 16; ++i) p.viewProjection[i] = vp[i].get<double>();
            for (const auto& point : params.at("polygon")) {
                p.polygon.push_back({point.at(0).get<double>(), point.at(1).get<double>()});
            }
            if (params.contains("eye")) {
                M2C_TRY(eye, requireVec3(params, "eye"));
                p.eye = *eye;
            }
            M2C_TRY(visible, optionalBool(params, "visible_only", false));
            p.visibleOnly = *visible;
            M2C_TRY(faces, lassoSelect(*mesh->mesh, p));
            return storeSelection(context, *mesh, std::move(*faces), params);
        }));

    out.push_back(makeCommand(
        "select.crease",
        "Flood-fill from a face across edges flatter than an angle (selects a smooth region "
        "bounded by creases).",
        R"({"type":"object","properties":{
            "id":{"type":"integer","minimum":0},
            "face":{"type":"integer","minimum":0,"description":"Seed face, e.g. from mesh.pick."},
            "max_angle_deg":{"type":"number","minimum":0,"maximum":180,"default":20},)" M2C_SELECTION_TARGET
        R"(},"required":["id","face"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            M2C_TRY(face, requireId(params, "face"));
            M2C_TRY(angle, optionalNumber(params, "max_angle_deg", 20.0, 0.0, 180.0));
            if (*face >= mesh->mesh->faceCount()) {
                return fail(ErrorCode::InvalidArgument,
                            "face " + std::to_string(*face) + " does not exist");
            }
            M2C_TRY(faces,
                    creaseFloodSelect(*mesh->mesh, static_cast<std::uint32_t>(*face), *angle));
            return storeSelection(context, *mesh, std::move(*faces), params);
        }));

    out.push_back(
        makeCommand("select.shell",
                    "Select the whole shell (connected part) containing a face.",
                    R"({"type":"object","properties":{
            "id":{"type":"integer","minimum":0},
            "face":{"type":"integer","minimum":0},)" M2C_SELECTION_TARGET
                    R"(},"required":["id","face"],"additionalProperties":false})",
                    true,
                    [](CommandContext& context, const Json& params) -> Result<Json> {
                        M2C_TRY(mesh, requireMesh(context, params));
                        M2C_TRY(face, requireId(params, "face"));
                        if (*face >= mesh->mesh->faceCount()) {
                            return fail(ErrorCode::InvalidArgument,
                                        "face " + std::to_string(*face) + " does not exist");
                        }
                        const ShellReport report = detectShells(*mesh->mesh);
                        return storeSelection(
                            context, *mesh, shellFaces(report, {report.faceShell[*face]}), params);
                    }));

    out.push_back(selectionEdit(
        "select.grow",
        "Grow a selection by rings of neighbouring faces.",
        R"({"type":"object","properties":{
            "selection_id":{"type":"integer","minimum":0},
            "rings":{"type":"integer","minimum":1,"maximum":1000,"default":1}},
            "required":["selection_id"],"additionalProperties":false})",
        [](const TriMesh& mesh, const MeshSelection& s, const Json& params) {
            return growSelection(mesh, s, std::clamp(params.value("rings", 1), 1, 1000));
        }));

    out.push_back(selectionEdit(
        "select.shrink",
        "Shrink a selection by rings of faces along its border.",
        R"({"type":"object","properties":{
            "selection_id":{"type":"integer","minimum":0},
            "rings":{"type":"integer","minimum":1,"maximum":1000,"default":1}},
            "required":["selection_id"],"additionalProperties":false})",
        [](const TriMesh& mesh, const MeshSelection& s, const Json& params) {
            return growSelection(mesh, s, -std::clamp(params.value("rings", 1), 1, 1000));
        }));

    out.push_back(selectionEdit(
        "select.invert",
        "Invert a selection.",
        R"({"type":"object","properties":{"selection_id":{"type":"integer","minimum":0}},
            "required":["selection_id"],"additionalProperties":false})",
        [](const TriMesh&, const MeshSelection& s, const Json&) {
            MeshSelection inverted = s;
            inverted.invert();
            return inverted;
        }));
}

} // namespace m2c::cmd

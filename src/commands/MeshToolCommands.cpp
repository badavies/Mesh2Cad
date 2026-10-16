// SPDX-License-Identifier: GPL-3.0-or-later
// Mesh tidy-up tools (ARCHITECTURE.md section 7). Each edits its mesh in place, as one undo step.

#include <cmath>
#include <limits>

#include "m2c/commands/JsonConvert.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/pipeline/Measure.h"
#include "m2c/pipeline/MeshTools.h"
#include "m2c/pipeline/Smooth.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

constexpr double kBig = 1e12;

Result<Json> afterEdit(CommandContext& context, ObjectId id, Json report) {
    Json result = describeMesh(context.document, id);
    result["report"] = std::move(report);
    return result;
}

} // namespace

void addMeshToolCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "mesh.repair",
        "Clean up a mesh: merge duplicate vertices, remove degenerate and duplicate faces and fins "
        "on non-manifold edges, fix non-manifold vertices and orient faces consistently (outward "
        "if closed). Holes stay open.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "remove_self_intersections":{"type":"boolean","default":false,
                    "description":"Also remove self-intersections: locally within a shell, and by uniting closed shells that pass through each other (slow on large scans)."}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            RepairParams p;
            M2C_TRY(si, optionalBool(params, "remove_self_intersections", false));
            p.removeSelfIntersections = *si;
            M2C_TRY(repaired, repair(*object->mesh, p));
            Json report{{"removed_faces", repaired->removedFaces},
                        {"split_vertices", repaired->splitVertices},
                        {"orientation_fixed", repaired->orientationFixed},
                        {"self_intersecting", repaired->selfIntersecting}};
            M2C_TRY(done, commitMesh(context, *object, std::move(repaired->mesh)));
            return afterEdit(context, object->id, std::move(report));
        }));

    out.push_back(makeCommand(
        "holes.fill",
        "Fill open boundaries with patches that blend into the surrounding surface. Holes larger "
        "than the limits are left open so real openings survive.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "max_perimeter_mm":{"type":"number","minimum":0,"default":0,"description":"Leave holes with a longer boundary open; 0 = no limit."},
                "max_edges":{"type":"integer","minimum":0,"default":0,"description":"Leave holes with more boundary edges open; 0 = no limit."},
                "fair":{"type":"boolean","default":true,"description":"Refine and fair the patch to match the surrounding curvature. Where the surface meets the hole at more than 45 degrees (an open end) the patch stays flat instead of doming."}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            HoleFillParams p;
            M2C_TRY(perimeter, optionalNumber(params, "max_perimeter_mm", 0.0, 0.0, kBig));
            M2C_TRY(edges, optionalInteger(params, "max_edges", 0, 0, INT32_MAX));
            M2C_TRY(fair, optionalBool(params, "fair", true));
            p.maxPerimeterMm = *perimeter;
            p.maxEdges = static_cast<std::size_t>(*edges);
            p.refineAndFair = *fair;
            M2C_TRY(filled, fillHoles(*object->mesh, p));
            Json report{{"filled", filled->filled},
                        {"skipped", filled->skipped},
                        {"failed", filled->failed},
                        {"added_faces", filled->addedFaces}};
            M2C_TRY(done, commitMesh(context, *object, std::move(filled->mesh)));
            return afterEdit(context, object->id, std::move(report));
        }));

    out.push_back(makeCommand(
        "mesh.smooth",
        "Taubin smoothing (no shrinkage) of the whole mesh or of a selection. Open edges stay "
        "fixed; sharp edges can be kept.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "selection_id":{"type":"integer","minimum":0,"description":"Smooth only these faces."},
                "iterations":{"type":"integer","minimum":1,"maximum":1000,"default":10},
                "strength":{"type":"number","exclusiveMinimum":0,"maximum":1,"default":0.5},
                "preserve_sharp_edges":{"type":"boolean","default":false},
                "sharp_angle_deg":{"type":"number","minimum":0,"maximum":180,"default":40}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(selection, optionalSelection(context, params, "selection_id", object->id));
            SmoothParams p;
            M2C_TRY(iterations, optionalInteger(params, "iterations", 10, 1, 1000));
            M2C_TRY(strength, optionalNumber(params, "strength", 0.5, 1e-6, 1.0));
            M2C_TRY(sharp, optionalBool(params, "preserve_sharp_edges", false));
            M2C_TRY(angle, optionalNumber(params, "sharp_angle_deg", 40.0, 0.0, 180.0));
            p.iterations = static_cast<unsigned>(*iterations);
            p.strength = *strength;
            p.preserveSharpEdges = *sharp;
            p.sharpAngleDeg = *angle;
            M2C_TRY(
                smoothed,
                smoothTaubin(*object->mesh, p, *selection ? (*selection)->faces.get() : nullptr));
            std::vector<ObjectId> inputs;
            if (*selection) inputs.push_back((*selection)->id);
            M2C_TRY(done, commitMesh(context, *object, std::move(*smoothed), inputs));
            return afterEdit(context, object->id, Json::object());
        }));

    out.push_back(makeCommand(
        "mesh.simplify",
        "Reduce the triangle count (quadric edge collapse), keeping open boundaries in place.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "target_faces":{"type":"integer","minimum":1,"description":"Target triangle count."},
                "target_ratio":{"type":"number","exclusiveMinimum":0,"maximum":1,"description":"Target as a fraction of the current count (used when target_faces is not given)."},
                "preserve_borders":{"type":"boolean","default":true}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const std::size_t before = object->mesh->faceCount();
            M2C_TRY(target, optionalInteger(params, "target_faces", 0, 1, INT64_MAX));
            M2C_TRY(ratio, optionalNumber(params, "target_ratio", 0.5, 1e-9, 1.0));
            M2C_TRY(borders, optionalBool(params, "preserve_borders", true));
            SimplifyParams p;
            p.targetFaces = *target > 0
                                ? static_cast<std::size_t>(*target)
                                : std::max<std::size_t>(1,
                                                        static_cast<std::size_t>(std::llround(
                                                            *ratio * static_cast<double>(before))));
            p.preserveBorders = *borders;
            M2C_TRY(simplified, simplifyMesh(*object->mesh, p));
            Json report{{"faces_before", before}, {"faces_after", simplified->faceCount()}};
            M2C_TRY(done, commitMesh(context, *object, std::move(*simplified)));
            return afterEdit(context, object->id, std::move(report));
        }));

    out.push_back(makeCommand(
        "mesh.remesh",
        "Rebuild an even triangle mesh at a set edge length, keeping sharp edges.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "target_edge_mm":{"type":"number","exclusiveMinimum":0},
                "iterations":{"type":"integer","minimum":1,"maximum":100,"default":3},
                "sharp_angle_deg":{"type":"number","minimum":0,"maximum":180,"default":60,"description":"Keep edges sharper than this; 0 keeps none."}
            },
            "required":["id","target_edge_mm"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            if (!params.contains("target_edge_mm")) {
                return fail(ErrorCode::InvalidArgument, "parameter 'target_edge_mm' is required");
            }
            RemeshParams p;
            M2C_TRY(edge, optionalNumber(params, "target_edge_mm", 1.0, 1e-9, kBig));
            M2C_TRY(iterations, optionalInteger(params, "iterations", 3, 1, 100));
            M2C_TRY(angle, optionalNumber(params, "sharp_angle_deg", 60.0, 0.0, 180.0));
            p.targetEdgeMm = *edge;
            p.iterations = static_cast<unsigned>(*iterations);
            p.sharpAngleDeg = *angle;
            M2C_TRY(remeshed, remeshMesh(*object->mesh, p));
            M2C_TRY(done, commitMesh(context, *object, std::move(*remeshed)));
            return afterEdit(context, object->id, Json::object());
        }));

    out.push_back(makeCommand(
        "mesh.wrap",
        "Build a watertight, manifold mesh around a badly damaged scan (alpha wrapping) as a new "
        "object. Detail smaller than alpha_mm is lost.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "alpha_mm":{"type":"number","exclusiveMinimum":0,"description":"Probe size. Defaults to 1/100 of the bounding-box diagonal."},
                "offset_mm":{"type":"number","exclusiveMinimum":0,"description":"Distance from the input. Defaults to alpha_mm / 30."},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const Aabb& box = object->mesh->bounds();
            const double diagonal = length(box.max - box.min);
            M2C_TRY(alpha, optionalNumber(params, "alpha_mm", diagonal / 100.0, 1e-9, kBig));
            M2C_TRY(offset, optionalNumber(params, "offset_mm", *alpha / 30.0, 1e-9, kBig));
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(wrapped, shrinkWrap(*object->mesh, WrapParams{*alpha, *offset}));
            M2C_TRY(id,
                    context.document.addMesh(name->value_or(object->name + " (wrap)"),
                                             std::make_shared<const TriMesh>(std::move(*wrapped)),
                                             context.provenance({object->id})));
            Json result = describeMesh(context.document, *id);
            result["report"] = Json{{"alpha_mm", *alpha}, {"offset_mm", *offset}};
            return result;
        }));

    out.push_back(makeCommand(
        "faces.delete",
        "Delete the faces of a selection from its mesh.",
        R"({
            "type":"object",
            "properties":{"selection_id":{"type":"integer","minimum":0}},
            "required":["selection_id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(selection, requireSelection(context, params, "selection_id"));
            M2C_TRY(object, requireMesh(context, Json{{"id", selection->meshId}}));
            M2C_TRY(kept, deleteFaces(*object->mesh, *selection->faces));
            const std::size_t removed = object->mesh->faceCount() - kept->faceCount();
            M2C_TRY(done, commitMesh(context, *object, std::move(*kept), {selection->id}));
            return afterEdit(context, object->id, Json{{"deleted_faces", removed}});
        }));

    out.push_back(makeCommand("mesh.flip",
                              "Reverse the orientation of every face of a mesh.",
                              R"({
            "type":"object","properties":{"id":{"type":"integer","minimum":0}},
            "required":["id"],"additionalProperties":false})",
                              true,
                              [](CommandContext& context, const Json& params) -> Result<Json> {
                                  M2C_TRY(object, requireMesh(context, params));
                                  M2C_TRY(flipped, flipFaces(*object->mesh));
                                  M2C_TRY(done, commitMesh(context, *object, std::move(*flipped)));
                                  return afterEdit(context, object->id, Json::object());
                              }));

    out.push_back(makeCommand(
        "mesh.pick",
        "Cast a ray against visible meshes (or one mesh) at full resolution and return the "
        "nearest hit: object, face, point and normal.",
        R"({
            "type":"object",
            "properties":{
                "origin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                "direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                "id":{"type":"integer","minimum":0,"description":"Only this mesh."}
            },
            "required":["origin","direction"],"additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(origin, requireVec3(params, "origin"));
            M2C_TRY(direction, requireVec3(params, "direction"));
            M2C_TRY(only, optionalId(params, "id"));
            Json best;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (const DocumentObject& o : context.document.objects()) {
                if (o.mesh() == nullptr) continue;
                if (*only ? o.id != **only : !o.visible) continue;
                const TriMesh& mesh = *o.mesh()->mesh;
                const auto hit = pickMesh(mesh, *origin, *direction);
                if (!hit || hit->distance >= bestDistance) continue;
                bestDistance = hit->distance;
                best = Json{{"id", o.id},
                            {"face", hit->face},
                            {"point", toJson(hit->point)},
                            {"normal", toJson(faceNormal(mesh, hit->face))},
                            {"distance_mm", hit->distance}};
            }
            if (best.is_null()) return Json{{"hit", false}};
            best["hit"] = true;
            return best;
        }));
}

} // namespace m2c::cmd

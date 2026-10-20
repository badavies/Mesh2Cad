// SPDX-License-Identifier: GPL-3.0-or-later
// Alignment (3-2-1 datum and automatic) and point-to-point measurement.

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/Align.h"
#include "m2c/pipeline/Measure.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json planeJson(const PlaneFit& p) {
    return Json{{"point", toJson(p.point)}, {"normal", toJson(p.normal)}, {"rms_mm", p.rmsMm}};
}

/// Applies `t` to a mesh object, accumulating it into the object's stored alignment. Selections
/// survive (the faces are unchanged); measurements on the mesh are moved with it.
Result<Json> applyTransform(CommandContext& context,
                            const MeshObject& object,
                            const Transform3d& t,
                            std::vector<ObjectId> inputs) {
    M2C_TRY(moved, transformMesh(*object.mesh, t));
    const auto current = context.document.find(object.id);
    const Transform3d total = t * current->mesh()->alignment;
    inputs.insert(inputs.begin(), object.id);
    M2C_TRY(done,
            context.document.replaceMesh(object.id,
                                         std::make_shared<const TriMesh>(std::move(*moved)),
                                         context.provenance(std::move(inputs)),
                                         total));
    // Move measurements that sit on this mesh.
    for (const DocumentObject& o : context.document.objects()) {
        const MeasurementData* m = o.measurement();
        if (m == nullptr) continue;
        Measurement updated = m->measurement;
        bool touched = false;
        for (PickedPoint* p : {&updated.a, &updated.b}) {
            if (p->objectId == object.id) {
                p->position = t.apply(p->position);
                touched = true;
            }
        }
        if (!touched) continue;
        M2C_TRY(replaced, context.document.replaceMeasurement(o.id, updated));
    }
    Json result = describeMesh(context.document, object.id);
    result["transform"] = toJson(t);
    return result;
}

Result<AlignOrigin> originParam(const Json& params) {
    M2C_TRY(name, optionalString(params, "origin"));
    const std::string o = name->value_or("centroid");
    if (o == "centroid") return AlignOrigin::Centroid;
    if (o == "bounds_center") return AlignOrigin::BoundsCenter;
    if (o == "bounds_bottom_center") return AlignOrigin::BoundsBottomCenter;
    return fail(ErrorCode::InvalidArgument,
                "origin must be centroid, bounds_center or bounds_bottom_center");
}

Result<PickedPoint> pointParam(const CommandContext& context,
                               const Json& params,
                               const std::string& key,
                               const double snapMm) {
    if (!params.contains(key) || !params.at(key).is_object()) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key +
                        R"(' must be an object {"point": [x, y, z], "id": mesh})");
    }
    const Json& spec = params.at(key);
    M2C_TRY(point, requireVec3(spec, "point"));
    M2C_TRY(id, optionalId(spec, "id"));
    if (!*id)
        return PickedPoint{.position = *point, .objectId = 0, .snap = SnapKind::Free, .element = 0};
    M2C_TRY(mesh, requireMesh(context, Json{{"id", **id}}));
    return snapToMesh(*mesh->mesh, mesh->id, *point, snapMm);
}

} // namespace

void addAlignMeasureCommands(std::vector<Command>& out) {
    out.push_back(
        makeCommand("plane.fit",
                    "Fit a least-squares plane to the faces of a selection.",
                    R"({
            "type":"object","properties":{"selection_id":{"type":"integer","x-object":"selection","minimum":0}},
            "required":["selection_id"],"additionalProperties":false})",
                    false,
                    [](const CommandContext& context, const Json& params) -> Result<Json> {
                        M2C_TRY(sel, requireSelection(context, params, "selection_id"));
                        M2C_TRY(mesh, requireMesh(context, Json{{"id", sel->meshId}}));
                        M2C_TRY(plane, fitPlane(*mesh->mesh, *sel->faces));
                        return planeJson(*plane);
                    }));

    out.push_back(makeCommand(
        "align.principal",
        "Automatic alignment: move the mesh so its principal axes lie along X (longest), Y and Z "
        "(shortest), with the origin at the centroid or the bounding box.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0},
                "origin":{"type":"string","enum":["centroid","bounds_center","bounds_bottom_center"],"default":"centroid"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(origin, originParam(params));
            M2C_TRY(t, principalAxesAlignment(*object->mesh, *origin));
            return applyTransform(context, *object, *t, {});
        }));

    out.push_back(makeCommand(
        "align.symmetry",
        "Automatic alignment by the best mirror-symmetry plane: that plane becomes X = 0, Y and Z "
        "follow the principal axes within it. Reports how far mirrored points land from the "
        "surface, so a poor fit shows.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0},
                "origin":{"type":"string","enum":["centroid","bounds_center","bounds_bottom_center"],"default":"centroid","description":"Positions the part in Y and Z; X = 0 stays on the plane."}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(origin, originParam(params));
            M2C_TRY(aligned, symmetryAlignment(*object->mesh, *origin));
            M2C_TRY(result, applyTransform(context, *object, aligned->transform, {}));
            (*result)["report"] = Json{{"plane_point", toJson(aligned->planePoint)},
                                       {"plane_normal", toJson(aligned->planeNormal)},
                                       {"residual_mm", aligned->residualMm}};
            return result;
        }));

    out.push_back(makeCommand(
        "align.datum",
        "3-2-1 datum alignment: the primary selection's plane becomes Z = 0, the secondary's Y = 0 "
        "and the tertiary's X = 0. The part ends up on the positive side of each plane.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0},
                "primary":{"type":"integer","x-object":"selection","minimum":0,"description":"Selection id of the primary datum face."},
                "secondary":{"type":"integer","x-object":"selection","minimum":0,"description":"Selection id of the secondary datum face."},
                "tertiary":{"type":"integer","x-object":"selection","minimum":0,"description":"Selection id of the tertiary datum face."},
                "flip_primary":{"type":"boolean","default":false,"description":"Put the part below Z = 0 instead."}
            },
            "required":["id","primary"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(primary, requireSelection(context, params, "primary", object->id));
            M2C_TRY(secondary, optionalSelection(context, params, "secondary", object->id));
            M2C_TRY(tertiary, optionalSelection(context, params, "tertiary", object->id));
            M2C_TRY(flip, optionalBool(params, "flip_primary", false));
            DatumAlignParams p;
            p.primary = *primary->faces;
            std::vector inputs{primary->id};
            if (*secondary) {
                p.secondary = *(*secondary)->faces;
                inputs.push_back((*secondary)->id);
            }
            if (*tertiary) {
                p.tertiary = *(*tertiary)->faces;
                inputs.push_back((*tertiary)->id);
            }
            p.flipPrimary = *flip;
            M2C_TRY(aligned, datumAlignment(*object->mesh, p));
            M2C_TRY(result, applyTransform(context, *object, aligned->transform, inputs));
            (*result)["primary_plane"] = planeJson(aligned->primary);
            if (aligned->secondary) (*result)["secondary_plane"] = planeJson(*aligned->secondary);
            if (aligned->tertiary) (*result)["tertiary_plane"] = planeJson(*aligned->tertiary);
            return *result;
        }));

    out.push_back(makeCommand(
        "align.transform",
        "Apply a rigid transform (rotation matrix and translation) to a mesh.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0},
                "rotation":{"type":"array","items":{"type":"number"},"minItems":9,"maxItems":9,"description":"Row-major rotation matrix; identity if omitted."},
                "translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            Transform3d t;
            if (params.contains("rotation")) {
                const Json& r = params.at("rotation");
                if (!r.is_array() || r.size() != 9) {
                    return fail(ErrorCode::InvalidArgument, "rotation must have 9 numbers");
                }
                for (std::size_t i = 0; i < 9; ++i) t.rotation[i] = r[i].get<double>();
                // Must be orthonormal with determinant +1.
                const Transform3d check = t * t.inverse();
                double error = 0.0;
                for (std::size_t i = 0; i < 9; ++i) {
                    error += std::abs(check.rotation[i] - Transform3d{}.rotation[i]);
                }
                const auto& m = t.rotation;
                const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                                   m[1] * (m[3] * m[8] - m[5] * m[6]) +
                                   m[2] * (m[3] * m[7] - m[4] * m[6]);
                if (error > 1e-6 || det <= 0.0) { // also refuses NaN
                    return fail(ErrorCode::InvalidArgument, "rotation is not a rotation matrix");
                }
            }
            if (params.contains("translation")) {
                M2C_TRY(tr, requireVec3(params, "translation"));
                t.translation = *tr;
            }
            return applyTransform(context, *object, t, {});
        }));

    out.push_back(makeCommand(
        "align.reset",
        "Undo every alignment applied to a mesh, returning it to its scanned pose.",
        R"({"type":"object","properties":{"id":{"type":"integer","x-object":"mesh","minimum":0}},
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            const Transform3d inverse =
                context.document.find(object->id)->mesh()->alignment.inverse();
            return applyTransform(context, *object, inverse, {});
        }));

    out.push_back(makeCommand(
        "measure.distance",
        "Measure the distance between two points and save it as a measurement object. A point "
        "given with a mesh id snaps to that mesh (vertex, edge or surface).",
        R"({
            "type":"object",
            "properties":{
                "a":{"type":"object","properties":{"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"id":{"type":"integer","x-object":"mesh","minimum":0}},"required":["point"]},
                "b":{"type":"object","properties":{"point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"id":{"type":"integer","x-object":"mesh","minimum":0}},"required":["point"]},
                "snap_mm":{"type":"number","minimum":0,"default":0,"description":"Snap to a vertex or edge within this distance."},
                "name":{"type":"string"}
            },
            "required":["a","b"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(snap, optionalNumber(params, "snap_mm", 0.0, 0.0, 1e12));
            M2C_TRY(a, pointParam(context, params, "a", *snap));
            M2C_TRY(b, pointParam(context, params, "b", *snap));
            M2C_TRY(name, optionalString(params, "name"));
            const Measurement m{.a = *a, .b = *b};
            std::vector<ObjectId> inputs;
            for (const ObjectId id : {a->objectId, b->objectId}) {
                if (id != 0) inputs.push_back(id);
            }
            M2C_TRY(id,
                    context.document.add(name->value_or("Distance"),
                                         MeasurementData{m},
                                         context.provenance(inputs)));
            Json result = toJson(m);
            result["id"] = *id;
            return result;
        }));
}

} // namespace m2c::cmd

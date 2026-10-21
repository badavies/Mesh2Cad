// SPDX-License-Identifier: GPL-3.0-or-later
// Primitive fitting: an exact plane, cylinder, sphere or cone fitted to a mesh region
// (ARCHITECTURE.md section 7, "Primitive fitting").

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/PrimitiveFit.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// The primitive objects listed by params[key].
Result<std::vector<DocumentObject>>
primitives(const CommandContext& context, const Json& params, const std::string& key) {
    if (!params.contains(key) || !params[key].is_array() || params[key].empty()) {
        return fail(ErrorCode::InvalidArgument, key + " must list primitives");
    }
    std::vector<DocumentObject> out;
    for (const Json& id : params[key]) {
        M2C_TRY(object, requireObject(context, Json{{"id", id}}, "id", "primitive"));
        if (object->primitive()->fit.surface.kind == PrimitiveKind::Sphere) {
            return fail(ErrorCode::InvalidArgument,
                        "primitive " + std::to_string(object->id) +
                            " is a sphere, which has no "
                            "direction to align");
        }
        out.push_back(std::move(*object));
    }
    return out;
}

/// A plane is aligned by its normal, a cylinder or cone by its axis.
bool isPlane(const DocumentObject& o) {
    return o.primitive()->fit.surface.kind == PrimitiveKind::Plane;
}

/// The reference direction primitive.align turns to, and whether it is a plane's normal.
struct Reference {
    Vec3d direction;
    bool normal{false};
};

Result<Reference> reference(const CommandContext& context,
                            const Json& params,
                            const std::vector<DocumentObject>& targets) {
    const int given = static_cast<int>(params.contains("reference_ids")) +
                      static_cast<int>(params.contains("axis")) +
                      static_cast<int>(params.contains("plane"));
    if (given > 1) {
        return fail(ErrorCode::InvalidArgument, "give one of reference_ids, axis or plane");
    }
    M2C_TRY(axis, optionalString(params, "axis"));
    if (*axis) {
        const std::string& a = **axis;
        if (a != "x" && a != "y" && a != "z") {
            return fail(ErrorCode::InvalidArgument, "axis must be x, y or z");
        }
        return Reference{
            .direction = {a == "x" ? 1.0 : 0.0, a == "y" ? 1.0 : 0.0, a == "z" ? 1.0 : 0.0}};
    }
    M2C_TRY(plane, optionalString(params, "plane"));
    if (*plane) {
        const std::string& a = **plane;
        if (a != "xy" && a != "yz" && a != "zx") {
            return fail(ErrorCode::InvalidArgument, "plane must be xy, yz or zx");
        }
        return Reference{
            .direction = {a == "yz" ? 1.0 : 0.0, a == "zx" ? 1.0 : 0.0, a == "xy" ? 1.0 : 0.0},
            .normal = true};
    }
    std::vector<DocumentObject> listed;
    if (params.contains("reference_ids")) {
        M2C_TRY(found, primitives(context, params, "reference_ids"));
        listed = std::move(*found);
    }
    const std::vector<DocumentObject>& from = listed.empty() ? targets : listed;
    // The mean of the directions, each turned to the first's side.
    const Vec3d first = from.front().primitive()->fit.surface.direction;
    Vec3d sum;
    for (const DocumentObject& o : from) {
        if (isPlane(o) != isPlane(from.front())) {
            return fail(ErrorCode::InvalidArgument,
                        "the reference must be all planes or all cylinders and cones");
        }
        const Vec3d d = o.primitive()->fit.surface.direction;
        sum = sum + (dot(d, first) < 0.0 ? d * -1.0 : d);
    }
    if (!(length(sum) > 1e-9)) {
        return fail(ErrorCode::InvalidArgument, "the reference directions cancel out");
    }
    return Reference{.direction = sum * (1.0 / length(sum)), .normal = isPlane(from.front())};
}

/// The mesh vertices of the selection a primitive was fitted to.
Result<std::vector<Vec3d>> fittedRegion(const CommandContext& context, const DocumentObject& o) {
    const std::string missing = "the selection primitive " + std::to_string(o.id) +
                                " was fitted to is gone, so it cannot be refitted";
    if (o.provenance.inputs.empty()) return fail(ErrorCode::InvalidState, missing);
    const auto sel = requireSelection(context, Json{{"id", o.provenance.inputs.front()}}, "id");
    if (!sel) return fail(ErrorCode::InvalidState, missing);
    M2C_TRY(mesh, requireMesh(context, Json{{"id", sel->meshId}}));
    std::vector<Vec3d> points = regionPoints(*mesh->mesh, sel->faces->indices());
    if (points.empty()) return fail(ErrorCode::InvalidState, missing);
    return points;
}

} // namespace

void addPrimitiveCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "primitive.fit",
        "Fit an exact, unbounded plane, cylinder, sphere or cone to the faces of a selection by "
        "least squares, starting from CGAL's estimate, and add it as a primitive. With kind "
        "\"auto\" it takes the simplest kind that fits about as well as any. Fails, adding "
        "nothing, when 95% of the region's vertices are not within max_residual_mm of the "
        "surface. The result gives the primitive, its RMS, largest and P95 distance, and the "
        "region's bounding box.",
        R"({
            "type":"object",
            "properties":{
                "selection_id":{"type":"integer","x-object":"selection","minimum":0},
                "kind":{"type":"string","enum":["auto","plane","cylinder","sphere","cone"],"default":"auto"},
                "max_residual_mm":{"type":"number","exclusiveMinimum":0,"default":0.1,"description":"Largest P95 distance from the region's vertices to the surface."}
            },
            "required":["selection_id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(sel, requireSelection(context, params, "selection_id"));
            M2C_TRY(mesh, requireMesh(context, Json{{"id", sel->meshId}}));
            M2C_TRY(kindName, optionalString(params, "kind"));
            RegionFitParams p;
            if (*kindName && **kindName != "auto") {
                p.kind = primitiveKindFromName(**kindName);
                if (!p.kind) {
                    return fail(ErrorCode::InvalidArgument,
                                "kind must be auto, plane, cylinder, sphere or cone");
                }
            }
            M2C_TRY(limit, optionalNumber(params, "max_residual_mm", 0.1, 1e-9, 1e6));
            p.maxResidualMm = *limit;
            M2C_TRY(fitted, fitRegionPrimitive(*mesh->mesh, sel->faces->indices(), p));
            M2C_TRY(id,
                    context.document.add(fittedName(fitted->surface.kind),
                                         primitiveData(*fitted),
                                         context.provenance({sel->id, mesh->id})));
            Json report = toJson(fitted->surface);
            report["id"] = *id;
            report["rms_mm"] = fitted->rmsMm;
            report["max_mm"] = fitted->maxMm;
            report["p95_mm"] = fitted->p95Mm;
            report["extent"] = toJson(fitted->extent);
            return report;
        }));

    out.push_back(makeCommand(
        "primitive.align",
        "Make planes, cylinders and cones exactly parallel, perpendicular or at a set angle to a "
        "reference, then refit everything but the direction to the regions they were fitted "
        "to. The reference is the mean of reference_ids, a world axis, or a world plane; with "
        "none, the mean of the primitives' own directions (parallel only). Angles have their "
        "CAD meaning: a plane is measured by its normal and a cylinder or cone by its axis, so "
        "a cylinder perpendicular to a plane has its axis along the normal. Replaces the "
        "primitives in place as one step. Fails, changing nothing, for a sphere, a turn whose "
        "way is undefined, a region that is gone, or a new P95 above max_residual_mm. The "
        "result gives each primitive's turn in degrees and its P95 before and after.",
        R"({
            "type":"object",
            "properties":{
                "ids":{"type":"array","items":{"type":"integer","x-object":"primitive","minimum":0},"minItems":1,"description":"The primitives to align."},
                "reference_ids":{"type":"array","items":{"type":"integer","x-object":"primitive","minimum":0},"minItems":1,"description":"Primitives whose mean direction is the reference: all planes, or all cylinders and cones."},
                "axis":{"type":"string","enum":["x","y","z"],"description":"A world axis as the reference, a line."},
                "plane":{"type":"string","enum":["xy","yz","zx"],"description":"A world plane as the reference."},
                "relation":{"type":"string","enum":["parallel","perpendicular","angle"],"default":"parallel"},
                "angle_deg":{"type":"number","minimum":0,"maximum":90,"description":"The angle, for relation angle."},
                "max_residual_mm":{"type":"number","exclusiveMinimum":0,"description":"Largest P95 distance allowed after aligning."}
            },
            "required":["ids"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(targets, primitives(context, params, "ids"));
            M2C_TRY(ref, reference(context, params, *targets));
            M2C_TRY(relation, optionalString(params, "relation"));
            const std::string how = relation->value_or("parallel");
            double angle = 0.0;
            if (how == "perpendicular") {
                angle = 90.0;
            } else if (how == "angle") {
                if (!params.contains("angle_deg")) {
                    return fail(ErrorCode::InvalidArgument, "relation angle needs angle_deg");
                }
                M2C_TRY(given, optionalNumber(params, "angle_deg", 0.0, 0.0, 90.0));
                angle = *given;
            } else if (how != "parallel") {
                return fail(ErrorCode::InvalidArgument,
                            "relation must be parallel, perpendicular or angle");
            }
            const bool ownMean = !params.contains("reference_ids") && !params.contains("axis") &&
                                 !params.contains("plane");
            if (ownMean && how != "parallel") {
                return fail(ErrorCode::InvalidArgument,
                            "without a reference, primitives can only be made parallel");
            }
            const bool limited = params.contains("max_residual_mm");
            M2C_TRY(limit, optionalNumber(params, "max_residual_mm", 0.0, 1e-9, 1e6));

            std::vector<FittedPrimitive> aligned;
            Json report = Json::array();
            for (const DocumentObject& o : *targets) {
                const FittedPrimitive& old = o.primitive()->fit;
                // A line against a plane's normal: parallel to the plane is perpendicular to it.
                const double between = isPlane(o) == ref->normal ? angle : 90.0 - angle;
                M2C_TRY(direction,
                        alignedDirection(old.surface.direction, ref->direction, between * kDeg));
                M2C_TRY(points, fittedRegion(context, o));
                Primitive start = old.surface;
                start.direction = *direction;
                M2C_TRY(fit, fitWithDirection(start, *points));
                if (limited && fit->p95Mm > *limit) {
                    return fail(ErrorCode::InvalidState,
                                "primitive " + std::to_string(o.id) + " would be " +
                                    std::to_string(fit->p95Mm) +
                                    " mm from 95% of its region, "
                                    "more than max_residual_mm");
                }
                aligned.push_back(FittedPrimitive{.surface = fit->primitive,
                                                  .extent = old.extent,
                                                  .rmsMm = fit->rmsMm,
                                                  .maxMm = fit->maxMm,
                                                  .p95Mm = fit->p95Mm});
                Json line = toJson(fit->primitive);
                line["id"] = o.id;
                line["turned_deg"] =
                    std::acos(std::clamp(dot(old.surface.direction, *direction), -1.0, 1.0)) / kDeg;
                line["p95_before_mm"] = old.p95Mm;
                line["p95_mm"] = fit->p95Mm;
                line["rms_mm"] = fit->rmsMm;
                report.push_back(std::move(line));
            }
            for (std::size_t i = 0; i < aligned.size(); ++i) {
                M2C_TRY(
                    done,
                    context.document.replacePrimitive((*targets)[i].id, primitiveData(aligned[i])));
            }
            return Json{{"primitives", std::move(report)}, {"reference", toJson(ref->direction)}};
        }));
}

} // namespace m2c::cmd

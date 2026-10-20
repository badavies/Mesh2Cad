// SPDX-License-Identifier: GPL-3.0-or-later
// Section sketches (ARCHITECTURE.md section 7): a mesh cut with a plane, and the lines, arcs and
// splines fitted to the cut.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Sketch.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/Section.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

/// The plane the parameters name: an origin plane, a fitted plane primitive, or a point and a
/// normal; moved offset_mm along its normal.
Result<SketchPlane> sectionPlane(const CommandContext& context, const Json& params) {
    const int given = static_cast<int>(params.contains("plane")) +
                      static_cast<int>(params.contains("plane_id")) +
                      static_cast<int>(params.contains("point"));
    if (given != 1 || params.contains("point") != params.contains("normal")) {
        return fail(ErrorCode::InvalidArgument,
                    "give one of plane, plane_id, or point with normal");
    }
    SketchPlane plane;
    if (params.contains("plane")) {
        M2C_TRY(name, requireString(params, "plane"));
        if (*name == "yz") {
            plane.u = {0, 1, 0};
            plane.v = {0, 0, 1};
        } else if (*name == "zx") {
            plane.u = {0, 0, 1};
            plane.v = {1, 0, 0};
        } else if (*name != "xy") {
            return fail(ErrorCode::InvalidArgument, "plane must be xy, yz or zx");
        }
    } else if (params.contains("plane_id")) {
        M2C_TRY(object, requireObject(context, params, "plane_id", "primitive"));
        const Primitive& surface = object->primitive()->fit.surface;
        if (surface.kind != PrimitiveKind::Plane) {
            return fail(ErrorCode::InvalidArgument,
                        "primitive " + std::to_string(object->id) + " is not a plane");
        }
        plane = planeThrough(surface.point, surface.direction);
    } else {
        M2C_TRY(point, requireVec3(params, "point"));
        M2C_TRY(normal, requireVec3(params, "normal"));
        if (!(length(*normal) > 0.0)) {
            return fail(ErrorCode::InvalidArgument, "normal must not be zero");
        }
        plane = planeThrough(*point, *normal);
    }
    M2C_TRY(offset, optionalNumber(params, "offset_mm", 0.0, -1e6, 1e6));
    plane.origin = plane.origin + plane.normal() * *offset;
    return plane;
}

} // namespace

void addSketchCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "sketch.section",
        "Cut a mesh with a plane and add a sketch of the cut: each closed piece becomes a loop of "
        "lines, arcs and splines within max_error_mm of the section points (a full circle when "
        "it is one). The plane is an origin plane (xy, yz or zx), a fitted plane primitive, or a "
        "point and a normal, moved offset_mm along its normal. Pieces that run into a hole or "
        "border are left out and counted. Fails, adding nothing, if no piece closes. Sketches are "
        "fitted, not constrained.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh to cut."},
                "plane":{"type":"string","enum":["xy","yz","zx"],"description":"An origin plane."},
                "plane_id":{"type":"integer","x-object":"primitive","minimum":0,"description":"A fitted plane primitive."},
                "point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                "normal":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                "offset_mm":{"type":"number","default":0,"description":"Moves the plane along its normal."},
                "max_error_mm":{"type":"number","exclusiveMinimum":0,"default":0.05,"description":"Largest distance from a section point to its curve."},
                "min_curve_points":{"type":"integer","minimum":2,"default":6,"description":"Smooth runs of lines and arcs that each cover fewer section points become a spline."},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            M2C_TRY(plane, sectionPlane(context, params));
            SectionParams p;
            M2C_TRY(limit, optionalNumber(params, "max_error_mm", 0.05, 1e-9, 1e6));
            p.maxErrorMm = *limit;
            M2C_TRY(minPoints, optionalInteger(params, "min_curve_points", 6, 2, 1000000));
            p.minCurvePoints = static_cast<int>(*minPoints);
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(section, sectionMesh(*mesh->mesh, *plane, p));
            std::vector<ObjectId> inputs{mesh->id};
            if (params.contains("plane_id"))
                inputs.push_back(params.at("plane_id").get<ObjectId>());
            Json loops = Json::array();
            for (const SketchLoop& loop : section->sketch.loops) {
                std::size_t count[3]{}; // by CurveKind
                for (const SketchCurve& c : loop.curves) ++count[static_cast<int>(c.kind)];
                loops.push_back({{"lines", count[0]}, {"arcs", count[1]}, {"splines", count[2]}});
            }
            M2C_TRY(id,
                    context.document.add(name->value_or("Section"),
                                         SketchData{section->sketch},
                                         context.provenance(std::move(inputs))));
            return Json{{"id", *id},
                        {"loops", std::move(loops)},
                        {"open_pieces", section->openPieces},
                        {"max_error_mm", section->maxErrorMm},
                        {"plane",
                         {{"origin", toJson(plane->origin)},
                          {"u", toJson(plane->u)},
                          {"v", toJson(plane->v)}}}};
        }));
}

} // namespace m2c::cmd

// SPDX-License-Identifier: GPL-3.0-or-later
// Surface build (ARCHITECTURE.md section 6): a quad layout and its scan in, a B-rep solid out.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"
#include "m2c/document/Document.h"
#include "m2c/io/Step.h"
#include "m2c/pipeline/Boolean.h"
#include "m2c/pipeline/Profile.h"
#include "m2c/pipeline/Surface.h"
#include "m2c/pipeline/Trim.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json toJson(const SolidSummary& s) {
    return Json{{"face_count", s.faceCount},
                {"edge_count", s.edgeCount},
                {"vertex_count", s.vertexCount},
                {"shell_count", s.shellCount},
                {"solid_count", s.solidCount},
                {"free_edges", s.freeEdges},
                {"closed", s.closed},
                {"valid", s.valid},
                {"volume_mm3", s.volumeMm3},
                {"area_mm2", s.areaMm2},
                {"max_tolerance_mm", s.maxToleranceMm},
                {"pole_count", s.poleCount},
                {"bounds", toJson(s.bounds)}};
}

bool isStepPath(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return std::tolower(c); });
    return ext == ".step" || ext == ".stp";
}

/// Re-read volume may differ from the original by this fraction (the kernel's own volume
/// integration on the same faces; tessellation does not enter).
constexpr double kStepVolumeTolerance = 1e-3;

} // namespace

void addSurfaceCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "surface.build",
        "Build a closed B-rep solid from a quad layout: fit the cage so its Catmull-Clark limit "
        "surface follows the mesh, convert the limit surface to B-spline faces (exact where the "
        "cage is regular, close-fitting caps at irregular vertices) and sew them. A fit that folds "
        "the surface (faces meeting at more than 10 degrees) is made again with twice the "
        "smoothing, up to four fits in all. Rectangles of cage quads, bounded by lines traced "
        "from the irregular vertices, are merged into one face each. With refine_to_mm, a fit "
        "farther from the mesh than that (95th percentile) has its cage subdivided once and is "
        "fitted again, up to max_refinements times. An open cage (from quad.remesh "
        "keep_outline) needs thickness_mm: its fitted sheet becomes a wall that thick, behind "
        "the faces' side, with a sharp-edged rim. Adds a solid object; the quad layout and "
        "mesh are unchanged. Fails, adding nothing, unless the result is one valid closed solid "
        "of positive volume whose faces, after the last fit, meet at no more than 90 degrees.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"quad_layout","minimum":0,"description":"The quad layout."},
                "mesh_id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh to fit to; required unless fit is false."},
                "fit":{"type":"boolean","default":true,"description":"Fit the cage to the mesh first."},
                "iterations":{"type":"integer","minimum":1,"maximum":10,"default":3,"description":"Rounds of fitting."},
                "smoothing":{"type":"number","minimum":0,"maximum":10,"default":0.05,"description":"How strongly the cage keeps its own shape while fitting."},
                "max_samples":{"type":"integer","minimum":100,"maximum":10000000,"default":60000,"description":"Mesh vertices used for fitting (evenly thinned beyond this)."},
                "max_sample_distance_mm":{"type":"number","minimum":0,"default":0,"description":"Mesh points farther than this from the surface are left out of the fit (parts the cage does not cover); 0 = the mean cage edge length."},
                "isolation_level":{"type":"integer","minimum":1,"maximum":4,"default":2,"description":"Refinement around irregular vertices: higher gives smaller caps and more faces' pieces."},
                "merge":{"type":"boolean","default":true,"description":"Merge rectangles of cage quads into one face each (fewer faces, same surface)."},
                "refine_to_mm":{"type":"number","minimum":0,"default":0,"description":"While the fit's 95th-percentile distance to the mesh is above this, subdivide the fitted cage once (four times the quads, same layout) and fit again; 0 = off."},
                "max_refinements":{"type":"integer","minimum":0,"maximum":3,"default":2,"description":"Most subdivisions refine_to_mm may make."},
                "thickness_mm":{"type":"number","minimum":0,"default":0,"description":"For an open cage: thicken the fitted sheet by this much, against its faces' direction; required for an open cage and only for one."},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(layout, requireObject(context, params, "id", "quad_layout"));
            SurfaceBuildParams p;
            M2C_TRY(fit, optionalBool(params, "fit", true));
            M2C_TRY(iterations, optionalInteger(params, "iterations", 3, 1, 10));
            M2C_TRY(smoothing, optionalNumber(params, "smoothing", 0.05, 0.0, 10.0));
            M2C_TRY(samples, optionalInteger(params, "max_samples", 60000, 100, 10000000));
            M2C_TRY(level, optionalInteger(params, "isolation_level", 2, 1, 4));
            M2C_TRY(gate, optionalNumber(params, "max_sample_distance_mm", 0.0, 0.0, 1e6));
            p.fitting.maxSampleDistanceMm = *gate;
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(merge, optionalBool(params, "merge", true));
            p.merge = *merge;
            M2C_TRY(refineTo, optionalNumber(params, "refine_to_mm", 0.0, 0.0, 1e6));
            M2C_TRY(maxRefinements, optionalInteger(params, "max_refinements", 2, 0, 3));
            p.refineToMm = *refineTo;
            p.maxRefinements = static_cast<unsigned>(*maxRefinements);
            M2C_TRY(thickness, optionalNumber(params, "thickness_mm", 0.0, 0.0, 1e6));
            p.thicknessMm = *thickness;
            p.fit = *fit;
            p.fitting.iterations = static_cast<unsigned>(*iterations);
            p.fitting.smoothing = *smoothing;
            p.fitting.maxSamples = static_cast<std::size_t>(*samples);
            p.fitting.isolationLevel = static_cast<int>(*level);

            std::vector inputs{layout->id};
            std::shared_ptr<const TriMesh> scan;
            if (params.contains("mesh_id")) {
                M2C_TRY(mesh, requireMesh(context, params, "mesh_id"));
                scan = mesh->mesh;
                inputs.push_back(mesh->id);
            } else if (p.fit) {
                return fail(ErrorCode::InvalidArgument, "mesh_id is required when fit is true");
            }
            M2C_TRY(built, buildSurface(layout->quadLayout()->cage, scan.get(), p));
            const SolidSummary summary = built->solid.summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(layout->name + " (solid)"),
                                         SolidData{std::move(built->solid)},
                                         context.provenance(std::move(inputs))));
            Json result{{"id", *id},
                        {"summary", toJson(summary)},
                        {"regular_pieces", built->regularPieces},
                        {"cap_pieces", built->capPieces},
                        {"merge",
                         {{"faces_before", built->facesBeforeMerge},
                          {"faces_after", summary.faceCount},
                          {"merged_faces", built->mergedFaces},
                          {"unmerged_quads", built->unmergedQuads}}},
                        {"max_cap_error_mm", built->maxCapErrorMm},
                        {"seams",
                         {{"max_gap_mm", built->seams.maxGapMm},
                          {"max_normal_angle_deg", built->seams.maxNormalAngleDeg},
                          {"max_cap_normal_angle_deg", built->seams.maxCapNormalAngleDeg},
                          {"max_regular_normal_angle_deg", built->seams.maxRegularNormalAngleDeg},
                          {"unmatched_samples", built->seams.unmatchedSamples}}}};
            if (p.fit) {
                result["fit"] = Json{{"samples", built->fit.samples},
                                     {"ignored_samples", built->fit.ignoredSamples},
                                     {"rms_distance_mm", built->fit.rmsDistanceMm},
                                     {"max_distance_mm", built->fit.maxDistanceMm},
                                     {"p95_distance_mm", built->fit.p95DistanceMm},
                                     {"rms_before_round_mm", built->fit.rmsBeforeRoundMm},
                                     {"smoothing", built->smoothing},
                                     {"fits", built->fits},
                                     {"refinements", built->p95BeforeRefinementMm.size()},
                                     {"p95_before_refinement_mm", built->p95BeforeRefinementMm}};
            }
            return result;
        }));

    out.push_back(makeCommand(
        "primitive.trim",
        "Make a solid with sharp edges from fitted primitives (primitive.fit): each is bounded by "
        "the mesh's box, grown by a tenth of its diagonal, and all are split against each other "
        "(Open CASCADE General Fuse). A split face is kept when min_coverage of its area lies "
        "within max_distance_mm of the mesh; the kept faces are sewn into a solid and faces of "
        "one primitive merged. The faces are the primitives' exact planes, cylinders, spheres "
        "and cones. Adds a solid object; fails, adding nothing, naming where the faces leave the "
        "solid open (a primitive there is missing or does not fit), or unless the result is one "
        "valid closed solid of positive volume.",
        R"({
            "type":"object",
            "properties":{
                "primitive_ids":{"type":"array","items":{"type":"integer","x-object":"primitive","minimum":0},"minItems":1,"description":"The primitives to trim."},
                "mesh_id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh they were fitted to."},
                "max_distance_mm":{"type":"number","exclusiveMinimum":0,"default":0.5,"description":"A point of a split face lies on the mesh when it is this close."},
                "min_coverage":{"type":"number","exclusiveMinimum":0,"maximum":1,"default":0.9,"description":"Share of a split face's area that must lie on the mesh for it to be kept."},
                "name":{"type":"string"}
            },
            "required":["primitive_ids","mesh_id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params, "mesh_id"));
            if (!params.contains("primitive_ids") || !params["primitive_ids"].is_array() ||
                params["primitive_ids"].empty()) {
                return fail(ErrorCode::InvalidArgument, "primitive_ids must list primitives");
            }
            std::vector<FittedPrimitive> primitives;
            std::vector<ObjectId> inputs;
            for (const Json& id : params["primitive_ids"]) {
                M2C_TRY(object, requireObject(context, Json{{"id", id}}, "id", "primitive"));
                primitives.push_back(object->primitive()->fit);
                inputs.push_back(object->id);
            }
            inputs.push_back(mesh->id);
            TrimParams p;
            M2C_TRY(distance, optionalNumber(params, "max_distance_mm", 0.5, 1e-9, 1e6));
            M2C_TRY(coverage, optionalNumber(params, "min_coverage", 0.9, 1e-9, 1.0));
            M2C_TRY(name, optionalString(params, "name"));
            p.maxDistanceMm = *distance;
            p.minCoverage = *coverage;
            M2C_TRY(trimmed, trimPrimitives(primitives, *mesh->mesh, p));
            const SolidSummary summary = trimmed->solid.summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(mesh->name + " (trimmed solid)"),
                                         SolidData{std::move(trimmed->solid)},
                                         context.provenance(std::move(inputs))));
            return Json{{"id", *id},
                        {"summary", toJson(summary)},
                        {"split_faces", trimmed->splitFaces},
                        {"kept_faces", trimmed->keptFaces},
                        {"max_gap_mm", summary.maxToleranceMm}};
        }));

    out.push_back(makeCommand(
        "solid.boolean",
        "Combine two solids into a new one: fuse joins them, cut removes tool_id from id, common "
        "keeps what they share. Boundaries closer than fuzzy_mm count as touching. Faces of one "
        "surface are merged, so a cylinder cut from a body leaves one exact cylindrical face. Adds "
        "a solid object; both inputs are unchanged. Fails, adding nothing, when nothing is left, "
        "when the result is more than one solid (for example fusing solids that do not touch), "
        "or unless it is one valid closed solid of positive volume.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"solid","minimum":0,"description":"The solid to fuse to, cut from or intersect."},
                "tool_id":{"type":"integer","x-object":"solid","minimum":0,"description":"The other solid."},
                "operation":{"type":"string","enum":["fuse","cut","common"]},
                "fuzzy_mm":{"type":"number","minimum":0,"default":0.001,"description":"Boundaries this close are treated as touching."},
                "name":{"type":"string"}
            },
            "required":["id","tool_id","operation"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(a, requireObject(context, params, "id", "solid"));
            M2C_TRY(b, requireObject(context, params, "tool_id", "solid"));
            M2C_TRY(operation, requireString(params, "operation"));
            BooleanOp op = BooleanOp::Fuse;
            if (*operation == "cut") {
                op = BooleanOp::Cut;
            } else if (*operation == "common") {
                op = BooleanOp::Common;
            } else if (*operation != "fuse") {
                return fail(ErrorCode::InvalidArgument,
                            "operation must be fuse, cut or common, not " + *operation);
            }
            if (a->id == b->id) {
                return fail(ErrorCode::InvalidArgument, "id and tool_id must be different solids");
            }
            M2C_TRY(fuzzy, optionalNumber(params, "fuzzy_mm", 0.001, 0.0, 1e6));
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(combined, booleanSolids(a->solid()->solid, b->solid()->solid, op, *fuzzy));
            const SolidSummary summary = combined->summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(a->name + " " + *operation + " " + b->name),
                                         SolidData{std::move(*combined)},
                                         context.provenance({a->id, b->id})));
            return Json{{"id", *id}, {"summary", toJson(summary)}};
        }));

    out.push_back(makeCommand(
        "primitive.solid",
        "Make a fitted cylinder or sphere (primitive.fit) a solid of its exact surface, to fuse "
        "to or cut from another solid with solid.boolean, such as a true-cylinder hole through "
        "an organic body. A sphere is whole; a cylinder runs along its axis, centred on the "
        "fitted region, length_mm long (0 = as long as the region). radius_mm replaces the "
        "fitted radius, for example to drill a fuzzy scanned hole clean. Adds a solid object; "
        "fails for a plane or cone.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"primitive","minimum":0,"description":"The cylinder or sphere primitive."},
                "length_mm":{"type":"number","minimum":0,"default":0,"description":"A cylinder's length; 0 = the length of the region it was fitted to."},
                "radius_mm":{"type":"number","minimum":0,"default":0,"description":"The solid's radius; 0 = the fitted radius."},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "primitive"));
            M2C_TRY(length, optionalNumber(params, "length_mm", 0.0, 0.0, 1e6));
            M2C_TRY(radius, optionalNumber(params, "radius_mm", 0.0, 0.0, 1e6));
            M2C_TRY(name, optionalString(params, "name"));
            FittedPrimitive fit = object->primitive()->fit;
            if (*radius > 0.0) fit.surface.radiusMm = *radius;
            M2C_TRY(solid, primitiveSolid(fit, *length));
            const SolidSummary summary = solid->summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(object->name + " (solid)"),
                                         SolidData{std::move(*solid)},
                                         context.provenance({object->id})));
            return Json{{"id", *id}, {"summary", toJson(summary)}};
        }));

    out.push_back(makeCommand(
        "solid.info",
        "Describe a solid: face, edge and shell counts, whether it is closed and valid, its "
        "volume, area and bounds.",
        R"({
            "type":"object",
            "properties":{"id":{"type":"integer","x-object":"solid","minimum":0,"description":"The solid."}},
            "required":["id"],"additionalProperties":false})",
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "solid"));
            return Json{{"id", object->id},
                        {"name", object->name},
                        {"summary", toJson(object->solid()->solid.summary())}};
        }));

    Command exportStep = makeCommand(
        "solid.export",
        "Write a solid to a STEP AP242 file in millimetres, then read the file back and check it "
        "holds one valid closed solid with the same volume (within 0.1%). Fails, writing "
        "nothing, if the solid is not valid.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"solid","minimum":0,"description":"The solid."},
                "path":{"type":"string","x-file-types":["step","stp"],"description":"The .step or .stp file to write."},
                "product_name":{"type":"string","description":"Product name in the file; the object name if omitted."}
            },
            "required":["id","path"],"additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "solid"));
            M2C_TRY(path, requireString(params, "path"));
            M2C_TRY(product, optionalString(params, "product_name"));
            const Solid& solid = object->solid()->solid;
            const std::filesystem::path file(*path);
            if (!isStepPath(file)) {
                return fail(ErrorCode::InvalidArgument, "the path must end in .step or .stp");
            }
            M2C_TRY(written, writeStep(solid, file, product->value_or(object->name)));
            M2C_TRY(reread, readStep(file));
            const double expected = solid.summary().volumeMm3;
            const double error =
                std::abs(reread->volumeMm3 - expected) / std::max(std::abs(expected), 1e-12);
            const bool passed = reread->solidCount == 1 && reread->closed && reread->valid &&
                                error <= kStepVolumeTolerance;
            return Json{{"id", object->id},
                        {"path", *path},
                        {"check",
                         {{"passed", passed},
                          {"solid_count", reread->solidCount},
                          {"closed", reread->closed},
                          {"valid", reread->valid},
                          {"face_count", reread->faceCount},
                          {"volume_mm3", reread->volumeMm3},
                          {"volume_error", error}}}};
        });
    exportStep.writesFiles = true;
    out.push_back(std::move(exportStep));

    out.push_back(makeCommand(
        "solid.extrude",
        "Extrude a sketch's profile (its largest loop less the loops inside it) along the "
        "sketch plane's normal into a solid. one_sided goes distance_mm along the normal "
        "(against it when negative), symmetric distance_mm in all, half each way, and two_sided "
        "distance_mm along the normal and back_distance_mm against it. Lines and arcs stay "
        "exact. Adds a solid object; fails for an open profile.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"sketch","minimum":0,"description":"The sketch."},
                "distance_mm":{"type":"number"},
                "extent":{"type":"string","enum":["one_sided","symmetric","two_sided"],"default":"one_sided"},
                "back_distance_mm":{"type":"number","minimum":0,"default":0,"description":"two_sided: the distance against the normal."},
                "name":{"type":"string"}
            },
            "required":["id","distance_mm"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "sketch"));
            M2C_TRY(distance, optionalNumber(params, "distance_mm", 0.0, -1e6, 1e6));
            M2C_TRY(back, optionalNumber(params, "back_distance_mm", 0.0, 0.0, 1e6));
            M2C_TRY(extent, optionalString(params, "extent"));
            M2C_TRY(name, optionalString(params, "name"));
            const std::string mode = extent->value_or("one_sided");
            double forward = std::max(*distance, 0.0), backward = std::max(-*distance, 0.0);
            if (mode == "symmetric") {
                forward = backward = *distance / 2.0;
            } else if (mode == "two_sided") {
                forward = *distance;
                backward = *back;
            } else if (mode != "one_sided") {
                return fail(ErrorCode::InvalidArgument,
                            "extent must be one_sided, symmetric or two_sided");
            }
            M2C_TRY(solid, extrudeSketch(object->sketch()->sketch, forward, backward));
            const SolidSummary summary = solid->summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(object->name + " extrusion"),
                                         SolidData{std::move(*solid)},
                                         context.provenance({object->id})));
            return Json{{"id", *id}, {"summary", toJson(summary)}};
        }));

    out.push_back(makeCommand(
        "solid.revolve",
        "Revolve a sketch's profile (its largest loop less the loops inside it) by angle_deg "
        "about an axis in the sketch plane into a solid: the plane's u or v axis through its "
        "origin, or axis_point and axis_direction in sketch coordinates. Adds a solid object; "
        "fails for an open profile or one that crosses the axis.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"sketch","minimum":0,"description":"The sketch."},
                "axis":{"type":"string","enum":["u","v"],"description":"The sketch plane's u or v axis."},
                "axis_point":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},
                "axis_direction":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},
                "angle_deg":{"type":"number","exclusiveMinimum":0,"maximum":360,"default":360},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "sketch"));
            M2C_TRY(axis, optionalString(params, "axis"));
            M2C_TRY(angle, optionalNumber(params, "angle_deg", 360.0, 1e-9, 360.0));
            M2C_TRY(name, optionalString(params, "name"));
            const bool given = params.contains("axis_point") && params.contains("axis_direction");
            if (axis->has_value() == given ||
                params.contains("axis_point") != params.contains("axis_direction")) {
                return fail(ErrorCode::InvalidArgument,
                            "give axis, or axis_point with axis_direction");
            }
            Vec2d point, direction{1, 0};
            if (given) {
                const auto two = [&](const char* key) -> Result<Vec2d> {
                    const Json& j = params.at(key);
                    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) {
                        return fail(ErrorCode::InvalidArgument,
                                    std::string(key) + " must be two numbers");
                    }
                    return Vec2d{j[0].get<double>(), j[1].get<double>()};
                };
                M2C_TRY(p, two("axis_point"));
                M2C_TRY(d, two("axis_direction"));
                point = *p;
                direction = *d;
            } else if (**axis == "v") {
                direction = {0, 1};
            } else if (**axis != "u") {
                return fail(ErrorCode::InvalidArgument, "axis must be u or v");
            }
            M2C_TRY(
                solid,
                revolveSketch(
                    object->sketch()->sketch, point, direction, *angle * std::numbers::pi / 180.0));
            const SolidSummary summary = solid->summary();
            M2C_TRY(id,
                    context.document.add(name->value_or(object->name + " revolution"),
                                         SolidData{std::move(*solid)},
                                         context.provenance({object->id})));
            return Json{{"id", *id}, {"summary", toJson(summary)}};
        }));
}

} // namespace m2c::cmd

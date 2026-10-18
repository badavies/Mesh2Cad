// SPDX-License-Identifier: GPL-3.0-or-later
// Surface build (ARCHITECTURE.md section 6): a quad layout and its scan in, a B-rep solid out.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

#include "m2c/commands/JsonConvert.h"
#include "m2c/io/Step.h"
#include "m2c/pipeline/Surface.h"

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
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
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
        "from the irregular vertices, are merged into one face each. Adds a solid object; the quad "
        "layout and mesh are "
        "unchanged. Fails, adding nothing, unless the "
        "result is one valid closed solid.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0,"description":"The quad layout."},
                "mesh_id":{"type":"integer","minimum":0,"description":"The mesh to fit to; required unless fit is false."},
                "fit":{"type":"boolean","default":true,"description":"Fit the cage to the mesh first."},
                "iterations":{"type":"integer","minimum":1,"maximum":10,"default":3,"description":"Rounds of fitting."},
                "smoothing":{"type":"number","minimum":0,"maximum":10,"default":0.05,"description":"How strongly the cage keeps its own shape while fitting."},
                "max_samples":{"type":"integer","minimum":100,"maximum":10000000,"default":60000,"description":"Mesh vertices used for fitting (evenly thinned beyond this)."},
                "max_sample_distance_mm":{"type":"number","minimum":0,"default":0,"description":"Mesh points farther than this from the surface are left out of the fit (parts the cage does not cover); 0 = the mean cage edge length."},
                "isolation_level":{"type":"integer","minimum":1,"maximum":4,"default":2,"description":"Refinement around irregular vertices: higher gives smaller caps and more faces' pieces."},
                "merge":{"type":"boolean","default":true,"description":"Merge rectangles of cage quads into one face each (fewer faces, same surface)."},
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
            p.fit = *fit;
            p.fitting.iterations = static_cast<unsigned>(*iterations);
            p.fitting.smoothing = *smoothing;
            p.fitting.maxSamples = static_cast<std::size_t>(*samples);
            p.fitting.isolationLevel = static_cast<int>(*level);

            std::vector<ObjectId> inputs{layout->id};
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
                                     {"rms_before_round_mm", built->fit.rmsBeforeRoundMm},
                                     {"smoothing", built->smoothing},
                                     {"fits", built->fits}};
            }
            return result;
        }));

    out.push_back(makeCommand(
        "solid.info",
        "Describe a solid: face, edge and shell counts, whether it is closed and valid, its "
        "volume, area and bounds.",
        R"({
            "type":"object",
            "properties":{"id":{"type":"integer","minimum":0,"description":"The solid."}},
            "required":["id"],"additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "solid"));
            return Json{{"id", object->id},
                        {"name", object->name},
                        {"summary", toJson(object->solid()->solid.summary())}};
        }));

    out.push_back(makeCommand(
        "solid.export",
        "Write a solid to a STEP AP242 file in millimetres, then read the file back and check it "
        "holds one valid closed solid with the same volume (within 0.1%). Fails, writing "
        "nothing, if the solid is not valid.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0,"description":"The solid."},
                "path":{"type":"string","description":"The .step or .stp file to write."},
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
        }));
}

} // namespace m2c::cmd

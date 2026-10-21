// SPDX-License-Identifier: GPL-3.0-or-later
// Segmentation: split a mesh into primitive regions (planes, cylinders, spheres) and the organic
// rest, each stored as a selection for the fitting and surfacing tools.

#include <array>
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
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Primitive.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/PrimitiveFit.h"
#include "m2c/pipeline/Segment.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json regionJson(const SegmentRegion& r) {
    Json out = toJson(r.primitive);
    out["face_count"] = r.faces.size();
    out["area_mm2"] = r.areaMm2;
    out["rms_mm"] = r.rmsMm;
    out["max_mm"] = r.maxMm;
    return out;
}

Result<ObjectId> addSelection(CommandContext& context,
                              const MeshObject& mesh,
                              const std::vector<std::uint32_t>& faces,
                              std::string name) {
    MeshSelection selection(mesh.mesh->faceCount());
    for (const std::uint32_t f : faces) selection.set(f);
    return context.document.add(
        std::move(name),
        SelectionData{.meshId = mesh.id,
                      .faces = std::make_shared<const MeshSelection>(std::move(selection))},
        context.provenance({mesh.id}));
}

} // namespace

void addSegmentCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "mesh.segment",
        "Split a mesh into spheres, cylinders and planes (CGAL region growing, in that order, "
        "each region refit by least squares) and the organic rest. Each region becomes a "
        "selection; the result lists each region's kind, area, fit and primitive. A region is "
        "kept only when its area reaches min_area_mm2 and its RMS distance to the primitive is "
        "within max_distance_mm. Run it on an even mesh at working density (mesh.remesh): the "
        "normals of a raw scan's small triangles are too noisy for the angle limit. The mesh is "
        "unchanged; it must be manifold (mesh.repair). With create_primitives each region also "
        "becomes a primitive, fitted as primitive.fit fits its selection. covered_area_fraction "
        "(region area over the mesh's) and unclassified_face_count say how much of the part is "
        "analytic: high coverage suits primitive.trim, low coverage surface.build.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0},
                "max_distance_mm":{"type":"number","exclusiveMinimum":0,"default":0.1,"description":"Largest distance from a face to its region's primitive."},
                "max_angle_deg":{"type":"number","exclusiveMinimum":0,"maximum":90,"default":10,"description":"Largest angle between a face normal and the primitive's normal there."},
                "min_area_mm2":{"type":"number","minimum":0,"default":0,"description":"Smaller regions are left organic; 0 = 1% of the mesh's area."},
                "kinds":{"type":"array","items":{"type":"string","enum":["plane","cylinder","sphere"]},"default":["plane","cylinder"],"description":"The primitives to look for."},
                "create_primitives":{"type":"boolean","default":false,"description":"Also add each region's primitive, as primitive.fit on its selection would."}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params));
            SegmentParams p;
            M2C_TRY(distance, optionalNumber(params, "max_distance_mm", 0.1, 1e-9, 1e6));
            M2C_TRY(angle, optionalNumber(params, "max_angle_deg", 10.0, 1e-9, 90.0));
            M2C_TRY(area, optionalNumber(params, "min_area_mm2", 0.0, 0.0, 1e18));
            p.maxDistanceMm = *distance;
            p.maxAngleDeg = *angle;
            p.minAreaMm2 = *area;
            if (params.contains("kinds")) {
                const Json& kinds = params.at("kinds");
                if (!kinds.is_array() || kinds.empty()) {
                    return fail(ErrorCode::InvalidArgument,
                                "parameter 'kinds' must be a non-empty array");
                }
                p.planes = p.cylinders = p.spheres = false;
                for (const Json& k : kinds) {
                    const std::string kind = k.is_string() ? k.get<std::string>() : "";
                    if (kind == "plane") {
                        p.planes = true;
                    } else if (kind == "cylinder") {
                        p.cylinders = true;
                    } else if (kind == "sphere") {
                        p.spheres = true;
                    } else {
                        return fail(ErrorCode::InvalidArgument,
                                    "kinds must be plane, cylinder or sphere");
                    }
                }
            }
            M2C_TRY(createPrimitives, optionalBool(params, "create_primitives", false));
            M2C_TRY(result, segmentMesh(*mesh->mesh, p));
            double meshArea = 0.0;
            for (std::size_t f = 0; f < mesh->mesh->faceCount(); ++f)
                meshArea += faceArea(*mesh->mesh, f);
            double coveredArea = 0.0;
            Json regions = Json::array();
            std::array<std::size_t, 3> counts{};
            for (const SegmentRegion& r : result->regions) {
                const auto k = static_cast<std::size_t>(r.primitive.kind);
                std::string name = primitiveKindName(r.primitive.kind);
                name[0] = static_cast<char>(name[0] - 'a' + 'A');
                M2C_TRY(id,
                        addSelection(
                            context, *mesh, r.faces, name + " " + std::to_string(++counts.at(k))));
                Json j = regionJson(r);
                j["selection_id"] = *id;
                coveredArea += r.areaMm2;
                if (*createPrimitives) {
                    // As primitive.fit on the selection with the region's kind, with no residual
                    // limit: segmentation already kept the region.
                    const RegionFitParams fit{.kind = r.primitive.kind, .maxResidualMm = 1e18};
                    M2C_TRY(fitted, fitRegionPrimitive(*mesh->mesh, r.faces, fit));
                    M2C_TRY(primitive,
                            context.document.add(fittedName(fitted->surface.kind),
                                                 primitiveData(*fitted),
                                                 context.provenance({*id, mesh->id})));
                    j["primitive_id"] = *primitive;
                }
                regions.push_back(std::move(j));
            }
            Json report{{"id", mesh->id},
                        {"regions", std::move(regions)},
                        {"organic_face_count", result->organicFaces.size()},
                        {"unclassified_face_count", result->organicFaces.size()},
                        {"covered_area_fraction", meshArea > 0.0 ? coveredArea / meshArea : 0.0}};
            if (!result->organicFaces.empty()) {
                M2C_TRY(id, addSelection(context, *mesh, result->organicFaces, "Organic"));
                report["organic_selection_id"] = *id;
            }
            return report;
        }));
}

} // namespace m2c::cmd

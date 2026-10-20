// SPDX-License-Identifier: GPL-3.0-or-later
// Primitive fitting: an exact plane, cylinder, sphere or cone fitted to a mesh region
// (ARCHITECTURE.md section 7, "Primitive fitting").

#include <algorithm>
#include <cstdint>
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
            const std::vector<std::uint32_t> faces = sel->faces->indices();
            M2C_TRY(fit, fitRegion(*mesh->mesh, faces, p));
            FittedPrimitive fitted{.surface = fit->primitive,
                                   .rmsMm = fit->rmsMm,
                                   .maxMm = fit->maxMm,
                                   .p95Mm = fit->p95Mm};
            const std::vector<Vec3d> points = regionPoints(*mesh->mesh, faces);
            Aabb& box = fitted.extent;
            box = {.min = points.front(), .max = points.front()};
            for (const Vec3d& q : points) {
                box.min = {
                    std::min(box.min.x, q.x), std::min(box.min.y, q.y), std::min(box.min.z, q.z)};
                box.max = {
                    std::max(box.max.x, q.x), std::max(box.max.y, q.y), std::max(box.max.z, q.z)};
            }
            std::string name = primitiveKindName(fitted.surface.kind);
            name[0] = static_cast<char>(name[0] - 'a' + 'A');
            M2C_TRY(id,
                    context.document.add(name + " fit",
                                         primitiveData(fitted),
                                         context.provenance({sel->id, mesh->id})));
            Json report = toJson(fitted.surface);
            report["id"] = *id;
            report["rms_mm"] = fitted.rmsMm;
            report["max_mm"] = fitted.maxMm;
            report["p95_mm"] = fitted.p95Mm;
            report["extent"] = toJson(fitted.extent);
            return report;
        }));
}

} // namespace m2c::cmd

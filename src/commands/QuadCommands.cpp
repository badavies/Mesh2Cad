// SPDX-License-Identifier: GPL-3.0-or-later
// Quad layout (ARCHITECTURE.md section 6): a closed scan mesh in, a quad control cage out.

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Vec3.h"
#include "m2c/document/Document.h"
#include "m2c/pipeline/QuadRemesh.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json toJson(const CageStats& s) {
    Json degrees = Json::object();
    for (const auto& [degree, count] : s.faceDegrees) degrees[std::to_string(degree)] = count;
    Json valences = Json::object();
    for (const auto& [valence, count] : s.valences) valences[std::to_string(valence)] = count;
    return Json{{"vertex_count", s.vertexCount},
                {"face_count", s.faceCount},
                {"edge_count", s.edgeCount},
                {"closed", s.closed()},
                {"components", s.components},
                {"euler_characteristic", s.eulerCharacteristic()},
                {"extraordinary_vertices", s.extraordinaryVertices},
                {"face_degrees", std::move(degrees)},
                {"valences", std::move(valences)}};
}

Json toJson(const CageCleanup& c) {
    return Json{{"dropped_faces", c.droppedFaces},
                {"dropped_components", c.droppedComponents},
                {"dropped_component_faces", c.droppedComponentFaces},
                {"joined_components", c.joinedComponents},
                {"joined_component_faces", c.joinedComponentFaces},
                {"split_vertices", c.splitVertices},
                {"filled_loops", c.filledLoops},
                {"largest_loop", c.largestLoop},
                {"dissolved_vertices", c.dissolvedVertices},
                {"flipped", c.flipped}};
}

/// The guides parameter: polylines of [x, y, z] points in millimetres.
Result<std::vector<std::vector<Vec3d>>> guides(const Json& params) {
    std::vector<std::vector<Vec3d>> out;
    const auto it = params.find("guides");
    if (it == params.end()) return out;
    const auto bad = [] {
        return fail(ErrorCode::InvalidArgument,
                    "parameter 'guides' must be an array of polylines of [x, y, z] points");
    };
    if (!it->is_array()) return bad();
    for (const Json& line : *it) {
        if (!line.is_array()) return bad();
        std::vector<Vec3d>& points = out.emplace_back();
        for (const Json& p : line) {
            if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() ||
                !p[2].is_number()) {
                return bad();
            }
            const Vec3d v{
                .x = p[0].get<double>(), .y = p[1].get<double>(), .z = p[2].get<double>()};
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return bad();
            points.push_back(v);
        }
    }
    return out;
}

} // namespace

void addQuadCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "quad.remesh",
        "Make a quad control cage from a closed mesh with Instant Meshes, then close it: faces on "
        "non-manifold edges are dropped, open loops filled and faces pointed outward. Loose parts "
        "(such as the head of a thin post whose stem the remesher missed) are joined to the main "
        "part with a tube of quads, or left out and counted when they cannot be. Guides, polylines "
        "drawn on the mesh, lay out the patches by hand: cage edges run along them. Adds a "
        "quad_layout object; the mesh is unchanged.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","minimum":0},
                "target_faces":{"type":"integer","minimum":8,"maximum":1000000,"default":2000,"description":"Approximate number of quads in the cage."},
                "crease_angle_deg":{"type":"number","minimum":0,"maximum":180,"default":0,"description":"Align the quads to edges sharper than this; 0 = treat the surface as smooth."},
                "smooth_iterations":{"type":"integer","minimum":0,"maximum":10,"default":2,"description":"Rounds of smoothing that pull the cage vertices back onto the mesh."},
                "join_parts":{"type":"boolean","default":true,"description":"Join loose closed parts to the main one instead of leaving them out."},
                "guides":{"type":"array","default":[],"description":"Polylines on the mesh, in mm, that cage edges should follow (manual patch layout); each needs two or more points within a cage edge length of the surface.","items":{"type":"array","minItems":2,"items":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}}}},
                "name":{"type":"string"}
            },
            "required":["id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireMesh(context, params));
            M2C_TRY(target, optionalInteger(params, "target_faces", 2000, 8, 1000000));
            M2C_TRY(crease, optionalNumber(params, "crease_angle_deg", 0.0, 0.0, 180.0));
            M2C_TRY(smooth, optionalInteger(params, "smooth_iterations", 2, 0, 10));
            M2C_TRY(join, optionalBool(params, "join_parts", true));
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(lines, guides(params));
            QuadRemeshParams p;
            p.guides = std::move(*lines);
            p.targetFaces = static_cast<std::size_t>(*target);
            p.creaseAngleDeg = *crease;
            p.smoothIterations = static_cast<unsigned>(*smooth);
            CageCloseParams close;
            close.joinParts = *join;
            M2C_TRY(result, quadRemesh(*object->mesh, p, close));
            M2C_TRY(id,
                    context.document.add(name->value_or(object->name + " (quads)"),
                                         QuadLayoutData{result->cage},
                                         context.provenance({object->id})));
            return Json{{"id", *id},
                        {"mesh_id", object->id},
                        {"stats", toJson(result->stats)},
                        {"cleanup", toJson(result->cleanup)},
                        {"max_distance_mm", result->maxDistanceMm},
                        {"mean_distance_mm", result->meanDistanceMm}};
        }));
}

} // namespace m2c::cmd

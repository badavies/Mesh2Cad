// SPDX-License-Identifier: GPL-3.0-or-later
// Deviation analysis (ARCHITECTURE.md section 7): how far a mesh lies from a solid or a mesh.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Progress.h"
#include "m2c/core/TriMesh.h"
#include "m2c/document/Document.h"
#include "m2c/io/Report.h"
#include "m2c/pipeline/Deviation.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

Json toJson(const DeviationStats& s) {
    return Json{{"count", s.count},
                {"min_mm", s.minMm},
                {"max_mm", s.maxMm},
                {"mean_mm", s.meanMm},
                {"rms_mm", s.rmsMm},
                {"p95_abs_mm", s.p95AbsMm},
                {"tolerance_mm", s.toleranceMm},
                {"within_tolerance", s.withinTolerance}};
}

Json toJson(const DeviationHotspot& h) {
    return Json{{"sign", h.sign > 0 ? "+" : "-"},
                {"face_count", h.faces.size()},
                {"area_mm2", h.areaMm2},
                {"max_signed_mm", h.maxSignedMm},
                {"rms_mm", h.rmsMm},
                {"seed_face", h.seedFace},
                {"centroid", m2c::toJson(h.centroid)},
                {"bounds", m2c::toJson(h.bounds)}};
}

/// The largest `max` hotspots of a deviation of `mesh`.
std::vector<DeviationHotspot>
largestHotspots(const TriMesh& mesh, const DeviationField& field, const std::size_t max) {
    if (max == 0) return {};
    std::vector<DeviationHotspot> all = deviationHotspots(mesh, field);
    if (all.size() > max) all.resize(max);
    return all;
}

bool isCsvPath(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".csv";
}

} // namespace

void addDeviationCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "deviation.compute",
        "Signed distance from every vertex of a mesh to a solid (exact, onto its B-spline faces) "
        "or to another mesh, positive outside the reference, with statistics. Adds a deviation "
        "object that belongs to the mesh: it goes when the mesh or the reference is removed or "
        "changed. Also reports where it is out of tolerance: hotspots, the connected patches of "
        "faces beyond tolerance_mm on one side (+ outside: material the reference lacks; - "
        "inside: material it has that the scan lacks), largest first, each with its area, worst "
        "distance, centroid and bounds; select_hotspots makes each a selection, ready for "
        "primitive.fit.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh whose vertices are measured."},
                "reference_id":{"type":"integer","x-object":["solid","mesh"],"minimum":0,"description":"The solid or mesh to measure to."},
                "tolerance_mm":{"type":"number","exclusiveMinimum":0,"maximum":1000,"default":0.1,"description":"Distances within this count as in tolerance."},
                "max_hotspots":{"type":"integer","minimum":0,"maximum":1000,"default":10,"description":"How many hotspots to report, largest first; 0 for none."},
                "select_hotspots":{"type":"boolean","default":false,"description":"Make each reported hotspot a selection on the mesh (Deviation +1, Deviation -1, ...)."},
                "name":{"type":"string"}
            },
            "required":["id","reference_id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params, "id"));
            M2C_TRY(referenceId, requireId(params, "reference_id"));
            M2C_TRY(tolerance, optionalNumber(params, "tolerance_mm", 0.1, 1e-9, 1000.0));
            M2C_TRY(name, optionalString(params, "name"));
            M2C_TRY(maxHotspots, optionalInteger(params, "max_hotspots", 10, 0, 1000));
            M2C_TRY(selectHotspots, optionalBool(params, "select_hotspots", false));
            if (*referenceId == mesh->id) {
                return fail(ErrorCode::InvalidArgument, "a mesh cannot be its own reference");
            }
            const auto reference = context.document.find(*referenceId);
            if (!reference) {
                return fail(ErrorCode::NotFound,
                            "no object with id " + std::to_string(*referenceId));
            }
            M2C_TRY(go, checkpoint(context.progress, "measuring the scan against the reference"));
            Result<DeviationField> field =
                reference->solid() != nullptr
                    ? deviationToSolid(
                          *mesh->mesh, reference->solid()->solid, *tolerance, context.progress)
                : reference->mesh() != nullptr
                    ? deviationToMesh(
                          *mesh->mesh, *reference->mesh()->mesh, *tolerance, context.progress)
                    : fail(ErrorCode::InvalidArgument,
                           "object " + std::to_string(*referenceId) + " is a " + reference->kind() +
                               ", not a solid or a mesh");
            if (!field) return std::unexpected(field.error());
            const DeviationStats stats = field->stats();
            M2C_TRY(found, checkpoint(context.progress, "finding hotspots"));
            const std::vector<DeviationHotspot> hotspots =
                largestHotspots(*mesh->mesh, *field, static_cast<std::size_t>(*maxHotspots));
            M2C_TRY(id,
                    context.document.add(name->value_or(mesh->name + " (deviation)"),
                                         DeviationData{.meshId = mesh->id,
                                                       .referenceId = *referenceId,
                                                       .field = std::move(*field)},
                                         context.provenance({mesh->id, *referenceId})));
            Json listed = Json::array();
            int outside = 0, inside = 0;
            for (const DeviationHotspot& h : hotspots) {
                Json j = toJson(h);
                if (*selectHotspots) {
                    MeshSelection faces(mesh->mesh->faceCount());
                    for (const std::uint32_t f : h.faces) faces.set(f);
                    const std::string label = h.sign > 0 ? "+" + std::to_string(++outside)
                                                         : "-" + std::to_string(++inside);
                    M2C_TRY(selection,
                            context.document.add(
                                "Deviation " + label,
                                SelectionData{.meshId = mesh->id,
                                              .faces = std::make_shared<const MeshSelection>(
                                                  std::move(faces))},
                                context.provenance({mesh->id})));
                    j["selection_id"] = *selection;
                }
                listed.push_back(std::move(j));
            }
            return Json{{"id", *id}, {"stats", toJson(stats)}, {"hotspots", std::move(listed)}};
        }));

    out.push_back(makeCommand(
        "deviation.info",
        "Describe a deviation: the mesh it measures, and the statistics and largest hotspots "
        "deviation.compute returned.",
        R"({
            "type":"object",
            "properties":{"id":{"type":"integer","x-object":"deviation","minimum":0,"description":"The deviation."}},
            "required":["id"],"additionalProperties":false})",
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "deviation"));
            const DeviationData& d = *object->deviation();
            Json info{{"id", object->id},
                      {"name", object->name},
                      {"mesh_id", d.meshId},
                      {"reference_id", d.referenceId},
                      {"stats", toJson(d.field.stats())},
                      {"hotspots", Json::array()}};
            // A deviation goes with its mesh, so the mesh is there.
            if (const auto mesh = context.document.find(d.meshId); mesh && mesh->mesh()) {
                for (const DeviationHotspot& h : largestHotspots(*mesh->mesh()->mesh, d.field, 10))
                    info["hotspots"].push_back(toJson(h));
            }
            return info;
        }));

    Command report = makeCommand(
        "deviation.report",
        "Write a deviation's statistics and a histogram of its distances to a CSV file.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"deviation","minimum":0,"description":"The deviation."},
                "path":{"type":"string","x-file-types":["csv"],"description":"The .csv file to write."},
                "bins":{"type":"integer","minimum":2,"maximum":200,"default":20,"description":"Histogram bins from the smallest to the largest distance."}
            },
            "required":["id","path"],"additionalProperties":false})",
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(object, requireObject(context, params, "id", "deviation"));
            M2C_TRY(path, requireString(params, "path"));
            M2C_TRY(bins, optionalInteger(params, "bins", 20, 2, 200));
            const std::filesystem::path file(*path);
            if (!isCsvPath(file))
                return fail(ErrorCode::InvalidArgument, "the path must end in .csv");
            const DeviationField& field = object->deviation()->field;
            M2C_TRY(written, writeDeviationReport(field, static_cast<std::size_t>(*bins), file));
            return Json{{"id", object->id}, {"path", *path}, {"stats", toJson(field.stats())}};
        });
    report.writesFiles = true;
    out.push_back(std::move(report));
}

} // namespace m2c::cmd

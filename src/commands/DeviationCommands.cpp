// SPDX-License-Identifier: GPL-3.0-or-later
// Deviation analysis (ARCHITECTURE.md section 7): how far a mesh lies from a solid or a mesh.

#include <cctype>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
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
        "changed.",
        R"({
            "type":"object",
            "properties":{
                "id":{"type":"integer","x-object":"mesh","minimum":0,"description":"The mesh whose vertices are measured."},
                "reference_id":{"type":"integer","x-object":["solid","mesh"],"minimum":0,"description":"The solid or mesh to measure to."},
                "tolerance_mm":{"type":"number","exclusiveMinimum":0,"maximum":1000,"default":0.1,"description":"Distances within this count as in tolerance."},
                "name":{"type":"string"}
            },
            "required":["id","reference_id"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(mesh, requireMesh(context, params, "id"));
            M2C_TRY(referenceId, requireId(params, "reference_id"));
            M2C_TRY(tolerance, optionalNumber(params, "tolerance_mm", 0.1, 1e-9, 1000.0));
            M2C_TRY(name, optionalString(params, "name"));
            if (*referenceId == mesh->id) {
                return fail(ErrorCode::InvalidArgument, "a mesh cannot be its own reference");
            }
            const auto reference = context.document.find(*referenceId);
            if (!reference) {
                return fail(ErrorCode::NotFound,
                            "no object with id " + std::to_string(*referenceId));
            }
            report(context.progress, "measuring the scan against the reference");
            Result<DeviationField> field =
                reference->solid() != nullptr
                    ? deviationToSolid(*mesh->mesh, reference->solid()->solid, *tolerance)
                : reference->mesh() != nullptr
                    ? deviationToMesh(*mesh->mesh, *reference->mesh()->mesh, *tolerance)
                    : fail(ErrorCode::InvalidArgument,
                           "object " + std::to_string(*referenceId) + " is a " + reference->kind() +
                               ", not a solid or a mesh");
            if (!field) return std::unexpected(field.error());
            const DeviationStats stats = field->stats();
            M2C_TRY(id,
                    context.document.add(name->value_or(mesh->name + " (deviation)"),
                                         DeviationData{.meshId = mesh->id,
                                                       .referenceId = *referenceId,
                                                       .field = std::move(*field)},
                                         context.provenance({mesh->id, *referenceId})));
            return Json{{"id", *id}, {"stats", toJson(stats)}};
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

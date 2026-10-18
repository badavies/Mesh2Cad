// SPDX-License-Identifier: GPL-3.0-or-later
#include "detail/Params.h"

#include <cmath>

#include "m2c/commands/JsonConvert.h"
#include "m2c/core/MeshStats.h"

namespace m2c::cmd {

Command makeCommand(std::string name,
                    std::string summary,
                    const char* schema,
                    const bool mutates,
                    std::function<Result<Json>(CommandContext&, const Json&)> handler) {
    return Command{.name = std::move(name),
                   .summary = std::move(summary),
                   .paramsSchema = Json::parse(schema),
                   .mutatesDocument = mutates,
                   .handler = std::move(handler)};
}

Result<std::string> requireString(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' is required and must be a string");
    }
    return it->get<std::string>();
}

Result<std::optional<std::string>> optionalString(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    if (it == params.end()) return std::optional<std::string>{};
    if (!it->is_string()) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be a string");
    }
    return std::optional{it->get<std::string>()};
}

Result<double> optionalNumber(
    const Json& params, const std::string& key, double fallback, double min, double max) {
    const auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (!it->is_number()) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be a number");
    }
    const double v = it->get<double>();
    if (!std::isfinite(v) || v < min || v > max) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' must be between " + Json(min).dump() + " and " +
                        Json(max).dump());
    }
    return v;
}

Result<std::int64_t> optionalInteger(const Json& params,
                                     const std::string& key,
                                     std::int64_t fallback,
                                     const std::int64_t min,
                                     const std::int64_t max) {
    const auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (!it->is_number_integer()) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be an integer");
    }
    if (it->is_number_unsigned() &&
        it->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' is too large");
    }
    const auto v = it->get<std::int64_t>();
    if (v < min || v > max) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' must be between " + std::to_string(min) + " and " +
                        std::to_string(max));
    }
    return v;
}

Result<bool> optionalBool(const Json& params, const std::string& key, bool fallback) {
    const auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (!it->is_boolean()) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be true or false");
    }
    return it->get<bool>();
}

Result<ObjectId> requireId(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    // Parsed JSON stores non-negative integers as unsigned, but a Json built in code from an int
    // is signed, so accept both.
    const bool isId =
        it != params.end() &&
        (it->is_number_unsigned() || (it->is_number_integer() && it->get<std::int64_t>() >= 0));
    if (!isId) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' is required and must be a non-negative integer");
    }
    return it->get<ObjectId>();
}

Result<std::optional<ObjectId>> optionalId(const Json& params, const std::string& key) {
    if (!params.contains(key) || params.at(key).is_null()) return std::optional<ObjectId>{};
    M2C_TRY(id, requireId(params, key));
    return std::optional{*id};
}

Result<Vec3d> requireVec3(const Json& params, const std::string& key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_array() || it->size() != 3 || !(*it)[0].is_number() ||
        !(*it)[1].is_number() || !(*it)[2].is_number()) {
        return fail(ErrorCode::InvalidArgument,
                    "parameter '" + key + "' is required and must be an array of three numbers");
    }
    const Vec3d v{
        .x = (*it)[0].get<double>(), .y = (*it)[1].get<double>(), .z = (*it)[2].get<double>()};
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return fail(ErrorCode::InvalidArgument, "parameter '" + key + "' must be finite");
    }
    return v;
}

Result<MeshObject>
requireMesh(const CommandContext& context, const Json& params, const std::string& key) {
    M2C_TRY(id, requireId(params, key));
    const auto object = context.document.find(*id);
    if (!object) return fail(ErrorCode::NotFound, "no object with id " + std::to_string(*id));
    if (object->mesh() == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "object " + std::to_string(*id) + " is a " + object->kind() + ", not a mesh");
    }
    return MeshObject{.id = object->id, .name = object->name, .mesh = object->mesh()->mesh};
}

Result<DocumentObject> requireObject(const CommandContext& context,
                                     const Json& params,
                                     const std::string& key,
                                     const std::string_view kind) {
    M2C_TRY(id, requireId(params, key));
    auto object = context.document.find(*id);
    if (!object) return fail(ErrorCode::NotFound, "no object with id " + std::to_string(*id));
    if (object->kind() != kind) {
        return fail(ErrorCode::InvalidArgument,
                    "object " + std::to_string(*id) + " is a " + object->kind() + ", not a " +
                        std::string(kind));
    }
    return std::move(*object);
}

Result<SelectionRef> requireSelection(const CommandContext& context,
                                      const Json& params,
                                      const std::string& key,
                                      const std::optional<ObjectId> meshId) {
    M2C_TRY(id, requireId(params, key));
    const auto object = context.document.find(*id);
    if (!object) return fail(ErrorCode::NotFound, "no object with id " + std::to_string(*id));
    const SelectionData* s = object->selection();
    if (s == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "object " + std::to_string(*id) + " is a " + object->kind() +
                        ", not a selection");
    }
    if (meshId && s->meshId != *meshId) {
        return fail(ErrorCode::InvalidArgument,
                    "selection " + std::to_string(*id) + " belongs to mesh " +
                        std::to_string(s->meshId) + ", not mesh " + std::to_string(*meshId));
    }
    return SelectionRef{.id = *id, .meshId = s->meshId, .faces = s->faces};
}

Result<std::optional<SelectionRef>> optionalSelection(const CommandContext& context,
                                                      const Json& params,
                                                      const std::string& key,
                                                      const std::optional<ObjectId> meshId) {
    if (!params.contains(key) || params.at(key).is_null()) {
        return std::optional<SelectionRef>{};
    }
    M2C_TRY(sel, requireSelection(context, params, key, meshId));
    return std::optional{std::move(*sel)};
}

Result<void> commitMesh(CommandContext& context,
                        const MeshObject& object,
                        TriMesh mesh,
                        std::vector<ObjectId> inputs) {
    inputs.insert(inputs.begin(), object.id);
    return context.document.replaceMesh(object.id,
                                        std::make_shared<const TriMesh>(std::move(mesh)),
                                        context.provenance(std::move(inputs)));
}

Json describe(const DocumentObject& object) {
    Json j{{"id", object.id},
           {"name", object.name},
           {"kind", object.kind()},
           {"visible", object.visible}};
    if (const MeshData* m = object.mesh()) {
        j["vertex_count"] = m->mesh->vertexCount();
        j["face_count"] = m->mesh->faceCount();
    } else if (const SelectionData* s = object.selection()) {
        j["mesh_id"] = s->meshId;
        j["face_count"] = s->faces->count();
    } else if (const MeasurementData* d = object.measurement()) {
        j["measurement"] = toJson(d->measurement);
    } else if (const QuadLayoutData* q = object.quadLayout()) {
        j["vertex_count"] = q->cage.vertexCount();
        j["face_count"] = q->cage.faceCount();
    } else if (const SolidData* solid = object.solid()) {
        const SolidSummary& summary = solid->solid.summary();
        j["face_count"] = summary.faceCount;
        j["valid"] = summary.valid;
        j["volume_mm3"] = summary.volumeMm3;
    } else if (const DeviationData* dev = object.deviation()) {
        j["mesh_id"] = dev->meshId;
        j["reference_id"] = dev->referenceId;
        j["rms_mm"] = dev->field.stats().rmsMm;
    }
    if (!object.provenance.command.empty()) {
        j["made_by"] = object.provenance.command;
    }
    return j;
}

Json describeMesh(const Document& document, ObjectId id) {
    const auto object = document.find(id);
    if (!object || object->mesh() == nullptr) return Json{{"id", id}};
    return Json{{"id", id},
                {"name", object->name},
                {"stats", toJson(computeStats(*object->mesh()->mesh))},
                {"alignment", toJson(object->mesh()->alignment)}};
}

} // namespace m2c::cmd

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private helpers for command handlers: parameter parsing and document lookups. Every helper
// returns a Result, so handlers report bad input as InvalidArgument or NotFound, never by throwing.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Primitive.h"
#include "m2c/pipeline/PrimitiveFit.h"

/// Evaluates a Result-returning expression and returns its error from the enclosing handler.
#define M2C_TRY(var, expr)                                                                         \
    auto var = (expr);                                                                             \
    if (!var) return std::unexpected(var.error())

namespace m2c::cmd {

/// Builds a command from a schema given as JSON text.
[[nodiscard]] Command
makeCommand(std::string name,
            std::string summary,
            const char* schema,
            bool mutates,
            std::function<Result<Json>(CommandContext&, const Json&)> handler);

[[nodiscard]] Result<std::string> requireString(const Json& params, const std::string& key);
[[nodiscard]] Result<std::optional<std::string>> optionalString(const Json& params,
                                                                const std::string& key);
[[nodiscard]] Result<double>
optionalNumber(const Json& params, const std::string& key, double fallback, double min, double max);
[[nodiscard]] Result<std::int64_t> optionalInteger(const Json& params,
                                                   const std::string& key,
                                                   std::int64_t fallback,
                                                   std::int64_t min,
                                                   std::int64_t max);
[[nodiscard]] Result<bool> optionalBool(const Json& params, const std::string& key, bool fallback);
[[nodiscard]] Result<ObjectId> requireId(const Json& params, const std::string& key);
[[nodiscard]] Result<std::optional<ObjectId>> optionalId(const Json& params,
                                                         const std::string& key);
[[nodiscard]] Result<Vec3d> requireVec3(const Json& params, const std::string& key);

/// The mesh object named by params[key].
[[nodiscard]] Result<MeshObject>
requireMesh(const CommandContext& context, const Json& params, const std::string& key = "id");

/// The object named by params[key], which must be of the given kind ("quad_layout", "solid", ...).
[[nodiscard]] Result<DocumentObject> requireObject(const CommandContext& context,
                                                   const Json& params,
                                                   const std::string& key,
                                                   std::string_view kind);

struct SelectionRef {
    ObjectId id{0};
    ObjectId meshId{0};
    std::shared_ptr<const MeshSelection> faces;
};

/// A primitive object's data, with its display mesh.
[[nodiscard]] PrimitiveData primitiveData(const FittedPrimitive& fit);

/// A fit to a region's points as primitive.fit records it: the fit, its residuals and the
/// points' bounding box. The points must not be empty.
[[nodiscard]] FittedPrimitive fittedPrimitive(const PrimitiveFit& fit,
                                              const std::vector<Vec3d>& points);

/// A primitive fitted to faces of a mesh as primitive.fit records it: the fit, its residuals and
/// the region's bounding box.
[[nodiscard]] Result<FittedPrimitive> fitRegionPrimitive(const TriMesh& mesh,
                                                         const std::vector<std::uint32_t>& faces,
                                                         const RegionFitParams& params);

/// The name primitive.fit gives a fitted primitive: "Plane fit", "Cylinder fit", ...
[[nodiscard]] std::string fittedName(PrimitiveKind kind);

/// The selection object named by params[key]. With `meshId`, it must belong to that mesh.
[[nodiscard]] Result<SelectionRef> requireSelection(const CommandContext& context,
                                                    const Json& params,
                                                    const std::string& key,
                                                    std::optional<ObjectId> meshId = std::nullopt);
[[nodiscard]] Result<std::optional<SelectionRef>>
optionalSelection(const CommandContext& context,
                  const Json& params,
                  const std::string& key,
                  std::optional<ObjectId> meshId = std::nullopt);

/// Replaces a mesh object's mesh as an edit in place, recording the command as provenance.
[[nodiscard]] Result<void> commitMesh(CommandContext& context,
                                      const MeshObject& object,
                                      TriMesh mesh,
                                      std::vector<ObjectId> inputs = {});

/// Plain-data description of any object, as listed by doc.list.
[[nodiscard]] Json describe(const DocumentObject& object);

/// Mesh description with full statistics (mesh.info and tool results).
[[nodiscard]] Json describeMesh(const Document& document, ObjectId id);

} // namespace m2c::cmd

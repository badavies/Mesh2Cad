// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct ProjectMesh {
    std::shared_ptr<const TriMesh> mesh;
    Transform3d alignment{};
};

struct ProjectSelection {
    std::uint64_t meshId{0};
    std::shared_ptr<const MeshSelection> faces;
};

struct ProjectDeviation {
    std::uint64_t meshId{0};
    std::uint64_t referenceId{0};
    DeviationField field;
};

/// Plain data of one saved object. The I/O module cannot see the document (ARCHITECTURE.md
/// section 4), so the command engine converts between these and document objects.
struct ProjectObject {
    std::uint64_t id{0};
    std::string name;
    bool visible{true};
    std::variant<ProjectMesh,
                 ProjectSelection,
                 Measurement,
                 QuadLayout,
                 Solid,
                 ProjectDeviation,
                 FittedPrimitive>
        data;
    std::string madeBy;   ///< Provenance: command name.
    std::string madeWith; ///< Provenance: parameters as JSON text.
    std::vector<std::uint64_t> inputs;
};

struct ProjectData {
    std::uint64_t nextId{1};
    std::vector<ProjectObject> objects;
};

/// Current project format version. Reading an older version runs its migration; a newer one is
/// refused with ErrorCode::Unsupported.
/// Version 2 added quad layouts, solids and deviations; version 1 files hold a subset of it.
/// Version 3 lets a solid's face cover several cage quads (merged faces with their own breaks).
/// Version 4 added fitted primitives. Version 5 added solids with no B-spline faces (trimmed
/// primitives), stored as BREP alone.
inline constexpr int kProjectFormatVersion = 5;

/// Writes a project file: a zip holding manifest.json, meshes as binary PLY (double precision),
/// selections as bitsets, quad layouts and deviations as little-endian arrays, solids as
/// Open CASCADE binary BREP with their B-spline faces, and primitives in the manifest
/// (ARCHITECTURE.md section 5). The file is
/// replaced atomically: it is written next to the target and renamed into place.
[[nodiscard]] Result<void> writeProject(const ProjectData& project,
                                        const std::filesystem::path& path);

[[nodiscard]] Result<ProjectData> readProject(const std::filesystem::path& path);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshSelection.h"
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

/// Plain data of one saved object. The I/O module cannot see the document (ARCHITECTURE.md
/// section 4), so the command engine converts between these and document objects.
struct ProjectObject {
    std::uint64_t id{0};
    std::string name;
    bool visible{true};
    std::variant<ProjectMesh, ProjectSelection, Measurement> data;
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
inline constexpr int kProjectFormatVersion = 1;

/// Writes a project file: a zip holding manifest.json, meshes as binary PLY (double precision) and
/// selections as bitsets (ARCHITECTURE.md section 5). The file is replaced atomically: it is
/// written next to the target and renamed into place.
[[nodiscard]] Result<void> writeProject(const ProjectData& project,
                                        const std::filesystem::path& path);

[[nodiscard]] Result<ProjectData> readProject(const std::filesystem::path& path);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// glTF 2.0 files for Blender (ADR-005, ADR-037): our own writer from a triangle mesh.

#include <iosfwd>
#include <string>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct GltfOptions {
    bool binary{true};           ///< A .glb; false writes a .gltf with the buffer embedded.
    std::string name{"mesh"};    ///< The node's and the mesh's name.
    double creaseAngleDeg{30.0}; ///< Normals are split where neighbouring triangles turn more.
};

/// Writes the mesh as one node with one mesh and a plain grey material. glTF is in metres with Y
/// up, so millimetres are scaled by 0.001 and Z up is turned to Y up (x, y, z becomes x, z, -y),
/// which Blender's importer turns back: the part keeps its size and orientation there. Each
/// vertex gets a normal per corner, the area-weighted mean of the triangles around it that turn
/// less than creaseAngleDeg from the corner's own, so curved faces shade smoothly and sharp edges
/// stay sharp; corners with equal normals share a vertex. Positions and normals are float. Fails
/// with InvalidArgument for a crease angle outside 0 to 180 degrees and Unsupported past 2^32-1
/// vertices.
[[nodiscard]] Result<void>
writeGltf(const TriMesh& mesh, std::ostream& out, const GltfOptions& options = {});

} // namespace m2c

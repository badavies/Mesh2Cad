// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Deviation.h"

#include <expected>
#include <utility>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

Result<DeviationField>
deviationToSolid(const TriMesh& mesh, const Solid& reference, const double toleranceMm) {
    auto distances = occt::signedDistances(reference, mesh.positions());
    if (!distances) return std::unexpected(distances.error());
    return DeviationField::create(std::move(*distances), toleranceMm);
}

Result<DeviationField>
deviationToMesh(const TriMesh& mesh, const TriMesh& reference, const double toleranceMm) {
    const SpatialIndex& index = reference.spatialIndex();
    std::vector<double> distances;
    distances.reserve(mesh.vertexCount());
    for (const Vec3d& p : mesh.positions()) {
        const auto [face, distance, point] = index.closestPoint(p);
        // The side of the face holding the closest point; near an edge or a vertex the offset
        // lies in the normal cone there, so either neighbour usually agrees.
        const bool outside = dot(p - point, faceNormal(reference, face)) >= 0.0;
        distances.push_back(outside ? distance : -distance);
    }
    return DeviationField::create(std::move(distances), toleranceMm);
}

} // namespace m2c

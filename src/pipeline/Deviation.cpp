// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Deviation.h"

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/SpatialIndex.h"

namespace m2c {

Result<DeviationField>
deviationToSolid(const TriMesh& mesh, const Solid& reference, double toleranceMm) {
    auto distances = occt::signedDistances(reference, mesh.positions());
    if (!distances) return std::unexpected(distances.error());
    return DeviationField::create(std::move(*distances), toleranceMm);
}

Result<DeviationField>
deviationToMesh(const TriMesh& mesh, const TriMesh& reference, double toleranceMm) {
    const SpatialIndex& index = reference.spatialIndex();
    std::vector<double> distances;
    distances.reserve(mesh.vertexCount());
    for (const Vec3d& p : mesh.positions()) {
        const ClosestPoint near = index.closestPoint(p);
        // The side of the face holding the closest point; near an edge or a vertex the offset
        // lies in the normal cone there, so either neighbour usually agrees.
        const bool outside = dot(p - near.point, faceNormal(reference, near.face)) >= 0.0;
        distances.push_back(outside ? near.distance : -near.distance);
    }
    return DeviationField::create(std::move(distances), toleranceMm);
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#include <vector>

#include <CGAL/Polygon_mesh_processing/detect_features.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace PMP = CGAL::Polygon_mesh_processing;

Result<TriMesh> remesh(const TriMesh& input, const RemeshParams& params) {
    if (!(params.targetEdgeMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "target edge length must be positive");
    }
    return guarded("remeshing", [&] -> Result<TriMesh> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        using Edge = SurfaceMesh::Edge_index;
        auto sharp = mesh.add_property_map<Edge, bool>("e:m2c_sharp", false).first;
        if (params.sharpAngleDeg > 0.0) {
            PMP::detect_sharp_edges(mesh, params.sharpAngleDeg, sharp);
            // Protected edges must already be short enough, or remeshing cannot keep them.
            std::vector<Edge> featureEdges;
            for (const Edge e : mesh.edges()) {
                if (sharp[e]) featureEdges.push_back(e);
            }
            PMP::split_long_edges(featureEdges,
                                  params.targetEdgeMm,
                                  mesh,
                                  CGAL::parameters::edge_is_constrained_map(sharp));
        }
        PMP::isotropic_remeshing(faces(mesh),
                                 params.targetEdgeMm,
                                 mesh,
                                 CGAL::parameters::number_of_iterations(params.iterations)
                                     .edge_is_constrained_map(sharp)
                                     .protect_constraints(params.sharpAngleDeg > 0.0));
        mesh.collect_garbage();
        return fromSurfaceMesh(mesh);
    });
}

} // namespace m2c::cgal

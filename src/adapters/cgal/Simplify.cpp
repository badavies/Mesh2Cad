// SPDX-License-Identifier: GPL-3.0-or-later
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Constrained_placement.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/GarlandHeckbert_plane_policies.h>
#include <CGAL/Surface_mesh_simplification/edge_collapse.h>

#include "m2c/adapters/cgal/MeshTools.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace SMS = CGAL::Surface_mesh_simplification;

namespace {

// Stops when the face count falls below the target. CGAL's Face_count_stop_predicate counts the
// faces by walking the whole face range on every collapse, which makes simplification quadratic
// (a 1.3M-face scan never finished); Surface_mesh keeps the live count, so read that instead.
struct FaceCountStop {
    std::size_t targetFaces;

    template <typename Cost, typename Profile>
    bool operator()(const Cost& /*cost*/,
                    const Profile& profile,
                    std::size_t /*initialEdges*/,
                    std::size_t /*currentEdges*/) const {
        return profile.surface_mesh().number_of_faces() < targetFaces;
    }
};

} // namespace

Result<TriMesh> simplify(const TriMesh& input, const SimplifyParams& params) {
    if (params.targetFaces == 0) {
        return fail(ErrorCode::InvalidArgument, "target face count must be at least 1");
    }
    if (params.targetFaces >= input.faceCount()) return input;
    return guarded("simplification", [&]() -> Result<TriMesh> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        using Edge = SurfaceMesh::Edge_index;

        auto constrained = mesh.add_property_map<Edge, bool>("e:m2c_constrained", false).first;
        if (params.preserveBorders) {
            for (const Edge e : mesh.edges()) {
                if (mesh.is_border(e)) constrained[e] = true;
            }
        }

        using Policies = SMS::GarlandHeckbert_plane_policies<SurfaceMesh, Kernel>;
        Policies policies(mesh);
        const SMS::Constrained_placement<Policies::Get_placement, decltype(constrained)> placement(
            constrained, policies.get_placement());
        const FaceCountStop stop{params.targetFaces};
        SMS::edge_collapse(mesh,
                           stop,
                           CGAL::parameters::get_cost(policies.get_cost())
                               .get_placement(placement)
                               .edge_is_constrained_map(constrained));
        mesh.collect_garbage();
        return fromSurfaceMesh(mesh);
    });
}

} // namespace m2c::cgal

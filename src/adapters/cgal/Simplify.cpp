// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstddef>

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h> // for the bounded placement
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Bounded_distance_placement.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Constrained_placement.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/GarlandHeckbert_plane_policies.h>
#include <CGAL/Surface_mesh_simplification/edge_collapse.h>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace SMS = CGAL::Surface_mesh_simplification;

namespace {

// Stops when the face count falls below the target, or when the cheapest collapse left costs
// more than the error limit. CGAL's Face_count_stop_predicate counts the faces by walking the
// whole face range on every collapse, which makes simplification quadratic (a 1.3M-face scan
// never finished); Surface_mesh keeps the live count, so read that instead. The Garland-Heckbert
// cost is a sum of squared distances to the original planes, so it is compared with the squared
// limit. Collapses come cheapest first, so when this one costs too much, every one left does.
struct SimplifyStop {
    std::size_t targetFaces;
    double maxSquaredError; // 0 = no limit

    template <typename Cost, typename Profile>
    bool operator()(const Cost& cost,
                    const Profile& profile,
                    std::size_t /*initialEdges*/,
                    std::size_t /*currentEdges*/) const {
        if (profile.surface_mesh().number_of_faces() < targetFaces) return true;
        return maxSquaredError > 0.0 && CGAL::to_double(cost) > maxSquaredError;
    }
};

} // namespace

Result<TriMesh> simplify(const TriMesh& input, const SimplifyParams& params) {
    if (params.maxErrorMm < 0.0 || !std::isfinite(params.maxErrorMm)) {
        return fail(ErrorCode::InvalidArgument, "maximum error must be zero or positive");
    }
    if (params.targetFaces == 0 && params.maxErrorMm == 0.0) {
        return fail(ErrorCode::InvalidArgument,
                    "give a target face count of at least 1 or a maximum error");
    }
    if (params.targetFaces >= input.faceCount()) return input;
    return guarded("simplification", [&] -> Result<TriMesh> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        using Edge = SurfaceMesh::Edge_index;

        auto constrained = mesh.add_property_map<Edge, bool>("e:m2c_constrained", false).first;
        if (params.preserveBorders) {
            for (const Edge e : mesh.edges()) {
                if (mesh.is_border(e)) constrained[e] = true;
            }
        }

        using Policies = SMS::GarlandHeckbert_plane_policies<SurfaceMesh, Kernel>;
        const Policies policies(mesh);
        const SimplifyStop stop{.targetFaces = params.targetFaces,
                                .maxSquaredError = params.maxErrorMm * params.maxErrorMm};
        const auto collapse = [&]<typename BasePlacement>(const BasePlacement& basePlacement) {
            const SMS::Constrained_placement<BasePlacement, decltype(constrained)> placement(
                constrained, basePlacement);
            SMS::edge_collapse(mesh,
                               stop,
                               CGAL::parameters::get_cost(policies.get_cost())
                                   .get_placement(placement)
                                   .edge_is_constrained_map(constrained));
        };
        if (params.maxErrorMm > 0.0) {
            // The cost limit alone does not bound how far a moved vertex ends up from the input
            // surface; CGAL's bounded placement refuses any collapse that would put it further.
            collapse(SMS::Bounded_distance_placement<Policies::Get_placement, Kernel>(
                params.maxErrorMm, policies.get_placement()));
        } else {
            collapse(policies.get_placement());
        }
        mesh.collect_garbage();
        return fromSurfaceMesh(mesh);
    });
}

} // namespace m2c::cgal

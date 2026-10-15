// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <vector>

#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>

#include "m2c/adapters/cgal/MeshTools.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace PMP = CGAL::Polygon_mesh_processing;

Result<RepairResult> repair(const TriMesh& input, const RepairParams& params) {
    return guarded("mesh repair", [&]() -> Result<RepairResult> {
        // Work on the polygon soup: it can represent any broken input, and CGAL's soup repair and
        // orientation are the most robust way back to a valid mesh.
        std::vector<Point> points;
        points.reserve(input.vertexCount());
        for (const Vec3d& p : input.positions()) points.emplace_back(p.x, p.y, p.z);
        std::vector<std::vector<std::size_t>> polygons;
        polygons.reserve(input.faceCount());
        for (std::size_t f = 0; f < input.faceCount(); ++f) {
            const auto [a, b, c] = input.face(f);
            polygons.push_back({a, b, c});
        }

        PMP::repair_polygon_soup(points, polygons);
        const std::size_t afterSoup = polygons.size();
        // Returns false if some points had to be duplicated to make the soup orientable.
        const std::size_t pointsBefore = points.size();
        const bool orientable = PMP::orient_polygon_soup(points, polygons);
        if (polygons.empty()) {
            return fail(ErrorCode::InvalidArgument, "repair removed every face (all degenerate)");
        }

        SurfaceMesh mesh;
        PMP::polygon_soup_to_polygon_mesh(points, polygons, mesh);
        PMP::stitch_borders(mesh);
        PMP::remove_degenerate_faces(mesh);
        std::vector<std::vector<SurfaceMesh::Vertex_index>> duplicated;
        const std::size_t split = PMP::duplicate_non_manifold_vertices(
            mesh, CGAL::parameters::output_iterator(std::back_inserter(duplicated)));

        bool flipped = !orientable;
        if (CGAL::is_closed(mesh)) {
            const bool outward = PMP::is_outward_oriented(mesh);
            PMP::orient_to_bound_a_volume(mesh);
            flipped = flipped || !outward;
        }

        if (params.removeSelfIntersections) {
            PMP::experimental::remove_self_intersections(mesh);
        }
        CGAL::Polygon_mesh_processing::remove_isolated_vertices(mesh);
        mesh.collect_garbage();

        auto out = fromSurfaceMesh(mesh);
        if (!out) return std::unexpected(out.error());
        RepairResult result{std::move(*out)};
        result.removedFaces = input.faceCount() > afterSoup ? input.faceCount() - afterSoup : 0;
        if (result.mesh.faceCount() < afterSoup) {
            result.removedFaces += afterSoup - result.mesh.faceCount();
        }
        result.splitVertices = split + (points.size() - pointsBefore);
        result.orientationFixed = flipped;
        result.selfIntersecting = PMP::does_self_intersect(mesh);
        return result;
    });
}

} // namespace m2c::cgal

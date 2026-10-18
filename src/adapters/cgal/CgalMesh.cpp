// SPDX-License-Identifier: GPL-3.0-or-later
#include "CgalMesh.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::cgal {

SurfaceMesh toSurfaceMesh(const TriMesh& mesh) {
    SurfaceMesh out;
    out.reserve(static_cast<SurfaceMesh::size_type>(mesh.vertexCount()),
                static_cast<SurfaceMesh::size_type>(mesh.faceCount() * 3 / 2),
                static_cast<SurfaceMesh::size_type>(mesh.faceCount()));
    std::vector<Point> points;
    points.reserve(mesh.vertexCount());
    for (const auto& [x, y, z] : mesh.positions()) points.emplace_back(x, y, z);
    std::vector<std::array<std::size_t, 3>> faces;
    faces.reserve(mesh.faceCount());
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        faces.push_back({a, b, c});
    }
    // The conversion skips faces it cannot add to a manifold mesh. Orienting the soup first
    // splits non-manifold vertices and flips faces into a consistent orientation, so no face is
    // lost on a mesh that was not repaired.
    if (!CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(faces)) {
        CGAL::Polygon_mesh_processing::orient_polygon_soup(points, faces);
    }
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(points, faces, out);
    return out;
}

Result<TriMesh> fromSurfaceMesh(const SurfaceMesh& mesh) {
    std::vector<Vec3d> positions;
    positions.reserve(mesh.number_of_vertices());
    std::vector<std::uint32_t> index(mesh.num_vertices(), 0);
    for (const auto v : mesh.vertices()) {
        index[v.idx()] = static_cast<std::uint32_t>(positions.size());
        const Point& p = mesh.point(v);
        positions.push_back({.x = p.x(), .y = p.y(), .z = p.z()});
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(mesh.number_of_faces()) * 3);
    for (const auto f : mesh.faces()) {
        const auto h = mesh.halfedge(f);
        if (mesh.next(mesh.next(mesh.next(h))) != h) continue; // not a triangle
        indices.push_back(index[mesh.target(h).idx()]);
        indices.push_back(index[mesh.target(mesh.next(h)).idx()]);
        indices.push_back(index[mesh.target(mesh.next(mesh.next(h))).idx()]);
    }
    return TriMesh::create(std::move(positions), std::move(indices));
}

} // namespace m2c::cgal

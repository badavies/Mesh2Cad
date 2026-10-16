// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <iterator>
#include <vector>

#include <CGAL/Polygon_mesh_processing/fair.h>
#include <CGAL/Polygon_mesh_processing/refine.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <CGAL/boost/graph/Euler_operations.h>
// Newer CGAL (6.2.1 from vcpkg) moved extract_boundary_cycles() from Polygon_mesh_processing to
// the BGL border header and deprecated the old one; the downloaded CGAL 6.0.1 has only the old one.
#if __has_include(<CGAL/boost/graph/border.h>)
#include <CGAL/boost/graph/border.h>
#define M2C_CGAL_BORDER_NS CGAL
#else
#include <CGAL/Polygon_mesh_processing/border.h>
#define M2C_CGAL_BORDER_NS CGAL::Polygon_mesh_processing
#endif

#include "m2c/adapters/cgal/MeshTools.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace PMP = CGAL::Polygon_mesh_processing;

namespace {

using Vector = Kernel::Vector_3;

Vector unit(const Vector& v) {
    const double len = std::sqrt(CGAL::to_double(v.squared_length()));
    return len > 0.0 ? v / len : Vector(0, 0, 0);
}

// How the surface around a hole lies relative to the hole's plane, averaged along the rim and
// weighted by edge length: 1 where the surface carries on across the hole (a hole in a wall), 0
// where it turns away at a right angle (the open end of a tube).
double rimAlignment(const SurfaceMesh& mesh, SurfaceMesh::Halfedge_index start) {
    Vector holeNormal(0, 0, 0); // Newell's method: the area normal of the boundary loop.
    auto h = start;
    do {
        const Point& p = mesh.point(mesh.source(h));
        const Point& q = mesh.point(mesh.target(h));
        holeNormal = holeNormal + CGAL::cross_product(p - CGAL::ORIGIN, q - CGAL::ORIGIN);
        h = mesh.next(h);
    } while (h != start);
    holeNormal = unit(holeNormal);
    if (holeNormal == Vector(0, 0, 0)) return 1.0;

    double weighted = 0.0;
    double total = 0.0;
    do {
        const auto inside = mesh.opposite(h);
        const Point& a = mesh.point(mesh.source(inside));
        const Point& b = mesh.point(mesh.target(inside));
        const Point& c = mesh.point(mesh.target(mesh.next(inside)));
        const double len = std::sqrt(CGAL::to_double(CGAL::squared_distance(a, b)));
        weighted += len * std::abs(CGAL::to_double(unit(CGAL::normal(a, b, c)) * holeNormal));
        total += len;
        h = mesh.next(h);
    } while (h != start);
    return total > 0.0 ? weighted / total : 1.0;
}

// Last resort for a hole CGAL cannot triangulate. Its triangulation never adds an edge that already
// exists, so a rim whose vertices are already joined across the surface (common where a scan is
// thin) can have no valid triangulation at all. A fan from a new vertex at the rim's centroid adds
// no edge between rim vertices. The fan is then refined and, if asked, faired like CGAL's patches.
std::vector<SurfaceMesh::Face_index> fanFill(SurfaceMesh& mesh,
                                             SurfaceMesh::Halfedge_index border,
                                             bool refineAndFair,
                                             unsigned continuity) {
    Vector sum(0, 0, 0);
    std::size_t count = 0;
    for (const auto h : CGAL::halfedges_around_face(border, mesh)) {
        sum = sum + (mesh.point(mesh.target(h)) - CGAL::ORIGIN);
        ++count;
    }
    CGAL::Euler::fill_hole(border, mesh);
    const auto spoke = CGAL::Euler::add_center_vertex(border, mesh);
    const auto center = mesh.target(spoke);
    mesh.point(center) = CGAL::ORIGIN + sum / static_cast<double>(count);

    std::vector<SurfaceMesh::Face_index> faces;
    for (const auto f : CGAL::faces_around_target(spoke, mesh)) faces.push_back(f);
    if (!refineAndFair) return faces;
    std::vector<SurfaceMesh::Face_index> newFaces;
    std::vector<SurfaceMesh::Vertex_index> interior{center};
    PMP::refine(mesh, faces, std::back_inserter(newFaces), std::back_inserter(interior));
    (void)PMP::fair(mesh, interior, CGAL::parameters::fairing_continuity(continuity));
    faces.insert(faces.end(), newFaces.begin(), newFaces.end());
    return faces;
}

} // namespace

Result<HoleFillResult> fillHoles(const TriMesh& input, const HoleFillParams& params) {
    return guarded("hole filling", [&]() -> Result<HoleFillResult> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        using Halfedge = SurfaceMesh::Halfedge_index;
        using Face = SurfaceMesh::Face_index;

        std::vector<Halfedge> cycles;
        M2C_CGAL_BORDER_NS::extract_boundary_cycles(mesh, std::back_inserter(cycles));

        std::size_t filled = 0;
        std::size_t skipped = 0;
        std::size_t failed = 0;
        std::size_t added = 0;
        for (const Halfedge start : cycles) {
            double perimeter = 0.0;
            std::size_t edges = 0;
            Halfedge h = start;
            do {
                perimeter += std::sqrt(CGAL::to_double(CGAL::squared_distance(
                    mesh.point(mesh.source(h)), mesh.point(mesh.target(h)))));
                ++edges;
                h = mesh.next(h);
            } while (h != start);

            if ((params.maxPerimeterMm > 0.0 && perimeter > params.maxPerimeterMm) ||
                (params.maxEdges > 0 && edges > params.maxEdges)) {
                ++skipped;
                continue;
            }
            std::vector<Face> newFaces;
            bool ok = true;
            // Tangent continuity blends the patch into the surrounding curvature, but where the
            // surface meets the hole's plane at more than 45 degrees (an open end) it forces a
            // dome: the patch must leave the rim parallel to the walls. There the patch only
            // keeps position continuity, which gives a flat cap on a flat rim.
            const unsigned continuity = rimAlignment(mesh, start) >= std::sqrt(0.5) ? 1 : 0;
            if (params.refineAndFair) {
                const auto outcome = PMP::triangulate_refine_and_fair_hole(
                    mesh,
                    start,
                    CGAL::parameters::face_output_iterator(std::back_inserter(newFaces))
                        .fairing_continuity(continuity));
                // Fairing can fail on awkward holes; the hole is still triangulated and refined.
                ok = !newFaces.empty();
                (void)outcome;
            } else {
                PMP::triangulate_hole(
                    mesh,
                    start,
                    CGAL::parameters::face_output_iterator(std::back_inserter(newFaces)));
                ok = !newFaces.empty();
            }
            if (!ok && mesh.is_border(start)) {
                newFaces = fanFill(mesh, start, params.refineAndFair, continuity);
                ok = !newFaces.empty();
            }
            if (ok) {
                ++filled;
                added += newFaces.size();
            } else {
                ++failed;
            }
        }
        mesh.collect_garbage();
        auto out = fromSurfaceMesh(mesh);
        if (!out) return std::unexpected(out.error());
        return HoleFillResult{std::move(*out), filled, skipped, failed, added};
    });
}

} // namespace m2c::cgal

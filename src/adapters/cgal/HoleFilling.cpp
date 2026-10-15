// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <iterator>
#include <vector>

#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>

#include "m2c/adapters/cgal/MeshTools.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace PMP = CGAL::Polygon_mesh_processing;

Result<HoleFillResult> fillHoles(const TriMesh& input, const HoleFillParams& params) {
    return guarded("hole filling", [&]() -> Result<HoleFillResult> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        using Halfedge = SurfaceMesh::Halfedge_index;
        using Face = SurfaceMesh::Face_index;

        std::vector<Halfedge> cycles;
        PMP::extract_boundary_cycles(mesh, std::back_inserter(cycles));

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
            if (params.refineAndFair) {
                const auto outcome = PMP::triangulate_refine_and_fair_hole(
                    mesh,
                    start,
                    CGAL::parameters::face_output_iterator(std::back_inserter(newFaces)));
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

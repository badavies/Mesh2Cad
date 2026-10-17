// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

#include <CGAL/Intersections_3/Segment_3_Triangle_3.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <boost/iterator/function_output_iterator.hpp>

#include "m2c/adapters/cgal/MeshTools.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace PMP = CGAL::Polygon_mesh_processing;

namespace {

using Polygon = std::vector<std::size_t>;
using EdgeKey = std::pair<std::size_t, std::size_t>;

EdgeKey edgeKey(std::size_t a, std::size_t b) {
    return {std::min(a, b), std::max(a, b)};
}

// Removes fins: a face hanging off an edge that already joins two other faces, recognised by
// having a free (boundary) edge of its own. Splitting the non-manifold edge instead would leave
// the fin behind as a loose sliver touching the surface. Only removes when at least two faces
// without free edges stay on the edge, so open scans that meet at a border are left alone.
void removeFins(std::vector<Polygon>& polygons) {
    std::map<EdgeKey, std::vector<std::size_t>> edgeFaces;
    for (std::size_t f = 0; f < polygons.size(); ++f) {
        const Polygon& poly = polygons[f];
        for (std::size_t i = 0; i < poly.size(); ++i) {
            edgeFaces[edgeKey(poly[i], poly[(i + 1) % poly.size()])].push_back(f);
        }
    }
    const auto hasFreeEdge = [&](std::size_t f) {
        const Polygon& poly = polygons[f];
        for (std::size_t i = 0; i < poly.size(); ++i) {
            if (edgeFaces[edgeKey(poly[i], poly[(i + 1) % poly.size()])].size() == 1) return true;
        }
        return false;
    };
    std::vector<bool> remove(polygons.size(), false);
    for (const auto& [edge, faces] : edgeFaces) {
        if (faces.size() <= 2) continue;
        std::vector<std::size_t> fins;
        for (const std::size_t f : faces) {
            if (hasFreeEdge(f)) fins.push_back(f);
        }
        if (fins.empty() || faces.size() - fins.size() < 2) continue;
        for (const std::size_t f : fins) remove[f] = true;
    }
    std::vector<Polygon> kept;
    kept.reserve(polygons.size());
    for (std::size_t f = 0; f < polygons.size(); ++f) {
        if (!remove[f]) kept.push_back(std::move(polygons[f]));
    }
    polygons = std::move(kept);
}

using Face = SurfaceMesh::Face_index;
using Triangle = std::array<Point, 3>;

Triangle triangle(const SurfaceMesh& mesh, Face f) {
    const auto h = mesh.halfedge(f);
    return {mesh.point(mesh.source(h)),
            mesh.point(mesh.target(h)),
            mesh.point(mesh.target(mesh.next(h)))};
}

// CGAL counts two faces as intersecting when they touch at a point they do not share as a
// vertex. Repair itself creates such points: splitting a non-manifold vertex or edge leaves
// coincident copies. Faces that only meet at coincident corners (or along a coincident edge
// without overlapping) touch rather than cross, so they are not reported.
bool crosses(const Triangle& a, const Triangle& b) {
    std::vector<std::pair<std::size_t, std::size_t>> shared;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            if (a[i] == b[j]) shared.emplace_back(i, j);
        }
    }
    if (shared.size() == 1) {
        // Meeting at one corner, they overlap only if the edge opposite that corner in one face
        // passes through the other.
        const auto [i, j] = shared.front();
        const Kernel::Triangle_3 ta(a[0], a[1], a[2]);
        const Kernel::Triangle_3 tb(b[0], b[1], b[2]);
        return CGAL::do_intersect(Kernel::Segment_3(a[(i + 1) % 3], a[(i + 2) % 3]), tb) ||
               CGAL::do_intersect(Kernel::Segment_3(b[(j + 1) % 3], b[(j + 2) % 3]), ta);
    }
    if (shared.size() == 2) {
        // A corner matched twice means a face with coincident corners; leave it reported.
        if (shared[0].first == shared[1].first || shared[0].second == shared[1].second) {
            return true;
        }
        // Sharing an edge, they overlap only if coplanar and on the same side of it.
        const Point& p = a[shared[0].first];
        const Point& q = a[shared[1].first];
        const Point& ra = a[3 - shared[0].first - shared[1].first];
        const Point& rb = b[3 - shared[0].second - shared[1].second];
        // coplanar_orientation() needs p, q and ra not collinear: a flat face is reported too.
        if (CGAL::collinear(p, q, ra)) return true;
        return CGAL::coplanar(p, q, ra, rb) &&
               CGAL::coplanar_orientation(p, q, ra, rb) == CGAL::POSITIVE;
    }
    return true;
}

struct IntersectionScan {
    bool touching{false}; ///< Any contact CGAL reports, crossing or not.
    bool crossing{false};
    std::vector<std::size_t> shellOf;                         ///< Shell index of each face.
    std::set<std::pair<std::size_t, std::size_t>> shellPairs; ///< Shells that cross each other.
};

// Needs a mesh without garbage (face indices 0..n-1).
IntersectionScan scanIntersections(const SurfaceMesh& mesh) {
    IntersectionScan scan;
    scan.shellOf.resize(mesh.number_of_faces());
    PMP::connected_components(mesh, CGAL::make_property_map(scan.shellOf));
    PMP::self_intersections(
        mesh,
        boost::iterators::make_function_output_iterator([&](const std::pair<Face, Face>& pair) {
            scan.touching = true;
            if (!crosses(triangle(mesh, pair.first), triangle(mesh, pair.second))) return;
            scan.crossing = true;
            const std::size_t a = scan.shellOf[pair.first];
            const std::size_t b = scan.shellOf[pair.second];
            if (a != b) scan.shellPairs.emplace(std::min(a, b), std::max(a, b));
        }));
    return scan;
}

// Two closed shells that pass through each other (a part pushed into another) cannot be fixed
// by local self-intersection removal. Replace each such group with the union of its shells,
// computed by CGAL corefinement. Groups whose shells are open, self-intersecting or fail to
// unite are left as they are.
void uniteCrossingShells(SurfaceMesh& mesh) {
    IntersectionScan scan = scanIntersections(mesh);
    if (scan.shellPairs.empty()) return;
    std::vector<SurfaceMesh> shells;
    PMP::split_connected_components(
        mesh, shells, CGAL::parameters::face_patch_map(CGAL::make_property_map(scan.shellOf)));

    std::vector<std::size_t> group(shells.size());
    std::iota(group.begin(), group.end(), 0);
    const auto root = [&](std::size_t s) {
        while (group[s] != s) s = group[s] = group[group[s]];
        return s;
    };
    for (const auto& [a, b] : scan.shellPairs) group[root(a)] = root(b);

    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t s = 0; s < shells.size(); ++s) groups[root(s)].push_back(s);

    SurfaceMesh result;
    for (auto& [id, members] : groups) {
        bool united = members.size() > 1;
        for (const std::size_t s : members) {
            united = united && CGAL::is_closed(shells[s]) && !PMP::does_self_intersect(shells[s]);
        }
        SurfaceMesh unionMesh;
        if (united) {
            for (const std::size_t s : members) {
                if (!PMP::is_outward_oriented(shells[s])) {
                    PMP::reverse_face_orientations(shells[s]);
                }
            }
            unionMesh = shells[members.front()];
            for (std::size_t k = 1; united && k < members.size(); ++k) {
                united = PMP::corefine_and_compute_union(unionMesh, shells[members[k]], unionMesh);
            }
        }
        if (united) {
            CGAL::copy_face_graph(unionMesh, result);
        } else {
            for (const std::size_t s : members) CGAL::copy_face_graph(shells[s], result);
        }
    }
    mesh = std::move(result);
}

} // namespace

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
        removeFins(polygons);
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

        if (params.removeSelfIntersections) {
            PMP::experimental::remove_self_intersections(mesh);
            mesh.collect_garbage();
            uniteCrossingShells(mesh);
        }
        // Orientation crashes on vertices without faces (unused input points, a removed fin's tip).
        PMP::remove_isolated_vertices(mesh);
        mesh.collect_garbage();
        const IntersectionScan intersections = scanIntersections(mesh);

        bool flipped = !orientable;
        if (CGAL::is_closed(mesh)) {
            const bool outward = PMP::is_outward_oriented(mesh);
            // orient_to_bound_a_volume() also gets nested shells (cavities) right, but assumes no
            // face touches another; otherwise orient each shell outward on its own.
            if (intersections.touching) {
                PMP::orient(mesh);
            } else {
                PMP::orient_to_bound_a_volume(mesh);
            }
            flipped = flipped || !outward;
        }

        auto out = fromSurfaceMesh(mesh);
        if (!out) return std::unexpected(out.error());
        RepairResult result{std::move(*out)};
        result.removedFaces = input.faceCount() > afterSoup ? input.faceCount() - afterSoup : 0;
        if (result.mesh.faceCount() < afterSoup) {
            result.removedFaces += afterSoup - result.mesh.faceCount();
        }
        result.splitVertices = split + (points.size() - pointsBefore);
        result.orientationFixed = flipped;
        result.selfIntersecting = intersections.crossing;
        return result;
    });
}

} // namespace m2c::cgal

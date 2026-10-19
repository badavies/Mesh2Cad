// SPDX-License-Identifier: GPL-3.0-or-later
// Region growing with CGAL Shape_detection (ARCHITECTURE.md section 6, "Segment").
#include "m2c/adapters/cgal/RegionGrowing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

#include <CGAL/Shape_detection/Region_growing/Point_set.h>
#include <CGAL/Shape_detection/Region_growing/Polygon_mesh.h>
#include <CGAL/Shape_detection/Region_growing/Region_growing.h>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "CgalMesh.h"

namespace m2c::cgal {

namespace {

namespace SD = CGAL::Shape_detection;
using Face = SurfaceMesh::Face_index;
using Vector = Kernel::Vector_3;
using PointMap = SurfaceMesh::Property_map<Face, Point>;
using NormalMap = SurfaceMesh::Property_map<Face, Vector>;

/// Neighbours across edges, among the free faces.
struct FreeNeighbours {
    const SurfaceMesh* mesh;
    const std::vector<char>* free;
    void operator()(const Face f, std::vector<Face>& out) const {
        for (const auto h : CGAL::halfedges_around_face(mesh->halfedge(f), *mesh)) {
            const Face g = mesh->face(mesh->opposite(h));
            if (g != SurfaceMesh::null_face() && (*free)[g.idx()]) out.push_back(g);
        }
    }
};

/// The free faces within two edge steps, for the local fits that order the seeds (one ring of
/// a triangle is too few points to fit a cylinder or sphere to).
struct FreeTwoRings {
    FreeNeighbours step;
    void operator()(const Face f, std::vector<Face>& out) const {
        step(f, out);
        const std::size_t first = out.size();
        for (std::size_t i = 0; i < first; ++i) step(out[i], out);
        std::ranges::sort(out);
        const auto [from, to] = std::ranges::unique(out);
        out.erase(from, to);
        std::erase(out, f);
    }
};

Vec3d toVec(const Point& p) {
    return {.x = p.x(), .y = p.y(), .z = p.z()};
}
Vec3d toVec(const Vector& v) {
    return {.x = v.x(), .y = v.y(), .z = v.z()};
}
Vec3d unit(const Vec3d& v) {
    const double n = length(v);
    return n > 0.0 ? v * (1.0 / n) : Vec3d{};
}

Primitive toPrimitive(const Kernel::Plane_3& p) {
    return {.kind = PrimitiveKind::Plane,
            .point = toVec(p.point()),
            .direction = unit(toVec(p.orthogonal_vector()))};
}
template <class Cylinder>
    requires requires(Cylinder c) { c.axis; }
Primitive toPrimitive(const Cylinder& c) {
    return {.kind = PrimitiveKind::Cylinder,
            .point = toVec(c.axis.point()),
            .direction = unit(toVec(c.axis.to_vector())),
            .radiusMm = c.radius};
}
Primitive toPrimitive(const Kernel::Sphere_3& s) {
    return {.kind = PrimitiveKind::Sphere,
            .point = toVec(s.center()),
            .radiusMm = std::sqrt(s.squared_radius())};
}

template <class Region, class Seeds, class Items>
std::vector<GrownRegion>
detect(const Items& items, Seeds& seeds, FreeNeighbours& neighbours, Region& region) {
    std::vector<GrownRegion> out;
    if (seeds.empty()) return out;
    SD::Region_growing<FreeNeighbours, Region> growing(items, seeds, neighbours, region);
    std::vector<typename SD::Region_growing<FreeNeighbours, Region>::Primitive_and_region> found;
    growing.detect(std::back_inserter(found));
    for (const auto& [primitive, faces] : found) {
        GrownRegion& r = out.emplace_back(GrownRegion{.primitive = toPrimitive(primitive)});
        for (const Face f : faces) r.faces.push_back(static_cast<std::uint32_t>(f.idx()));
    }
    return out;
}

} // namespace

Result<std::vector<GrownRegion>>
growRegions(const TriMesh& input, const std::vector<char>& free, const GrowParams& params) {
    if (free.size() != input.faceCount() || !(params.maxDistanceMm > 0.0) ||
        !(params.maxAngleDeg > 0.0 && params.maxAngleDeg <= 90.0) || !(params.maxRadiusMm >= 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "one free flag per face; distance positive; angle above 0 and at most 90 "
                    "degrees; radius at least 0");
    }
    return guarded("region growing", [&]() -> Result<std::vector<GrownRegion>> {
        SurfaceMesh mesh = toSurfaceMesh(input);
        if (mesh.number_of_faces() != input.faceCount()) {
            return fail(ErrorCode::InvalidArgument,
                        "the mesh is not manifold; repair it before segmenting");
        }
        std::vector<Face> items;
        for (const Face f : mesh.faces()) {
            if (free[f.idx()]) items.push_back(f);
        }
        FreeNeighbours neighbours{&mesh, &free};
        if (params.kind == PrimitiveKind::Plane) {
            using Region = SD::Polygon_mesh::Least_squares_plane_fit_region<Kernel, SurfaceMesh>;
            using Sorting = SD::Polygon_mesh::
                Least_squares_plane_fit_sorting<Kernel, SurfaceMesh, FreeNeighbours>;
            Region region(mesh,
                          CGAL::parameters::maximum_distance(params.maxDistanceMm)
                              .maximum_angle(params.maxAngleDeg)
                              .minimum_region_size(1));
            Sorting sorting(mesh, neighbours);
            sorting.sort();
            std::vector<Face> seeds;
            for (const Face f : sorting.ordered()) {
                if (free[f.idx()]) seeds.push_back(f);
            }
            return detect(items, seeds, neighbours, region);
        }
        if (items.size() < 6) return std::vector<GrownRegion>{};
        auto centroids = mesh.add_property_map<Face, Point>("f:m2c_centroid").first;
        auto normals = mesh.add_property_map<Face, Vector>("f:m2c_normal").first;
        for (const Face f : mesh.faces()) {
            const auto h = mesh.halfedge(f);
            const Point& a = mesh.point(mesh.source(h));
            const Point& b = mesh.point(mesh.target(h));
            const Point& c = mesh.point(mesh.target(mesh.next(h)));
            centroids[f] = CGAL::centroid(a, b, c);
            const Vector n = CGAL::cross_product(b - a, c - a);
            const double size = std::sqrt(n.squared_length());
            normals[f] = size > 0.0 ? n / size : Vector(0, 0, 0);
        }
        const auto np =
            CGAL::parameters::maximum_distance(params.maxDistanceMm)
                .maximum_angle(params.maxAngleDeg)
                .minimum_region_size(6)
                .maximum_radius(params.maxRadiusMm > 0.0 ? params.maxRadiusMm
                                                         : std::numeric_limits<double>::max())
                .point_map(centroids)
                .normal_map(normals);
        FreeTwoRings rings{neighbours};
        const auto seedsOf = [&](auto& sorting) {
            sorting.sort();
            return sorting.ordered();
        };
        if (params.kind == PrimitiveKind::Cylinder) {
            using Region =
                SD::Point_set::Least_squares_cylinder_fit_region<Kernel, Face, PointMap, NormalMap>;
            SD::Point_set::
                Least_squares_cylinder_fit_sorting<Kernel, Face, FreeTwoRings, PointMap, NormalMap>
                    sorting(items, rings, np);
            auto seeds = seedsOf(sorting);
            Region region(np);
            return detect(items, seeds, neighbours, region);
        }
        using Region =
            SD::Point_set::Least_squares_sphere_fit_region<Kernel, Face, PointMap, NormalMap>;
        SD::Point_set::Least_squares_sphere_fit_sorting<Kernel, Face, FreeTwoRings, PointMap>
            sorting(items, rings, np);
        auto seeds = seedsOf(sorting);
        Region region(np);
        return detect(items, seeds, neighbours, region);
    });
}

} // namespace m2c::cgal

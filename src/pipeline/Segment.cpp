// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Segment.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numbers>
#include <utility>
#include <vector>

#include "m2c/adapters/cgal/RegionGrowing.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"
#include "m2c/pipeline/PrimitiveFit.h"

namespace m2c {

namespace {

/// Refits, grows and checks the regions region growing finds, and keeps track of free faces.
class Collector {
public:
    Collector(const TriMesh& mesh, const SegmentParams& params, const double minArea)
        : mesh_(mesh), params_(params), minArea_(minArea), free_(mesh.faceCount(), 1),
          mark_(mesh.faceCount(), 0),
          cosLimit_(std::cos(params.maxAngleDeg * std::numbers::pi / 180.0)) {}

    [[nodiscard]] const std::vector<char>& free() const { return free_; }
    [[nodiscard]] std::vector<SegmentRegion>& regions() { return regions_; }

    void add(const cgal::GrownRegion& grown) {
        SegmentRegion r{.primitive = grown.primitive};
        // Faces an earlier region grew over are no longer free.
        for (const std::uint32_t f : grown.faces) {
            if (free_[f]) r.faces.push_back(f);
        }
        if (r.faces.empty()) return;
        r.primitive = fitPrimitive(r.primitive, pointsOf(r.faces)).primitive;
        // Region growing stops where its rough primitive stops fitting; grow on over the free
        // faces the fitted one explains, then fit again.
        growOver(r);
        const std::vector<Vec3d> points = pointsOf(r.faces);
        const PrimitiveFit fit = fitPrimitive(r.primitive, points);
        r.primitive = fit.primitive;
        r.rmsMm = fit.rmsMm;
        r.maxMm = fit.maxMm;
        Vec3d centroid, normal;
        const auto& p = mesh_.positions();
        for (const std::uint32_t f : r.faces) {
            const auto [a, b, c] = mesh_.face(f);
            const Vec3d n = cross(p[b] - p[a], p[c] - p[a]);
            r.areaMm2 += 0.5 * length(n);
            centroid = centroid + (p[a] + p[b] + p[c]) * (0.5 * length(n) / 3.0);
            normal = normal + n;
        }
        if (!(r.areaMm2 >= minArea_) || !(r.rmsMm <= params_.maxDistanceMm)) return;
        if (turnAngle(r.primitive, points) < 3.0 * params_.maxAngleDeg * std::numbers::pi / 180.0 &&
            r.primitive.kind != PrimitiveKind::Plane) {
            return;
        }
        // A few rows of vertices along a sharp edge fit a thin cylinder too, but the faces
        // between them do not face the way it does.
        const auto fitting =
            std::ranges::count_if(r.faces, [&](const std::uint32_t f) { return fits(r, f); });
        if (static_cast<double>(fitting) < 0.95 * static_cast<double>(r.faces.size())) return;
        if (r.primitive.kind == PrimitiveKind::Plane) {
            Primitive& plane = r.primitive;
            if (dot(plane.direction, normal) < 0.0) plane.direction = plane.direction * -1.0;
            centroid = centroid * (1.0 / r.areaMm2);
            plane.point = centroid - plane.direction * dot(centroid - plane.point, plane.direction);
        }
        for (const std::uint32_t f : r.faces) free_[f] = 0;
        regions_.push_back(std::move(r));
    }

private:
    [[nodiscard]] std::vector<Vec3d> pointsOf(const std::vector<std::uint32_t>& faces) const {
        return regionPoints(mesh_, faces);
    }

    /// The face lies within the distance limit of the primitive, facing within the angle limit.
    [[nodiscard]] bool fits(const SegmentRegion& r, const std::uint32_t f) const {
        const auto [a, b, c] = mesh_.face(f);
        const auto& p = mesh_.positions();
        for (const std::uint32_t v : {a, b, c}) {
            if (distanceTo(r.primitive, p[v]) > params_.maxDistanceMm) return false;
        }
        const Vec3d n = cross(p[b] - p[a], p[c] - p[a]);
        const Vec3d expected = normalAt(r.primitive, (p[a] + p[b] + p[c]) * (1.0 / 3.0));
        return std::abs(dot(n, expected)) >= cosLimit_ * length(n);
    }

    /// Adds every free face connected to the region that fits its primitive; r.faces ends sorted.
    void growOver(SegmentRegion& r) {
        const FaceAdjacency& adjacency = mesh_.faceAdjacency();
        for (const std::uint32_t f : r.faces) mark_[f] = 1;
        for (std::size_t i = 0; i < r.faces.size(); ++i) {
            const std::uint32_t f = r.faces[i];
            for (std::uint32_t k = adjacency.offsets[f]; k < adjacency.offsets[f + 1]; ++k) {
                const std::uint32_t g = adjacency.neighbours[k];
                if (mark_[g] || !free_[g]) continue;
                mark_[g] = 1;
                if (fits(r, g)) {
                    r.faces.push_back(g);
                } else {
                    rejected_.push_back(g);
                }
            }
        }
        for (const std::uint32_t f : r.faces) mark_[f] = 0;
        for (const std::uint32_t f : rejected_) mark_[f] = 0;
        rejected_.clear();
        std::ranges::sort(r.faces);
    }

    const TriMesh& mesh_;
    const SegmentParams& params_;
    double minArea_;
    std::vector<char> free_;
    std::vector<char> mark_;              ///< Scratch for growOver(), all zero between calls.
    std::vector<std::uint32_t> rejected_; ///< Scratch for growOver().
    double cosLimit_;
    std::vector<SegmentRegion> regions_;
};

} // namespace

Result<SegmentResult> segmentMesh(const TriMesh& mesh, const SegmentParams& params) {
    if (!(params.minAreaMm2 >= 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the area limit must be at least 0");
    }
    double total = 0.0;
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        const auto& p = mesh.positions();
        total += 0.5 * length(cross(p[b] - p[a], p[c] - p[a]));
    }
    Collector collector(mesh, params, params.minAreaMm2 > 0.0 ? params.minAreaMm2 : 0.01 * total);
    // Spheres and cylinders first: planes would take their faces as narrow strips.
    for (const auto& [kind, wanted] : {std::pair{PrimitiveKind::Sphere, params.spheres},
                                       std::pair{PrimitiveKind::Cylinder, params.cylinders},
                                       std::pair{PrimitiveKind::Plane, params.planes}}) {
        if (!wanted) continue;
        auto grown =
            cgal::growRegions(mesh,
                              collector.free(),
                              {.kind = kind,
                               .maxDistanceMm = params.maxDistanceMm,
                               .maxAngleDeg = params.maxAngleDeg,
                               .maxRadiusMm = length(mesh.bounds().max - mesh.bounds().min)});
        if (!grown) return std::unexpected(grown.error());
        for (const cgal::GrownRegion& g : *grown) collector.add(g);
    }
    SegmentResult out;
    out.regions = std::move(collector.regions());
    std::ranges::stable_sort(
        out.regions, {}, [](const SegmentRegion& r) { return r.primitive.kind; });
    for (std::uint32_t f = 0; f < mesh.faceCount(); ++f) {
        if (collector.free()[f]) out.organicFaces.push_back(f);
    }
    return out;
}

} // namespace m2c

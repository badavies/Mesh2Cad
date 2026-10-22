// SPDX-License-Identifier: GPL-3.0-or-later
// One uniform Catmull-Clark step of a cage (deviation-driven refinement, ARCHITECTURE.md
// section 6): four times the quads, the same limit surface and the same irregular vertices. And
// the limit surface's normals at the cage vertices, for thickening a sheet.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/sdc/crease.h>
#include <opensubdiv/sdc/options.h>
#include <opensubdiv/sdc/types.h>

#include "m2c/adapters/osd/Subdivision.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Vec3.h"

namespace m2c::osd {

namespace {

namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

/// A vertex as OpenSubdiv's primvar refiner interpolates it.
struct Point {
    Vec3d p;
    void Clear() { p = {}; }
    void AddWithWeight(const Point& src, const double w) { p = p + src.p * w; }
};

/// The cage's topology for OpenSubdiv, boundaries as in the rest of the adapter, with
/// infinitely sharp creases.
std::unique_ptr<Far::TopologyRefiner>
refinerOf(const QuadLayout& cage, const std::vector<std::array<std::uint32_t, 2>>& creases = {}) {
    std::vector<int> sizes(cage.faceCount());
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        sizes[f] = static_cast<int>(cage.face(f).size());
    }
    const std::vector<int> indices(cage.faceVertices().begin(), cage.faceVertices().end());
    Far::TopologyDescriptor desc;
    desc.numVertices = static_cast<int>(cage.vertexCount());
    desc.numFaces = static_cast<int>(cage.faceCount());
    desc.numVertsPerFace = sizes.data();
    desc.vertIndicesPerFace = indices.data();
    std::vector<int> creasePairs;
    for (const auto& [a, b] : creases) {
        creasePairs.insert(creasePairs.end(), {static_cast<int>(a), static_cast<int>(b)});
    }
    const std::vector<float> creaseWeights(creases.size(), Sdc::Crease::SHARPNESS_INFINITE);
    desc.numCreases = static_cast<int>(creases.size());
    desc.creaseVertexIndexPairs = creasePairs.data();
    desc.creaseWeights = creaseWeights.data();
    Sdc::Options sdc;
    sdc.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);
    return std::unique_ptr<Far::TopologyRefiner>(
        Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc,
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options(Sdc::SCHEME_CATMARK,
                                                                          sdc)));
}

} // namespace

Result<QuadLayout> subdivideCage(const QuadLayout& cage,
                                 std::vector<std::array<std::uint32_t, 2>>* creases) {
    if (!cageStats(cage).orientedManifold()) {
        return fail(ErrorCode::InvalidArgument,
                    "the cage must be a consistently oriented manifold");
    }
    if (creases != nullptr) {
        for (const auto& [a, b] : *creases) {
            if (a >= cage.vertexCount() || b >= cage.vertexCount()) {
                return fail(ErrorCode::InvalidArgument, "no such crease");
            }
        }
    }
    try {
        const std::unique_ptr<Far::TopologyRefiner> refiner = refinerOf(
            cage, creases != nullptr ? *creases : std::vector<std::array<std::uint32_t, 2>>{});
        if (!refiner)
            return fail(ErrorCode::InvalidArgument, "OpenSubdiv rejected the cage topology");
        Far::TopologyRefiner::UniformOptions once(1);
        once.fullTopologyInLastLevel = creases != nullptr; // its edges, to find the creases
        refiner->RefineUniform(once);
        const Far::TopologyLevel& fine = refiner->GetLevel(1);
        std::vector<Point> points(cage.vertexCount() +
                                  static_cast<std::size_t>(fine.GetNumVertices()));
        for (std::size_t v = 0; v < cage.vertexCount(); ++v) points[v].p = cage.positions()[v];
        Point* source = points.data();
        Point* destination = points.data() + cage.vertexCount();
        Far::PrimvarRefinerReal<double>(*refiner).Interpolate(1, source, destination);

        std::vector<Vec3d> positions;
        positions.reserve(static_cast<std::size_t>(fine.GetNumVertices()));
        for (int v = 0; v < fine.GetNumVertices(); ++v) {
            positions.push_back(destination[v].p);
        }
        std::vector<std::uint32_t> starts{0};
        std::vector<std::uint32_t> vertices;
        vertices.reserve(static_cast<std::size_t>(fine.GetNumFaces()) * 4);
        for (int f = 0; f < fine.GetNumFaces(); ++f) {
            for (const int v : fine.GetFaceVertices(f)) {
                vertices.push_back(static_cast<std::uint32_t>(v));
            }
            starts.push_back(static_cast<std::uint32_t>(vertices.size()));
        }
        if (creases != nullptr) {
            creases->clear();
            for (int e = 0; e < fine.GetNumEdges(); ++e) {
                if (!Sdc::Crease::IsInfinite(fine.GetEdgeSharpness(e))) continue;
                const auto ends = fine.GetEdgeVertices(e);
                const auto a = static_cast<std::uint32_t>(ends[0]);
                const auto b = static_cast<std::uint32_t>(ends[1]);
                creases->push_back({std::min(a, b), std::max(a, b)});
            }
            std::ranges::sort(*creases);
        }
        return QuadLayout::create(std::move(positions), std::move(starts), std::move(vertices));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("OpenSubdiv: ") + e.what());
    }
}

Result<std::vector<Vec3d>> limitNormals(const QuadLayout& cage) {
    if (!cageStats(cage).orientedManifold()) {
        return fail(ErrorCode::InvalidArgument,
                    "the cage must be a consistently oriented manifold");
    }
    try {
        const std::unique_ptr<Far::TopologyRefiner> refiner = refinerOf(cage);
        if (!refiner)
            return fail(ErrorCode::InvalidArgument, "OpenSubdiv rejected the cage topology");
        std::vector<Point> source(cage.vertexCount());
        for (std::size_t v = 0; v < cage.vertexCount(); ++v) source[v].p = cage.positions()[v];
        std::vector<Point> limit(cage.vertexCount());
        std::vector<Point> du(cage.vertexCount());
        std::vector<Point> dv(cage.vertexCount());
        // Unrefined, the limit masks apply to the cage vertices themselves.
        Far::PrimvarRefinerReal<double>(*refiner).Limit(source, limit, du, dv);
        std::vector<Vec3d> normals(cage.vertexCount());
        for (std::size_t v = 0; v < cage.vertexCount(); ++v) {
            const Vec3d n = cross(du[v].p, dv[v].p);
            const double size = length(n);
            normals[v] = size > 0.0 ? n * (1.0 / size) : Vec3d{};
        }
        return normals;
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("OpenSubdiv: ") + e.what());
    }
}

} // namespace m2c::osd

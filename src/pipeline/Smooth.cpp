// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Smooth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <numeric>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

struct VertexRing {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> neighbours;
};

/// One-ring vertex neighbours (sorted, unique) from the faces: counted, laid out in place, then
/// each vertex's short list sorted and deduplicated.
VertexRing buildVertexRing(const TriMesh& mesh) {
    const auto forEachEdge = [&](auto&& visit) {
        for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
            const auto v = mesh.face(f);
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = v[static_cast<std::size_t>(k)];
                if (const std::uint32_t b = v[static_cast<std::size_t>((k + 1) % 3)]; a != b)
                    visit(a, b);
            }
        }
    };
    VertexRing ring;
    ring.offsets.assign(mesh.vertexCount() + 1, 0);
    forEachEdge([&](const std::uint32_t a, const std::uint32_t b) {
        ++ring.offsets[a + 1];
        ++ring.offsets[b + 1];
    });
    std::partial_sum(ring.offsets.begin(), ring.offsets.end(), ring.offsets.begin());
    ring.neighbours.resize(ring.offsets.back());
    std::vector next(ring.offsets.begin(), ring.offsets.end() - 1);
    forEachEdge([&](const std::uint32_t a, const std::uint32_t b) {
        ring.neighbours[next[a]++] = b;
        ring.neighbours[next[b]++] = a;
    });
    std::uint32_t begin = 0;
    std::uint32_t kept = 0;
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        const std::uint32_t end = ring.offsets[v + 1];
        std::uint32_t* const first = ring.neighbours.data() + begin;
        std::sort(first, ring.neighbours.data() + end);
        const std::uint32_t* const last = std::unique(first, ring.neighbours.data() + end);
        ring.offsets[v] = kept;
        for (const std::uint32_t* w = first; w != last; ++w) ring.neighbours[kept++] = *w;
        begin = end;
    }
    ring.offsets[mesh.vertexCount()] = kept;
    ring.neighbours.resize(kept);
    return ring;
}

} // namespace

Result<TriMesh>
smoothTaubin(const TriMesh& mesh, const SmoothParams& params, const MeshSelection* selection) {
    if (params.strength <= 0.0 || params.strength > 1.0) {
        return fail(ErrorCode::InvalidArgument, "strength must be in (0, 1]");
    }
    if (selection != nullptr && selection->size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    const std::size_t n = mesh.vertexCount();

    // Which vertices may move.
    std::vector movable(n, selection == nullptr);
    if (selection != nullptr) {
        for (const std::uint32_t f : selection->indices()) {
            for (const std::uint32_t v : mesh.face(f)) movable[v] = true;
        }
        // Vertices on the selection's border are outside it too: moving them would bend the
        // unselected faces around the selection.
        for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
            if (selection->test(f)) continue;
            for (const std::uint32_t v : mesh.face(f)) movable[v] = false;
        }
    }
    // Fix open-boundary vertices and, if asked, vertices on sharp edges.
    {
        const auto& [offsets, neighbours] = mesh.faceAdjacency();
        const double cosSharp = std::cos(params.sharpAngleDeg * std::numbers::pi / 180.0);
        for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
            const auto v = mesh.face(f);
            const Vec3d nf = faceNormal(mesh, f);
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = v[static_cast<std::size_t>(k)];
                const std::uint32_t b = v[static_cast<std::size_t>((k + 1) % 3)];
                // Find the faces across edge (a, b).
                std::size_t across = 0;
                bool sharp = false;
                for (std::uint32_t i = offsets[f]; i < offsets[f + 1]; ++i) {
                    const std::uint32_t g = neighbours[i];
                    const auto w = mesh.face(g);
                    const bool hasA = w[0] == a || w[1] == a || w[2] == a;
                    if (const bool hasB = w[0] == b || w[1] == b || w[2] == b; !(hasA && hasB))
                        continue;
                    ++across;
                    if (params.preserveSharpEdges && dot(nf, faceNormal(mesh, g)) < cosSharp) {
                        sharp = true;
                    }
                }
                if (across == 0 || sharp) {
                    movable[a] = false;
                    movable[b] = false;
                }
            }
        }
    }

    const auto [ringOffsets, ringNeighbours] = buildVertexRing(mesh);
    // Taubin: lambda > 0 then mu < -lambda; 1/lambda + 1/mu = k_pb, with pass band k_pb = 0.1.
    const double lambda = params.strength;
    const double mu = 1.0 / (0.1 - 1.0 / lambda);

    std::vector<Vec3d> current = mesh.positions();
    std::vector<Vec3d> next(n);
    const auto step = [&](const double factor) {
        for (std::size_t v = 0; v < n; ++v) {
            next[v] = current[v];
            if (!movable[v]) continue;
            const std::uint32_t begin = ringOffsets[v];
            const std::uint32_t end = ringOffsets[v + 1];
            if (begin == end) continue;
            Vec3d sum{};
            for (std::uint32_t i = begin; i < end; ++i) sum = sum + current[ringNeighbours[i]];
            const Vec3d laplacian = sum * (1.0 / static_cast<double>(end - begin)) - current[v];
            next[v] = current[v] + laplacian * factor;
        }
        current.swap(next);
    };
    for (unsigned it = 0; it < params.iterations; ++it) {
        step(lambda);
        step(mu);
    }
    return TriMesh::create(std::make_shared<const std::vector<Vec3d>>(std::move(current)),
                           mesh.sharedIndices());
}

} // namespace m2c

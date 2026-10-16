// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Smooth.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <numeric>
#include <vector>

#include "m2c/core/MeshTopology.h"

namespace m2c {

namespace {

struct VertexRing {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> neighbours;
};

/// One-ring vertex neighbours (sorted, unique) from the faces.
VertexRing buildVertexRing(const TriMesh& mesh) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(mesh.faceCount() * 6);
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto v = mesh.face(f);
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = v[static_cast<std::size_t>(k)];
            const std::uint32_t b = v[static_cast<std::size_t>((k + 1) % 3)];
            if (a == b) continue;
            pairs.emplace_back(a, b);
            pairs.emplace_back(b, a);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    VertexRing ring;
    ring.offsets.assign(mesh.vertexCount() + 1, 0);
    for (const auto& [a, b] : pairs) ++ring.offsets[a + 1];
    std::partial_sum(ring.offsets.begin(), ring.offsets.end(), ring.offsets.begin());
    ring.neighbours.reserve(pairs.size());
    for (const auto& [a, b] : pairs) ring.neighbours.push_back(b);
    return ring;
}

} // namespace

Result<TriMesh>
smoothTaubin(const TriMesh& mesh, const SmoothParams& params, const MeshSelection* selection) {
    if (!(params.strength > 0.0 && params.strength <= 1.0)) {
        return fail(ErrorCode::InvalidArgument, "strength must be in (0, 1]");
    }
    if (selection != nullptr && selection->size() != mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument, "selection does not belong to this mesh");
    }
    const std::size_t n = mesh.vertexCount();

    // Which vertices may move.
    std::vector<bool> movable(n, selection == nullptr);
    if (selection != nullptr) {
        for (const std::uint32_t f : selection->indices()) {
            for (const std::uint32_t v : mesh.face(f)) movable[v] = true;
        }
    }
    // Fix open-boundary vertices and, if asked, vertices on sharp edges.
    {
        const FaceAdjacency& adj = mesh.faceAdjacency();
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
                for (std::uint32_t i = adj.offsets[f]; i < adj.offsets[f + 1]; ++i) {
                    const std::uint32_t g = adj.neighbours[i];
                    const auto w = mesh.face(g);
                    const bool hasA = w[0] == a || w[1] == a || w[2] == a;
                    const bool hasB = w[0] == b || w[1] == b || w[2] == b;
                    if (!(hasA && hasB)) continue;
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

    const VertexRing ring = buildVertexRing(mesh);
    // Taubin: lambda > 0 then mu < -lambda; 1/lambda + 1/mu = k_pb, with pass band k_pb = 0.1.
    const double lambda = params.strength;
    const double mu = 1.0 / (0.1 - 1.0 / lambda);

    std::vector<Vec3d> current = mesh.positions();
    std::vector<Vec3d> next(n);
    const auto step = [&](double factor) {
        for (std::size_t v = 0; v < n; ++v) {
            next[v] = current[v];
            if (!movable[v]) continue;
            const std::uint32_t begin = ring.offsets[v];
            const std::uint32_t end = ring.offsets[v + 1];
            if (begin == end) continue;
            Vec3d sum{};
            for (std::uint32_t i = begin; i < end; ++i) sum = sum + current[ring.neighbours[i]];
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

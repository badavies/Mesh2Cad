// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/MeshStats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

[[nodiscard]] std::uint64_t edgeKey(const std::uint32_t a, const std::uint32_t b) noexcept {
    const auto lo = std::min(a, b);
    const auto hi = std::max(a, b);
    return (static_cast<std::uint64_t>(lo) << 32U) | hi;
}

} // namespace

MeshStats computeStats(const TriMesh& mesh) {
    MeshStats stats;
    stats.vertexCount = mesh.vertexCount();
    stats.faceCount = mesh.faceCount();
    stats.bounds = mesh.bounds();

    const auto& pos = mesh.positions();
    std::vector<std::uint64_t> edges;
    edges.reserve(mesh.faceCount() * 3);

    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        if (a == b || b == c || c == a) {
            // Repeated vertex: no area, no volume, and no usable topology. Counted as degenerate
            // here so it does not show up as bogus boundary or non-manifold edges.
            ++stats.degenerateFaceCount;
            continue;
        }
        const Vec3d& pa = pos[a];
        const Vec3d& pb = pos[b];
        const Vec3d& pc = pos[c];

        const Vec3d n = cross(pb - pa, pc - pa);
        const double area = 0.5 * length(n);
        stats.surfaceArea += area;
        stats.signedVolume += dot(pa, cross(pb, pc)) / 6.0;
        if (area <= 0.0) {
            ++stats.degenerateFaceCount;
        }

        edges.push_back(edgeKey(a, b));
        edges.push_back(edgeKey(b, c));
        edges.push_back(edgeKey(c, a));
    }

    std::ranges::sort(edges);
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        const std::size_t uses = j - i;
        ++stats.edgeCount;
        if (uses == 1) {
            ++stats.boundaryEdgeCount;
        } else if (uses > 2) {
            ++stats.nonManifoldEdgeCount;
        }
        i = j;
    }
    return stats;
}

} // namespace m2c

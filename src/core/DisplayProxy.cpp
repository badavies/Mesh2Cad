// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/DisplayProxy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace m2c {

namespace {

std::optional<TriMesh> cluster(const TriMesh& mesh, double cell) {
    const Aabb& box = mesh.bounds();
    const auto cellOf = [&](const Vec3d& p) {
        const auto ix = static_cast<std::uint64_t>((p.x - box.min.x) / cell);
        const auto iy = static_cast<std::uint64_t>((p.y - box.min.y) / cell);
        const auto iz = static_cast<std::uint64_t>((p.z - box.min.z) / cell);
        return (ix << 42) | (iy << 21) | iz; // 21 bits per axis is plenty for a display grid
    };
    std::unordered_map<std::uint64_t, std::uint32_t> cellIndex;
    std::vector<Vec3d> sum;
    std::vector<std::uint32_t> count;
    std::vector<std::uint32_t> remap(mesh.vertexCount());
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        const Vec3d& p = mesh.positions()[v];
        const auto [it, inserted] =
            cellIndex.try_emplace(cellOf(p), static_cast<std::uint32_t>(sum.size()));
        if (inserted) {
            sum.push_back({});
            count.push_back(0);
        }
        sum[it->second] = sum[it->second] + p;
        ++count[it->second];
        remap[v] = it->second;
    }
    std::vector<Vec3d> positions(sum.size());
    for (std::size_t i = 0; i < sum.size(); ++i) positions[i] = sum[i] * (1.0 / count[i]);

    std::vector<std::uint32_t> indices;
    indices.reserve(mesh.faceCount() / 2);
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        const std::uint32_t ra = remap[a], rb = remap[b], rc = remap[c];
        if (ra == rb || rb == rc || rc == ra) continue;
        indices.insert(indices.end(), {ra, rb, rc});
    }
    if (indices.empty()) return std::nullopt;
    auto proxy = TriMesh::create(std::move(positions), std::move(indices));
    if (!proxy) return std::nullopt;
    return *proxy;
}

} // namespace

TriMesh buildDisplayProxy(const TriMesh& mesh, std::size_t targetFaces) {
    if (mesh.faceCount() <= targetFaces || targetFaces == 0) return mesh;
    const Aabb& box = mesh.bounds();
    const Vec3d extent = box.max - box.min;
    const double diagonal = length(extent);
    if (!(diagonal > 0.0)) return mesh;
    // A surface sampled on a grid of cell size h keeps roughly area / h^2 vertices and twice as
    // many faces; estimate h from the surface area, then correct once from the measured result.
    double area = 0.0;
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        const auto& p = mesh.positions();
        area += 0.5 * length(cross(p[b] - p[a], p[c] - p[a]));
    }
    double cell = std::sqrt(2.0 * area / static_cast<double>(targetFaces));
    cell = std::max(cell, diagonal / 2'000'000.0);
    auto proxy = cluster(mesh, cell);
    // Grid alignment makes the count noisy, so keep growing the cell (a few rounds at most).
    for (int round = 0; round < 8 && proxy && proxy->faceCount() > targetFaces; ++round) {
        cell *= 1.05 * std::sqrt(static_cast<double>(proxy->faceCount()) /
                                 static_cast<double>(targetFaces));
        proxy = cluster(mesh, cell);
    }
    return proxy ? *proxy : mesh;
}

} // namespace m2c

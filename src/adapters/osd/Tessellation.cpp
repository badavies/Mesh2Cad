// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tessellation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Limit.h"

namespace m2c::osd {

Result<Tessellation> tessellate(const Limit& limit, const std::vector<Vec3d>& points, const int n) {
    const auto row = static_cast<std::uint32_t>(n + 1);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    positions.reserve(limit.patchCount() * row * row);
    indices.reserve(limit.patchCount() * static_cast<std::size_t>(6 * n * n));
    for (std::size_t p = 0; p < limit.patchCount(); ++p) {
        const auto base = static_cast<std::uint32_t>(positions.size());
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                positions.push_back(limit.evaluate(
                    points, p, static_cast<double>(i) / n, static_cast<double>(j) / n));
            }
        }
        for (std::uint32_t j = 0; j < row - 1; ++j) {
            for (std::uint32_t i = 0; i < row - 1; ++i) {
                const std::uint32_t v00 = base + j * row + i;
                indices.insert(indices.end(),
                               {v00, v00 + 1, v00 + row + 1, v00, v00 + row + 1, v00 + row});
            }
        }
    }
    auto mesh = TriMesh::create(std::move(positions), std::move(indices));
    if (!mesh) return std::unexpected(mesh.error());
    return Tessellation{.mesh = std::move(*mesh), .n = n};
}

SurfacePoint locate(const Tessellation& tess, const Vec3d& q) {
    const auto [face, distance, point] = tess.mesh.spatialIndex().closestPoint(q);
    const auto [ia, ib, ic] = tess.mesh.face(face);
    const auto& p = tess.mesh.positions();
    // Barycentric coordinates of the closest point in its triangle.
    const Vec3d e0 = p[ib] - p[ia];
    const Vec3d e1 = p[ic] - p[ia];
    const Vec3d d = point - p[ia];
    const double d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1);
    const double det = d00 * d11 - d01 * d01;
    double b1 = 0.0, b2 = 0.0;
    if (det > 0.0) {
        b1 = std::clamp((d11 * dot(d, e0) - d01 * dot(d, e1)) / det, 0.0, 1.0);
        b2 = std::clamp((d00 * dot(d, e1) - d01 * dot(d, e0)) / det, 0.0, 1.0 - b1);
    }
    const auto row = static_cast<std::uint32_t>(tess.n + 1);
    const std::uint32_t perPatch = row * row;
    const auto st = [&](const std::uint32_t v) {
        const std::uint32_t k = v % perPatch;
        return std::pair{static_cast<double>(k % row) / tess.n,
                         static_cast<double>(k / row) / tess.n};
    };
    const auto [sa, ta] = st(ia);
    const auto [sb, tb] = st(ib);
    const auto [sc, tc] = st(ic);
    const double b0 = 1.0 - b1 - b2;
    const Vec3d n = cross(e0, e1);
    const double len = length(n);
    return {.patch = ia / perPatch,
            .s = b0 * sa + b1 * sb + b2 * sc,
            .t = b0 * ta + b1 * tb + b2 * tc,
            .distance = distance,
            .normal = len > 0.0 ? n * (1.0 / len) : Vec3d{}};
}

} // namespace m2c::osd

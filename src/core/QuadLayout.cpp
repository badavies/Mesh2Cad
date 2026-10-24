// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/QuadLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) noexcept {
    if (a > b) std::swap(a, b);
    return (std::uint64_t{a} << 32U) | b;
}

Result<Aabb> boundsOf(const std::vector<Vec3d>& positions) {
    if (positions.empty()) return fail(ErrorCode::InvalidArgument, "cage has no vertices");
    Aabb box{.min = positions.front(), .max = positions.front()};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto& [x, y, z] = positions[i];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return fail(ErrorCode::InvalidArgument,
                        "cage vertex " + std::to_string(i) + " has a non-finite coordinate");
        }
        box.min = {
            .x = std::min(box.min.x, x), .y = std::min(box.min.y, y), .z = std::min(box.min.z, z)};
        box.max = {
            .x = std::max(box.max.x, x), .y = std::max(box.max.y, y), .z = std::max(box.max.z, z)};
    }
    return box;
}

std::uint32_t findRoot(std::vector<std::uint32_t>& parent, std::uint32_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

} // namespace

QuadLayout::QuadLayout(std::shared_ptr<const std::vector<Vec3d>> positions,
                       std::shared_ptr<const Topology> topology,
                       const Aabb& bounds)
    : positions_(std::move(positions)), topology_(std::move(topology)), bounds_(bounds) {}

Result<QuadLayout> QuadLayout::create(std::vector<Vec3d> positions,
                                      std::vector<std::uint32_t> faceStarts,
                                      std::vector<std::uint32_t> faceVertices) {
    auto box = boundsOf(positions);
    if (!box) return std::unexpected(box.error());
    if (positions.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::InvalidArgument, "cage has more than 2^32-1 vertices");
    }
    if (faceStarts.size() < 2 || faceStarts.front() != 0 ||
        faceStarts.back() != faceVertices.size()) {
        return fail(ErrorCode::InvalidArgument, "cage face offsets do not match its vertex list");
    }
    std::vector<std::uint32_t> uses(positions.size(), 0);
    for (std::size_t f = 0; f + 1 < faceStarts.size(); ++f) {
        // Checked without adding to an offset, which could wrap round.
        if (faceStarts[f + 1] > faceVertices.size() || faceStarts[f + 1] < faceStarts[f] ||
            faceStarts[f + 1] - faceStarts[f] < 3) {
            return fail(ErrorCode::InvalidArgument,
                        "cage face " + std::to_string(f) + " has fewer than three vertices");
        }
        for (std::uint32_t i = faceStarts[f]; i < faceStarts[f + 1]; ++i) {
            const std::uint32_t v = faceVertices[i];
            if (v >= positions.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "cage face " + std::to_string(f) + " references vertex " +
                                std::to_string(v) + " but the cage has only " +
                                std::to_string(positions.size()) + " vertices");
            }
            for (std::uint32_t j = faceStarts[f]; j < i; ++j) {
                if (faceVertices[j] == v) {
                    return fail(ErrorCode::InvalidArgument,
                                "cage face " + std::to_string(f) + " repeats vertex " +
                                    std::to_string(v));
                }
            }
            ++uses[v];
        }
    }
    auto topology = std::make_shared<Topology>();
    for (std::uint32_t v = 0; v < uses.size(); ++v) {
        if (uses[v] != 0 && uses[v] != 4) topology->extraordinary.push_back(v);
    }
    topology->starts = std::move(faceStarts);
    topology->vertices = std::move(faceVertices);
    return QuadLayout(std::make_shared<const std::vector<Vec3d>>(std::move(positions)),
                      std::move(topology),
                      *box);
}

Result<QuadLayout> QuadLayout::withPositions(std::vector<Vec3d> positions) const {
    if (positions.size() != vertexCount()) {
        return fail(ErrorCode::InvalidArgument,
                    "expected " + std::to_string(vertexCount()) + " cage positions, got " +
                        std::to_string(positions.size()));
    }
    auto box = boundsOf(positions);
    if (!box) return std::unexpected(box.error());
    return QuadLayout(
        std::make_shared<const std::vector<Vec3d>>(std::move(positions)), topology_, *box);
}

CageStats cageStats(const QuadLayout& cage) {
    CageStats stats;
    stats.vertexCount = cage.vertexCount();
    stats.faceCount = cage.faceCount();

    // Per undirected edge: how many faces use it, and the net direction (+1 for a -> b with
    // a < b, -1 for b -> a); a manifold, consistently oriented edge has two faces and net 0.
    struct EdgeUse {
        int faces{0};
        int direction{0};
    };
    std::unordered_map<std::uint64_t, EdgeUse> edges;
    edges.reserve(cage.faceVertices().size());
    std::vector<std::uint32_t> uses(cage.vertexCount(), 0);
    std::vector<std::uint32_t> parent(cage.vertexCount());
    std::ranges::iota(parent, 0u);
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        ++stats.faceDegrees[face.size()];
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % face.size()];
            auto& [faces, direction] = edges[edgeKey(a, b)];
            ++faces;
            direction += a < b ? 1 : -1;
            ++uses[a];
            parent[findRoot(parent, a)] = findRoot(parent, b);
        }
    }
    stats.edgeCount = edges.size();
    for (const EdgeUse& use : std::views::values(edges)) {
        if (use.faces == 1) ++stats.boundaryEdges;
        if (use.faces > 2) ++stats.nonManifoldEdges;
        if (use.faces == 2 && use.direction != 0) ++stats.misorientedEdges;
    }

    // A vertex is non-manifold when its faces form more than one fan: count the faces around it
    // that are joined to each other through edges at the vertex (union-find over corners).
    std::vector<std::uint32_t> cornerStart(cage.vertexCount() + 1, 0);
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v)
        cornerStart[v + 1] = cornerStart[v] + uses[v];
    std::vector<std::uint32_t> cornerFace(cornerStart.back());
    std::vector fill(cornerStart.begin(), cornerStart.end() - 1);
    for (std::uint32_t f = 0; f < cage.faceCount(); ++f) {
        for (std::uint32_t v : cage.face(f)) cornerFace[fill[v]++] = f;
    }
    std::vector<std::uint32_t> fan;
    std::unordered_map<std::uint32_t, std::uint32_t> firstFaceOfNeighbour;
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        const std::uint32_t n = uses[v];
        if (n == 0) continue;
        ++stats.valences[n];
        if (n != 4) ++stats.extraordinaryVertices;
        fan.resize(n);
        std::ranges::iota(fan, 0u);
        firstFaceOfNeighbour.clear();
        for (std::uint32_t k = 0; k < n; ++k) {
            const auto face = cage.face(cornerFace[cornerStart[v] + k]);
            const auto at = std::ranges::find(face, v) - face.begin();
            for (const auto size = static_cast<std::ptrdiff_t>(face.size());
                 const std::uint32_t w : {face[static_cast<std::size_t>((at + 1) % size)],
                                          face[static_cast<std::size_t>((at + size - 1) % size)]}) {
                if (const auto [it, inserted] = firstFaceOfNeighbour.emplace(w, k); !inserted)
                    fan[findRoot(fan, k)] = findRoot(fan, it->second);
            }
        }
        std::size_t fans = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            if (findRoot(fan, k) == k) ++fans;
        }
        if (fans > 1) ++stats.nonManifoldVertices;
    }

    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        if (uses[v] != 0 && findRoot(parent, v) == v) ++stats.components;
    }
    return stats;
}

double signedVolume(const QuadLayout& cage) {
    const auto& p = cage.positions();
    double volume = 0.0;
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            volume += dot(p[face[0]], cross(p[face[i]], p[face[i + 1]]));
        }
    }
    return volume / 6.0;
}

std::vector<std::uint32_t> cageEdges(const QuadLayout& cage) {
    // Lower vertex first, sorted and without repeats: an open sheet's boundary edges are in one
    // face only, in either direction.
    std::vector<std::uint64_t> keys;
    keys.reserve(cage.faceVertices().size());
    for (std::size_t f = 0; f + 1 < cage.faceStarts().size(); ++f) {
        const std::uint32_t begin = cage.faceStarts()[f], end = cage.faceStarts()[f + 1];
        for (std::uint32_t k = begin; k < end; ++k) {
            keys.push_back(
                edgeKey(cage.faceVertices()[k], cage.faceVertices()[k + 1 < end ? k + 1 : begin]));
        }
    }
    std::ranges::sort(keys);
    const auto repeats = std::ranges::unique(keys);
    keys.erase(repeats.begin(), repeats.end());
    std::vector<std::uint32_t> edges;
    edges.reserve(2 * keys.size());
    for (const std::uint64_t key : keys) {
        edges.insert(edges.end(),
                     {static_cast<std::uint32_t>(key >> 32U), static_cast<std::uint32_t>(key)});
    }
    return edges;
}

std::vector<std::array<std::uint32_t, 2>> sharpCageEdges(const QuadLayout& cage,
                                                         const double angleDeg) {
    std::map<std::array<std::uint32_t, 2>, std::vector<Vec3d>> normals;
    const auto& p = cage.positions();
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        Vec3d area{};
        for (std::size_t i = 0; i < face.size(); ++i) {
            area = area + cross(p[face[i]], p[face[(i + 1) % face.size()]]);
        }
        for (std::size_t i = 0; i < face.size(); ++i) {
            const auto [a, b] = std::minmax(face[i], face[(i + 1) % face.size()]);
            normals[{a, b}].push_back(area);
        }
    }
    const double limit = std::cos(angleDeg * std::numbers::pi / 180.0);
    std::vector<std::array<std::uint32_t, 2>> sharp;
    for (const auto& [edge, n] : normals) {
        if (n.size() != 2) continue;
        if (const double size = length(n[0]) * length(n[1]);
            size > 0.0 && dot(n[0], n[1]) < limit * size)
            sharp.push_back(edge);
    }
    return sharp;
}

} // namespace m2c

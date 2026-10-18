// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/MeshTopology.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace m2c {

FaceAdjacency buildFaceAdjacency(const TriMesh& mesh) {
    const std::size_t faceCount = mesh.faceCount();
    struct EdgeUse {
        std::uint64_t key;
        std::uint32_t face;
    };
    std::vector<EdgeUse> uses;
    uses.reserve(faceCount * 3);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const auto v = mesh.face(f);
        if (v[0] == v[1] || v[1] == v[2] || v[2] == v[0]) continue;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = v[static_cast<std::size_t>(k)];
            const std::uint32_t b = v[static_cast<std::size_t>((k + 1) % 3)];
            const std::uint64_t key = (std::uint64_t{std::min(a, b)} << 32U) | std::max(a, b);
            uses.push_back({.key = key, .face = static_cast<std::uint32_t>(f)});
        }
    }
    std::ranges::sort(uses, [](const EdgeUse& l, const EdgeUse& r) {
        return l.key < r.key || (l.key == r.key && l.face < r.face);
    });

    // Faces sharing an edge are neighbours. Count each face's neighbours, lay the lists out
    // in place and then sort and deduplicate each face's short list, rather than sorting one
    // list of every pair.
    const auto forEachPair = [&](auto&& visit) {
        for (std::size_t i = 0; i < uses.size();) {
            std::size_t j = i;
            while (j < uses.size() && uses[j].key == uses[i].key) ++j;
            for (std::size_t p = i; p < j; ++p) {
                for (std::size_t q = p + 1; q < j; ++q) {
                    if (uses[p].face != uses[q].face) visit(uses[p].face, uses[q].face);
                }
            }
            i = j;
        }
    };
    FaceAdjacency adj;
    adj.offsets.assign(faceCount + 1, 0);
    forEachPair([&](const std::uint32_t f, const std::uint32_t g) {
        ++adj.offsets[f + 1];
        ++adj.offsets[g + 1];
    });
    std::partial_sum(adj.offsets.begin(), adj.offsets.end(), adj.offsets.begin());
    adj.neighbours.resize(adj.offsets.back());
    std::vector next(adj.offsets.begin(), adj.offsets.end() - 1);
    forEachPair([&](const std::uint32_t f, const std::uint32_t g) {
        adj.neighbours[next[f]++] = g;
        adj.neighbours[next[g]++] = f;
    });
    std::uint32_t begin = 0;
    std::uint32_t kept = 0;
    for (std::size_t f = 0; f < faceCount; ++f) {
        const std::uint32_t end = adj.offsets[f + 1];
        std::uint32_t* const first = adj.neighbours.data() + begin;
        std::sort(first, adj.neighbours.data() + end);
        const std::uint32_t* const last = std::unique(first, adj.neighbours.data() + end);
        adj.offsets[f] = kept;
        for (const std::uint32_t* g = first; g != last; ++g) adj.neighbours[kept++] = *g;
        begin = end;
    }
    adj.offsets[faceCount] = kept;
    adj.neighbours.resize(kept);
    return adj;
}

std::vector<std::uint32_t> faceComponents(const TriMesh& mesh, std::uint32_t* componentCount) {
    const auto& [offsets, neighbours] = mesh.faceAdjacency();
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector component(mesh.faceCount(), kUnset);
    std::vector<std::uint32_t> stack;
    std::uint32_t next = 0;
    for (std::uint32_t seed = 0; seed < component.size(); ++seed) {
        if (component[seed] != kUnset) continue;
        component[seed] = next;
        stack.push_back(seed);
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            for (std::uint32_t i = offsets[f]; i < offsets[f + 1]; ++i) {
                if (const std::uint32_t g = neighbours[i]; component[g] == kUnset) {
                    component[g] = next;
                    stack.push_back(g);
                }
            }
        }
        ++next;
    }
    if (componentCount != nullptr) *componentCount = next;
    return component;
}

Vec3d faceNormal(const TriMesh& mesh, const std::size_t f) noexcept {
    const auto [a, b, c] = mesh.face(f);
    const auto& p = mesh.positions();
    const Vec3d n = cross(p[b] - p[a], p[c] - p[a]);
    const double len = length(n);
    return len > 0.0 ? n * (1.0 / len) : Vec3d{};
}

double faceArea(const TriMesh& mesh, const std::size_t f) noexcept {
    const auto [a, b, c] = mesh.face(f);
    const auto& p = mesh.positions();
    return 0.5 * length(cross(p[b] - p[a], p[c] - p[a]));
}

Result<TriMesh> extractFaces(const TriMesh& mesh, const std::vector<bool>& keep) {
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector used(mesh.vertexCount(), false);
    std::size_t kept = 0;
    for (std::size_t f = 0; f < mesh.faceCount() && f < keep.size(); ++f) {
        if (!keep[f]) continue;
        ++kept;
        for (const std::uint32_t v : mesh.face(f)) used[v] = true;
    }
    if (kept == 0) return fail(ErrorCode::InvalidArgument, "the result would have no faces");

    std::vector remap(mesh.vertexCount(), kUnset);
    std::vector<Vec3d> positions;
    for (std::size_t v = 0; v < used.size(); ++v) {
        if (!used[v]) continue;
        remap[v] = static_cast<std::uint32_t>(positions.size());
        positions.push_back(mesh.positions()[v]);
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(kept * 3);
    for (std::size_t f = 0; f < mesh.faceCount() && f < keep.size(); ++f) {
        if (!keep[f]) continue;
        for (const std::uint32_t v : mesh.face(f)) indices.push_back(remap[v]);
    }
    return TriMesh::create(std::move(positions), std::move(indices));
}

} // namespace m2c

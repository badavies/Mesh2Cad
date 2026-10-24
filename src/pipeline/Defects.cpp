// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Defects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Progress.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kChunk = 4096; // vertices between progress reports and cancel checks
/// A spike grows over neighbouring faces whose normal is more than this from the surrounding
/// surface's: the steep sides of a spike and the back of a flap.
constexpr double kSteepDeg = 60.0;
/// The surrounding surface of a spike is at least this many faces.
constexpr std::size_t kSurround = 50;

/// Edge and vertex neighbourhoods of a mesh, in the form the defect searches walk.
struct Neighbourhoods {
    /// across[3 f + i]: the face on the other side of edge i of f (corner i to corner i + 1), or
    /// kNone on an open edge. At a non-manifold edge, one of the other faces.
    std::vector<std::uint32_t> across;
    /// Faces around each vertex, in compressed rows.
    std::vector<std::uint32_t> vertexOffsets;
    std::vector<std::uint32_t> vertexFaces;
    /// The vertex's faces close into one fan around it (an interior manifold vertex).
    std::vector<std::uint8_t> closed;

    [[nodiscard]] std::uint32_t valence(const std::uint32_t v) const {
        return vertexOffsets[v + 1] - vertexOffsets[v];
    }
};

/// Index of vertex v among the corners of face c (c holds v).
std::size_t cornerOf(const std::array<std::uint32_t, 3>& c, const std::uint32_t v) {
    return c[0] == v ? 0 : (c[1] == v ? 1 : 2);
}

Neighbourhoods buildNeighbourhoods(const TriMesh& mesh) {
    const std::size_t faceCount = mesh.faceCount();
    const std::size_t vertexCount = mesh.vertexCount();
    const auto& [offsets, neighbours] = mesh.faceAdjacency();
    Neighbourhoods n;
    n.across.assign(3 * faceCount, kNone);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const auto c = mesh.face(f);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::uint32_t a = c[i];
            const std::uint32_t b = c[(i + 1) % 3];
            for (std::uint32_t k = offsets[f]; k < offsets[f + 1]; ++k) {
                if (const auto g = mesh.face(neighbours[k]);
                    std::ranges::contains(g, a) && std::ranges::contains(g, b)) {
                    n.across[3 * f + i] = neighbours[k];
                    break;
                }
            }
        }
    }
    n.vertexOffsets.assign(vertexCount + 1, 0);
    for (const std::uint32_t v : mesh.indices()) ++n.vertexOffsets[v + 1];
    for (std::size_t v = 0; v < vertexCount; ++v) n.vertexOffsets[v + 1] += n.vertexOffsets[v];
    n.vertexFaces.resize(mesh.indices().size());
    std::vector fill(n.vertexOffsets.begin(), n.vertexOffsets.end() - 1);
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        for (const std::uint32_t v : mesh.face(f)) n.vertexFaces[fill[v]++] = f;
    }
    // Walk around each vertex from its first face: it is closed when the walk comes back to that
    // face after visiting every face once.
    n.closed.assign(vertexCount, 0);
    for (std::uint32_t v = 0; v < vertexCount; ++v) {
        const std::uint32_t valence = n.valence(v);
        if (valence < 3) continue;
        const std::uint32_t start = n.vertexFaces[n.vertexOffsets[v]];
        std::uint32_t f = start;
        std::uint32_t from = kNone;
        std::uint32_t steps = 0;
        while (steps <= valence) {
            const auto c = mesh.face(f);
            const std::size_t i = cornerOf(c, v);
            // Leave by the edge at v that does not lead back where the walk came from.
            std::uint32_t next = n.across[std::size_t{3} * f + i];
            if (next == from) next = n.across[std::size_t{3} * f + (i + 2) % 3];
            ++steps;
            if (next == kNone) break;
            from = f;
            f = next;
            if (f == start) break;
        }
        n.closed[v] = f == start && steps == valence ? 1 : 0;
    }
    return n;
}

/// Small union-find over dense indices.
class Sets {
public:
    void reset(const std::size_t n) {
        parent_.resize(n);
        for (std::size_t i = 0; i < n; ++i) parent_[i] = static_cast<std::uint32_t>(i);
    }
    std::uint32_t find(std::uint32_t i) {
        while (parent_[i] != i) {
            parent_[i] = parent_[parent_[i]];
            i = parent_[i];
        }
        return i;
    }
    /// Whether i and j were in different sets.
    bool unite(const std::uint32_t i, const std::uint32_t j) {
        const std::uint32_t a = find(i);
        const std::uint32_t b = find(j);
        if (a == b) return false;
        parent_[std::max(a, b)] = std::min(a, b);
        return true;
    }

private:
    std::vector<std::uint32_t> parent_;
};

/// Genus of a set of faces as a surface: (2 parts - Euler characteristic - boundary loops) / 2,
/// counting a vertex once per fan of the faces around it. `inside(f)` says whether face f is in
/// the set and `local(f)` gives its position in `faces`.
template <class Inside, class Local>
std::size_t genusOf(const TriMesh& mesh,
                    const Neighbourhoods& n,
                    const std::span<const std::uint32_t> faces,
                    Inside inside,
                    Local local) {
    const std::size_t count = faces.size();
    Sets fans;  // corners 3 k + i, joined across inner edges into fans around their vertex
    Sets parts; // faces, joined across inner edges
    fans.reset(3 * count);
    parts.reset(count);
    std::int64_t edges = 0;
    auto partCount = static_cast<std::int64_t>(count);
    auto fanCount = static_cast<std::int64_t>(3 * count);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> open; // boundary edges as corner pairs
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t f = faces[k];
        const auto c = mesh.face(f);
        for (std::uint32_t i = 0; i < 3; ++i) {
            const std::uint32_t g = n.across[3 * f + i];
            const std::uint32_t j = (i + 1) % 3;
            if (g == kNone || !inside(g)) {
                ++edges;
                open.emplace_back(3 * k + i, 3 * k + j);
                continue;
            }
            if (f < g) ++edges;
            const std::uint32_t kg = local(g);
            if (parts.unite(k, kg)) --partCount;
            const auto cg = mesh.face(g);
            for (const std::uint32_t corner : {i, j}) {
                if (const auto other = static_cast<std::uint32_t>(cornerOf(cg, c[corner]));
                    fans.unite(3 * k + corner, 3 * kg + other))
                    --fanCount;
            }
        }
    }
    // Each fan on the boundary has two boundary edges, so the boundary edges between fans form
    // disjoint loops: loops = fans on the boundary - joins that merged two of them.
    Sets loops;
    loops.reset(3 * count);
    std::vector<std::uint8_t> onBoundary(3 * count, 0);
    std::int64_t boundaryFans = 0;
    std::int64_t merges = 0;
    for (const auto& [a, b] : open) {
        const std::uint32_t fa = fans.find(a);
        const std::uint32_t fb = fans.find(b);
        for (const std::uint32_t fan : {fa, fb}) {
            if (onBoundary[fan] == 0) {
                onBoundary[fan] = 1;
                ++boundaryFans;
            }
        }
        if (loops.unite(fa, fb)) ++merges;
    }
    const std::int64_t euler = fanCount - edges + static_cast<std::int64_t>(count);
    const std::int64_t twiceGenus = 2 * partCount - euler - (boundaryFans - merges);
    return twiceGenus > 0 ? static_cast<std::size_t>(twiceGenus / 2) : 0;
}

/// State for growing patches of faces around a face. Marks are stamps, so a new patch needs no
/// clearing.
struct Grower {
    const TriMesh& mesh;
    const Neighbourhoods& n;
    const MeshSelection& blocked; ///< Faces treated as already deleted.
    std::vector<std::uint32_t> queued;
    std::vector<std::uint32_t> inside;
    std::vector<std::uint32_t> local;
    std::vector<std::uint32_t> ring;
    std::vector<std::uint32_t> parent;
    std::vector<std::uint32_t> vertexStamp;
    std::vector<std::uint32_t> vertexCount;
    std::vector<std::uint32_t> edgeWalked;
    std::vector<std::uint32_t> patch; ///< Faces in the order they were added.
    std::vector<std::uint32_t> queue;
    std::uint32_t stamp{0};
    std::uint32_t walk{0};

    Grower(const TriMesh& m, const Neighbourhoods& nb, const MeshSelection& b)
        : mesh(m), n(nb), blocked(b), queued(m.faceCount(), 0), inside(m.faceCount(), 0),
          local(m.faceCount(), 0), ring(m.faceCount(), 0), parent(m.faceCount(), kNone),
          vertexStamp(m.vertexCount(), 0), vertexCount(m.vertexCount(), 0),
          edgeWalked(3 * m.faceCount(), 0) {}

    [[nodiscard]] bool in(const std::uint32_t f) const { return f != kNone && inside[f] == stamp; }

    /// Exact genus of the patch.
    [[nodiscard]] std::size_t genus() {
        return genusOf(
            mesh,
            n,
            patch,
            [&](const std::uint32_t f) { return in(f); },
            [&](const std::uint32_t f) { return local[f]; });
    }

    /// Boundary loops of the patch, walked edge by edge (turning around each vertex through the
    /// patch's faces to the next open edge). Returns kNone if a walk does not close, as it may
    /// where the mesh is not manifold.
    [[nodiscard]] std::uint32_t loops() {
        if (++walk == 0) {
            std::ranges::fill(edgeWalked, 0);
            walk = 1;
        }
        std::uint32_t count = 0;
        const std::size_t guard = 3 * patch.size() + 3;
        for (const std::uint32_t start : patch) {
            for (std::uint32_t si = 0; si < 3; ++si) {
                if (edgeWalked[3 * start + si] == walk || in(n.across[3 * start + si])) continue;
                ++count;
                std::uint32_t f = start;
                std::uint32_t e = si;
                std::uint32_t v = mesh.face(f)[(e + 1) % 3];
                for (std::size_t step = 0;; ++step) {
                    if (step > guard) return kNone;
                    edgeWalked[3 * f + e] = walk;
                    // Turn around v from edge e of f, through the patch, to the next open edge.
                    auto k = static_cast<std::uint32_t>(cornerOf(mesh.face(f), v));
                    e = e == k ? (k + 2) % 3 : k;
                    for (std::size_t turn = 0; in(n.across[3 * f + e]); ++turn) {
                        if (turn > guard) return kNone;
                        const std::uint32_t g = n.across[3 * f + e];
                        k = static_cast<std::uint32_t>(cornerOf(mesh.face(g), v));
                        e = n.across[3 * g + k] == f ? (k + 2) % 3 : k;
                        f = g;
                    }
                    if (f == start && e == si) break;
                    const auto c = mesh.face(f);
                    v = c[e] == v ? c[(e + 1) % 3] : c[e];
                }
            }
        }
        return count;
    }

    /// Adds faces in breadth-first rings around face `seed` (ring 0) until the patch holds a handle
    /// or has `limit` faces. Returns the patch size when it holds a handle, else 0; the faces are
    /// left in `patch`, the one that closed the handle last. The Euler characteristic is kept up
    /// to date face by face; the boundary loops, and with them the genus, are counted only where
    /// it drops to -1 or below, as a handle needs it to.
    std::size_t grow(const std::uint32_t seed, const std::size_t limit) {
        if (++stamp == 0) { // wrapped: old marks could match again
            std::ranges::fill(queued, 0);
            std::ranges::fill(inside, 0);
            std::ranges::fill(vertexStamp, 0);
            stamp = 1;
        }
        patch.clear();
        queue.assign(1, seed);
        queued[seed] = stamp;
        ring[seed] = 0;
        parent[seed] = kNone;
        std::int64_t euler = 0;
        std::size_t head = 0;
        while (head < queue.size() && patch.size() < limit) {
            const std::uint32_t f = queue[head++];
            const auto c = mesh.face(f);
            std::int64_t shared = 0;
            std::array<bool, 3> joins{};
            for (std::size_t i = 0; i < 3; ++i) {
                joins[i] = in(n.across[std::size_t{3} * f + i]);
                if (joins[i]) ++shared;
            }
            // Fans: a corner touching no face of the patch starts one; one joining two faces
            // already there merges two fans, unless it closes the last gap around its vertex.
            std::int64_t fanChange = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                const std::uint32_t v = c[i];
                const std::uint32_t had = vertexStamp[v] == stamp ? vertexCount[v] : 0;
                const bool next = joins[i];
                if (const bool previous = joins[(i + 2) % 3]; !next && !previous) {
                    ++fanChange;
                } else if (next && previous && (n.closed[v] == 0 || had + 1 != n.valence(v))) {
                    --fanChange;
                }
                vertexStamp[v] = stamp;
                vertexCount[v] = had + 1;
            }
            inside[f] = stamp;
            local[f] = static_cast<std::uint32_t>(patch.size());
            patch.push_back(f);
            const std::int64_t change = fanChange - (3 - shared) + 1;
            euler += change;
            if (change < 0 && euler <= -1) {
                // One connected patch: genus = (2 - euler - loops) / 2, confirmed exactly.
                if (const std::uint32_t b = loops();
                    (b == kNone || 2 - euler - b >= 2) && genus() > 0)
                    return patch.size();
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (const std::uint32_t g = n.across[std::size_t{3} * f + i];
                    g != kNone && queued[g] != stamp && !blocked.test(g)) {
                    queued[g] = stamp;
                    ring[g] = ring[f] + 1;
                    parent[g] = f;
                    queue.push_back(g);
                }
            }
        }
        // A whole small closed part (a tiny torus) never has its characteristic drop below 0.
        if (head == queue.size() && euler <= 0 && genus() > 0) return patch.size();
        return 0;
    }

    /// From a patch of `size` faces around `seed` that holds a handle, looks for a centre that
    /// holds it in fewer faces: the handle lies between the face that closed it and the face that
    /// closes it in a patch grown from there, so the middle of that path is tried, then
    /// neighbouring faces while they do better. Returns the centre and the size; `patch` is left
    /// holding the final patch.
    std::pair<std::uint32_t, std::size_t> centre(std::uint32_t seed, std::size_t size) {
        const std::uint32_t far = patch.back();
        if (const std::size_t s = grow(far, size); s > 0) {
            std::uint32_t middle = patch.back();
            for (std::uint32_t up = ring[middle] / 2; up > 0; --up) middle = parent[middle];
            if (s < size) {
                seed = far;
                size = s;
            }
            if (const std::size_t t = grow(middle, size - 1); t > 0) {
                seed = middle;
                size = t;
            }
        }
        for (bool moved = true; moved;) {
            moved = false;
            for (std::size_t i = 0; i < 3 && !moved; ++i) {
                const std::uint32_t g = n.across[std::size_t{3} * seed + i];
                if (g == kNone || blocked.test(g)) continue;
                if (const std::size_t s = grow(g, size - 1); s > 0) {
                    seed = g;
                    size = s;
                    moved = true;
                }
            }
        }
        grow(seed, size);
        return {seed, size};
    }
};

double cornerAngle(const Vec3d& at, const Vec3d& b, const Vec3d& c) {
    const Vec3d u = b - at;
    const Vec3d w = c - at;
    return std::atan2(length(cross(u, w)), dot(u, w));
}

/// Spikes: faces around tips and along folds, in connected clusters.
void findSpikes(const TriMesh& mesh,
                const Neighbourhoods& n,
                const ScanDefectParams& params,
                ScanDefectReport& out) {
    const std::size_t faceCount = mesh.faceCount();
    const auto& p = mesh.positions();
    const double tipLimit = params.tipAngleDeg * std::numbers::pi / 180.0;
    const double foldCos = std::cos(params.foldAngleDeg * std::numbers::pi / 180.0);
    std::vector angleSum(mesh.vertexCount(), 0.0);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const auto c = mesh.face(f);
        for (std::size_t i = 0; i < 3; ++i) {
            angleSum[c[i]] += cornerAngle(p[c[i]], p[c[(i + 1) % 3]], p[c[(i + 2) % 3]]);
        }
    }
    MeshSelection marked(faceCount);
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        if (n.closed[v] == 0 || !(angleSum[v] < tipLimit)) continue;
        for (std::uint32_t k = n.vertexOffsets[v]; k < n.vertexOffsets[v + 1]; ++k)
            marked.set(n.vertexFaces[k]);
    }
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        const Vec3d nf = faceNormal(mesh, f);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::uint32_t g = n.across[std::size_t{3} * f + i];
            if (g == kNone || g < f) continue;
            // Degenerate faces have a zero normal and never fold.
            if (const Vec3d ng = faceNormal(mesh, g);
                length(nf) > 0.0 && length(ng) > 0.0 && dot(nf, ng) < foldCos) {
                marked.set(f);
                marked.set(g);
            }
        }
    }
    // Each cluster grows over the faces next to it that are steep against the surrounding
    // surface, so the whole spike or flap goes, not only its tip or fold.
    const double steepCos = std::cos(kSteepDeg * std::numbers::pi / 180.0);
    std::vector<std::uint32_t> visit(faceCount, 0);
    std::uint32_t stamp = 0;
    MeshSelection seen(faceCount);
    MeshSelection spikes(faceCount);
    std::vector<std::uint32_t> cluster;
    std::vector<std::uint32_t> around;
    for (const std::uint32_t start : marked.indices()) {
        if (seen.test(start)) continue;
        cluster.assign(1, start);
        seen.set(start);
        for (std::size_t k = 0; k < cluster.size(); ++k) {
            for (std::size_t i = 0; i < 3; ++i) {
                if (const std::uint32_t g = n.across[std::size_t{3} * cluster[k] + i];
                    g != kNone && marked.test(g) && !seen.test(g)) {
                    seen.set(g);
                    cluster.push_back(g);
                }
            }
        }
        if (cluster.size() > params.maxFaces) {
            ++out.tooLarge;
            continue;
        }
        // The surrounding surface: rings of faces around the cluster, at least kSurround and
        // four times the cluster, and their area-weighted normal.
        const std::uint32_t outside = ++stamp;
        for (const std::uint32_t f : cluster) visit[f] = outside;
        around.assign(cluster.begin(), cluster.end());
        const std::size_t wanted = std::max(kSurround, 4 * cluster.size());
        Vec3d up{};
        for (std::size_t k = 0; k < around.size() && around.size() < cluster.size() + wanted; ++k) {
            for (std::size_t i = 0; i < 3; ++i) {
                const std::uint32_t g = n.across[std::size_t{3} * around[k] + i];
                if (g == kNone || visit[g] == outside) continue;
                visit[g] = outside;
                around.push_back(g);
                up = up + faceNormal(mesh, g) * faceArea(mesh, g);
            }
        }
        const std::size_t seeds = cluster.size();
        if (length(up) > 0.0) {
            up = up * (1.0 / length(up));
            const std::uint32_t body = ++stamp;
            for (const std::uint32_t f : cluster) visit[f] = body;
            for (std::size_t k = 0; k < cluster.size() && cluster.size() <= params.maxFaces; ++k) {
                for (std::size_t i = 0; i < 3; ++i) {
                    const std::uint32_t g = n.across[std::size_t{3} * cluster[k] + i];
                    if (g == kNone || visit[g] == body ||
                        !(dot(faceNormal(mesh, g), up) < steepCos))
                        continue;
                    visit[g] = body;
                    cluster.push_back(g);
                }
            }
            // Steep all the way out: a real wall, not a spike; keep only the tip or fold.
            if (cluster.size() > params.maxFaces) cluster.resize(seeds);
        }
        for (const std::uint32_t f : cluster) {
            seen.set(f);
            spikes.set(f);
        }
    }
    // Count the spikes as the connected groups selected, since grown clusters can merge.
    MeshSelection counted(faceCount);
    for (const std::uint32_t start : spikes.indices()) {
        out.faces.set(start);
        if (counted.test(start)) continue;
        ++out.spikes;
        counted.set(start);
        cluster.assign(1, start);
        for (std::size_t k = 0; k < cluster.size(); ++k) {
            for (std::size_t i = 0; i < 3; ++i) {
                if (const std::uint32_t g = n.across[std::size_t{3} * cluster[k] + i];
                    g != kNone && spikes.test(g) && !counted.test(g)) {
                    counted.set(g);
                    cluster.push_back(g);
                }
            }
        }
    }
}

/// Tunnels. Every vertex is covered by a patch of 4 maxFaces faces grown around a face nearby: a
/// patch without a handle clears the vertices in its inner third of rings, whose own patches of
/// maxFaces faces it contains. A patch with a handle is moved to the centre that holds the handle
/// in the fewest faces; that patch is selected (or left alone when larger than maxFaces) and
/// treated as deleted while the search goes on, so each handle is found once.
Result<void> findTunnels(const TriMesh& mesh,
                         const Neighbourhoods& n,
                         const ScanDefectParams& params,
                         const Progress& progress,
                         ScanDefectReport& out) {
    // Parts without handles cannot hold a tunnel; skip their vertices.
    std::uint32_t partCount = 0;
    const std::vector<std::uint32_t> part = faceComponents(mesh, &partCount);
    std::vector<std::vector<std::uint32_t>> partFaces(partCount);
    for (std::uint32_t f = 0; f < mesh.faceCount(); ++f) partFaces[part[f]].push_back(f);
    std::vector<std::uint8_t> handled(partCount, 0);
    {
        std::vector<std::uint32_t> local(mesh.faceCount(), 0);
        for (std::uint32_t s = 0; s < partCount; ++s) {
            const auto& faces = partFaces[s];
            for (std::uint32_t k = 0; k < faces.size(); ++k) local[faces[k]] = k;
            handled[s] = genusOf(
                             mesh,
                             n,
                             faces,
                             [&](const std::uint32_t f) { return part[f] == s; },
                             [&](const std::uint32_t f) { return local[f]; }) > 0
                             ? 1
                             : 0;
        }
    }

    report(progress, "looking for tunnels", 0.0);
    MeshSelection blocked(mesh.faceCount());
    Grower grower(mesh, n, blocked);
    std::vector<std::uint8_t> covered(mesh.vertexCount(), 0);
    const std::size_t wide = 4 * params.maxFaces;
    for (std::uint32_t q = 0; q < mesh.vertexCount(); ++q) {
        if (q % kChunk == 0) {
            report(progress,
                   "looking for tunnels",
                   static_cast<double>(q) / static_cast<double>(mesh.vertexCount()));
            if (auto go = checkCancelled(progress, "looking for tunnels"); !go)
                return std::unexpected(go.error());
        }
        if (covered[q] != 0 || n.valence(q) == 0 ||
            handled[part[n.vertexFaces[n.vertexOffsets[q]]]] == 0)
            continue;
        while (true) {
            // Grow from a face of q that is still there.
            std::uint32_t seed = kNone;
            for (std::uint32_t k = n.vertexOffsets[q]; k < n.vertexOffsets[q + 1]; ++k) {
                if (!blocked.test(n.vertexFaces[k])) {
                    seed = n.vertexFaces[k];
                    break;
                }
            }
            if (seed == kNone) break;
            if (const std::size_t size = grower.grow(seed, wide); size > 0) {
                const std::size_t best = grower.centre(seed, size).second;
                if (best <= params.maxFaces) {
                    ++out.tunnels;
                } else {
                    ++out.tooLarge;
                }
                for (const std::uint32_t f : grower.patch) {
                    blocked.set(f);
                    if (best <= params.maxFaces) out.faces.set(f);
                }
                continue;
            }
            // No handle: clear the vertices of the inner third of the rings.
            const std::uint32_t reach =
                grower.patch.empty() ? 0 : grower.ring[grower.patch.back()] / 3;
            const bool whole = grower.patch.size() < wide; // ran out of faces: saw everything
            for (const std::uint32_t f : grower.patch) {
                if (!whole && grower.ring[f] > reach) break;
                for (const std::uint32_t v : mesh.face(f)) covered[v] = 1;
            }
            break;
        }
    }
    return {};
}

} // namespace

Result<ScanDefectReport>
findScanDefects(const TriMesh& mesh, const ScanDefectParams& params, const Progress& progress) {
    if (params.maxFaces == 0) {
        return fail(ErrorCode::InvalidArgument, "the largest defect must have at least one face");
    }
    ScanDefectReport out;
    out.faces = MeshSelection(mesh.faceCount());
    const Neighbourhoods n = buildNeighbourhoods(mesh);
    if (params.spikes) {
        report(progress, "looking for spikes");
        findSpikes(mesh, n, params, out);
    }
    if (params.tunnels) {
        if (auto done = findTunnels(mesh, n, params, progress, out); !done)
            return std::unexpected(done.error());
    }
    return out;
}

std::size_t surfaceGenus(const TriMesh& mesh, const MeshSelection& faces) {
    if (faces.size() != mesh.faceCount()) return 0;
    const Neighbourhoods n = buildNeighbourhoods(mesh);
    const std::vector<std::uint32_t> list = faces.indices();
    std::vector<std::uint32_t> local(mesh.faceCount(), 0);
    for (std::uint32_t k = 0; k < list.size(); ++k) local[list[k]] = k;
    return genusOf(
        mesh,
        n,
        list,
        [&](const std::uint32_t f) { return faces.test(f); },
        [&](const std::uint32_t f) { return local[f]; });
}

} // namespace m2c

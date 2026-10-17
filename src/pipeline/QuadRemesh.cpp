// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/QuadRemesh.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

#include "m2c/core/SpatialIndex.h"

namespace m2c {

namespace {

using Face = std::vector<std::uint32_t>;

std::uint64_t undirected(std::uint32_t a, std::uint32_t b) noexcept {
    if (a > b) std::swap(a, b);
    return (std::uint64_t{a} << 32) | b;
}

std::uint64_t directed(std::uint32_t a, std::uint32_t b) noexcept {
    return (std::uint64_t{a} << 32) | b;
}

/// Faces using each undirected edge, in face order.
std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
edgeFaces(const std::vector<Face>& faces) {
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edges;
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        for (std::size_t i = 0; i < face.size(); ++i) {
            edges[undirected(face[i], face[(i + 1) % face.size()])].push_back(f);
        }
    }
    return edges;
}

/// True when the face runs a -> b (b directly after a).
bool runs(const Face& face, std::uint32_t a, std::uint32_t b) {
    for (std::size_t i = 0; i < face.size(); ++i) {
        if (face[i] == a) return face[(i + 1) % face.size()] == b;
    }
    return false;
}

std::size_t dropNonManifoldEdgeFaces(std::vector<Face>& faces) {
    const auto edges = edgeFaces(faces);
    std::vector<bool> drop(faces.size(), false);
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        for (std::size_t i = 0; i < face.size(); ++i) {
            if (edges.at(undirected(face[i], face[(i + 1) % face.size()])).size() > 2) {
                drop[f] = true;
            }
        }
    }
    std::vector<Face> kept;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        if (!drop[f]) kept.push_back(std::move(faces[f]));
    }
    const std::size_t dropped = faces.size() - kept.size();
    faces = std::move(kept);
    return dropped;
}

/// Keeps the edge-connected part with the most faces (the lowest-numbered on a tie).
void keepLargestComponent(std::vector<Face>& faces, CageCleanup& report) {
    const auto edges = edgeFaces(faces);
    std::vector<std::uint32_t> component(faces.size(), UINT32_MAX);
    std::vector<std::size_t> sizes;
    for (std::uint32_t seed = 0; seed < faces.size(); ++seed) {
        if (component[seed] != UINT32_MAX) continue;
        const auto id = static_cast<std::uint32_t>(sizes.size());
        std::size_t size = 0;
        std::queue<std::uint32_t> open;
        open.push(seed);
        component[seed] = id;
        while (!open.empty()) {
            const std::uint32_t f = open.front();
            open.pop();
            ++size;
            const Face& face = faces[f];
            for (std::size_t i = 0; i < face.size(); ++i) {
                for (std::uint32_t g : edges.at(undirected(face[i], face[(i + 1) % face.size()]))) {
                    if (component[g] == UINT32_MAX) {
                        component[g] = id;
                        open.push(g);
                    }
                }
            }
        }
        sizes.push_back(size);
    }
    const auto main =
        static_cast<std::uint32_t>(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
    report.droppedComponents = sizes.size() - 1;
    report.droppedComponentFaces = faces.size() - sizes[main];
    std::vector<Face> kept;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        if (component[f] == main) kept.push_back(std::move(faces[f]));
    }
    faces = std::move(kept);
}

/// Flips faces so each edge is run in opposite directions by its two faces. False if that is
/// impossible (a non-orientable surface).
bool orientConsistently(std::vector<Face>& faces) {
    const auto edges = edgeFaces(faces);
    std::vector<int> flip(faces.size(), -1);
    for (std::uint32_t seed = 0; seed < faces.size(); ++seed) {
        if (flip[seed] != -1) continue;
        flip[seed] = 0;
        std::queue<std::uint32_t> open;
        open.push(seed);
        while (!open.empty()) {
            const std::uint32_t f = open.front();
            open.pop();
            const Face& face = faces[f];
            for (std::size_t i = 0; i < face.size(); ++i) {
                const std::uint32_t a = face[i];
                const std::uint32_t b = face[(i + 1) % face.size()];
                // f runs a -> b originally; with its flip it runs that way iff flip[f] == 0.
                const bool fForward = flip[f] == 0;
                for (std::uint32_t g : edges.at(undirected(a, b))) {
                    if (g == f) continue;
                    const bool gForward = runs(faces[g], a, b);
                    // g must end up running b -> a: forward(g) xor flip(g) != fForward.
                    const int need = (gForward == fForward) ? 1 : 0;
                    if (flip[g] == -1) {
                        flip[g] = need;
                        open.push(g);
                    } else if (flip[g] != need) {
                        return false;
                    }
                }
            }
        }
    }
    for (std::size_t f = 0; f < faces.size(); ++f) {
        if (flip[f] == 1) std::reverse(faces[f].begin(), faces[f].end());
    }
    return true;
}

/// Gives each fan of faces around a vertex its own vertex (the first fan keeps the original).
std::size_t splitNonManifoldVertices(std::vector<Vec3d>& positions, std::vector<Face>& faces) {
    std::vector<std::vector<std::uint32_t>> around(positions.size());
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        for (std::uint32_t v : faces[f]) around[v].push_back(f);
    }
    std::size_t split = 0;
    const auto originalCount = static_cast<std::uint32_t>(positions.size());
    for (std::uint32_t v = 0; v < originalCount; ++v) {
        const auto& incident = around[v];
        if (incident.size() < 2) continue;
        // Union faces that share an edge at v (the same neighbour vertex next to v).
        std::vector<std::uint32_t> parent(incident.size());
        std::iota(parent.begin(), parent.end(), 0u);
        auto root = [&parent](std::uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::unordered_map<std::uint32_t, std::uint32_t> seen;
        for (std::uint32_t k = 0; k < incident.size(); ++k) {
            const Face& face = faces[incident[k]];
            const auto at =
                static_cast<std::size_t>(std::find(face.begin(), face.end(), v) - face.begin());
            for (std::uint32_t w :
                 {face[(at + 1) % face.size()], face[(at + face.size() - 1) % face.size()]}) {
                const auto [it, inserted] = seen.emplace(w, k);
                if (!inserted) parent[root(k)] = root(it->second);
            }
        }
        std::unordered_map<std::uint32_t, std::uint32_t> copyOfFan;
        const std::uint32_t firstFan = root(0);
        for (std::uint32_t k = 0; k < incident.size(); ++k) {
            const std::uint32_t fan = root(k);
            if (fan == firstFan) continue;
            auto [it, inserted] = copyOfFan.emplace(fan, 0u);
            if (inserted) {
                it->second = static_cast<std::uint32_t>(positions.size());
                positions.push_back(positions[v]);
                ++split;
            }
            for (std::uint32_t& u : faces[incident[k]]) {
                if (u == v) u = it->second;
            }
        }
    }
    return split;
}

/// Closes each loop of boundary edges with one polygon running the other way.
void fillLoops(std::vector<Face>& faces, CageCleanup& report) {
    std::unordered_map<std::uint64_t, bool> halfEdges;
    for (const Face& face : faces) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            halfEdges[directed(face[i], face[(i + 1) % face.size()])] = true;
        }
    }
    // Every vertex is a single fan now, so each boundary vertex starts exactly one boundary edge.
    std::unordered_map<std::uint32_t, std::uint32_t> next;
    std::vector<std::uint32_t> starts;
    for (const Face& face : faces) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % face.size()];
            if (!halfEdges.contains(directed(b, a))) {
                next[a] = b;
                starts.push_back(a);
            }
        }
    }
    std::unordered_map<std::uint32_t, bool> used;
    for (std::uint32_t start : starts) {
        if (used[start]) continue;
        Face loop;
        for (std::uint32_t v = start; !used[v]; v = next.at(v)) {
            used[v] = true;
            loop.push_back(v);
        }
        if (loop.size() < 3) continue;
        std::reverse(loop.begin(), loop.end());
        report.largestLoop = std::max(report.largestLoop, loop.size());
        ++report.filledLoops;
        faces.push_back(std::move(loop));
    }
}

/// Removes vertices that only two faces share (repeatedly, as removing one can make another).
std::size_t dissolveValenceTwo(std::size_t vertexCount, std::vector<Face>& faces) {
    std::size_t dissolved = 0;
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<std::vector<std::uint32_t>> around(vertexCount);
        for (std::uint32_t f = 0; f < faces.size(); ++f) {
            for (std::uint32_t v : faces[f]) around[v].push_back(f);
        }
        std::vector<bool> dropFace(faces.size(), false);
        std::vector<bool> touched(faces.size(), false);
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            if (around[v].size() != 2) continue;
            const std::uint32_t f1 = around[v][0];
            const std::uint32_t f2 = around[v][1];
            if (touched[f1] || touched[f2]) continue; // one change per face per pass
            const bool small1 = faces[f1].size() == 3;
            const bool small2 = faces[f2].size() == 3;
            if (small1 && small2) continue; // two triangles on the same three vertices
            for (std::uint32_t f : {f1, f2}) {
                std::erase(faces[f], v);
                if (faces[f].size() < 3) dropFace[f] = true;
                touched[f] = true;
            }
            ++dissolved;
            changed = true;
        }
        std::vector<Face> kept;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (!dropFace[f]) kept.push_back(std::move(faces[f]));
        }
        faces = std::move(kept);
    }
    return dissolved;
}

Result<QuadLayout> assemble(const std::vector<Vec3d>& positions, const std::vector<Face>& faces) {
    std::vector<std::uint32_t> remap(positions.size(), UINT32_MAX);
    std::vector<Vec3d> used;
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> vertices;
    for (const Face& face : faces) {
        for (std::uint32_t v : face) {
            if (remap[v] == UINT32_MAX) {
                remap[v] = static_cast<std::uint32_t>(used.size());
                used.push_back(positions[v]);
            }
            vertices.push_back(remap[v]);
        }
        starts.push_back(static_cast<std::uint32_t>(vertices.size()));
    }
    return QuadLayout::create(std::move(used), std::move(starts), std::move(vertices));
}

} // namespace

Result<QuadLayout> closeCage(const QuadLayout& raw, CageCleanup& report) {
    report = {};
    std::vector<Vec3d> positions = raw.positions();
    std::vector<Face> faces;
    faces.reserve(raw.faceCount());
    for (std::size_t f = 0; f < raw.faceCount(); ++f) {
        const auto face = raw.face(f);
        faces.emplace_back(face.begin(), face.end());
    }

    report.droppedFaces = dropNonManifoldEdgeFaces(faces);
    if (faces.empty()) return fail(ErrorCode::InvalidState, "no manifold faces in the cage");
    keepLargestComponent(faces, report);
    if (!orientConsistently(faces)) {
        return fail(ErrorCode::InvalidState, "the quad layout cannot be oriented consistently");
    }
    report.splitVertices = splitNonManifoldVertices(positions, faces);
    fillLoops(faces, report);
    report.dissolvedVertices = dissolveValenceTwo(positions.size(), faces);

    auto cage = assemble(positions, faces);
    if (!cage) return cage;
    if (signedVolume(*cage) < 0.0) {
        for (Face& face : faces) std::reverse(face.begin(), face.end());
        report.flipped = true;
        cage = assemble(positions, faces);
        if (!cage) return cage;
    }
    const CageStats stats = cageStats(*cage);
    if (!stats.closed() || stats.components != 1) {
        return fail(ErrorCode::InvalidState,
                    "the quad layout could not be closed: " + std::to_string(stats.boundaryEdges) +
                        " open, " + std::to_string(stats.nonManifoldEdges) + " non-manifold and " +
                        std::to_string(stats.misorientedEdges) + " misoriented edges, " +
                        std::to_string(stats.nonManifoldVertices) + " non-manifold vertices, " +
                        std::to_string(stats.components) + " parts");
    }
    return cage;
}

Result<QuadRemeshResult> quadRemesh(const TriMesh& scan, const QuadRemeshParams& params) {
    auto raw = quad::remesh(scan, params);
    if (!raw) return std::unexpected(raw.error());
    CageCleanup cleanup;
    auto cage = closeCage(*raw, cleanup);
    if (!cage) return std::unexpected(cage.error());

    const SpatialIndex& index = scan.spatialIndex();
    double maxDistance = 0.0;
    double sum = 0.0;
    for (const Vec3d& p : cage->positions()) {
        const double d = index.closestPoint(p).distance;
        maxDistance = std::max(maxDistance, d);
        sum += d;
    }
    const CageStats stats = cageStats(*cage);
    const double mean = sum / static_cast<double>(cage->vertexCount());
    return QuadRemeshResult{std::move(*cage), cleanup, stats, maxDistance, mean};
}

} // namespace m2c

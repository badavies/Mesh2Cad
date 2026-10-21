// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/QuadRemesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numeric>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "m2c/adapters/quad/QuadRemesh.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

using Face = std::vector<std::uint32_t>;

std::uint64_t undirected(std::uint32_t a, std::uint32_t b) noexcept {
    if (a > b) std::swap(a, b);
    return (std::uint64_t{a} << 32U) | b;
}

std::uint64_t directed(const std::uint32_t a, const std::uint32_t b) noexcept {
    return (std::uint64_t{a} << 32U) | b;
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
bool runs(const Face& face, const std::uint32_t a, const std::uint32_t b) {
    for (std::size_t i = 0; i < face.size(); ++i) {
        if (face[i] == a) return face[(i + 1) % face.size()] == b;
    }
    return false;
}

std::size_t dropNonManifoldEdgeFaces(std::vector<Face>& faces) {
    const auto edges = edgeFaces(faces);
    std::vector drop(faces.size(), false);
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

/// The edge-connected parts, the one with the most faces first (the lowest-numbered on a tie),
/// then the others in the order of their first face.
std::vector<std::vector<Face>> splitComponents(std::vector<Face> faces) {
    const auto edges = edgeFaces(faces);
    std::vector component(faces.size(), UINT32_MAX);
    std::vector<std::vector<Face>> parts;
    for (std::uint32_t seed = 0; seed < faces.size(); ++seed) {
        if (component[seed] != UINT32_MAX) continue;
        const auto id = static_cast<std::uint32_t>(parts.size());
        std::vector<std::uint32_t> members;
        std::queue<std::uint32_t> open;
        open.push(seed);
        component[seed] = id;
        while (!open.empty()) {
            const std::uint32_t f = open.front();
            open.pop();
            members.push_back(f);
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
        std::ranges::sort(members);
        parts.emplace_back();
        for (const std::uint32_t f : members) parts.back().push_back(std::move(faces[f]));
    }
    const auto main = std::ranges::max_element(
        parts, [](const auto& a, const auto& b) { return a.size() < b.size(); });
    std::rotate(parts.begin(), main, main + 1);
    return parts;
}

/// Flips faces so each edge is run in opposite directions by its two faces. False if that is
/// impossible (a non-orientable surface).
bool orientConsistently(std::vector<Face>& faces) {
    const auto edges = edgeFaces(faces);
    std::vector flip(faces.size(), -1);
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
                    const int need = gForward == fForward ? 1 : 0;
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
        if (flip[f] == 1) std::ranges::reverse(faces[f]);
    }
    return true;
}

/// Gives each fan of faces around a vertex its own vertex (the first fan keeps the original).
std::size_t splitNonManifoldVertices(std::vector<Vec3d>& positions, std::vector<Face>& faces) {
    std::vector<std::vector<std::uint32_t>> around(positions.size());
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        for (const std::uint32_t v : faces[f]) around[v].push_back(f);
    }
    std::size_t split = 0;
    const auto originalCount = static_cast<std::uint32_t>(positions.size());
    for (std::uint32_t v = 0; v < originalCount; ++v) {
        const auto& incident = around[v];
        if (incident.size() < 2) continue;
        // Union faces that share an edge at v (the same neighbour vertex next to v).
        std::vector<std::uint32_t> parent(incident.size());
        std::ranges::iota(parent, 0u);
        auto root = [&parent](std::uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::unordered_map<std::uint32_t, std::uint32_t> seen;
        for (std::uint32_t k = 0; k < incident.size(); ++k) {
            const Face& face = faces[incident[k]];
            for (const auto at =
                     static_cast<std::size_t>(std::ranges::find(face, v) - face.begin());
                 std::uint32_t w :
                 {face[(at + 1) % face.size()], face[(at + face.size() - 1) % face.size()]}) {
                if (const auto [it, inserted] = seen.emplace(w, k); !inserted)
                    parent[root(k)] = root(it->second);
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

/// Closes each loop of boundary edges with one polygon running the other way, apart from the
/// longest when keepLongest is set.
void fillLoops(std::vector<Face>& faces, CageCleanup& report, const bool keepLongest) {
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
            const std::uint32_t from = face[i];
            if (const std::uint32_t to = face[(i + 1) % face.size()];
                !halfEdges.contains(directed(to, from))) {
                next[from] = to;
                starts.push_back(from);
            }
        }
    }
    std::unordered_map<std::uint32_t, bool> used;
    std::vector<Face> loops;
    for (std::uint32_t start : starts) {
        if (used[start]) continue;
        Face loop;
        for (std::uint32_t v = start; !used[v]; v = next.at(v)) {
            used[v] = true;
            loop.push_back(v);
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    if (keepLongest && !loops.empty()) {
        // The first longest, so the choice does not depend on anything but the loop order.
        const auto longest = std::ranges::max_element(
            loops, [](const Face& a, const Face& b) { return a.size() < b.size(); });
        loops.erase(longest);
    }
    for (Face& loop : loops) {
        std::ranges::reverse(loop);
        report.largestLoop = std::max(report.largestLoop, loop.size());
        ++report.filledLoops;
        faces.push_back(std::move(loop));
    }
}

/// Whether v, shared by faces a and b only, lies on an open edge: inside a cage the two faces
/// share both of v's edges, so v has the same two neighbours in each.
bool onOutline(const Face& a, const Face& b, const std::uint32_t v) {
    const auto neighbours = [v](const Face& f) {
        const auto at = static_cast<std::size_t>(std::ranges::find(f, v) - f.begin());
        const std::uint32_t p = f[(at + f.size() - 1) % f.size()];
        const std::uint32_t q = f[(at + 1) % f.size()];
        return std::pair{std::min(p, q), std::max(p, q)};
    };
    return neighbours(a) != neighbours(b);
}

/// Removes vertices that only two faces share (repeatedly, as removing one can make another).
std::size_t dissolveValenceTwo(const std::size_t vertexCount, std::vector<Face>& faces) {
    std::size_t dissolved = 0;
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<std::vector<std::uint32_t>> around(vertexCount);
        for (std::uint32_t f = 0; f < faces.size(); ++f) {
            for (const std::uint32_t v : faces[f]) around[v].push_back(f);
        }
        std::vector dropFace(faces.size(), false);
        std::vector touched(faces.size(), false);
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            if (around[v].size() != 2) continue;
            const std::uint32_t f1 = around[v][0];
            const std::uint32_t f2 = around[v][1];
            if (onOutline(faces[f1], faces[f2], v)) continue;
            if (touched[f1] || touched[f2]) continue; // one change per face per pass
            const bool small1 = faces[f1].size() == 3;
            // Two triangles on the same three vertices.
            if (const bool small2 = faces[f2].size() == 3; small1 && small2) continue;
            for (const std::uint32_t f : {f1, f2}) {
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
    std::vector remap(positions.size(), UINT32_MAX);
    std::vector<Vec3d> used;
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> vertices;
    for (const Face& face : faces) {
        for (const std::uint32_t v : face) {
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

double volumeOf(const std::vector<Vec3d>& p, const std::vector<Face>& faces) {
    double volume = 0.0;
    for (const Face& face : faces) {
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            volume += dot(p[face[0]], cross(p[face[i]], p[face[i + 1]]));
        }
    }
    return volume / 6.0;
}

/// Orients a part, splits its non-manifold vertices, fills its open loops (all but the longest
/// with keepOutline), dissolves its valence-2 vertices and points it outward (closed parts only).
/// False if it cannot be oriented.
bool closePart(std::vector<Vec3d>& positions,
               std::vector<Face>& faces,
               CageCleanup& report,
               const bool keepOutline = false) {
    if (!orientConsistently(faces)) return false;
    report.splitVertices += splitNonManifoldVertices(positions, faces);
    fillLoops(faces, report, keepOutline);
    report.dissolvedVertices += dissolveValenceTwo(positions.size(), faces);
    if (!keepOutline && volumeOf(positions, faces) < 0.0) {
        for (Face& face : faces) std::ranges::reverse(face);
        report.flipped = true;
    }
    return true;
}

bool everyEdgeShared(const std::vector<Face>& faces) {
    const auto edges = edgeFaces(faces);
    return !faces.empty() &&
           std::ranges::all_of(edges, [](const auto& e) { return e.second.size() == 2; });
}

double meanEdgeLength(const std::vector<Vec3d>& p, const std::vector<Face>& faces) {
    double sum = 0.0;
    std::size_t count = 0;
    for (const Face& face : faces) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            sum += length(p[face[(i + 1) % face.size()]] - p[face[i]]);
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

struct Frame {
    Vec3d centre;
    Vec3d normal; ///< Unnormalised (Newell); points outward.
};

Frame frameOf(const std::vector<Vec3d>& p, const Face& face) {
    Frame out{};
    for (std::size_t i = 0; i < face.size(); ++i) {
        out.centre = out.centre + p[face[i]] * (1.0 / static_cast<double>(face.size()));
        out.normal = out.normal + cross(p[face[i]], p[face[(i + 1) % face.size()]]);
    }
    return out;
}

/// Joins a closed part to the body with a tube of quads from a quad of the body to the nearest
/// quad of the part facing it, in rings about `edge` apart (a thin feature such as a post whose
/// stem the remesher left out). False when no facing pair is closer than twice the part's size.
bool bridge(std::vector<Vec3d>& positions,
            std::vector<Face>& body,
            std::vector<Face> part,
            const double edge) {
    Vec3d lo = positions[part.front().front()], hi = lo;
    for (const Face& face : part) {
        for (const std::uint32_t v : face) {
            const auto& [x, y, z] = positions[v];
            lo = {.x = std::min(lo.x, x), .y = std::min(lo.y, y), .z = std::min(lo.z, z)};
            hi = {.x = std::max(hi.x, x), .y = std::max(hi.y, y), .z = std::max(hi.z, z)};
        }
    }
    double best = 2.0 * length(hi - lo);
    std::size_t from = SIZE_MAX, to = SIZE_MAX;
    std::vector<Frame> bodyFrames(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) bodyFrames[i] = frameOf(positions, body[i]);
    for (std::size_t j = 0; j < part.size(); ++j) {
        if (part[j].size() != 4) continue;
        const auto [partCentre, partNormal] = frameOf(positions, part[j]);
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i].size() != 4) continue;
            const auto& [bodyCentre, bodyNormal] = bodyFrames[i];
            const Vec3d gap = partCentre - bodyCentre;
            if (const double d = length(gap);
                d < best && dot(bodyNormal, gap) > 0.0 && dot(partNormal, gap) < 0.0) {
                best = d;
                from = i;
                to = j;
            }
        }
    }
    if (from == SIZE_MAX) return false;
    const Face a = body[from], b = part[to];
    // Ring vertex i of the body quad meets vertex (shift - i) of the part quad, which runs the
    // other way round the tube; take the shift with the shortest connections.
    std::size_t shift = 0;
    double shortest = 0.0;
    for (std::size_t s = 0; s < 4; ++s) {
        double total = 0.0;
        for (std::size_t i = 0; i < 4; ++i)
            total += length(positions[b[(s + 4 - i) % 4]] - positions[a[i]]);
        if (s == 0 || total < shortest) {
            shortest = total;
            shift = s;
        }
    }
    const auto steps =
        static_cast<std::size_t>(std::max(1L, std::lround(best / std::max(edge, 1e-12))));
    std::vector<Face> rings{a};
    for (std::size_t t = 1; t < steps; ++t) {
        Face ring;
        const double w = static_cast<double>(t) / static_cast<double>(steps);
        for (std::size_t i = 0; i < 4; ++i) {
            const Vec3d& p = positions[a[i]];
            const Vec3d& q = positions[b[(shift + 4 - i) % 4]];
            ring.push_back(static_cast<std::uint32_t>(positions.size()));
            positions.push_back(p + (q - p) * w);
        }
        rings.push_back(std::move(ring));
    }
    Face last;
    for (std::size_t i = 0; i < 4; ++i) last.push_back(b[(shift + 4 - i) % 4]);
    rings.push_back(std::move(last));
    body.erase(body.begin() + static_cast<std::ptrdiff_t>(from));
    part.erase(part.begin() + static_cast<std::ptrdiff_t>(to));
    for (Face& face : part) body.push_back(std::move(face));
    for (std::size_t t = 0; t + 1 < rings.size(); ++t) {
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t k = (i + 1) % 4;
            body.push_back({rings[t][i], rings[t][k], rings[t + 1][k], rings[t + 1][i]});
        }
    }
    return true;
}

/// An open cage has no inside, so it faces the way the scan faces: each face votes, weighted by
/// its area, with the normal of the closest scan triangle. Reverses all faces if the vote is
/// against.
Result<QuadLayout> faceLikeScan(const QuadLayout& cage, const TriMesh& scan, bool& flipped) {
    const SpatialIndex& index = scan.spatialIndex();
    const std::vector<Vec3d>& p = cage.positions();
    double vote = 0.0;
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        Vec3d normal{};
        Vec3d centre{};
        for (std::size_t i = 0; i < face.size(); ++i) {
            normal = normal + cross(p[face[i]], p[face[(i + 1) % face.size()]]);
            centre = centre + p[face[i]];
        }
        const auto tri =
            scan.face(index.closestPoint(centre * (1.0 / static_cast<double>(face.size()))).face);
        const Vec3d& a = scan.positions()[tri[0]];
        const Vec3d scanNormal = cross(scan.positions()[tri[1]] - a, scan.positions()[tri[2]] - a);
        if (const double size = length(scanNormal); size > 0.0) {
            vote += dot(normal, scanNormal) / size;
        }
    }
    flipped = vote < 0.0;
    if (!flipped) return cage;
    std::vector<std::uint32_t> vertices;
    vertices.reserve(cage.faceVertices().size());
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        vertices.insert(vertices.end(), face.rbegin(), face.rend());
    }
    return QuadLayout::create(p, cage.faceStarts(), std::move(vertices));
}

} // namespace

Result<QuadLayout>
closeCage(const QuadLayout& raw, CageCleanup& report, const CageCloseParams& params) {
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
    auto parts = splitComponents(std::move(faces));
    std::vector<Face> body = std::move(parts.front());
    if (!closePart(positions, body, report, params.keepOutline)) {
        return fail(ErrorCode::InvalidState, "the quad layout cannot be oriented consistently");
    }
    const double edge = meanEdgeLength(positions, body);
    for (std::size_t k = 1; k < parts.size(); ++k) {
        const std::size_t size = parts[k].size();
        CageCleanup partReport;
        const bool joined = params.joinParts && size >= params.minPartFaces &&
                            closePart(positions, parts[k], partReport) &&
                            everyEdgeShared(parts[k]) &&
                            bridge(positions, body, std::move(parts[k]), edge);
        if (joined) {
            ++report.joinedComponents;
            report.joinedComponentFaces += size;
        } else {
            ++report.droppedComponents;
            report.droppedComponentFaces += size;
        }
    }
    auto cage = assemble(positions, body);
    if (!cage) return cage;
    if (const CageStats stats = cageStats(*cage);
        !(params.keepOutline ? stats.orientedManifold() : stats.closed()) ||
        stats.components != 1) {
        return fail(ErrorCode::InvalidState,
                    "the quad layout could not be closed: " + std::to_string(stats.boundaryEdges) +
                        " open, " + std::to_string(stats.nonManifoldEdges) + " non-manifold and " +
                        std::to_string(stats.misorientedEdges) + " misoriented edges, " +
                        std::to_string(stats.nonManifoldVertices) + " non-manifold vertices, " +
                        std::to_string(stats.components) + " parts");
    }
    return cage;
}

Result<QuadRemeshResult> quadRemesh(const TriMesh& scan,
                                    const QuadRemeshParams& params,
                                    const CageCloseParams& close,
                                    const Progress& progress) {
    report(progress, "remeshing into quads");
    auto raw = quad::remesh(scan, params);
    if (!raw) return std::unexpected(raw.error());
    report(progress, "closing the cage");
    CageCleanup cleanup;
    auto cage = closeCage(*raw, cleanup, close);
    if (!cage) return std::unexpected(cage.error());
    if (close.keepOutline) {
        cage = faceLikeScan(*cage, scan, cleanup.flipped);
        if (!cage) return std::unexpected(cage.error());
    }

    report(progress, "measuring the cage against the scan");
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
    return QuadRemeshResult{.cage = std::move(*cage),
                            .cleanup = cleanup,
                            .stats = stats,
                            .maxDistanceMm = maxDistance,
                            .meanDistanceMm = mean};
}

} // namespace m2c

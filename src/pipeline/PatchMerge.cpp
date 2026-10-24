// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/PatchMerge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

using EdgeKey = std::uint64_t;

EdgeKey key(std::uint32_t a, std::uint32_t b) noexcept {
    if (a > b) std::swap(a, b);
    return (std::uint64_t{a} << 32U) | b;
}

/// Edge and vertex adjacency of a closed cage.
struct Topology {
    const QuadLayout& cage;
    std::unordered_map<EdgeKey, std::array<std::uint32_t, 2>> edgeFaces;
    std::vector<std::vector<std::uint32_t>> neighbours; ///< Per vertex, in first-seen order.
    std::vector<bool> singular;                         ///< Not used by exactly four quads.

    explicit Topology(const QuadLayout& c) : cage(c) {
        neighbours.resize(cage.vertexCount());
        std::vector<std::uint32_t> uses(cage.vertexCount(), 0);
        singular.assign(cage.vertexCount(), false);
        for (std::uint32_t f = 0; f < cage.faceCount(); ++f) {
            const auto face = cage.face(f);
            for (std::size_t i = 0; i < face.size(); ++i) {
                const std::uint32_t a = face[i], b = face[(i + 1) % face.size()];
                if (auto [it, added] = edgeFaces.try_emplace(key(a, b), std::array{f, f}); added) {
                    neighbours[a].push_back(b);
                    neighbours[b].push_back(a);
                } else {
                    it->second[1] = f;
                }
                ++uses[a];
                if (face.size() != 4) singular[a] = true;
            }
        }
        for (std::size_t v = 0; v < uses.size(); ++v) singular[v] = singular[v] || uses[v] != 4;
    }

    std::uint32_t otherFace(const EdgeKey e, const std::uint32_t f) const {
        const auto& two = edgeFaces.at(e);
        return two[0] == f ? two[1] : two[0];
    }

    /// The vertex of face f next to v that is not w.
    std::uint32_t
    otherNeighbour(const std::uint32_t f, const std::uint32_t v, const std::uint32_t w) const {
        const auto face = cage.face(f);
        const std::size_t n = face.size();
        const std::size_t k = static_cast<std::size_t>(std::ranges::find(face, v) - face.begin());
        const std::uint32_t next = face[(k + 1) % n], prev = face[(k + n - 1) % n];
        return next == w ? prev : next;
    }

    /// At a regular vertex v, the end of the edge from v straight across from the edge to w.
    std::uint32_t straightOn(const std::uint32_t v, const std::uint32_t w) const {
        const std::uint32_t f1 = edgeFaces.at(key(v, w))[0];
        const std::uint32_t x = otherNeighbour(f1, v, w);
        const std::uint32_t f2 = otherFace(key(v, x), f1);
        return otherNeighbour(f2, v, x);
    }
};

/// Index of vertex v in face f.
std::uint8_t cornerOf(std::span<const std::uint32_t> face, const std::uint32_t v) {
    return static_cast<std::uint8_t>(std::ranges::find(face, v) - face.begin());
}

/// Lays out the region holding `seed` as a grid, starting from a face with a corner of the
/// region. Empty when the region is not a rectangle.
QuadRectangle layOut(const Topology& topo,
                     const std::unordered_set<EdgeKey>& cut,
                     const std::vector<std::uint32_t>& region) {
    const QuadLayout& cage = topo.cage;
    const auto isCut = [&](const std::uint32_t a, const std::uint32_t b) {
        return cut.contains(key(a, b));
    };
    // A corner: a vertex of a face both of whose edges there are partition lines.
    std::uint32_t start = UINT32_MAX;
    std::uint8_t startCorner = 0;
    for (const std::uint32_t f : region) {
        const auto face = cage.face(f);
        for (std::size_t k = 0; k < 4 && start == UINT32_MAX; ++k) {
            if (isCut(face[(k + 3) % 4], face[k]) && isCut(face[k], face[(k + 1) % 4])) {
                start = f;
                startCorner = static_cast<std::uint8_t>(k);
            }
        }
        if (start != UINT32_MAX) break;
    }
    if (start == UINT32_MAX) return {};
    struct Cell {
        int i, j;
        std::uint8_t r;
    };
    std::unordered_map<std::uint32_t, Cell> cells;
    std::queue<std::uint32_t> open;
    cells[start] = {.i = 0, .j = 0, .r = startCorner};
    open.push(start);
    int maxI = 0, maxJ = 0;
    while (!open.empty()) {
        const std::uint32_t f = open.front();
        open.pop();
        const auto [ci, cj, cr] = cells.at(f);
        const auto face = cage.face(f);
        const auto at = [&](const int k) { return face[static_cast<std::size_t>((cr + k) % 4)]; };
        const std::uint32_t c00 = at(0), c10 = at(1), c11 = at(2), c01 = at(3);
        // Each side: its ends, the neighbour's cell and which of the ends is that cell's corner
        // at offset `shift` from its own (0, 0).
        struct Side {
            std::uint32_t a, b;
            int di, dj;
            std::uint32_t anchor;
            int shift;
        };
        const std::array sides{
            Side{.a = c10, .b = c11, .di = 1, .dj = 0, .anchor = c10, .shift = 0},
            Side{.a = c11, .b = c01, .di = 0, .dj = 1, .anchor = c01, .shift = 0},
            Side{.a = c01, .b = c00, .di = -1, .dj = 0, .anchor = c00, .shift = 3},
            Side{.a = c00, .b = c10, .di = 0, .dj = -1, .anchor = c00, .shift = 1}};
        for (const auto& [a, b, di, dj, anchor, shift] : sides) {
            if (isCut(a, b)) continue;
            const std::uint32_t g = topo.otherFace(key(a, b), f);
            const auto other = cage.face(g);
            const auto r = static_cast<std::uint8_t>((cornerOf(other, anchor) + shift) % 4);
            const Cell next{.i = ci + di, .j = cj + dj, .r = r};
            if (const auto [it, added] = cells.try_emplace(g, next); !added) {
                if (it->second.i != next.i || it->second.j != next.j || it->second.r != next.r) {
                    return {}; // the region wraps around onto itself
                }
                continue;
            }
            if (next.i < 0 || next.j < 0) return {}; // the start was not a corner after all
            maxI = std::max(maxI, next.i);
            maxJ = std::max(maxJ, next.j);
            open.push(g);
        }
    }
    QuadRectangle rect;
    rect.cellsU = static_cast<std::uint32_t>(maxI + 1);
    rect.cellsV = static_cast<std::uint32_t>(maxJ + 1);
    if (std::size_t{rect.cellsU} * rect.cellsV != region.size()) return {};
    rect.faces.assign(region.size(), UINT32_MAX);
    rect.rotations.assign(region.size(), 0);
    for (const auto& [f, c] : cells) {
        const auto k = static_cast<std::size_t>(c.j) * rect.cellsU + static_cast<std::size_t>(c.i);
        if (rect.faces[k] != UINT32_MAX) return {};
        rect.faces[k] = f;
        rect.rotations[k] = c.r;
    }
    return rect;
}

/// The bicubic g with g(a, b) = f(a, b) at the 16 points (i / 3, j / 3): exact for a bicubic f.
template <class F>
BicubicPatch interpolate(F f) {
    // Inverse of the cubic Bernstein weights at 0, 1/3, 2/3 and 1.
    static constexpr std::array<std::array<double, 4>, 4> k{{{1.0, 0.0, 0.0, 0.0},
                                                             {-5.0 / 6.0, 3.0, -1.5, 1.0 / 3.0},
                                                             {1.0 / 3.0, -1.5, 3.0, -5.0 / 6.0},
                                                             {0.0, 0.0, 0.0, 1.0}}};
    std::array<Vec3d, 16> values;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i)
            values[4 * static_cast<std::size_t>(j) + static_cast<std::size_t>(i)] =
                f(i / 3.0, j / 3.0);
    }
    BicubicPatch out;
    for (std::size_t l = 0; l < 4; ++l) {
        for (std::size_t m = 0; m < 4; ++m) {
            Vec3d p{};
            for (std::size_t j = 0; j < 4; ++j) {
                for (std::size_t i = 0; i < 4; ++i) {
                    p = p + values[4 * j + i] * (k[m][i] * k[l][j]);
                }
            }
            out.poles[4 * l + m] = p;
        }
    }
    return out;
}

/// Cell parameters (a, b) to the parameters of a face whose corner r is the cell's (0, 0).
std::pair<double, double> rotate(const std::uint8_t r, double a, double b) {
    switch (r) {
    case 1:
        return {1.0 - b, a};
    case 2:
        return {1.0 - a, 1.0 - b};
    case 3:
        return {b, 1.0 - a};
    default:
        return {a, b};
    }
}

SurfacePatch merge(const QuadRectangle& rect, const std::vector<const SurfacePatch*>& cells) {
    // Every column (row) is split into as many equal pieces as its finest face has.
    std::vector<std::size_t> cols(rect.cellsU, 1), rows(rect.cellsV, 1);
    for (std::uint32_t j = 0; j < rect.cellsV; ++j) {
        for (std::uint32_t i = 0; i < rect.cellsU; ++i) {
            const std::size_t n = cells[j * rect.cellsU + i]->piecesU();
            cols[i] = std::max(cols[i], n);
            rows[j] = std::max(rows[j], n);
        }
    }
    const auto breaks = [](const std::vector<std::size_t>& pieces) {
        std::vector out{0.0};
        const auto n = static_cast<double>(pieces.size());
        for (std::size_t c = 0; c < pieces.size(); ++c) {
            for (std::size_t p = 1; p <= pieces[c]; ++p) {
                out.push_back(c + 1 == pieces.size() && p == pieces[c]
                                  ? 1.0
                                  : (static_cast<double>(c) +
                                     static_cast<double>(p) / static_cast<double>(pieces[c])) /
                                        n);
            }
        }
        return out;
    };
    SurfacePatch out;
    out.cageFace = rect.faces.front();
    out.cellsU = rect.cellsU;
    out.cellsV = rect.cellsV;
    out.uBreaks = breaks(cols);
    out.vBreaks = breaks(rows);
    const std::size_t row = out.polesPerRow();
    out.poles.resize(row * out.poleRows());
    std::size_t v0 = 0;
    for (std::uint32_t j = 0; j < rect.cellsV; ++j) {
        std::size_t u0 = 0;
        for (std::uint32_t i = 0; i < rect.cellsU; ++i) {
            const SurfacePatch& face = *cells[j * rect.cellsU + i];
            const std::uint8_t r = rect.rotations[j * rect.cellsU + i];
            const std::size_t su = cols[i], sv = rows[j];
            out.capPieces += face.capPieces * (su * sv) / (face.piecesU() * face.piecesV());
            for (std::size_t q = 0; q < sv; ++q) {
                for (std::size_t p = 0; p < su; ++p) {
                    // The face piece holding this sub-piece, from its centre.
                    const auto [cs, ct] =
                        rotate(r,
                               (static_cast<double>(p) + 0.5) / static_cast<double>(su),
                               (static_cast<double>(q) + 0.5) / static_cast<double>(sv));
                    const std::size_t pi = pieceAt(face.uBreaks, cs).first;
                    const std::size_t pj = pieceAt(face.vBreaks, ct).first;
                    const BicubicPatch piece = face.piece(pi, pj);
                    const double ws = face.uBreaks[pi + 1] - face.uBreaks[pi];
                    const double wt = face.vBreaks[pj + 1] - face.vBreaks[pj];
                    const auto [poles] = interpolate([&](const double a, const double b) {
                        const auto [s, t] =
                            rotate(r,
                                   (static_cast<double>(p) + a) / static_cast<double>(su),
                                   (static_cast<double>(q) + b) / static_cast<double>(sv));
                        return evaluate(
                            piece, (s - face.uBreaks[pi]) / ws, (t - face.vBreaks[pj]) / wt);
                    });
                    for (std::size_t y = 0; y < 4; ++y) {
                        for (std::size_t x = 0; x < 4; ++x) {
                            out.poles[(3 * (v0 + q) + y) * row + 3 * (u0 + p) + x] =
                                poles[4 * y + x];
                        }
                    }
                }
            }
            u0 += su;
        }
        v0 += rows[j];
    }
    return out;
}

} // namespace

QuadPartition partitionQuads(const QuadLayout& cage,
                             const std::vector<std::array<std::uint32_t, 2>>& fences) {
    const Topology topo(cage);
    std::unordered_set<EdgeKey> cut;
    for (std::uint32_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        if (face.size() == 4) continue;
        for (std::size_t i = 0; i < face.size(); ++i)
            cut.insert(key(face[i], face[(i + 1) % face.size()]));
    }
    const auto touchesCut = [&](const std::uint32_t v, const std::uint32_t except) {
        for (const std::uint32_t w : topo.neighbours[v]) {
            if (w != except && cut.contains(key(v, w))) return true;
        }
        return false;
    };
    // A regular vertex where fences meet other than as one straight line is a corner too: at a
    // part's corner three sharp edges meet, and the faces on each side turn there although the
    // quads run straight on. Lines leave it along the edges that are not fences.
    std::unordered_set<EdgeKey> fenced;
    std::vector<std::vector<std::uint32_t>> fencesAt(cage.vertexCount());
    for (const auto& [a, b] : fences) {
        if (fenced.insert(key(a, b)).second) {
            fencesAt[a].push_back(b);
            fencesAt[b].push_back(a);
        }
    }
    std::vector<bool> corner = topo.singular;
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        if (const auto& at = fencesAt[v]; !corner[v] && !at.empty())
            corner[v] = at.size() != 2 || topo.straightOn(v, at[0]) != at[1];
    }
    QuadPartition out;
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        if (!corner[v]) continue;
        for (const std::uint32_t first : topo.neighbours[v]) {
            if (cut.contains(key(v, first))) continue;
            if (!topo.singular[v] && fenced.contains(key(v, first))) continue;
            ++out.lines;
            std::uint32_t prev = v, cur = first;
            cut.insert(key(prev, cur));
            while (!corner[cur] && !touchesCut(cur, prev)) {
                const std::uint32_t next = topo.straightOn(cur, prev);
                cut.insert(key(cur, next));
                prev = std::exchange(cur, next);
            }
        }
    }
    cut.insert(fenced.begin(), fenced.end());
    // Regions: quads joined across edges that are not partition lines or fences.
    std::vector region(cage.faceCount(), UINT32_MAX);
    for (std::uint32_t seed = 0; seed < cage.faceCount(); ++seed) {
        if (cage.face(seed).size() != 4 || region[seed] != UINT32_MAX) continue;
        std::vector members{seed};
        region[seed] = seed;
        for (std::size_t k = 0; k < members.size(); ++k) {
            const auto face = cage.face(members[k]);
            for (std::size_t i = 0; i < 4; ++i) {
                const EdgeKey e = key(face[i], face[(i + 1) % 4]);
                if (cut.contains(e)) continue;
                if (const std::uint32_t g = topo.otherFace(e, members[k]);
                    region[g] == UINT32_MAX) {
                    region[g] = seed;
                    members.push_back(g);
                }
            }
        }
        if (QuadRectangle rect = layOut(topo, cut, members); !rect.faces.empty()) {
            out.rectangles.push_back(std::move(rect));
            continue;
        }
        out.unmergedQuads += members.size();
        std::ranges::sort(members);
        for (std::uint32_t f : members)
            out.rectangles.push_back({.cellsU = 1, .cellsV = 1, .faces = {f}, .rotations = {0}});
    }
    return out;
}

Result<MergeResult> mergeFaces(const QuadLayout& cage,
                               const std::vector<SurfacePatch>& faces,
                               const std::vector<std::array<std::uint32_t, 2>>& fences) {
    std::vector faceOf(cage.faceCount(), UINT32_MAX);
    for (std::uint32_t k = 0; k < faces.size(); ++k) {
        const std::uint32_t c = faces[k].cageFace;
        if (c >= cage.faceCount()) {
            return fail(ErrorCode::InvalidArgument, "a face belongs to no face of the cage");
        }
        if (cage.face(c).size() == 4) {
            if (faceOf[c] != UINT32_MAX || faces[k].piecesU() != faces[k].piecesV() ||
                faces[k].cellsU != 1 || faces[k].cellsV != 1) {
                return fail(ErrorCode::InvalidArgument, "faces must be one per cage quad");
            }
            faceOf[c] = k;
        }
    }
    MergeResult out;
    out.partition = partitionQuads(cage, fences);
    // Faces in the order of their first cage face: a rectangle comes where its first face was.
    std::vector rectOf(cage.faceCount(), SIZE_MAX);
    for (std::size_t r = 0; r < out.partition.rectangles.size(); ++r) {
        for (const std::uint32_t f : out.partition.rectangles[r].faces) {
            if (faceOf[f] == UINT32_MAX) {
                return fail(ErrorCode::InvalidArgument, "a cage quad has no face");
            }
            rectOf[f] = r;
        }
    }
    std::vector done(out.partition.rectangles.size(), false);
    for (const SurfacePatch& face : faces) {
        const std::size_t r = rectOf[face.cageFace];
        if (r == SIZE_MAX) {
            out.faces.push_back(face);
            continue;
        }
        if (done[r]) continue;
        done[r] = true;
        const QuadRectangle& rect = out.partition.rectangles[r];
        if (rect.faces.size() == 1) {
            out.faces.push_back(face);
            continue;
        }
        std::vector<const SurfacePatch*> cells;
        cells.reserve(rect.faces.size());
        for (const std::uint32_t f : rect.faces) cells.push_back(&faces[faceOf[f]]);
        out.faces.push_back(merge(rect, cells));
        ++out.mergedFaces;
    }
    return out;
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/PatchMerge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace m2c {

namespace {

using EdgeKey = std::uint64_t;

EdgeKey key(std::uint32_t a, std::uint32_t b) noexcept {
    if (a > b) std::swap(a, b);
    return (std::uint64_t{a} << 32) | b;
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
                auto [it, added] = edgeFaces.try_emplace(key(a, b), std::array{f, f});
                if (added) {
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

    std::uint32_t otherFace(EdgeKey e, std::uint32_t f) const {
        const auto& two = edgeFaces.at(e);
        return two[0] == f ? two[1] : two[0];
    }

    /// The vertex of face f next to v that is not w.
    std::uint32_t otherNeighbour(std::uint32_t f, std::uint32_t v, std::uint32_t w) const {
        const auto face = cage.face(f);
        const std::size_t n = face.size();
        const std::size_t k =
            static_cast<std::size_t>(std::find(face.begin(), face.end(), v) - face.begin());
        const std::uint32_t next = face[(k + 1) % n], prev = face[(k + n - 1) % n];
        return next == w ? prev : next;
    }

    /// At a regular vertex v, the end of the edge from v straight across from the edge to w.
    std::uint32_t straightOn(std::uint32_t v, std::uint32_t w) const {
        const std::uint32_t f1 = edgeFaces.at(key(v, w))[0];
        const std::uint32_t x = otherNeighbour(f1, v, w);
        const std::uint32_t f2 = otherFace(key(v, x), f1);
        return otherNeighbour(f2, v, x);
    }
};

/// Index of vertex v in face f.
std::uint8_t cornerOf(std::span<const std::uint32_t> face, std::uint32_t v) {
    return static_cast<std::uint8_t>(std::find(face.begin(), face.end(), v) - face.begin());
}

/// Lays out the region holding `seed` as a grid, starting from a face with a corner of the
/// region. Empty when the region is not a rectangle.
QuadRectangle layOut(const Topology& topo,
                     const std::unordered_set<EdgeKey>& cut,
                     const std::vector<std::uint32_t>& region) {
    const QuadLayout& cage = topo.cage;
    const auto isCut = [&](std::uint32_t a, std::uint32_t b) { return cut.contains(key(a, b)); };
    // A corner: a vertex of a face both of whose edges there are partition lines.
    std::uint32_t start = UINT32_MAX;
    std::uint8_t startCorner = 0;
    for (std::uint32_t f : region) {
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
    cells[start] = {0, 0, startCorner};
    open.push(start);
    int maxI = 0, maxJ = 0;
    while (!open.empty()) {
        const std::uint32_t f = open.front();
        open.pop();
        const Cell c = cells.at(f);
        const auto face = cage.face(f);
        const auto at = [&](int k) { return face[static_cast<std::size_t>((c.r + k) % 4)]; };
        const std::uint32_t c00 = at(0), c10 = at(1), c11 = at(2), c01 = at(3);
        // Each side: its ends, the neighbour's cell and which of the ends is that cell's corner
        // at offset `shift` from its own (0, 0).
        const struct {
            std::uint32_t a, b;
            int di, dj;
            std::uint32_t anchor;
            int shift;
        } sides[4] = {{c10, c11, 1, 0, c10, 0},
                      {c11, c01, 0, 1, c01, 0},
                      {c01, c00, -1, 0, c00, 3},
                      {c00, c10, 0, -1, c00, 1}};
        for (const auto& s : sides) {
            if (isCut(s.a, s.b)) continue;
            const std::uint32_t g = topo.otherFace(key(s.a, s.b), f);
            const auto other = cage.face(g);
            const auto r = static_cast<std::uint8_t>((cornerOf(other, s.anchor) + s.shift) % 4);
            const Cell next{c.i + s.di, c.j + s.dj, r};
            const auto [it, added] = cells.try_emplace(g, next);
            if (!added) {
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
    static constexpr double k[4][4] = {{1.0, 0.0, 0.0, 0.0},
                                       {-5.0 / 6.0, 3.0, -1.5, 1.0 / 3.0},
                                       {1.0 / 3.0, -1.5, 3.0, -5.0 / 6.0},
                                       {0.0, 0.0, 0.0, 1.0}};
    std::array<Vec3d, 16> values;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i)
            values[static_cast<std::size_t>(4 * j + i)] = f(i / 3.0, j / 3.0);
    }
    BicubicPatch out;
    for (int l = 0; l < 4; ++l) {
        for (int m = 0; m < 4; ++m) {
            Vec3d p{};
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    p = p + values[static_cast<std::size_t>(4 * j + i)] * (k[m][i] * k[l][j]);
                }
            }
            out.poles[static_cast<std::size_t>(4 * l + m)] = p;
        }
    }
    return out;
}

/// Cell parameters (a, b) to the parameters of a face whose corner r is the cell's (0, 0).
std::pair<double, double> rotate(std::uint8_t r, double a, double b) {
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
        std::vector<double> out{0.0};
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
                        rotate(r, (double(p) + 0.5) / double(su), (double(q) + 0.5) / double(sv));
                    const std::size_t pi = pieceAt(face.uBreaks, cs).first;
                    const std::size_t pj = pieceAt(face.vBreaks, ct).first;
                    const BicubicPatch piece = face.piece(pi, pj);
                    const double ws = face.uBreaks[pi + 1] - face.uBreaks[pi];
                    const double wt = face.vBreaks[pj + 1] - face.vBreaks[pj];
                    const BicubicPatch sub = interpolate([&](double a, double b) {
                        const auto [s, t] =
                            rotate(r, (double(p) + a) / double(su), (double(q) + b) / double(sv));
                        return evaluate(
                            piece, (s - face.uBreaks[pi]) / ws, (t - face.vBreaks[pj]) / wt);
                    });
                    for (std::size_t y = 0; y < 4; ++y) {
                        for (std::size_t x = 0; x < 4; ++x) {
                            out.poles[(3 * (v0 + q) + y) * row + 3 * (u0 + p) + x] =
                                sub.poles[4 * y + x];
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

QuadPartition partitionQuads(const QuadLayout& cage) {
    const Topology topo(cage);
    std::unordered_set<EdgeKey> cut;
    for (std::uint32_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        if (face.size() == 4) continue;
        for (std::size_t i = 0; i < face.size(); ++i)
            cut.insert(key(face[i], face[(i + 1) % face.size()]));
    }
    const auto touchesCut = [&](std::uint32_t v, std::uint32_t except) {
        for (std::uint32_t w : topo.neighbours[v]) {
            if (w != except && cut.contains(key(v, w))) return true;
        }
        return false;
    };
    QuadPartition out;
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        if (!topo.singular[v]) continue;
        for (std::uint32_t first : topo.neighbours[v]) {
            if (cut.contains(key(v, first))) continue;
            ++out.lines;
            std::uint32_t prev = v, cur = first;
            cut.insert(key(prev, cur));
            while (!topo.singular[cur] && !touchesCut(cur, prev)) {
                const std::uint32_t next = topo.straightOn(cur, prev);
                cut.insert(key(cur, next));
                prev = std::exchange(cur, next);
            }
        }
    }
    // Regions: quads joined across edges that are not partition lines.
    std::vector<std::uint32_t> region(cage.faceCount(), UINT32_MAX);
    for (std::uint32_t seed = 0; seed < cage.faceCount(); ++seed) {
        if (cage.face(seed).size() != 4 || region[seed] != UINT32_MAX) continue;
        std::vector<std::uint32_t> members{seed};
        region[seed] = seed;
        for (std::size_t k = 0; k < members.size(); ++k) {
            const auto face = cage.face(members[k]);
            for (std::size_t i = 0; i < 4; ++i) {
                const EdgeKey e = key(face[i], face[(i + 1) % 4]);
                if (cut.contains(e)) continue;
                const std::uint32_t g = topo.otherFace(e, members[k]);
                if (region[g] == UINT32_MAX) {
                    region[g] = seed;
                    members.push_back(g);
                }
            }
        }
        QuadRectangle rect = layOut(topo, cut, members);
        if (!rect.faces.empty()) {
            out.rectangles.push_back(std::move(rect));
            continue;
        }
        out.unmergedQuads += members.size();
        std::sort(members.begin(), members.end());
        for (std::uint32_t f : members) out.rectangles.push_back({1, 1, {f}, {0}});
    }
    return out;
}

Result<MergeResult> mergeFaces(const QuadLayout& cage, const std::vector<SurfacePatch>& faces) {
    std::vector<std::uint32_t> faceOf(cage.faceCount(), UINT32_MAX);
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
    out.partition = partitionQuads(cage);
    // Faces in the order of their first cage face: a rectangle comes where its first face was.
    std::vector<std::size_t> rectOf(cage.faceCount(), SIZE_MAX);
    for (std::size_t r = 0; r < out.partition.rectangles.size(); ++r) {
        for (std::uint32_t f : out.partition.rectangles[r].faces) {
            if (faceOf[f] == UINT32_MAX) {
                return fail(ErrorCode::InvalidArgument, "a cage quad has no face");
            }
            rectOf[f] = r;
        }
    }
    std::vector<bool> done(out.partition.rectangles.size(), false);
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
        for (std::uint32_t f : rect.faces) cells.push_back(&faces[faceOf[f]]);
        out.faces.push_back(merge(rect, cells));
        ++out.mergedFaces;
    }
    return out;
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Solid.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace m2c {

namespace {

struct EdgeSample {
    Vec3d p, n; // position, unit normal (zero where the surface is degenerate)
    std::size_t face;
};

Vec3d unit(const Vec3d& v) {
    const double l = length(v);
    return l > 0.0 ? v * (1.0 / l) : Vec3d{};
}

/// The piece of a face holding parameter x along one direction, and x in that piece.
std::pair<std::size_t, double> pieceAt(const SurfacePatch& face, double x) {
    const double n = static_cast<double>(face.spans);
    const std::size_t i = std::min(static_cast<std::size_t>(std::max(x * n, 0.0)), face.spans - 1);
    return {i, x * n - static_cast<double>(i)};
}

Vec3d unitNormal(const SurfacePatch& face, double u, double v) {
    const auto [i, s] = pieceAt(face, u);
    const auto [j, t] = pieceAt(face, v);
    return unit(normal(face.piece(i, j), s, t));
}

double angleDeg(const Vec3d& a, const Vec3d& b) {
    if (a == Vec3d{} || b == Vec3d{}) return 0.0;
    return std::acos(std::clamp(dot(a, b), -1.0, 1.0)) * 180.0 / std::numbers::pi;
}

} // namespace

Result<SeamReport>
measureSeams(const std::vector<SurfacePatch>& faces, int samplesPerEdge, double toleranceMm) {
    if (samplesPerEdge < 1 || samplesPerEdge > 64 || !(toleranceMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "samples per edge must be 1 to 64 and the tolerance positive");
    }
    // A cage face with other than four sides has one face per corner, each spanning half of
    // the cage edges it touches; a whole quad samples its edges twice as densely so that its
    // samples land on theirs.
    std::unordered_map<std::uint32_t, std::size_t> perCageFace;
    for (const SurfacePatch& face : faces) ++perCageFace[face.cageFace];
    SeamReport out;
    std::vector<EdgeSample> samples;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const SurfacePatch& face = faces[f];
        const int k = perCageFace[face.cageFace] == 1 ? 2 * samplesPerEdge : samplesPerEdge;
        for (int i = 0; i <= k; ++i) {
            const double s = static_cast<double>(i) / k;
            for (const auto& [u, v] : {std::pair{s, 0.0}, {s, 1.0}, {0.0, s}, {1.0, s}}) {
                samples.push_back({evaluate(face, u, v), unitNormal(face, u, v), f});
            }
        }
        // Knot lines inside the face: the pieces on either side share their poles.
        const int kk = samplesPerEdge;
        for (std::size_t a = 0; a < face.spans; ++a) {
            for (std::size_t b = 1; b < face.spans; ++b) {
                for (int i = 0; i <= kk; ++i) {
                    const double s = static_cast<double>(i) / kk;
                    out.maxNormalAngleDeg =
                        std::max({out.maxNormalAngleDeg,
                                  angleDeg(unit(normal(face.piece(a, b - 1), s, 1.0)),
                                           unit(normal(face.piece(a, b), s, 0.0))),
                                  angleDeg(unit(normal(face.piece(b - 1, a), 1.0, s)),
                                           unit(normal(face.piece(b, a), 0.0, s)))});
                }
            }
        }
    }
    // Hash samples on a grid of cells one tolerance wide; matches lie in neighbouring cells.
    using Cell = std::array<long long, 3>;
    const auto cell = [&](const Vec3d& p) {
        return Cell{std::llround(p.x / toleranceMm),
                    std::llround(p.y / toleranceMm),
                    std::llround(p.z / toleranceMm)};
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const noexcept {
            const auto h = [](long long x) { return std::hash<long long>{}(x); };
            return h(c[0]) ^ (h(c[1]) * 0x9e3779b97f4a7c15ULL) ^ (h(c[2]) * 0xc2b2ae3d27d4eb4fULL);
        }
    };
    std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid;
    for (std::size_t i = 0; i < samples.size(); ++i) grid[cell(samples[i].p)].push_back(i);
    for (const EdgeSample& a : samples) {
        const Cell c = cell(a.p);
        bool found = false;
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
                    if (it == grid.end()) continue;
                    for (std::size_t j : it->second) {
                        const EdgeSample& b = samples[j];
                        const double gap = length(a.p - b.p);
                        if (b.face == a.face || gap > toleranceMm) continue;
                        found = true;
                        out.maxGapMm = std::max(out.maxGapMm, gap);
                        out.maxNormalAngleDeg = std::max(out.maxNormalAngleDeg, angleDeg(a.n, b.n));
                    }
                }
            }
        }
        if (!found) ++out.unmatchedSamples;
    }
    return out;
}

Result<TriMesh> tessellate(const std::vector<SurfacePatch>& faces, int n) {
    if (n < 1 || n > 64) return fail(ErrorCode::InvalidArgument, "grid must be 1 to 64");
    const auto row = static_cast<std::uint32_t>(n + 1);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    positions.reserve(faces.size() * row * row);
    indices.reserve(faces.size() * static_cast<std::size_t>(6 * n * n));
    for (const SurfacePatch& face : faces) {
        const auto base = static_cast<std::uint32_t>(positions.size());
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                positions.push_back(
                    evaluate(face, static_cast<double>(i) / n, static_cast<double>(j) / n));
            }
        }
        for (std::uint32_t j = 0; j + 1 < row; ++j) {
            for (std::uint32_t i = 0; i + 1 < row; ++i) {
                const std::uint32_t v = base + j * row + i;
                indices.insert(indices.end(), {v, v + 1, v + row + 1, v, v + row + 1, v + row});
            }
        }
    }
    return TriMesh::create(std::move(positions), std::move(indices));
}

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Solid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numbers>
#include <unordered_map>
#include <utility>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

struct EdgeSample {
    Vec3d p, n; // position, unit normal (zero where the surface is degenerate)
    std::size_t face;
    bool cap;   // on a cap piece
    bool sharp; // on a sharp edge
};

Vec3d unit(const Vec3d& v) {
    const double l = length(v);
    return l > 0.0 ? v * (1.0 / l) : Vec3d{};
}

Vec3d unitNormal(const SurfacePatch& face, const double u, const double v) {
    const auto [i, s] = pieceAt(face.uBreaks, u);
    const auto [j, t] = pieceAt(face.vBreaks, v);
    return unit(normal(face.piece(i, j), s, t));
}

double angleDeg(const Vec3d& a, const Vec3d& b) {
    if (a == Vec3d{} || b == Vec3d{}) return 0.0;
    return std::acos(std::clamp(dot(a, b), -1.0, 1.0)) * 180.0 / std::numbers::pi;
}

} // namespace

Result<SeamReport> measureSeams(const std::vector<SurfacePatch>& faces,
                                const int samplesPerEdge,
                                const double toleranceMm,
                                const std::vector<std::vector<bool>>& caps,
                                const std::vector<std::array<bool, 4>>& sharp) {
    if (samplesPerEdge < 1 || samplesPerEdge > 64 || !(toleranceMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "samples per edge must be 1 to 64 and the tolerance positive");
    }
    if (!caps.empty()) {
        bool fits = caps.size() == faces.size();
        for (std::size_t f = 0; fits && f < faces.size(); ++f) {
            fits = caps[f].size() == faces[f].piecesU() * faces[f].piecesV();
        }
        if (!fits) return fail(ErrorCode::InvalidArgument, "the cap flags do not fit the faces");
    }
    if (!sharp.empty() && sharp.size() != faces.size()) {
        return fail(ErrorCode::InvalidArgument, "the sharp-edge flags do not fit the faces");
    }
    const auto isCap = [&](const std::size_t f, const std::size_t i, const std::size_t j) {
        return !caps.empty() && caps[f][j * faces[f].piecesU() + i];
    };
    // A cage face with other than four sides has one face per corner, each spanning half of
    // the cage edges it touches; a whole quad samples its edges twice as densely so that its
    // samples land on theirs.
    std::unordered_map<std::uint32_t, std::size_t> perCageFace;
    for (const SurfacePatch& face : faces) ++perCageFace[face.cageFace];
    SeamReport out;
    const auto record = [&out](const double angle, const bool cap) {
        out.maxNormalAngleDeg = std::max(out.maxNormalAngleDeg, angle);
        double& split = cap ? out.maxCapNormalAngleDeg : out.maxRegularNormalAngleDeg;
        split = std::max(split, angle);
    };
    std::vector<EdgeSample> samples;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const SurfacePatch& face = faces[f];
        const int k = perCageFace[face.cageFace] == 1 ? 2 * samplesPerEdge : samplesPerEdge;
        const std::array<bool, 4> edgeSharp = sharp.empty() ? std::array<bool, 4>{} : sharp[f];
        for (int i = 0; i <= k; ++i) {
            const double s = static_cast<double>(i) / k;
            const std::array<std::pair<double, double>, 4> at{
                std::pair{s, 0.0}, {s, 1.0}, {0.0, s}, {1.0, s}};
            for (std::size_t e = 0; e < 4; ++e) {
                const auto [u, v] = at[e];
                // A corner lies on two edges; it is on a sharp edge if either is.
                const bool onSharp = edgeSharp[e] || (u == 0.0 && edgeSharp[2]) ||
                                     (u == 1.0 && edgeSharp[3]) || (v == 0.0 && edgeSharp[0]) ||
                                     (v == 1.0 && edgeSharp[1]);
                samples.push_back(
                    {.p = evaluate(face, u, v),
                     .n = unitNormal(face, u, v),
                     .face = f,
                     .cap =
                         isCap(f, pieceAt(face.uBreaks, u).first, pieceAt(face.vBreaks, v).first),
                     .sharp = onSharp});
            }
        }
        // Knot lines inside the face: the pieces on either side share their poles.
        const int kk = samplesPerEdge;
        for (int i = 0; i <= kk; ++i) {
            const double s = static_cast<double>(i) / kk;
            for (std::size_t a = 0; a < face.piecesU(); ++a) {
                for (std::size_t b = 1; b < face.piecesV(); ++b) {
                    record(angleDeg(unit(normal(face.piece(a, b - 1), s, 1.0)),
                                    unit(normal(face.piece(a, b), s, 0.0))),
                           isCap(f, a, b - 1) || isCap(f, a, b));
                }
            }
            for (std::size_t b = 0; b < face.piecesV(); ++b) {
                for (std::size_t a = 1; a < face.piecesU(); ++a) {
                    record(angleDeg(unit(normal(face.piece(a - 1, b), 1.0, s)),
                                    unit(normal(face.piece(a, b), 0.0, s))),
                           isCap(f, a - 1, b) || isCap(f, a, b));
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
            const auto h = [](const long long x) { return std::hash<long long>{}(x); };
            return h(c[0]) ^ h(c[1]) * 0x9e3779b97f4a7c15ULL ^ h(c[2]) * 0xc2b2ae3d27d4eb4fULL;
        }
    };
    std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid;
    for (std::size_t i = 0; i < samples.size(); ++i) grid[cell(samples[i].p)].push_back(i);
    for (const auto& [pointA, normalA, faceA, capA, sharpA] : samples) {
        const Cell c = cell(pointA);
        bool found = false;
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
                    if (it == grid.end()) continue;
                    for (const std::size_t j : it->second) {
                        const auto& [pointB, normalB, faceB, capB, sharpB] = samples[j];
                        const double gap = length(pointA - pointB);
                        if (faceB == faceA || gap > toleranceMm) continue;
                        found = true;
                        out.maxGapMm = std::max(out.maxGapMm, gap);
                        if (!sharpA && !sharpB) record(angleDeg(normalA, normalB), capA || capB);
                    }
                }
            }
        }
        if (!found) ++out.unmatchedSamples;
    }
    return out;
}

Result<FaceTessellation> tessellate(const std::vector<SurfacePatch>& faces, const int n) {
    if (n < 1 || n > 64) return fail(ErrorCode::InvalidArgument, "grid must be 1 to 64");
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::array<double, 2>> uvs;
    std::vector<std::uint32_t> triangleFaces;
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        const SurfacePatch& face = faces[f];
        const std::uint32_t nu = static_cast<std::uint32_t>(n) * face.cellsU;
        const std::uint32_t nv = static_cast<std::uint32_t>(n) * face.cellsV;
        const auto base = static_cast<std::uint32_t>(positions.size());
        for (std::uint32_t j = 0; j <= nv; ++j) {
            for (std::uint32_t i = 0; i <= nu; ++i) {
                const double u = static_cast<double>(i) / nu, v = static_cast<double>(j) / nv;
                positions.push_back(evaluate(face, u, v));
                uvs.push_back({u, v});
            }
        }
        const std::uint32_t row = nu + 1;
        for (std::uint32_t j = 0; j < nv; ++j) {
            for (std::uint32_t i = 0; i < nu; ++i) {
                const std::uint32_t v = base + j * row + i;
                indices.insert(indices.end(), {v, v + 1, v + row + 1, v, v + row + 1, v + row});
                triangleFaces.insert(triangleFaces.end(), {f, f});
            }
        }
    }
    auto mesh = TriMesh::create(std::move(positions), std::move(indices));
    if (!mesh) return std::unexpected(mesh.error());
    return FaceTessellation{.mesh = std::move(*mesh),
                            .triangleFaces = std::move(triangleFaces),
                            .vertexUvs = std::move(uvs)};
}

} // namespace m2c

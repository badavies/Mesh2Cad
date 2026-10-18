// SPDX-License-Identifier: GPL-3.0-or-later
// Turning the limit surface into bicubic patches (ARCHITECTURE.md section 6, step 3).
#include "m2c/adapters/osd/Subdivision.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <Eigen/Dense>

#include "Limit.h"
#include "Tessellation.h"

namespace m2c::osd {

namespace {

constexpr int kCapFit = 8;    // samples per side for a cap's interior poles
constexpr int kCapCheck = 10; // samples per side for a cap's error

/// The bicubic through f at (i / 3, j / 3): exact when f is a bicubic polynomial.
template <class F>
BicubicPatch interpolate(F f) {
    static const Eigen::Matrix4d binv = [] {
        Eigen::Matrix4d b;
        for (int a = 0; a < 4; ++a) {
            const auto w = bernstein(a / 3.0);
            for (int k = 0; k < 4; ++k) b(a, k) = w[static_cast<std::size_t>(k)];
        }
        return Eigen::Matrix4d(b.inverse());
    }();
    // f(s_i, t_j) = sum B_ik B_jl X_kl, so X = B^-1 V B^-T per coordinate.
    Eigen::Matrix4d vx, vy, vz;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            const Vec3d q = f(i / 3.0, j / 3.0);
            vx(i, j) = q.x;
            vy(i, j) = q.y;
            vz(i, j) = q.z;
        }
    }
    const Eigen::Matrix4d x = binv * vx * binv.transpose();
    const Eigen::Matrix4d y = binv * vy * binv.transpose();
    const Eigen::Matrix4d z = binv * vz * binv.transpose();
    BicubicPatch patch;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            patch.poles[static_cast<std::size_t>(4 * j + i)] = {x(i, j), y(i, j), z(i, j)};
    }
    return patch;
}

/// Refits the four interior poles of a cap to f by least squares, keeping its boundary rows,
/// which interpolate() already made exact for a cubic boundary. The fit matches f's positions
/// inside the cap and its tangent planes along the cap's edges, which f shares with the
/// neighbouring pieces, so the normals across each seam nearly agree.
template <class F>
void fitCapInterior(BicubicPatch& patch, F f) {
    constexpr std::array<std::size_t, 4> interior{5, 6, 9, 10};
    constexpr double h = 1e-6; // step for derivatives of f
    BicubicPatch boundary = patch;
    for (std::size_t k : interior) boundary.poles[k] = {};
    const auto rest = [&](double s, double t) { return f(s, t) - evaluate(boundary, s, t); };
    // Unknowns: x, y and z of each interior pole. Rows: three per position sample, one per
    // edge sample for the derivative across the edge along f's normal there.
    constexpr int positions = kCapFit * kCapFit;
    Eigen::Matrix<double, 3 * positions + 4 * kCapFit, 12> design;
    Eigen::Matrix<double, 3 * positions + 4 * kCapFit, 1> target;
    design.setZero();
    int r = 0;
    for (int k = 0; k < positions; ++k) {
        const double s = (k % kCapFit + 0.5) / kCapFit;
        const double t = (k / kCapFit + 0.5) / kCapFit;
        const Vec3d want = rest(s, t);
        const auto bu = bernstein(s);
        const auto bv = bernstein(t);
        for (int c = 0; c < 3; ++c, ++r) {
            target(r) = c == 0 ? want.x : c == 1 ? want.y : want.z;
            for (std::size_t q = 0; q < 4; ++q) {
                design(r, static_cast<Eigen::Index>(3 * q) + c) =
                    bu[interior[q] % 4] * bv[interior[q] / 4];
            }
        }
    }
    // Edges t = 0, t = 1, s = 0 and s = 1. Of the interior poles only the row next to an edge
    // moves the derivative across it, by b_1'(0) = 3 or b_2'(1) = -3.
    for (int edge = 0; edge < 4; ++edge) {
        const bool alongU = edge < 2;
        const bool first = edge % 2 == 0;
        const double side = first ? 0.0 : 1.0;
        const double step = first ? h : -h; // from inside the cap
        for (int k = 0; k < kCapFit; ++k, ++r) {
            const double a = (k + 0.5) / kCapFit;
            const double s0 = alongU ? a : side, t0 = alongU ? side : a;
            const auto at = [&](auto g, double ds, double dt) { return g(s0 + ds, t0 + dt); };
            const Vec3d across = alongU ? (at(rest, 0.0, step) - at(rest, 0.0, 0.0)) * (1.0 / step)
                                        : (at(rest, step, 0.0) - at(rest, 0.0, 0.0)) * (1.0 / step);
            const double da = a < 0.5 ? h : -h; // along the edge, staying inside it
            const Vec3d fa =
                alongU ? at(f, da, 0.0) - at(f, 0.0, 0.0) : at(f, 0.0, da) - at(f, 0.0, 0.0);
            const Vec3d fc =
                alongU ? at(f, 0.0, step) - at(f, 0.0, 0.0) : at(f, step, 0.0) - at(f, 0.0, 0.0);
            Vec3d n = cross(fa, fc);
            const double len = length(n);
            if (!(len > 0.0)) continue; // the row stays zero
            n = n * (1.0 / len);
            target(r) = dot(n, across) / 3.0;
            const auto along = bernstein(a);
            const std::size_t next = first ? 1 : 2;
            for (std::size_t q = 0; q < 4; ++q) {
                const std::size_t i = interior[q] % 4, j = interior[q] / 4;
                if ((alongU ? j : i) != next) continue;
                const double w = (first ? 1.0 : -1.0) * along[alongU ? i : j];
                for (int c = 0; c < 3; ++c) {
                    design(r, static_cast<Eigen::Index>(3 * q) + c) = w * (c == 0   ? n.x
                                                                           : c == 1 ? n.y
                                                                                    : n.z);
                }
            }
        }
    }
    const Eigen::Matrix<double, 12, 1> fit =
        (design.transpose() * design).ldlt().solve(design.transpose() * target);
    for (std::size_t q = 0; q < 4; ++q) {
        const auto i = static_cast<Eigen::Index>(3 * q);
        patch.poles[interior[q]] = {fit(i), fit(i + 1), fit(i + 2)};
    }
}

} // namespace

Result<PatchResult> extractPatches(const QuadLayout& cage, const PatchParams& params) {
    auto limit = Limit::create(cage, params.isolationLevel);
    if (!limit) return std::unexpected(limit.error());
    const Limit& lim = **limit;
    const std::vector<Vec3d> points = lim.patchPoints(cage.positions());

    PatchResult result;
    // Bezier form of each OpenSubdiv patch.
    std::vector<BicubicPatch> pieces(lim.patchCount());
    for (std::size_t p = 0; p < lim.patchCount(); ++p) {
        const auto limitAt = [&](double s, double t) { return lim.evaluate(points, p, s, t); };
        pieces[p] = interpolate(limitAt);
        if (lim.isRegular(p)) {
            ++result.regularPieces;
            continue;
        }
        fitCapInterior(pieces[p], limitAt);
        ++result.capPieces;
        for (int i = 0; i <= kCapCheck; ++i) {
            for (int j = 0; j <= kCapCheck; ++j) {
                const double s = double(i) / kCapCheck, t = double(j) / kCapCheck;
                result.maxCapErrorMm = std::max(result.maxCapErrorMm,
                                                length(limitAt(s, t) - evaluate(pieces[p], s, t)));
            }
        }
    }

    // One uniform grid of pieces per ptex face; coarser patches are split to the finest size.
    std::vector<std::vector<std::size_t>> byFace(lim.ptexFaceCount());
    for (std::size_t p = 0; p < lim.patchCount(); ++p) byFace[lim.domain(p).ptexFace].push_back(p);
    result.faces.reserve(byFace.size());
    for (std::uint32_t f = 0; f < byFace.size(); ++f) {
        double finest = 1.0;
        for (std::size_t p : byFace[f]) finest = std::min(finest, lim.domain(p).size);
        SurfacePatch face;
        face.cageFace = lim.cageFace(f);
        face.spans = static_cast<std::size_t>(std::lround(1.0 / finest));
        const std::size_t row = face.polesPerRow();
        face.poles.resize(row * row);
        std::size_t covered = 0;
        for (std::size_t p : byFace[f]) {
            const PatchDomain& d = lim.domain(p);
            const auto k = static_cast<std::size_t>(std::lround(d.size / finest));
            const auto i0 = static_cast<std::size_t>(std::lround(d.u0 / finest));
            const auto j0 = static_cast<std::size_t>(std::lround(d.v0 / finest));
            for (std::size_t a = 0; a < k; ++a) {
                for (std::size_t b = 0; b < k; ++b) {
                    const BicubicPatch piece =
                        k == 1 ? pieces[p] : interpolate([&](double s, double t) {
                            return evaluate(pieces[p],
                                            (double(a) + s) / double(k),
                                            (double(b) + t) / double(k));
                        });
                    for (std::size_t r = 0; r < 4; ++r) {
                        for (std::size_t c = 0; c < 4; ++c) {
                            face.poles[(3 * (j0 + b) + r) * row + 3 * (i0 + a) + c] =
                                piece.poles[4 * r + c];
                        }
                    }
                }
            }
            covered += k * k;
            if (!lim.isRegular(p)) face.capPieces += k * k;
        }
        if (covered != face.spans * face.spans) {
            return fail(ErrorCode::Internal,
                        "OpenSubdiv patches do not tile ptex face " + std::to_string(f));
        }
        result.faces.push_back(std::move(face));
    }
    return result;
}

Result<TriMesh> tessellateLimit(const QuadLayout& cage, int isolationLevel, int n) {
    if (n < 1 || n > 64) return fail(ErrorCode::InvalidArgument, "grid must be 1 to 64");
    auto limit = Limit::create(cage, isolationLevel);
    if (!limit) return std::unexpected(limit.error());
    auto tess = tessellate(**limit, (*limit)->patchPoints(cage.positions()), n);
    if (!tess) return std::unexpected(tess.error());
    return std::move(tess->mesh);
}

} // namespace m2c::osd

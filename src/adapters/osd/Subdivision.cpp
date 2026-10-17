// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/adapters/osd/Subdivision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include "m2c/core/SpatialIndex.h"

#include "Limit.h"

namespace m2c::osd {

namespace {

/// A tessellation of the limit surface that remembers where each vertex lies: vertex
/// (n + 1)^2 * patch + (n + 1) * j + i is at (s, t) = (i / n, j / n) of that patch.
struct Tessellation {
    TriMesh mesh;
    int n{0};
};

Result<Tessellation> tessellate(const Limit& limit, const std::vector<Vec3d>& points, int n) {
    const auto row = static_cast<std::uint32_t>(n + 1);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices;
    positions.reserve(limit.patchCount() * row * row);
    indices.reserve(limit.patchCount() * static_cast<std::size_t>(6 * n * n));
    for (std::size_t p = 0; p < limit.patchCount(); ++p) {
        const auto base = static_cast<std::uint32_t>(positions.size());
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                positions.push_back(limit.evaluate(points, p, double(i) / n, double(j) / n));
            }
        }
        for (std::uint32_t j = 0; j < row - 1; ++j) {
            for (std::uint32_t i = 0; i < row - 1; ++i) {
                const std::uint32_t v00 = base + j * row + i;
                indices.insert(indices.end(),
                               {v00, v00 + 1, v00 + row + 1, v00, v00 + row + 1, v00 + row});
            }
        }
    }
    auto mesh = TriMesh::create(std::move(positions), std::move(indices));
    if (!mesh) return std::unexpected(mesh.error());
    return Tessellation{std::move(*mesh), n};
}

struct SurfacePoint {
    std::size_t patch{0};
    double s{0.0};
    double t{0.0};
    double distance{0.0};
    Vec3d normal; ///< Of the tessellation triangle, unit length (zero if degenerate).
};

/// Closest point of the tessellated limit surface, as patch parameters.
SurfacePoint locate(const Tessellation& tess, const Vec3d& q) {
    const ClosestPoint cp = tess.mesh.spatialIndex().closestPoint(q);
    const auto [ia, ib, ic] = tess.mesh.face(cp.face);
    const auto& p = tess.mesh.positions();
    // Barycentric coordinates of the closest point in its triangle.
    const Vec3d e0 = p[ib] - p[ia];
    const Vec3d e1 = p[ic] - p[ia];
    const Vec3d d = cp.point - p[ia];
    const double d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1);
    const double det = d00 * d11 - d01 * d01;
    double b1 = 0.0, b2 = 0.0;
    if (det > 0.0) {
        b1 = std::clamp((d11 * dot(d, e0) - d01 * dot(d, e1)) / det, 0.0, 1.0);
        b2 = std::clamp((d00 * dot(d, e1) - d01 * dot(d, e0)) / det, 0.0, 1.0 - b1);
    }
    const auto row = static_cast<std::uint32_t>(tess.n + 1);
    const std::uint32_t perPatch = row * row;
    const auto st = [&](std::uint32_t v) {
        const std::uint32_t k = v % perPatch;
        return std::pair{double(k % row) / tess.n, double(k / row) / tess.n};
    };
    const auto [sa, ta] = st(ia);
    const auto [sb, tb] = st(ib);
    const auto [sc, tc] = st(ic);
    const double b0 = 1.0 - b1 - b2;
    const Vec3d n = cross(e0, e1);
    const double len = length(n);
    return {ia / perPatch,
            b0 * sa + b1 * sb + b2 * sc,
            b0 * ta + b1 * tb + b2 * tc,
            cp.distance,
            len > 0.0 ? n * (1.0 / len) : Vec3d{}};
}

using SparseMatrix = Eigen::SparseMatrix<double>;

/// A matrix entry; SparseMatrix stores int indices, so the narrowing is spelled out here.
Eigen::Triplet<double> entry(Eigen::Index row, Eigen::Index col, double value) {
    return {static_cast<int>(row), static_cast<int>(col), value};
}

/// Umbrella Laplacian of the cage's edge graph.
SparseMatrix laplacian(const QuadLayout& cage) {
    std::vector<std::set<std::uint32_t>> neighbours(cage.vertexCount());
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % face.size()];
            neighbours[a].insert(b);
            neighbours[b].insert(a);
        }
    }
    std::vector<Eigen::Triplet<double>> entries;
    for (std::uint32_t v = 0; v < cage.vertexCount(); ++v) {
        const auto& ring = neighbours[v];
        entries.emplace_back(v, v, 1.0);
        for (std::uint32_t w : ring) entries.emplace_back(v, w, -1.0 / double(ring.size()));
    }
    const auto n = static_cast<Eigen::Index>(cage.vertexCount());
    SparseMatrix l(n, n);
    l.setFromTriplets(entries.begin(), entries.end());
    return l;
}

constexpr int kSearchGrid = 4;       // tessellation per patch for the closest-point search
constexpr double kPointWeight = 0.1; // share of point-to-point distance in the fit
constexpr int kCapFit = 8;           // samples per side for a cap's interior poles
constexpr int kCapCheck = 10;        // samples per side for a cap's error

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

Result<CageFitResult>
fitCage(const QuadLayout& cage, const TriMesh& scan, const CageFitParams& params) {
    if (params.smoothing < 0.0 || params.maxSampleDistanceMm < 0.0 || params.maxSamples < 1 ||
        params.iterations < 1) {
        return fail(ErrorCode::InvalidArgument,
                    "smoothing and distance must be at least 0; samples and iterations at least 1");
    }
    auto limit = Limit::create(cage, params.isolationLevel);
    if (!limit) return std::unexpected(limit.error());

    const std::size_t stride = (scan.vertexCount() + params.maxSamples - 1) / params.maxSamples;
    std::vector<Vec3d> samples;
    for (std::size_t v = 0; v < scan.vertexCount(); v += std::max<std::size_t>(stride, 1)) {
        samples.push_back(scan.positions()[v]);
    }
    const auto n = static_cast<Eigen::Index>(cage.vertexCount());
    const auto m = static_cast<Eigen::Index>(samples.size());
    double gate = params.maxSampleDistanceMm;
    if (gate == 0.0) {
        double sum = 0.0;
        for (std::size_t f = 0; f < cage.faceCount(); ++f) {
            const auto face = cage.face(f);
            for (std::size_t i = 0; i < face.size(); ++i) {
                sum += length(cage.positions()[face[i]] -
                              cage.positions()[face[(i + 1) % face.size()]]);
            }
        }
        gate = sum / double(cage.faceVertices().size());
    }
    const SparseMatrix l = laplacian(cage);
    const SparseMatrix ltl = SparseMatrix(l.transpose()) * l;
    const double mu = params.smoothing * double(m) / double(n);
    const double anchor = 1e-9 * double(m) / double(n); // keeps unsampled vertices in place

    // Unknowns are ordered coordinate-major: x of every vertex, then y, then z.
    const auto blockDiagonal = [n](const SparseMatrix& block) {
        std::vector<Eigen::Triplet<double>> entries;
        for (Eigen::Index c = 0; c < 3; ++c) {
            for (Eigen::Index k = 0; k < block.outerSize(); ++k) {
                for (SparseMatrix::InnerIterator it(block, k); it; ++it) {
                    entries.push_back(entry(c * n + it.row(), c * n + it.col(), it.value()));
                }
            }
        }
        SparseMatrix out(3 * n, 3 * n);
        out.setFromTriplets(entries.begin(), entries.end());
        return out;
    };
    const SparseMatrix smooth = blockDiagonal(ltl);
    Eigen::VectorXd start(3 * n);
    for (Eigen::Index v = 0; v < n; ++v) {
        const Vec3d& p = cage.positions()[static_cast<std::size_t>(v)];
        start(v) = p.x;
        start(n + v) = p.y;
        start(2 * n + v) = p.z;
    }
    Eigen::VectorXd current = start;
    std::vector<Vec3d> positions = cage.positions();
    std::vector<VertexWeight> w;
    std::vector<double> history;
    // Each round starts with the rms of the previous round's result; the best result is kept, in
    // case a round makes things worse (samples entering or leaving the gate can do that).
    std::vector<Vec3d> best = positions;
    double bestRms = std::numeric_limits<double>::infinity();
    for (unsigned it = 0; it <= params.iterations; ++it) {
        auto tess = tessellate(**limit, (*limit)->patchPoints(positions), kSearchGrid);
        if (!tess) return std::unexpected(tess.error());
        // Squared distance to each sample's tangent plane, plus a small share of the distance to
        // the sample itself (point-to-plane fitting converges in a few rounds; the point term
        // keeps the surface from sliding along itself).
        std::vector<Eigen::Triplet<double>> planeRows, pointRows;
        Eigen::VectorXd planeTargets(m);
        Eigen::VectorXd pointTargets(3 * m);
        double sum2 = 0.0;
        std::size_t used = 0;
        for (Eigen::Index k = 0; k < m; ++k) {
            const Vec3d& q = samples[static_cast<std::size_t>(k)];
            const SurfacePoint sp = locate(*tess, q);
            planeTargets(k) = 0.0;
            for (Eigen::Index c = 0; c < 3; ++c) pointTargets(c * m + k) = 0.0;
            if (sp.distance > gate) continue; // an empty row: the sample is left out
            sum2 += sp.distance * sp.distance;
            ++used;
            (*limit)->weights(sp.patch, sp.s, sp.t, w);
            const std::array<double, 3> nc{sp.normal.x, sp.normal.y, sp.normal.z};
            const std::array<double, 3> qc{q.x, q.y, q.z};
            for (const VertexWeight& vw : w) {
                for (Eigen::Index c = 0; c < 3; ++c) {
                    const auto cc = static_cast<std::size_t>(c);
                    planeRows.push_back(entry(k, c * n + vw.vertex, vw.weight * nc[cc]));
                    pointRows.push_back(entry(c * m + k, c * n + vw.vertex, vw.weight));
                }
            }
            planeTargets(k) = dot(sp.normal, q);
            for (Eigen::Index c = 0; c < 3; ++c)
                pointTargets(c * m + k) = qc[static_cast<std::size_t>(c)];
        }
        const double rms = std::sqrt(sum2 / double(std::max<std::size_t>(used, 1)));
        if (rms < bestRms) {
            bestRms = rms;
            best = positions;
        }
        if (it == params.iterations) break; // the last pass only measures
        history.push_back(rms);
        SparseMatrix plane(m, 3 * n);
        plane.setFromTriplets(planeRows.begin(), planeRows.end());
        SparseMatrix point(3 * m, 3 * n);
        point.setFromTriplets(pointRows.begin(), pointRows.end());
        SparseMatrix identity(3 * n, 3 * n);
        identity.setIdentity();
        const SparseMatrix planeT = plane.transpose();
        const SparseMatrix pointT = point.transpose();
        const SparseMatrix normal =
            planeT * plane + kPointWeight * (pointT * point) + mu * smooth + anchor * identity;
        const Eigen::VectorXd rhs = planeT * planeTargets + kPointWeight * (pointT * pointTargets) +
                                    mu * (smooth * start) + anchor * current;
        Eigen::SimplicialLDLT<SparseMatrix> solver(normal);
        if (solver.info() != Eigen::Success) {
            return fail(ErrorCode::Internal, "the cage fit's least-squares system is singular");
        }
        current = solver.solve(rhs);
        for (Eigen::Index v = 0; v < n; ++v) {
            positions[static_cast<std::size_t>(v)] = {
                current(v), current(n + v), current(2 * n + v)};
        }
    }

    positions = std::move(best);
    auto fitted = cage.withPositions(positions);
    if (!fitted) return std::unexpected(fitted.error());
    auto tess = tessellate(**limit, (*limit)->patchPoints(positions), kSearchGrid);
    if (!tess) return std::unexpected(tess.error());
    CageFitResult result{std::move(*fitted), 0, 0, 0.0, 0.0, std::move(history)};
    double sum2 = 0.0;
    for (const Vec3d& q : samples) {
        const double d = locate(*tess, q).distance;
        if (d > gate) {
            ++result.ignoredSamples;
            continue;
        }
        ++result.samples;
        sum2 += d * d;
        result.maxDistanceMm = std::max(result.maxDistanceMm, d);
    }
    result.rmsDistanceMm = std::sqrt(sum2 / double(std::max<std::size_t>(result.samples, 1)));
    return result;
}

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

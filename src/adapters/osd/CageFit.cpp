// SPDX-License-Identifier: GPL-3.0-or-later
// Fitting the cage to the scan through limit stencils (ARCHITECTURE.md section 6, step 2).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>

#include "m2c/adapters/osd/Subdivision.h"
#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Limit.h"
#include "Tessellation.h"

namespace m2c::osd {

namespace {

using SparseMatrix = Eigen::SparseMatrix<double>;

/// A matrix entry; SparseMatrix stores int indices, so the narrowing is spelled out here.
Eigen::Triplet<double> entry(const Eigen::Index row, const Eigen::Index col, double value) {
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
        for (std::uint32_t w : ring)
            entries.emplace_back(v, w, -1.0 / static_cast<double>(ring.size()));
    }
    const auto n = static_cast<Eigen::Index>(cage.vertexCount());
    SparseMatrix l(n, n);
    l.setFromTriplets(entries.begin(), entries.end());
    return l;
}

/// Moves the held vertices onto their planes. Alternating projections settle a vertex held by
/// two or three planes on (near) their intersection.
void holdOnPlanes(std::vector<Vec3d>& positions, const std::vector<HeldPlane>& held) {
    constexpr int kRounds = 20;
    for (int round = 0; round < kRounds; ++round) {
        for (const HeldPlane& plane : held) {
            for (const std::uint32_t v : plane.vertices) {
                Vec3d& p = positions[v];
                p = p - plane.normal * dot(p - plane.point, plane.normal);
            }
        }
        if (held.size() < 2) return; // one plane: one projection is exact
    }
}

constexpr int kSearchGrid = 4;               // tessellation per patch for the closest-point search
constexpr double kHoldWeight = 10.0;         // stiffness of the held-plane term, per vertex
constexpr double kPointWeight = 0.1;         // share of point-to-point distance in the fit
constexpr double kSolveTolerance = 1e-9;     // relative residual of each round's solve
constexpr double kSampleSpacing = 1.0 / 3.0; // most spacing of samples, in mean cage edges
constexpr double kTaubinShrink = 0.5;        // Taubin fairing: a smoothing step,
constexpr double kTaubinInflate = -0.53;     // then a slightly larger step back
constexpr Eigen::Index kCancelEvery = 4096;  // samples searched between cancel checks

} // namespace

Result<CageFitResult> fitCage(const QuadLayout& cage,
                              const TriMesh& scan,
                              const CageFitParams& params,
                              const Progress& progress) {
    if (params.smoothing < 0.0 || params.maxSampleDistanceMm < 0.0 || params.maxSamples < 1 ||
        params.iterations < 1) {
        return fail(ErrorCode::InvalidArgument,
                    "smoothing and distance must be at least 0; samples and iterations at least 1");
    }
    for (const HeldPlane& plane : params.held) {
        if (std::abs(length(plane.normal) - 1.0) > 1e-9 ||
            std::ranges::any_of(plane.vertices,
                                [&cage](std::uint32_t v) { return v >= cage.vertexCount(); })) {
            return fail(ErrorCode::InvalidArgument,
                        "a held plane needs a unit normal and vertices of the cage");
        }
    }
    auto limit = Limit::create(cage, params.isolationLevel, params.open, {}, params.creases);
    if (!limit) return std::unexpected(limit.error());

    double gate = params.maxSampleDistanceMm;
    double edgeSum = 0.0;
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        const auto face = cage.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            edgeSum +=
                length(cage.positions()[face[i]] - cage.positions()[face[(i + 1) % face.size()]]);
        }
    }
    const double meanEdge = edgeSum / static_cast<double>(cage.faceVertices().size());
    if (gate == 0.0) gate = meanEdge;
    const std::size_t stride = (scan.vertexCount() + params.maxSamples - 1) / params.maxSamples;
    std::vector<Vec3d> samples;
    for (std::size_t v = 0; v < scan.vertexCount(); v += std::max<std::size_t>(stride, 1)) {
        samples.push_back(scan.positions()[v]);
    }
    // A coarse scan (such as a CAD export, with long triangles across flat sides) has too few
    // vertices to hold the surface between them, which then bulges out past sharp edges.
    // Triangles longer than the spacing add points on a grid across them, within the budget.
    if (const double spacing = kSampleSpacing * meanEdge; spacing > 0.0) {
        std::vector<std::pair<std::size_t, int>> divided; // triangle, divisions per side
        std::size_t extra = 0;
        for (std::size_t f = 0; f < scan.faceCount(); ++f) {
            const auto [a, b, c] = scan.face(f);
            const auto& p = scan.positions();
            const double longest =
                std::max({length(p[b] - p[a]), length(p[c] - p[b]), length(p[a] - p[c])});
            if (longest <= spacing) continue;
            const int k = static_cast<int>(std::ceil(longest / spacing));
            divided.emplace_back(f, k);
            extra += static_cast<std::size_t>((k + 1) * (k + 2) / 2 - 3);
        }
        const std::size_t budget =
            params.maxSamples > samples.size() ? params.maxSamples - samples.size() : 0;
        // Over budget, every triangle gets proportionally fewer divisions.
        const double scale =
            extra > budget ? std::sqrt(static_cast<double>(budget) / static_cast<double>(extra))
                           : 1.0;
        for (const auto& [f, divisions] : divided) {
            const int k = std::max(1, static_cast<int>(std::floor(divisions * scale)));
            const auto [a, b, c] = scan.face(f);
            const auto& p = scan.positions();
            for (int i = 0; i <= k; ++i) {
                for (int j = 0; i + j <= k; ++j) {
                    if ((i == 0 && j == 0) || i == k || j == k) continue; // its corners
                    const double u = static_cast<double>(i) / k, v = static_cast<double>(j) / k;
                    samples.push_back(p[a] + (p[b] - p[a]) * u + (p[c] - p[a]) * v);
                }
            }
        }
    }
    const auto n = static_cast<Eigen::Index>(cage.vertexCount());
    const auto m = static_cast<Eigen::Index>(samples.size());
    const SparseMatrix l = laplacian(cage);
    const SparseMatrix ltl = SparseMatrix(l.transpose()) * l;
    const double mu = params.smoothing * static_cast<double>(m) / static_cast<double>(n);
    const double anchor =
        1e-9 * static_cast<double>(m) / static_cast<double>(n); // keeps unsampled vertices in place

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
    // Held vertices start on their planes, so the smoothing term keeps the flat shape rather
    // than the bumps they had.
    std::vector<Vec3d> positions = cage.positions();
    holdOnPlanes(positions, params.held);
    Eigen::VectorXd start(3 * n);
    for (Eigen::Index v = 0; v < n; ++v) {
        const auto& [x, y, z] = positions[static_cast<std::size_t>(v)];
        start(v) = x;
        start(n + v) = y;
        start(2 * n + v) = z;
    }
    Eigen::VectorXd current = start;
    // The shape the smoothing term keeps: the starting cage, faired, so the scan's noise that
    // its vertices picked up is not kept too. Vertices on the boundary or a crease stay put, so
    // fairing does not round off an edge.
    Eigen::VectorXd reference = start;
    if (params.fairingRounds > 0) {
        std::vector<bool> pinned(cage.vertexCount(), false);
        for (const auto& [a, b] : params.creases) pinned[a] = pinned[b] = true;
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeFaces;
        for (std::size_t f = 0; f < cage.faceCount(); ++f) {
            const auto face = cage.face(f);
            for (std::size_t i = 0; i < face.size(); ++i) {
                ++edgeFaces[std::minmax(face[i], face[(i + 1) % face.size()])];
            }
        }
        for (const auto& [edge, count] : edgeFaces) {
            if (count == 1) pinned[edge.first] = pinned[edge.second] = true;
        }
        for (unsigned round = 0; round < params.fairingRounds; ++round) {
            for (const double step : {kTaubinShrink, kTaubinInflate}) {
                for (Eigen::Index c = 0; c < 3; ++c) {
                    const Eigen::VectorXd move = l * reference.segment(c * n, n);
                    for (Eigen::Index v = 0; v < n; ++v) {
                        if (!pinned[static_cast<std::size_t>(v)]) {
                            reference(c * n + v) -= step * move(v);
                        }
                    }
                }
            }
        }
    }
    // The held-plane term: weight * (normal . p - normal . point)^2 per held vertex.
    std::vector<Eigen::Triplet<double>> holdEntries;
    Eigen::VectorXd holdRhs = Eigen::VectorXd::Zero(3 * n);
    const double hold = kHoldWeight * static_cast<double>(m) / static_cast<double>(n);
    for (const HeldPlane& plane : params.held) {
        const std::array nc{plane.normal.x, plane.normal.y, plane.normal.z};
        const double offset = dot(plane.normal, plane.point);
        for (const std::uint32_t v : plane.vertices) {
            for (Eigen::Index a = 0; a < 3; ++a) {
                const double na = nc[static_cast<std::size_t>(a)];
                for (Eigen::Index b = 0; b < 3; ++b) {
                    holdEntries.push_back(
                        entry(a * n + v, b * n + v, hold * na * nc[static_cast<std::size_t>(b)]));
                }
                holdRhs(a * n + v) += hold * na * offset;
            }
        }
    }
    SparseMatrix holdTerm(3 * n, 3 * n);
    holdTerm.setFromTriplets(holdEntries.begin(), holdEntries.end());
    std::vector<VertexWeight> w;
    std::vector<double> history;
    // Each round starts with the rms of the previous round's result; the best result is kept, in
    // case a round makes things worse (samples entering or leaving the gate can do that).
    // The best round's positions and their measurements, so they need not be measured again.
    std::vector<Vec3d> best = positions;
    double bestRms = std::numeric_limits<double>::infinity();
    std::size_t bestUsed = 0;
    double bestMax = 0.0;
    double bestP95 = 0.0;
    std::vector<double> distances;
    for (unsigned it = 0; it <= params.iterations; ++it) {
        auto tess = tessellate(**limit, (*limit)->patchPoints(positions), kSearchGrid);
        if (!tess) return std::unexpected(tess.error());
        // Squared distance to each sample's tangent plane, plus a small share of the distance to
        // the sample itself (point-to-plane fitting converges in a few rounds; the point term
        // keeps the surface from sliding along itself).
        std::vector<Eigen::Triplet<double>> planeRows, pointRows;
        planeRows.reserve(static_cast<std::size_t>(m) * 48); // 16 weights, 3 coordinates each
        pointRows.reserve(static_cast<std::size_t>(m) * 48);
        Eigen::VectorXd planeTargets(m);
        Eigen::VectorXd pointTargets(3 * m);
        double sum2 = 0.0;
        double maxD = 0.0;
        std::size_t used = 0;
        distances.clear();
        for (Eigen::Index k = 0; k < m; ++k) {
            if (k % kCancelEvery == 0) {
                if (auto go = checkCancelled(progress, "fitting the cage"); !go)
                    return std::unexpected(go.error());
            }
            const Vec3d& q = samples[static_cast<std::size_t>(k)];
            const auto [patch, s, t, distance, surfaceNormal] = locate(*tess, q);
            planeTargets(k) = 0.0;
            for (Eigen::Index c = 0; c < 3; ++c) pointTargets(c * m + k) = 0.0;
            if (distance > gate) continue; // an empty row: the sample is left out
            sum2 += distance * distance;
            maxD = std::max(maxD, distance);
            distances.push_back(distance);
            ++used;
            (*limit)->weights(patch, s, t, w);
            const std::array nc{surfaceNormal.x, surfaceNormal.y, surfaceNormal.z};
            const std::array qc{q.x, q.y, q.z};
            for (const auto& [vertex, weight] : w) {
                for (Eigen::Index c = 0; c < 3; ++c) {
                    const auto cc = static_cast<std::size_t>(c);
                    planeRows.push_back(entry(k, c * n + vertex, weight * nc[cc]));
                    pointRows.push_back(entry(c * m + k, c * n + vertex, weight));
                }
            }
            planeTargets(k) = dot(surfaceNormal, q);
            for (Eigen::Index c = 0; c < 3; ++c)
                pointTargets(c * m + k) = qc[static_cast<std::size_t>(c)];
        }
        const double rms = std::sqrt(sum2 / static_cast<double>(std::max<std::size_t>(used, 1)));
        // A round that leaves every sample out measures nothing; it is not a perfect fit.
        if (used > 0 && rms < bestRms) {
            bestRms = rms;
            bestUsed = used;
            bestMax = maxD;
            const auto at =
                distances.begin() + static_cast<std::ptrdiff_t>((distances.size() - 1) * 95 / 100);
            std::ranges::nth_element(distances, at);
            bestP95 = *at;
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
        const SparseMatrix normal = planeT * plane + kPointWeight * (pointT * point) + mu * smooth +
                                    anchor * identity + holdTerm;
        const Eigen::VectorXd rhs = planeT * planeTargets + kPointWeight * (pointT * pointTargets) +
                                    mu * (smooth * reference) + anchor * current + holdRhs;
        // Conjugate gradients from the last round's positions converge in a few dozen
        // iterations: the point and smoothing terms keep the system well conditioned. A direct
        // factorisation costs most of the fit on large cages, so it is only the fallback.
        Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower | Eigen::Upper> iterative;
        iterative.setTolerance(kSolveTolerance);
        iterative.compute(normal);
        const Eigen::VectorXd guess = current;
        current = iterative.solveWithGuess(rhs, guess);
        if (iterative.info() != Eigen::Success) {
            Eigen::SimplicialLDLT const solver(normal);
            if (solver.info() != Eigen::Success) {
                return fail(ErrorCode::Internal, "the cage fit's least-squares system is singular");
            }
            current = solver.solve(rhs);
        }
        for (Eigen::Index v = 0; v < n; ++v) {
            positions[static_cast<std::size_t>(v)] = {
                .x = current(v), .y = current(n + v), .z = current(2 * n + v)};
        }
        if (!params.held.empty()) {
            holdOnPlanes(positions, params.held);
            for (Eigen::Index v = 0; v < n; ++v) {
                const auto& [x, y, z] = positions[static_cast<std::size_t>(v)];
                current(v) = x;
                current(n + v) = y;
                current(2 * n + v) = z;
            }
        }
    }

    auto fitted = cage.withPositions(std::move(best));
    if (!fitted) return std::unexpected(fitted.error());
    return CageFitResult{.cage = std::move(*fitted),
                         .samples = bestUsed,
                         .ignoredSamples = samples.size() - bestUsed,
                         .rmsDistanceMm = bestUsed > 0 ? bestRms : 0.0,
                         .maxDistanceMm = bestMax,
                         .p95DistanceMm = bestP95,
                         .rmsBeforeRoundMm = std::move(history)};
}

} // namespace m2c::osd

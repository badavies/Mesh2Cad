// SPDX-License-Identifier: GPL-3.0-or-later
// Instant Meshes as a library (ADR-009). remesh() follows batch_process() in Instant Meshes'
// batch.cpp (BSD-3-Clause, Copyright (c) 2015 Wenzel Jakob, Daniele Panozzo, Marco Tarini and
// Olga Sorkine-Hornung), with in-memory input and output instead of files, fixed settings for a
// closed scan, and the console output discarded.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iostream>
#include <memory>
#include <set>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include "m2c/adapters/quad/QuadRemesh.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "adjacency.h"
#include "bvh.h"
#include "common.h"
#include "dedge.h"
#include "extract.h"
#include "field.h"
#include "hierarchy.h"
#include "meshstats.h"
#include "normal.h"
#include "subdivide.h"

/// Worker count read by Instant Meshes' optimiser (defined by its main.cpp, which we do not use).
int nprocs = 1;

namespace m2c::quad {

namespace {

/// Discards std::cout and std::cerr while it lives: Instant Meshes reports progress there, and
/// the CLI and JSON channel use standard output for results.
class SilenceConsole {
public:
    SilenceConsole() : out_(std::cout.rdbuf(&null_)), err_(std::cerr.rdbuf(&null_)) {}
    ~SilenceConsole() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    SilenceConsole(const SilenceConsole&) = delete;
    SilenceConsole& operator=(const SilenceConsole&) = delete;

private:
    struct NullBuffer : std::streambuf {
    protected:
        int overflow(const int c) override { return c; }
    };
    NullBuffer null_;
    std::streambuf* out_;
    std::streambuf* err_;
};

/// Releases the hierarchy's adjacency arrays, which Instant Meshes allocates with new[] and
/// leaves to the owner (its batch tool never frees them because the process exits).
class FreeHierarchy {
public:
    explicit FreeHierarchy(MultiResolutionHierarchy& hierarchy) : hierarchy_(hierarchy) {}
    ~FreeHierarchy() { hierarchy_.free(); }
    FreeHierarchy(const FreeHierarchy&) = delete;
    FreeHierarchy& operator=(const FreeHierarchy&) = delete;

private:
    MultiResolutionHierarchy& hierarchy_;
};

/// Holds the field to the mesh's open edges, as Instant Meshes' batch tool does with
/// align_to_boundaries: a sheet's cage then runs along its outline.
void alignToBoundaries(MultiResolutionHierarchy& hierarchy) {
    const MatrixXu& F = hierarchy.F();
    const MatrixXf& V = hierarchy.V();
    for (std::uint32_t i = 0; i < 3 * F.cols(); ++i) {
        if (hierarchy.E2E()[i] != INVALID) continue;
        const std::uint32_t i0 = F(i % 3, i / 3);
        const std::uint32_t i1 = F((i + 1) % 3, i / 3);
        const Vector3f edge = V.col(i1) - V.col(i0);
        if (edge.squaredNorm() <= 0) continue;
        hierarchy.CO().col(i0) = V.col(i0);
        hierarchy.CO().col(i1) = V.col(i1);
        hierarchy.CQ().col(i0) = hierarchy.CQ().col(i1) = edge.normalized();
        hierarchy.CQw()[i0] = hierarchy.CQw()[i1] = hierarchy.COw()[i0] = hierarchy.COw()[i1] =
            1.0f;
    }
}

/// Holds the field to the guides, as Instant Meshes' own edge strokes do: each guide is sampled
/// at half the finest edge length, and the corners of the triangle under each sample get the
/// guide's direction and the sample as a point the lattice passes through.
Result<void> constrainToGuides(MultiResolutionHierarchy& hierarchy,
                               const std::vector<std::vector<Vec3d>>& guides,
                               const double maxOffset) {
    const MatrixXf& V = hierarchy.V();
    const MatrixXu& F = hierarchy.F();
    std::vector<Vec3d> positions(static_cast<std::size_t>(V.cols()));
    for (Eigen::Index v = 0; v < V.cols(); ++v) {
        positions[static_cast<std::size_t>(v)] = {.x = V(0, v), .y = V(1, v), .z = V(2, v)};
    }
    std::vector indices(F.data(), F.data() + F.size());
    double edgeSum = 0.0;
    for (Eigen::Index f = 0; f < F.cols(); ++f) {
        for (Eigen::Index k = 0; k < 3; ++k) {
            edgeSum += length(positions[F(k, f)] - positions[F((k + 1) % 3, f)]);
        }
    }
    const double step = edgeSum / static_cast<double>(F.size()) / 2.0;
    auto surface = TriMesh::create(std::move(positions), std::move(indices));
    if (!surface) return std::unexpected(surface.error());
    const SpatialIndex& index = surface->spatialIndex();
    const MatrixXf& N = hierarchy.N();
    for (std::size_t g = 0; g < guides.size(); ++g) {
        const std::vector<Vec3d>& guide = guides[g];
        if (guide.size() < 2) {
            return fail(ErrorCode::InvalidArgument,
                        "guide " + std::to_string(g) + " needs at least two points");
        }
        for (std::size_t i = 0; i + 1 < guide.size(); ++i) {
            const Vec3d along = guide[i + 1] - guide[i];
            const double span = length(along);
            if (!(span > 0.0)) continue;
            const Vector3f tangent(static_cast<Float>(along.x / span),
                                   static_cast<Float>(along.y / span),
                                   static_cast<Float>(along.z / span));
            const auto samples = static_cast<int>(std::ceil(span / step));
            for (int k = 0; k <= samples; ++k) {
                const Vec3d q = guide[i] + along * (static_cast<double>(k) / samples);
                const ClosestPoint nearest = index.closestPoint(q);
                if (nearest.distance > maxOffset) {
                    return fail(ErrorCode::InvalidArgument,
                                "guide " + std::to_string(g) + " leaves the mesh");
                }
                const Vector3f point(static_cast<Float>(nearest.point.x),
                                     static_cast<Float>(nearest.point.y),
                                     static_cast<Float>(nearest.point.z));
                for (Eigen::Index j = 0; j < 3; ++j) {
                    const std::uint32_t v = F(j, nearest.face);
                    const Vector3f local = tangent - tangent.dot(N.col(v)) * N.col(v);
                    if (local.squaredNorm() <= 0) continue;
                    hierarchy.CQ().col(v) = local.normalized();
                    hierarchy.CQw()[v] = 1.0f;
                    hierarchy.CO().col(v) = point;
                    hierarchy.COw()[v] = 1.0f;
                }
            }
        }
    }
    return {};
}

Result<QuadLayout> run(const TriMesh& mesh, const QuadRemeshParams& params) {
    constexpr int rosy = 4;
    constexpr int posy = 4;
    constexpr bool extrinsic = true;
    constexpr bool deterministic = true;
    constexpr bool pureQuad = true;

    MatrixXu F(3, mesh.faceCount());
    MatrixXf V(3, mesh.vertexCount());
    MatrixXf N;
    VectorXf A;
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto corners = mesh.face(f);
        for (std::size_t k = 0; k < 3; ++k) {
            F(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(f)) = corners[k];
        }
    }
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        const auto& [x, y, z] = mesh.positions()[v];
        V.col(static_cast<Eigen::Index>(v)) =
            Vector3f(static_cast<Float>(x), static_cast<Float>(y), static_cast<Float>(z));
    }

    const MeshStats stats = compute_mesh_stats(F, V, deterministic);
    // Pure-quad extraction splits each face of the quad-dominant result into four, so ask the
    // field for a quarter of the target.
    const Float faceArea = static_cast<Float>(stats.mSurfaceArea) /
                           static_cast<Float>(std::max<std::size_t>(params.targetFaces / 4, 1));
    const Float scale = std::sqrt(faceArea);
    // Without area the edge-split pass below would split edges towards zero length forever.
    if (!(scale > 0) || !std::isfinite(scale)) {
        return fail(ErrorCode::InvalidArgument, "the mesh has no surface area to remesh");
    }

    VectorXu V2E, E2E;
    VectorXb boundary, nonManifold;
    const auto maxEdge = static_cast<Float>(stats.mMaximumEdgeLength);
    if (const auto meanEdge = static_cast<Float>(stats.mAverageEdgeLength);
        maxEdge * 2 > scale || maxEdge > meanEdge * 2) {
        build_dedge(F, V, V2E, E2E, boundary, nonManifold);
        subdivide(F,
                  V,
                  V2E,
                  E2E,
                  boundary,
                  nonManifold,
                  std::min(scale / 2, meanEdge * 2),
                  deterministic);
    }
    build_dedge(F, V, V2E, E2E, boundary, nonManifold);
    MultiResolutionHierarchy mRes;
    const FreeHierarchy freeHierarchy(mRes);
    mRes.setAdj(generate_adjacency_matrix_uniform(F, V2E, E2E, nonManifold));
    std::set<uint32_t> creaseIn, creaseOut;
    if (params.creaseAngleDeg > 0) {
        generate_crease_normals(F,
                                V,
                                V2E,
                                E2E,
                                boundary,
                                nonManifold,
                                static_cast<Float>(params.creaseAngleDeg),
                                N,
                                creaseIn);
    } else {
        generate_smooth_normals(F, V, V2E, E2E, nonManifold, N);
    }
    compute_dual_vertex_areas(F, V, V2E, E2E, nonManifold, A);

    mRes.setE2E(std::move(E2E));
    mRes.setF(std::move(F));
    mRes.setV(std::move(V));
    mRes.setA(A); // copied either way: setA() takes a MatrixXf
    mRes.setN(std::move(N));
    mRes.setScale(scale);
    mRes.build(deterministic);
    mRes.resetSolution();
    // Closed meshes without guides run unconstrained, exactly as before constraints existed.
    if (const bool open = (mRes.E2E().array() == INVALID).any(); open || !params.guides.empty()) {
        mRes.clearConstraints();
        if (open) alignToBoundaries(mRes);
        if (auto held = constrainToGuides(mRes, params.guides, static_cast<double>(scale)); !held) {
            return std::unexpected(held.error());
        }
        mRes.propagateConstraints(rosy, posy);
    }

    std::unique_ptr<BVH> bvh;
    if (params.smoothIterations > 0) {
        bvh = std::make_unique<BVH>(&mRes.F(), &mRes.V(), &mRes.N(), stats.mAABB);
        bvh->build();
    }

    Optimizer optimizer(mRes, false);
    optimizer.setRoSy(rosy);
    optimizer.setPoSy(posy);
    optimizer.setExtrinsic(extrinsic);
    optimizer.optimizeOrientations(-1);
    optimizer.notify();
    optimizer.wait();
    optimizer.optimizePositions(-1);
    optimizer.notify();
    optimizer.wait();
    optimizer.shutdown();

    MatrixXf O, Nv, Nf;
    std::vector<std::vector<TaggedLink>> graph;
    extract_graph(mRes, extrinsic, rosy, posy, graph, O, Nv, creaseIn, creaseOut, deterministic);
    MatrixXu Fq;
    extract_faces(graph,
                  O,
                  Nv,
                  Nf,
                  Fq,
                  posy,
                  mRes.scale(),
                  creaseOut,
                  true,
                  pureQuad,
                  bvh.get(),
                  static_cast<int>(params.smoothIterations));

    // Instant Meshes marks unused slots of a face with INVALID; faces that repeat a vertex are
    // dropped here (the pipeline repairs the holes they leave).
    std::vector<Vec3d> positions(static_cast<std::size_t>(O.cols()));
    for (Eigen::Index v = 0; v < O.cols(); ++v) {
        positions[static_cast<std::size_t>(v)] = {.x = O(0, v), .y = O(1, v), .z = O(2, v)};
    }
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> vertices;
    for (Eigen::Index f = 0; f < Fq.cols(); ++f) {
        std::vector<std::uint32_t> face;
        for (Eigen::Index k = 0; k < Fq.rows(); ++k) {
            const std::uint32_t v = Fq(k, f);
            if (v == INVALID) continue;
            if (std::ranges::find(face, v) == face.end()) face.push_back(v);
        }
        if (face.size() < 3) continue;
        vertices.insert(vertices.end(), face.begin(), face.end());
        starts.push_back(static_cast<std::uint32_t>(vertices.size()));
    }
    if (starts.size() < 2) {
        return fail(ErrorCode::InvalidState, "Instant Meshes produced no faces");
    }
    return QuadLayout::create(std::move(positions), std::move(starts), std::move(vertices));
}

} // namespace

Result<QuadLayout> remesh(const TriMesh& mesh, const QuadRemeshParams& params) {
    if (params.targetFaces < 8) {
        return fail(ErrorCode::InvalidArgument, "target_faces must be at least 8");
    }
    if (mesh.faceCount() < 4) {
        return fail(ErrorCode::InvalidArgument, "the mesh has too few faces to remesh");
    }
    SilenceConsole const silence;
    try {
        return run(mesh, params);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Instant Meshes failed: ") + e.what());
    }
}

} // namespace m2c::quad

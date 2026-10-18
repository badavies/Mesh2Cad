// SPDX-License-Identifier: GPL-3.0-or-later
// Instant Meshes as a library (ADR-009). remesh() follows batch_process() in Instant Meshes'
// batch.cpp (BSD-3-Clause, Copyright (c) 2015 Wenzel Jakob, Daniele Panozzo, Marco Tarini and
// Olga Sorkine-Hornung), with in-memory input and output instead of files, fixed settings for a
// closed scan, and the console output discarded.

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <streambuf>
#include <string>

#include "m2c/adapters/quad/QuadRemesh.h"

#include "bvh.h"
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
    SilenceConsole silence;
    try {
        return run(mesh, params);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Instant Meshes failed: ") + e.what());
    }
}

} // namespace m2c::quad

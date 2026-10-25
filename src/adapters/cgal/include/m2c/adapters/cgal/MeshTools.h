// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Mesh tools built on CGAL Polygon Mesh Processing (ARCHITECTURE.md section 7). Every function
// takes and returns app-owned types; CGAL types never leave this adapter, and CGAL exceptions are
// turned into Errors.

#include <cstddef>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::cgal {

struct RepairParams {
    /// Also remove self-intersections: CGAL's (experimental) local removal within each shell, then
    /// closed shells that pass through each other are united. Slow on large scans.
    bool removeSelfIntersections{false};
};

struct RepairResult {
    TriMesh mesh;
    std::size_t removedFaces{0};  ///< Degenerate and duplicate faces removed.
    std::size_t splitVertices{0}; ///< Non-manifold vertices duplicated.
    bool orientationFixed{false}; ///< Faces were flipped to a consistent orientation.
    bool selfIntersecting{false}; ///< Faces still cross (faces that only touch do not count).
};

/// Clean-up: merges duplicate points, removes degenerate and duplicate faces and fins (faces
/// hanging off an edge two other faces already share), makes the mesh manifold, orients faces
/// consistently (outward when the mesh is closed) and optionally removes self-intersections. Holes
/// are left open; see fillHoles(). Reports each stage and stops between stages once cancelled.
[[nodiscard]] Result<RepairResult>
repair(const TriMesh& input, const RepairParams& params, const Progress& progress = {});

/// Whether two faces of the mesh cross each other. Faces that only touch, as coincident copies
/// of a point or edge do, do not count.
[[nodiscard]] Result<bool> selfIntersects(const TriMesh& mesh);

struct HoleFillParams {
    double maxPerimeterMm{0.0}; ///< Holes with a longer boundary are left open; 0 = no limit.
    std::size_t maxEdges{0};    ///< Holes with more boundary edges are left open; 0 = no limit.
    /// Add interior vertices and blend into the surrounding curvature. Where the surface meets the
    /// hole's plane at more than 45 degrees (an open end) the patch stays flat instead of doming.
    bool refineAndFair{true};
};

struct HoleFillResult {
    TriMesh mesh;
    std::size_t filled{0};
    std::size_t skipped{0}; ///< Holes left open by the size limits.
    std::size_t failed{0};  ///< Holes CGAL could not fill (left open).
    std::size_t addedFaces{0};
};

/// Fills each boundary loop within the limits. Reports the fraction of holes done and stops before
/// the next hole once cancelled.
[[nodiscard]] Result<HoleFillResult>
fillHoles(const TriMesh& input, const HoleFillParams& params, const Progress& progress = {});

struct SimplifyParams {
    std::size_t targetFaces{0}; ///< 0 = no face count target (then maxErrorMm must be set).
    bool preserveBorders{true}; ///< Keep open boundaries exactly where they are.
    double maxErrorMm{0.0};     ///< Stop before a collapse moves the surface further; 0 = no limit.
};

/// Garland-Heckbert edge-collapse simplification to a target face count or a maximum error,
/// whichever is reached first.
[[nodiscard]] Result<TriMesh> simplify(const TriMesh& input, const SimplifyParams& params);

struct RemeshParams {
    double targetEdgeMm{1.0};
    unsigned iterations{3};
    double sharpAngleDeg{60.0}; ///< Edges with a sharper dihedral angle are kept; 0 = none.
};

/// Isotropic remeshing to an even triangle size, keeping sharp edges.
[[nodiscard]] Result<TriMesh> remesh(const TriMesh& input, const RemeshParams& params);

struct WrapParams {
    double alphaMm{1.0};  ///< Size of the probe; detail smaller than this is lost.
    double offsetMm{0.1}; ///< Distance of the wrap from the input surface.
};

/// 3D alpha wrapping: a watertight, manifold, intersection-free mesh around any input, as a
/// fallback when surface repair fails.
[[nodiscard]] Result<TriMesh> shrinkWrap(const TriMesh& input, const WrapParams& params);

/// Where the plane through `point` with normal `normal` cuts the mesh (CGAL's
/// Polygon_mesh_slicer): one polyline per piece of the cut. A piece that goes all the way round is
/// closed, its first point repeated last; one that runs into a hole or border is open.
[[nodiscard]] Result<std::vector<std::vector<Vec3d>>>
slice(const TriMesh& input, const Vec3d& point, const Vec3d& normal);

} // namespace m2c::cgal

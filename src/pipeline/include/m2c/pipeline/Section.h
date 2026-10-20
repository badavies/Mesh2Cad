// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Section sketches (ARCHITECTURE.md section 7): a mesh cut with a plane, and lines and circles
// fitted to the cut. Arcs and splines between lines are not fitted yet.

#include <cstddef>
#include <span>

#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct SectionParams {
    double maxErrorMm{0.05}; ///< Every section point lies at most this far from its curve.
};

struct LoopFit {
    SketchLoop loop;
    double maxErrorMm{0.0}; ///< Largest distance from a point to its curve.
};

/// Fits a closed polyline (its first point not repeated) within params.maxErrorMm. A polyline
/// that is one circle within the limit gives one full-circle arc. Otherwise it starts at its
/// sharpest corner and takes, line by line, the longest line within the limit; each line starts
/// and ends on a polyline point, so the loop closes exactly. Deterministic. Needs at least three
/// distinct points.
[[nodiscard]] Result<LoopFit> fitLoop(std::span<const Vec2d> points, const SectionParams& params);

struct SectionResult {
    Sketch sketch;             ///< One loop per closed piece of the cut, in the slicer's order.
    std::size_t openPieces{0}; ///< Pieces of the cut that run into a hole or border; left out.
    double maxErrorMm{0.0};    ///< Largest distance from a section point to its curve.
};

/// Cuts the mesh with the plane (CGAL's slicer), projects each closed piece onto it and fits it
/// with fitLoop. Fails if no piece of the cut is closed.
[[nodiscard]] Result<SectionResult>
sectionMesh(const TriMesh& mesh, const SketchPlane& plane, const SectionParams& params);

} // namespace m2c

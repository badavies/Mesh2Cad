// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Section sketches (ARCHITECTURE.md section 7): a mesh cut with a plane, and lines, arcs and
// splines fitted to the cut.

#include <cstddef>
#include <span>

#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct SectionParams {
    double maxErrorMm{0.05}; ///< Every section point lies at most this far from its curve.
    /// Two or more lines and arcs in a row that each cover fewer section points than this, and
    /// meet each other within 20 degrees of tangent, are one freeform stretch: they become a
    /// spline.
    int minCurvePoints{6};
};

struct LoopFit {
    SketchLoop loop;
    double maxErrorMm{0.0}; ///< Largest distance from a point to its curve.
};

/// Fits a closed polyline (its first point not repeated) with lines and arcs within
/// params.maxErrorMm. A polyline that is one circle within the limit gives one full-circle arc.
/// Otherwise it starts at its sharpest corner (or, with none sharper than 30 degrees, where the
/// first curve ends) and, curve by curve, takes the line or arc that covers the most points (the
/// line when they tie); an arc must rise more than the limit above its chord. Then each junction
/// moves to where the curves either side fit best, so an arc does not run on into a tangent line.
/// Each curve starts and ends on a polyline point, so the loop closes exactly. Smooth stretches
/// of short curves become splines through the points (fewer of them, within half the limit of
/// the polyline). Deterministic. Needs at least three distinct points.
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

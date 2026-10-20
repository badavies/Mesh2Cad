// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Least-squares fitting of planes, cylinders, spheres and cones to scan points (ARCHITECTURE.md
// section 7, "Primitive fitting").

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

struct PrimitiveFit {
    Primitive primitive;
    double rmsMm{0.0}; ///< Distance of the points to the fitted surface.
    double maxMm{0.0};
    double p95Mm{0.0}; ///< 95% of the points are at most this far from the surface.
};

/// Fits a primitive of start.kind to the points by least squares on their distances to it. A
/// plane comes from the points' principal axes, its normal on start.direction's side. A cylinder,
/// sphere or cone starts from `start` (for example a region-growing estimate) and is improved by
/// Gauss-Newton steps; the start is kept if no step fits better. A cylinder's axis point ends
/// level with the points' centroid. Deterministic. Needs at least one point (four for a sphere,
/// five for a cylinder and six for a cone, to be determined).
[[nodiscard]] PrimitiveFit fitPrimitive(const Primitive& start, std::span<const Vec3d> points);

/// fitPrimitive with start.direction held exactly (a plane's normal, a cylinder's or cone's axis):
/// a plane goes through the points' centroid, a cylinder's axis moves across itself and its
/// radius changes, a cone's apex and half-angle change. Fails for a sphere or a direction that
/// is not a unit vector. Deterministic.
[[nodiscard]] Result<PrimitiveFit> fitWithDirection(const Primitive& start,
                                                    std::span<const Vec3d> points);

/// The unit direction at angleRad (0 to pi/2) from `reference`, turned from it towards
/// `direction` in the plane they span: the reference itself (with direction's sign) for 0.
/// Fails for a zero vector, an angle outside 0 to pi/2, or a nonzero angle when direction lies
/// along the reference.
[[nodiscard]] Result<Vec3d>
alignedDirection(const Vec3d& direction, const Vec3d& reference, double angleRad);

/// How far a cylinder, cone or sphere turns over the points, in radians: around the axis (the
/// full circle less the largest gap), or the widest angle at the centre between a point and the
/// points' mean direction, doubled. Zero for a plane. A strip of a plane also fits a large
/// cylinder within a distance limit, but turns through very little of it.
[[nodiscard]] double turnAngle(const Primitive& s, std::span<const Vec3d> points);

/// The distinct vertices of the faces, in increasing vertex order.
[[nodiscard]] std::vector<Vec3d> regionPoints(const TriMesh& mesh,
                                              std::span<const std::uint32_t> faces);

struct RegionFitParams {
    std::optional<PrimitiveKind> kind; ///< Empty: the simplest kind that fits about as well as any.
    double maxResidualMm{0.1};         ///< The fit fails when its P95 distance is above this.
};

/// Fits an exact primitive to the vertices of a region of faces (the stage gate of section 6:
/// the residual must be within the limit). The start is CGAL's estimate for a cylinder or
/// sphere, the faces' mean normal for a plane's side, and for a cone the point nearest all the
/// faces' tangent planes (the apex) with the axis the normals turn around; then fitPrimitive
/// refines it. A plane's point is the vertices' centroid. With no kind given, it fits every kind
/// and takes the simplest (plane, sphere, cylinder, cone) whose RMS is within 20% of the best.
/// Deterministic.
[[nodiscard]] Result<PrimitiveFit>
fitRegion(const TriMesh& mesh, std::span<const std::uint32_t> faces, const RegionFitParams& params);

} // namespace m2c

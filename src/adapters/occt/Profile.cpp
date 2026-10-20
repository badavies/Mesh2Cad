// SPDX-License-Identifier: GPL-3.0-or-later
// Solids from section sketches: the profile made a planar face, then a prism or a revolution.
#include "m2c/adapters/occt/Profile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Version.hxx>
#if OCC_VERSION_HEX >= 0x080000
#include <NCollection_HArray1.hxx>
#else
#include <TColgp_HArray1OfPnt.hxx>
#endif
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "Shape.h"

namespace m2c::occt {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kGapMm = 1e-6;

// The interpolator's points: the TColgp class before OCCT 8, the template it is deprecated for
// from 8.
#if OCC_VERSION_HEX >= 0x080000
using PointArray = NCollection_HArray1<gp_Pnt>;
#else
using PointArray = TColgp_HArray1OfPnt;
#endif

gp_Pnt pnt(const Vec3d& p) {
    return {p.x, p.y, p.z};
}
gp_Dir dir(const Vec3d& d) {
    return {d.x, d.y, d.z};
}

Vec2d onArc(const SketchCurve& arc, double t) {
    const double angle = arc.startRad + arc.sweepRad * t;
    return arc.centre + Vec2d{std::cos(angle), std::sin(angle)} * arc.radiusMm;
}

/// The loop as a polygon, for area and containment: each line's start, a point every 5 degrees
/// along an arc, and a spline's points.
std::vector<Vec2d> polygon(const SketchLoop& loop) {
    std::vector<Vec2d> out;
    for (const SketchCurve& c : loop.curves) {
        if (c.kind == CurveKind::Arc) {
            const int n = std::max(2, static_cast<int>(std::ceil(std::abs(c.sweepRad) / 0.0873)));
            for (int k = 0; k < n; ++k) out.push_back(onArc(c, static_cast<double>(k) / n));
        } else {
            out.insert(out.end(), c.points.begin(), c.points.end() - 1);
        }
    }
    return out;
}

/// Positive when the polygon runs anticlockwise.
double signedArea(const std::vector<Vec2d>& p) {
    double sum = 0.0;
    for (std::size_t k = 0; k < p.size(); ++k) sum += cross(p[k], p[(k + 1) % p.size()]);
    return sum / 2.0;
}

/// Even-odd ray test.
bool inside(const std::vector<Vec2d>& p, const Vec2d& q) {
    bool in = false;
    for (std::size_t k = 0, j = p.size() - 1; k < p.size(); j = k++) {
        if ((p[k].y > q.y) != (p[j].y > q.y) &&
            q.x < p[j].x + (p[k].x - p[j].x) * (q.y - p[j].y) / (p[k].y - p[j].y)) {
            in = !in;
        }
    }
    return in;
}

/// The curve's edges: one for a line or arc, one per knot span for a spline. A spline made one
/// edge of many spans gives a face whose volume the kernel integrates up to 2% wrong.
Result<std::vector<TopoDS_Edge>> edges(const SketchPlane& plane, const SketchCurve& c, bool alone) {
    const auto at = [&](const Vec2d& p) { return pnt(plane.toWorld(p)); };
    if (c.kind == CurveKind::Line)
        return std::vector{BRepBuilderAPI_MakeEdge(at(c.start()), at(c.end())).Edge()};
    if (c.kind == CurveKind::Arc) {
        if (!(c.radiusMm > 0.0)) return fail(ErrorCode::InvalidArgument, "an arc has no radius");
        if (std::abs(c.sweepRad) >= kTwoPi - 1e-9) {
            const gp_Ax2 frame(at(c.centre), dir(plane.normal()), dir(plane.u));
            const TopoDS_Edge full = BRepBuilderAPI_MakeEdge(gp_Circ(frame, c.radiusMm)).Edge();
            return std::vector{
                TopoDS::Edge(c.sweepRad < 0.0 ? full.Reversed() : TopoDS_Shape(full))};
        }
        const GC_MakeArcOfCircle arc(at(c.start()), at(onArc(c, 0.5)), at(c.end()));
        if (!arc.IsDone()) return fail(ErrorCode::InvalidArgument, "an arc has no sweep");
        return std::vector{BRepBuilderAPI_MakeEdge(arc.Value()).Edge()};
    }
    // A spline that is the whole loop closes on itself: interpolate it periodically.
    const bool periodic = alone && length(c.end() - c.start()) <= kGapMm;
    const std::size_t n = c.points.size() - (periodic ? 1 : 0);
    if (n < 2) return fail(ErrorCode::InvalidArgument, "a spline needs at least two points");
    const opencascade::handle<PointArray> points = new PointArray(1, static_cast<int>(n));
    for (std::size_t k = 0; k < n; ++k) points->SetValue(static_cast<int>(k) + 1, at(c.points[k]));
    GeomAPI_Interpolate interpolate(points, periodic, 1e-9);
    interpolate.Perform();
    if (!interpolate.IsDone()) {
        return fail(ErrorCode::InvalidArgument, "no spline passes through the points");
    }
    const opencascade::handle<Geom_BSplineCurve> curve = interpolate.Curve();
    std::vector<TopoDS_Edge> out;
    for (int k = 1; k < curve->NbKnots(); ++k) {
        out.push_back(BRepBuilderAPI_MakeEdge(curve, curve->Knot(k), curve->Knot(k + 1)).Edge());
    }
    return out;
}

/// The loop's edges in its own direction, turned anticlockwise about the plane's normal or not.
Result<TopoDS_Wire>
wire(const SketchPlane& plane, const SketchLoop& loop, bool anticlockwise, double signedArea) {
    BRepBuilderAPI_MakeWire maker;
    for (const SketchCurve& c : loop.curves) {
        auto made = edges(plane, c, loop.curves.size() == 1);
        if (!made) return std::unexpected(made.error());
        for (const TopoDS_Edge& e : *made) {
            maker.Add(e);
            if (!maker.IsDone()) {
                return fail(ErrorCode::InvalidArgument, "the curves of a loop do not join");
            }
        }
    }
    const TopoDS_Wire out = maker.Wire();
    return (signedArea > 0.0) == anticlockwise ? out : TopoDS::Wire(out.Reversed());
}

/// The profile as one planar face: the loop of largest area, less the others inside it.
Result<TopoDS_Face> profileFace(const Sketch& sketch) {
    if (sketch.loops.empty()) return fail(ErrorCode::InvalidArgument, "the sketch has no loop");
    std::vector<std::vector<Vec2d>> polygons;
    for (const SketchLoop& loop : sketch.loops) {
        const auto& curves = loop.curves;
        for (std::size_t k = 0; k < curves.size(); ++k) {
            if (length(curves[k].end() - curves[(k + 1) % curves.size()].start()) > kGapMm) {
                return fail(
                    ErrorCode::InvalidArgument,
                    "the profile is open: a curve does not start where the one before ends");
            }
        }
        if (curves.empty()) return fail(ErrorCode::InvalidArgument, "a loop has no curves");
        for (const SketchCurve& c : curves) {
            if (c.kind != CurveKind::Arc && c.points.size() < 2) {
                return fail(ErrorCode::InvalidArgument,
                            "a line or spline has fewer than two points");
            }
        }
        polygons.push_back(polygon(loop));
    }
    std::size_t outer = 0;
    for (std::size_t k = 1; k < polygons.size(); ++k) {
        if (std::abs(signedArea(polygons[k])) > std::abs(signedArea(polygons[outer]))) outer = k;
    }
    for (std::size_t k = 0; k < polygons.size(); ++k) {
        if (k == outer) continue;
        bool nested = false;
        for (std::size_t j = 0; j < polygons.size(); ++j) {
            nested = nested || (j != outer && j != k && inside(polygons[j], polygons[k].front()));
        }
        if (!inside(polygons[outer], polygons[k].front()) || nested) {
            return fail(ErrorCode::InvalidArgument,
                        "every loop but the largest must be a hole in it, not outside it or "
                        "inside another hole");
        }
    }
    const SketchPlane& plane = sketch.plane;
    // The outer wire runs anticlockwise about the normal and the holes clockwise, so the face's
    // material is inside the one and outside the others.
    auto outerWire = wire(plane, sketch.loops[outer], true, signedArea(polygons[outer]));
    if (!outerWire) return std::unexpected(outerWire.error());
    BRepBuilderAPI_MakeFace maker(
        gp_Pln(gp_Ax3(pnt(plane.origin), dir(plane.normal()), dir(plane.u))), *outerWire, true);
    for (std::size_t k = 0; k < sketch.loops.size() && maker.IsDone(); ++k) {
        if (k == outer) continue;
        auto hole = wire(plane, sketch.loops[k], false, signedArea(polygons[k]));
        if (!hole) return std::unexpected(hole.error());
        maker.Add(*hole);
    }
    if (!maker.IsDone()) return fail(ErrorCode::InvalidArgument, "the loops make no face");
    return maker.Face();
}

} // namespace

Result<Solid> extrudeSketch(const Sketch& sketch, const double forwardMm, const double backwardMm) {
    if (!(forwardMm + backwardMm > 0.0) || !std::isfinite(forwardMm + backwardMm)) {
        return fail(ErrorCode::InvalidArgument, "the extrusion must have a positive length");
    }
    return guarded("extruding the sketch", [&]() -> Result<Solid> {
        auto face = profileFace(sketch);
        if (!face) return std::unexpected(face.error());
        const Vec3d n = sketch.plane.normal();
        gp_Trsf back;
        back.SetTranslation(gp_Vec(pnt(n * -backwardMm).XYZ()));
        const TopoDS_Shape start = BRepBuilderAPI_Transform(*face, back, true).Shape();
        const gp_Vec along(pnt(n * (forwardMm + backwardMm)).XYZ());
        return kernelSolid(BRepPrimAPI_MakePrism(start, along).Shape(), "the extruded profile is");
    });
}

Result<Solid> revolveSketch(const Sketch& sketch,
                            const Vec2d& axisPoint,
                            const Vec2d& axisDirection,
                            const double angleRad) {
    if (!(length(axisDirection) > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the axis direction must not be zero");
    }
    if (!(angleRad > 0.0) || angleRad > kTwoPi + 1e-12) {
        return fail(ErrorCode::InvalidArgument,
                    "the angle must be above 0 and at most 360 degrees");
    }
    // Which side of the axis each loop's points are on: a solid needs them all on one side.
    const Vec2d across{-axisDirection.y, axisDirection.x};
    double lo = 0.0, hi = 0.0;
    for (const SketchLoop& loop : sketch.loops) {
        for (const Vec2d& p : polygon(loop)) {
            const double side = dot(p - axisPoint, across) / length(across);
            lo = std::min(lo, side);
            hi = std::max(hi, side);
        }
    }
    if (lo < -kGapMm && hi > kGapMm) {
        return fail(ErrorCode::InvalidArgument, "the profile crosses the axis");
    }
    return guarded("revolving the sketch", [&]() -> Result<Solid> {
        auto face = profileFace(sketch);
        if (!face) return std::unexpected(face.error());
        const SketchPlane& plane = sketch.plane;
        const gp_Ax1 axis(pnt(plane.toWorld(axisPoint)),
                          dir(plane.u * axisDirection.x + plane.v * axisDirection.y));
        const TopoDS_Shape shape = angleRad >= kTwoPi - 1e-12
                                       ? BRepPrimAPI_MakeRevol(*face, axis).Shape()
                                       : BRepPrimAPI_MakeRevol(*face, axis, angleRad).Shape();
        return kernelSolid(shape, "the revolved profile is");
    });
}

} // namespace m2c::occt

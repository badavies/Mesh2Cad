// SPDX-License-Identifier: GPL-3.0-or-later
// Booleans between solids: Open CASCADE's fuzzy fuse, cut and common, then one solid out.
#include "m2c/adapters/occt/Boolean.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <NCollection_List.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "Shape.h"

namespace m2c::occt {

namespace {

const char* verb(const BooleanOp op) {
    switch (op) {
    case BooleanOp::Fuse:
        return "fuse";
    case BooleanOp::Cut:
        return "cut";
    case BooleanOp::Common:
        return "intersect";
    }
    return "combine";
}

std::unique_ptr<BRepAlgoAPI_BooleanOperation> algorithm(const BooleanOp op) {
    switch (op) {
    case BooleanOp::Fuse:
        return std::make_unique<BRepAlgoAPI_Fuse>();
    case BooleanOp::Cut:
        return std::make_unique<BRepAlgoAPI_Cut>();
    case BooleanOp::Common:
        return std::make_unique<BRepAlgoAPI_Common>();
    }
    return nullptr;
}

} // namespace

Result<Solid>
booleanSolids(const Solid& a, const Solid& b, const BooleanOp op, const double fuzzyMm) {
    if (!(fuzzyMm >= 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the fuzzy tolerance must not be negative");
    }
    return guarded("the boolean", [&]() -> Result<Solid> {
        const std::unique_ptr<BRepAlgoAPI_BooleanOperation> boolean = algorithm(op);
        NCollection_List<TopoDS_Shape> arguments, tools;
        arguments.Append(a.shape().shape);
        tools.Append(b.shape().shape);
        boolean->SetArguments(arguments);
        boolean->SetTools(tools);
        boolean->SetFuzzyValue(fuzzyMm);
        boolean->SetNonDestructive(true);
        boolean->SetRunParallel(false);
        boolean->Build();
        if (!boolean->IsDone() || boolean->HasErrors()) {
            return fail(ErrorCode::Internal,
                        std::string("Open CASCADE could not ") + verb(op) + " the solids");
        }
        ShapeUpgrade_UnifySameDomain merge(boolean->Shape(), true, true, false);
        merge.Build();
        ShapeFix_Shape fix(merge.Shape());
        fix.Perform();
        TopoDS_Shape solid;
        int solids = 0;
        for (TopExp_Explorer it(fix.Shape(), TopAbs_SOLID); it.More(); it.Next(), ++solids) {
            solid = it.Current();
        }
        if (solids == 0) {
            return fail(ErrorCode::InvalidState,
                        std::string("nothing is left after the ") + verb(op) +
                            ": the solids do not overlap where it needs them to");
        }
        if (solids > 1) {
            return fail(ErrorCode::InvalidState,
                        std::string("the ") + verb(op) + " leaves " + std::to_string(solids) +
                            " separate solids instead of one");
        }
        return kernelSolid(solid, std::string("the solids after the ") + verb(op) + " are");
    });
}

Result<Solid> primitiveSolid(const FittedPrimitive& fit, const double lengthMm) {
    const Primitive& s = fit.surface;
    if (s.kind != PrimitiveKind::Cylinder && s.kind != PrimitiveKind::Sphere) {
        return fail(ErrorCode::InvalidArgument,
                    std::string("only a cylinder or a sphere can be made a solid, not a ") +
                        primitiveKindName(s.kind));
    }
    if (!(s.radiusMm > 0.0) || !(lengthMm >= 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "the radius must be positive and the length not negative");
    }
    return guarded("making the primitive a solid", [&]() -> Result<Solid> {
        const gp_Pnt point(s.point.x, s.point.y, s.point.z);
        if (s.kind == PrimitiveKind::Sphere) {
            return kernelSolid(BRepPrimAPI_MakeSphere(point, s.radiusMm).Shape(), "the sphere is");
        }
        const Vec3d axis = s.direction * (1.0 / length(s.direction));
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int k = 0; k < 8; ++k) {
            const Vec3d corner{(k & 1) ? fit.extent.max.x : fit.extent.min.x,
                               (k & 2) ? fit.extent.max.y : fit.extent.min.y,
                               (k & 4) ? fit.extent.max.z : fit.extent.min.z};
            const double t = dot(corner - s.point, axis);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        const double span = lengthMm > 0.0 ? lengthMm : hi - lo;
        if (!(span > 0.0)) {
            return fail(ErrorCode::InvalidArgument,
                        "the cylinder's extent has no length along its axis; give a length");
        }
        const Vec3d base = s.point + axis * (0.5 * (lo + hi - span));
        const gp_Ax2 frame(gp_Pnt(base.x, base.y, base.z), gp_Dir(axis.x, axis.y, axis.z));
        return kernelSolid(BRepPrimAPI_MakeCylinder(frame, s.radiusMm, span).Shape(),
                           "the cylinder is");
    });
}

} // namespace m2c::occt

// SPDX-License-Identifier: GPL-3.0-or-later
// Fitted primitives as Open CASCADE surfaces (ARCHITECTURE.md section 7, "Primitive fitting").
#include "m2c/adapters/occt/Surface.h"

#include <cmath>
#include <numbers>
#include <vector>

#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_Surface.hxx>
#include <Standard_Handle.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "PrimitiveSurface.h"

namespace m2c::occt {

Result<opencascade::handle<Geom_Surface>> toGeomSurface(const Primitive& s) {
    const bool round = s.kind == PrimitiveKind::Cylinder || s.kind == PrimitiveKind::Sphere;
    if (!(length(s.direction) > 0.0) && s.kind != PrimitiveKind::Sphere) {
        return fail(ErrorCode::InvalidArgument, "the primitive has no direction");
    }
    if (round && !(s.radiusMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the primitive's radius must be positive");
    }
    if (s.kind == PrimitiveKind::Cone &&
        !(s.halfAngleRad > 0.0 && s.halfAngleRad < std::numbers::pi / 2)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cone's half-angle must be within (0, 90) degrees");
    }
    const Vec3d d = s.kind == PrimitiveKind::Sphere ? Vec3d{0, 0, 1} : s.direction;
    const gp_Ax3 frame(gp_Pnt(s.point.x, s.point.y, s.point.z), gp_Dir(d.x, d.y, d.z));
    switch (s.kind) {
    case PrimitiveKind::Plane:
        return opencascade::handle<Geom_Surface>(new Geom_Plane(frame));
    case PrimitiveKind::Cylinder:
        return opencascade::handle<Geom_Surface>(new Geom_CylindricalSurface(frame, s.radiusMm));
    case PrimitiveKind::Sphere:
        return opencascade::handle<Geom_Surface>(new Geom_SphericalSurface(frame, s.radiusMm));
    case PrimitiveKind::Cone:
        return opencascade::handle<Geom_Surface>(
            new Geom_ConicalSurface(frame, s.halfAngleRad, 0.0));
    }
    return fail(ErrorCode::InvalidArgument, "unknown primitive kind");
}

Result<std::vector<double>> surfaceDistances(const Primitive& primitive,
                                             const std::vector<Vec3d>& points) {
    return guarded("primitive surface", [&]() -> Result<std::vector<double>> {
        auto surface = toGeomSurface(primitive);
        if (!surface) return std::unexpected(surface.error());
        GeomAPI_ProjectPointOnSurf project;
        std::vector<double> out;
        out.reserve(points.size());
        for (const Vec3d& p : points) {
            project.Init(gp_Pnt(p.x, p.y, p.z), *surface);
            if (project.NbPoints() == 0) {
                return fail(ErrorCode::Internal, "a point did not project onto the surface");
            }
            out.push_back(project.LowerDistance());
        }
        return out;
    });
}

} // namespace m2c::occt

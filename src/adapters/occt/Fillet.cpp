// SPDX-License-Identifier: GPL-3.0-or-later
// Constant-radius fillets on a solid's edges, picked by points near them.
#include "m2c/adapters/occt/Fillet.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_List.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "Shape.h"

namespace m2c::occt {

namespace {

bool analytic(const TopoDS_Shape& face) {
    switch (BRepAdaptor_Surface(TopoDS::Face(face)).GetType()) {
    case GeomAbs_Plane:
    case GeomAbs_Cylinder:
    case GeomAbs_Cone:
    case GeomAbs_Sphere:
        return true;
    default:
        return false;
    }
}

} // namespace

Result<Solid>
filletEdges(const Solid& solid, const std::vector<Vec3d>& edgePoints, const double radiusMm) {
    if (edgePoints.empty()) return fail(ErrorCode::InvalidArgument, "no edges to round");
    if (!(radiusMm > 0.0)) return fail(ErrorCode::InvalidArgument, "the radius must be positive");
    return guarded("the fillet", [&]() -> Result<Solid> {
        const TopoDS_Shape& shape = solid.shape().shape;
        NCollection_IndexedDataMap<TopoDS_Shape,
                                   NCollection_List<TopoDS_Shape>,
                                   TopTools_ShapeMapHasher>
            edgeFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        BRepFilletAPI_MakeFillet fillet(shape);
        std::vector<int> picked;
        for (const Vec3d& p : edgePoints) {
            const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z)).Shape();
            int nearest = 0;
            double best = std::numeric_limits<double>::infinity();
            for (int i = 1; i <= edgeFaces.Extent(); ++i) {
                if (BRep_Tool::Degenerated(TopoDS::Edge(edgeFaces.FindKey(i)))) continue;
                BRepExtrema_DistShapeShape distance(vertex, edgeFaces.FindKey(i));
                if (distance.IsDone() && distance.Value() < best) {
                    best = distance.Value();
                    nearest = i;
                }
            }
            if (nearest == 0) return fail(ErrorCode::InvalidArgument, "the solid has no edges");
            const NCollection_List<TopoDS_Shape>& faces = edgeFaces(nearest);
            if (faces.Extent() != 2 || !analytic(faces.First()) || !analytic(faces.Last())) {
                return fail(ErrorCode::InvalidArgument,
                            "the edge nearest a point is not between two planes, cylinders, "
                            "cones or spheres");
            }
            if (std::ranges::find(picked, nearest) != picked.end()) continue;
            picked.push_back(nearest);
            fillet.Add(radiusMm, TopoDS::Edge(edgeFaces.FindKey(nearest)));
        }
        fillet.Build();
        if (!fillet.IsDone()) {
            return fail(ErrorCode::InvalidState,
                        "Open CASCADE cannot round the edges at " + std::to_string(radiusMm) +
                            " mm (too large for the faces either side?)");
        }
        TopoDS_Shape rounded;
        int solids = 0;
        for (TopExp_Explorer it(fillet.Shape(), TopAbs_SOLID); it.More(); it.Next(), ++solids) {
            rounded = it.Current();
        }
        if (solids != 1) {
            return fail(ErrorCode::InvalidState, "the rounded edges leave no single solid");
        }
        return kernelSolid(rounded, "the rounded solid is");
    });
}

} // namespace m2c::occt

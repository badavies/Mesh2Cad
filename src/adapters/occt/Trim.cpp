// SPDX-License-Identifier: GPL-3.0-or-later
// Intersect and trim (ARCHITECTURE.md section 7): primitives split against each other with
// General Fuse, the split faces on the scan kept and closed into a solid.
#include "m2c/adapters/occt/Trim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <ios>
#include <map>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_ElementarySurface.hxx>
#include <Geom_Surface.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Handle.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_State.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "PrimitiveSurface.h"
#include "Shape.h"

namespace m2c::occt {

namespace {

/// Samples per side of the grid laid over each split face's (u, v) bounds.
constexpr int kSamples = 40;

std::string point(const double x, const double y, const double z) {
    std::ostringstream out;
    const auto mm = [](const double t) { return std::round(t * 10.0) / 10.0 + 0.0; }; // no -0.0
    out << std::fixed << std::setprecision(1) << "(" << mm(x) << ", " << mm(y) << ", " << mm(z)
        << ")";
    return out.str();
}

/// The primitive's surface as a face covering `box` (its eight corners): a full turn of a
/// cylinder or cone, a whole sphere.
Result<TopoDS_Face> boundedFace(const FittedPrimitive& fit, const std::array<gp_Pnt, 8>& box) {
    auto surface = toGeomSurface(fit.surface);
    if (!surface) return std::unexpected(surface.error());
    if (fit.surface.kind == PrimitiveKind::Sphere) {
        return BRepBuilderAPI_MakeFace(*surface, Precision::Confusion()).Face();
    }
    const gp_Ax3 frame =
        opencascade::handle<Geom_ElementarySurface>::DownCast(*surface)->Position();
    std::array<double, 2> u{1e300, -1e300}, v{1e300, -1e300}, w{1e300, -1e300};
    for (const gp_Pnt& p : box) {
        const gp_Vec d(frame.Location(), p);
        const double x = d.Dot(frame.XDirection()), y = d.Dot(frame.YDirection());
        const double z = d.Dot(frame.Direction());
        u = {std::min(u[0], x), std::max(u[1], x)};
        v = {std::min(v[0], y), std::max(v[1], y)};
        w = {std::min(w[0], z), std::max(w[1], z)};
    }
    constexpr double kTurn = 2.0 * std::numbers::pi;
    switch (fit.surface.kind) {
    case PrimitiveKind::Plane:
        return BRepBuilderAPI_MakeFace(*surface, u[0], u[1], v[0], v[1], Precision::Confusion())
            .Face();
    case PrimitiveKind::Cylinder:
        return BRepBuilderAPI_MakeFace(*surface, 0.0, kTurn, w[0], w[1], Precision::Confusion())
            .Face();
    default: { // a cone: v runs along the generatrix from the apex
        const Vec3d c = (fit.extent.min + fit.extent.max) * 0.5;
        const double side = dot(c - fit.surface.point, fit.surface.direction) >= 0.0 ? 1.0 : -1.0;
        const double reach =
            std::max(side * w[0], side * w[1]) / std::cos(fit.surface.halfAngleRad);
        if (!(reach > 0.0)) return fail(ErrorCode::InvalidArgument, "a cone lies outside the scan");
        const double v0 = side > 0.0 ? 0.0 : -reach, v1 = side > 0.0 ? reach : 0.0;
        return BRepBuilderAPI_MakeFace(*surface, 0.0, kTurn, v0, v1, Precision::Confusion()).Face();
    }
    }
}

/// The share of the face's area within maxDistance of the scan, from a grid over its (u, v)
/// bounds weighted by the surface's area element; 0 when no grid point is inside the face.
double coverage(const TopoDS_Face& face, const SpatialIndex& scan, const double maxDistance) {
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const opencascade::handle<Geom_Surface> surface = BRep_Tool::Surface(face);
    const double tolerance = BRep_Tool::Tolerance(face);
    const BRepTopAdaptor_FClass2d classify(face, tolerance);
    double total = 0.0, near = 0.0;
    for (int j = 0; j < kSamples; ++j) {
        for (int i = 0; i < kSamples; ++i) {
            const double u = u0 + (u1 - u0) * (i + 0.5) / kSamples;
            const double v = v0 + (v1 - v0) * (j + 0.5) / kSamples;
            if (classify.Perform(gp_Pnt2d(u, v)) != TopAbs_IN) continue;
            gp_Pnt p;
            gp_Vec du, dv;
            surface->D1(u, v, p, du, dv);
            const double weight = du.Crossed(dv).Magnitude();
            total += weight;
            if (scan.closestPoint({.x = p.X(), .y = p.Y(), .z = p.Z()}).distance <= maxDistance) {
                near += weight;
            }
        }
    }
    return total > 0.0 ? near / total : 0.0;
}

/// The triangulation BRepMesh left on each face of the solid, in TopExp::MapShapes order and
/// turned to face outward.
Result<FaceTessellation> faceTriangulations(const TopoDS_Shape& solid) {
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces;
    TopExp::MapShapes(solid, TopAbs_FACE, faces);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> indices, triangleFaces;
    std::vector<std::array<double, 2>> uvs;
    for (int f = 1; f <= faces.Extent(); ++f) {
        const TopoDS_Face& face = TopoDS::Face(faces(f));
        TopLoc_Location location;
        const opencascade::handle<Poly_Triangulation> mesh =
            BRep_Tool::Triangulation(face, location);
        if (mesh.IsNull() || !mesh->HasUVNodes()) {
            return fail(ErrorCode::Internal, "Open CASCADE could not triangulate a face");
        }
        const gp_Trsf move = location.Transformation();
        const auto base = static_cast<std::uint32_t>(positions.size());
        for (int i = 1; i <= mesh->NbNodes(); ++i) {
            const gp_Pnt p = mesh->Node(i).Transformed(move);
            positions.push_back({.x = p.X(), .y = p.Y(), .z = p.Z()});
            uvs.push_back({mesh->UVNode(i).X(), mesh->UVNode(i).Y()});
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= mesh->NbTriangles(); ++t) {
            int a = 0, b = 0, c = 0;
            mesh->Triangle(t).Get(a, b, c);
            if (reversed) std::swap(b, c);
            for (const int k : {a, b, c}) {
                indices.push_back(base + static_cast<std::uint32_t>(k - 1));
            }
            triangleFaces.push_back(static_cast<std::uint32_t>(f - 1));
        }
    }
    auto mesh = TriMesh::create(std::move(positions), std::move(indices));
    if (!mesh) return std::unexpected(mesh.error());
    return FaceTessellation{.mesh = std::move(*mesh),
                            .triangleFaces = std::move(triangleFaces),
                            .vertexUvs = std::move(uvs)};
}

/// The faces' triangles as one mesh with equal points made one vertex: a face's nodes on an edge
/// are the edge's own points, so neighbouring faces then share them. With dropCollapsed,
/// triangles left with a repeated vertex (where a face narrows to a point, such as a sphere's
/// pole, whose nodes all lie on one point) are left out.
Result<TriMesh> weld(const TriMesh& faces, const bool dropCollapsed) {
    std::map<std::array<double, 3>, std::uint32_t> ids;
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> vertex;
    vertex.reserve(faces.vertexCount());
    for (const Vec3d& p : faces.positions()) {
        const auto [it, added] =
            ids.try_emplace({p.x, p.y, p.z}, static_cast<std::uint32_t>(positions.size()));
        if (added) positions.push_back(p);
        vertex.push_back(it->second);
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(faces.indices().size());
    for (std::size_t f = 0; f < faces.faceCount(); ++f) {
        const auto [a, b, c] = faces.face(f);
        const std::uint32_t va = vertex[a], vb = vertex[b], vc = vertex[c];
        if (dropCollapsed && (va == vb || vb == vc || vc == va)) continue;
        indices.insert(indices.end(), {va, vb, vc});
    }
    return TriMesh::create(std::move(positions), std::move(indices));
}

} // namespace

Result<FaceTessellation> triangulate(const TopoDS_Shape& solid) {
    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    const double deflection = 1e-3 * std::sqrt(box.SquareExtent());
    BRepMesh_IncrementalMesh const mesher(solid, deflection, false, 0.3, false);
    return faceTriangulations(solid);
}

Result<TriMesh> weldedTriangulation(const Solid& solid) {
    return guarded("triangulating the solid", [&] -> Result<TriMesh> {
        auto faces = triangulate(solid.shape().shape);
        if (!faces) return std::unexpected(faces.error());
        return weld(faces->mesh, false);
    });
}

Result<TriMesh> exportTriangulation(const Solid& solid, const ExportTessellation& params) {
    if (!(params.chordMm > 0.0) || !(params.angleDeg > 0.0) || !(params.angleDeg <= 90.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "the chord must be positive and the angle between 0 and 90 degrees");
    }
    return guarded("triangulating the solid for export", [&] -> Result<TriMesh> {
        // A copy of the topology without triangulations: BRepMesh keeps a face's triangulation
        // when it is already finer than asked, and the solid's own display stays as it was.
        const TopoDS_Shape copy = BRepBuilderAPI_Copy(solid.shape().shape, false, false).Shape();
        BRepMesh_IncrementalMesh const mesher(
            copy, params.chordMm, false, params.angleDeg * std::numbers::pi / 180.0, false);
        auto faces = faceTriangulations(copy);
        if (!faces) return std::unexpected(faces.error());
        return weld(faces->mesh, true);
    });
}

Result<Solid> kernelSolid(const TopoDS_Shape& solid, const std::string& what) {
    const SolidSummary summary = summarise(solid);
    if (summary.solidCount != 1 || !summary.closed || !summary.valid ||
        !(summary.volumeMm3 > 0.0)) {
        return fail(ErrorCode::InvalidState,
                    what + " not one valid closed solid (" + std::to_string(summary.freeEdges) +
                        " free edges, " + (summary.valid ? "valid" : "invalid") + ", volume " +
                        std::to_string(summary.volumeMm3) + " mm^3)");
    }
    auto display = triangulate(solid);
    if (!display) return std::unexpected(display.error());
    auto shape = std::make_shared<SolidShape>();
    shape->shape = solid;
    return Solid(std::move(shape),
                 std::make_shared<const std::vector<SurfacePatch>>(),
                 std::make_shared<const FaceTessellation>(std::move(*display)),
                 summary);
}

Result<TrimResult> trimPrimitives(const std::vector<FittedPrimitive>& primitives,
                                  const TriMesh& scan,
                                  const TrimParams& params) {
    if (primitives.empty()) return fail(ErrorCode::InvalidArgument, "no primitives to trim");
    if (!(params.maxDistanceMm > 0.0) || !(params.minCoverage > 0.0) ||
        !(params.minCoverage <= 1.0) || !(params.sewToleranceMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "the distance, sewing tolerance and coverage (0 to 1) must be positive");
    }
    const Aabb& bounds = scan.bounds();
    const double diagonal = length(bounds.max - bounds.min);
    const double margin = std::max(0.1 * diagonal, 10.0 * params.maxDistanceMm);
    std::array<gp_Pnt, 8> box;
    for (unsigned k = 0; k < 8; ++k) {
        box[static_cast<std::size_t>(k)] =
            gp_Pnt((k & 1U) != 0 ? bounds.max.x + margin : bounds.min.x - margin,
                   (k & 2U) != 0 ? bounds.max.y + margin : bounds.min.y - margin,
                   (k & 4U) != 0 ? bounds.max.z + margin : bounds.min.z - margin);
    }
    return guarded("trimming", [&] -> Result<TrimResult> {
        NCollection_List<TopoDS_Shape> faces;
        for (const FittedPrimitive& fit : primitives) {
            auto face = boundedFace(fit, box);
            if (!face) return std::unexpected(face.error());
            faces.Append(*face);
        }
        BRepAlgoAPI_BuilderAlgo split;
        split.SetArguments(faces);
        split.SetRunParallel(false);
        split.Build();
        if (!split.IsDone() || split.HasErrors()) {
            return fail(ErrorCode::Internal, "Open CASCADE could not split the primitives");
        }
        std::size_t splitFaces = 0, keptFaces = 0;
        const SpatialIndex& index = scan.spatialIndex();
        BRepBuilderAPI_Sewing sewing(params.sewToleranceMm);
        for (TopExp_Explorer it(split.Shape(), TopAbs_FACE); it.More(); it.Next()) {
            ++splitFaces;
            if (const TopoDS_Face& face = TopoDS::Face(it.Current());
                coverage(face, index, params.maxDistanceMm) >= params.minCoverage) {
                sewing.Add(face);
                ++keptFaces;
            }
        }
        if (keptFaces == 0) {
            return fail(ErrorCode::InvalidState, "no part of the primitives lies on the scan");
        }
        sewing.Perform();
        if (sewing.NbFreeEdges() > 0) {
            Bnd_Box open;
            for (int i = 1; i <= sewing.NbFreeEdges(); ++i)
                BRepBndLib::Add(sewing.FreeEdge(i), open);
            double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
            open.Get(x0, y0, z0, x1, y1, z1);
            return fail(ErrorCode::InvalidState,
                        "the trimmed faces leave the solid open between " + point(x0, y0, z0) +
                            " and " + point(x1, y1, z1) + " mm (" +
                            std::to_string(sewing.NbFreeEdges()) +
                            " open edges): a primitive there is missing or does not fit the scan");
        }
        std::vector<TopoDS_Shell> shells;
        for (TopExp_Explorer it(sewing.SewedShape(), TopAbs_SHELL); it.More(); it.Next()) {
            shells.push_back(TopoDS::Shell(it.Current()));
        }
        if (shells.size() != 1) {
            return fail(ErrorCode::InvalidState,
                        "the trimmed faces make " + std::to_string(shells.size()) +
                            " shells instead of one");
        }
        ShapeUpgrade_UnifySameDomain merge(
            ShapeFix_Solid().SolidFromShell(shells.front()), true, true, false);
        merge.Build();
        TopoDS_Shape solid;
        for (TopExp_Explorer it(merge.Shape(), TopAbs_SOLID); it.More(); it.Next()) {
            solid = it.Current();
        }
        auto made = kernelSolid(solid, "the trimmed faces are");
        if (!made) return std::unexpected(made.error());
        return TrimResult{
            .solid = std::move(*made), .splitFaces = splitFaces, .keptFaces = keptFaces};
    });
}

} // namespace m2c::occt

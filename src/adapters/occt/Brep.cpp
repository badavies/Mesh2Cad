// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/adapters/occt/Brep.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <ios>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BinTools.hxx>
#include <Bnd_Box.hxx>
#include <ElSLib.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BSplineSurface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <NCollection_Array1.hxx>
#include <NCollection_Array2.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <Precision.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Handle.hxx>
#include <Standard_Version.hxx>
#include <StepBasic_Product.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/Vec3.h"

#include "Guard.h"
#include "Shape.h"

namespace m2c::occt {

namespace {

std::string utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

/// Knots with multiplicity four at the ends and three inside: pieces meet with position
/// continuity.
void bezierKnots(const std::vector<double>& breaks,
                 NCollection_Array1<double>& knots,
                 NCollection_Array1<int>& mults) {
    const auto last = static_cast<int>(breaks.size()) - 1;
    knots.Resize(1, last + 1, false);
    mults.Resize(1, last + 1, false);
    for (int k = 0; k <= last; ++k) {
        knots.SetValue(k + 1, breaks[static_cast<std::size_t>(k)]);
        mults.SetValue(k + 1, k == 0 || k == last ? 4 : 3);
    }
}

/// Knots between pieces that join smoothly are removed (or lowered to multiplicity one or two)
/// when the surface moves by no more than this: regular regions of a merged face then become one
/// C2 B-spline, and only the knots next to caps stay triple.
constexpr double kKnotRemovalToleranceMm = 1e-7;

void removeKnots(Geom_BSplineSurface& surface) {
    for (int k = surface.NbUKnots() - 1; k >= 2; --k) {
        const int m = surface.UMultiplicity(k);
        for (int target = 0; target < m; ++target) {
            if (surface.RemoveUKnot(k, target, kKnotRemovalToleranceMm)) break;
        }
    }
    for (int k = surface.NbVKnots() - 1; k >= 2; --k) {
        const int m = surface.VMultiplicity(k);
        for (int target = 0; target < m; ++target) {
            if (surface.RemoveVKnot(k, target, kKnotRemovalToleranceMm)) break;
        }
    }
}

opencascade::handle<Geom_BSplineSurface> toSurface(const SurfacePatch& patch) {
    const auto row = static_cast<int>(patch.polesPerRow());
    const auto rows = static_cast<int>(patch.poleRows());
    NCollection_Array2<gp_Pnt> poles(1, row, 1, rows);
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < row; ++i) {
            const auto& [x, y, z] =
                patch.poles[static_cast<std::size_t>(j) * static_cast<std::size_t>(row) +
                            static_cast<std::size_t>(i)];
            poles.SetValue(i + 1, j + 1, gp_Pnt(x, y, z));
        }
    }
    NCollection_Array1<double> uKnots(1, 2), vKnots(1, 2);
    NCollection_Array1<int> uMults(1, 2), vMults(1, 2);
    bezierKnots(patch.uBreaks, uKnots, uMults);
    bezierKnots(patch.vBreaks, vKnots, vMults);
    opencascade::handle surface =
        new Geom_BSplineSurface(poles, uKnots, vKnots, uMults, vMults, 3, 3);
    removeKnots(*surface);
    return surface;
}

} // namespace

SolidSummary summarise(const TopoDS_Shape& shape) {
    SolidSummary s;
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces, edges, vertices;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
    for (TopExp_Explorer it(shape, TopAbs_SHELL); it.More(); it.Next()) ++s.shellCount;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) ++s.solidCount;
    s.faceCount = static_cast<std::size_t>(faces.Extent());
    s.edgeCount = static_cast<std::size_t>(edges.Extent());
    s.vertexCount = static_cast<std::size_t>(vertices.Extent());
    for (int i = 1; i <= faces.Extent(); ++i) {
        const auto surface = opencascade::handle<Geom_BSplineSurface>::DownCast(
            BRep_Tool::Surface(TopoDS::Face(faces(i))));
        if (!surface.IsNull()) {
            s.poleCount += static_cast<std::size_t>(surface->NbUPoles() * surface->NbVPoles());
        }
    }
    NCollection_IndexedDataMap<TopoDS_Shape,
                               NCollection_List<TopoDS_Shape>,
                               TopTools_ShapeMapHasher>
        edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edgeFaces.FindKey(i));
        if (!BRep_Tool::Degenerated(edge) && edgeFaces(i).Extent() < 2) ++s.freeEdges;
        s.maxToleranceMm = std::max(s.maxToleranceMm, BRep_Tool::Tolerance(edge));
    }
    for (int i = 1; i <= vertices.Extent(); ++i) {
        s.maxToleranceMm =
            std::max(s.maxToleranceMm, BRep_Tool::Tolerance(TopoDS::Vertex(vertices(i))));
    }
    s.closed = s.shellCount > 0 && s.freeEdges == 0;
    s.valid = BRepCheck_Analyzer(shape).IsValid();
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false); // tight: from the surfaces, not poles
    if (!box.IsVoid()) {
        double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
        box.Get(x0, y0, z0, x1, y1, z1);
        s.bounds = {.min = {.x = x0, .y = y0, .z = z0}, .max = {.x = x1, .y = y1, .z = z1}};
    }
    GProp_GProps volume, area;
    BRepGProp::VolumeProperties(shape, volume);
    BRepGProp::SurfaceProperties(shape, area);
    s.volumeMm3 = volume.Mass();
    s.areaMm2 = area.Mass();
    return s;
}

Result<Solid> makeSolid(std::vector<SurfacePatch> faces,
                        std::shared_ptr<const FaceTessellation> display,
                        const SewParams& params) {
    if (faces.empty()) return fail(ErrorCode::InvalidArgument, "no faces to sew");
    if (!(params.toleranceMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the sewing tolerance must be positive");
    }
    if (!display) return fail(ErrorCode::InvalidArgument, "a solid needs a display mesh");
    return guarded("sewing", [&]() -> Result<Solid> {
        BRepBuilderAPI_Sewing sewing(params.toleranceMm);
        for (const SurfacePatch& patch : faces) {
            BRepBuilderAPI_MakeFace const face(toSurface(patch), Precision::Confusion());
            if (!face.IsDone()) {
                return fail(ErrorCode::Internal, "Open CASCADE could not make a face");
            }
            sewing.Add(face.Face());
        }
        sewing.Perform();
        const TopoDS_Shape sewn = sewing.SewedShape();
        std::vector<TopoDS_Shell> shells;
        for (TopExp_Explorer it(sewn, TopAbs_SHELL); it.More(); it.Next()) {
            shells.push_back(TopoDS::Shell(it.Current()));
        }
        if (shells.size() != 1) {
            return fail(ErrorCode::InvalidState,
                        "the faces sewed into " + std::to_string(shells.size()) +
                            " shells instead of one (" + std::to_string(sewing.NbFreeEdges()) +
                            " free edges)");
        }
        // A solid bounded by the shell, oriented so its normals point outward.
        const TopoDS_Solid solid = ShapeFix_Solid().SolidFromShell(shells.front());
        const SolidSummary summary = summarise(solid);
        if (summary.solidCount != 1 || !summary.closed || !summary.valid) {
            return fail(ErrorCode::InvalidState,
                        "the sewn faces are not one valid closed solid (" +
                            std::to_string(summary.freeEdges) + " free edges, " +
                            (summary.valid ? "valid" : "invalid") + ")");
        }
        // ShapeFix_Solid orients a shell outward; one that crosses itself can still come out
        // with negative volume, and BRepCheck does not report the crossing.
        if (!(summary.volumeMm3 > 0.0)) {
            return fail(ErrorCode::InvalidState,
                        "the sewn faces cross themselves (the solid's volume is " +
                            std::to_string(summary.volumeMm3) + " mm^3)");
        }
        // A face that sewed into no shell is left out of the solid; the project file then
        // could not be read back, as its face table would not match.
        if (summary.faceCount != faces.size()) {
            return fail(ErrorCode::InvalidState,
                        "the sewn shell has " + std::to_string(summary.faceCount) + " of the " +
                            std::to_string(faces.size()) + " faces");
        }
        auto shape = std::make_shared<SolidShape>();
        shape->shape = solid;
        return Solid(std::move(shape),
                     std::make_shared<const std::vector<SurfacePatch>>(std::move(faces)),
                     std::move(display),
                     summary);
    });
}

namespace {

/// A face on the plane around its fit's extent: the extent's corners projected onto the plane,
/// bounded along the plane's own axes.
Result<TopoDS_Shape> planeFace(const FittedPrimitive& fit) {
    const Primitive& s = fit.surface;
    if (s.kind != PrimitiveKind::Plane || !(length(s.direction) > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "only a plane primitive can be written as a plane");
    }
    const gp_Pln plane(gp_Pnt(s.point.x, s.point.y, s.point.z),
                       gp_Dir(s.direction.x, s.direction.y, s.direction.z));
    double u0 = Precision::Infinite(), u1 = -Precision::Infinite();
    double v0 = u0, v1 = u1;
    for (int c = 0; c < 8; ++c) {
        const gp_Pnt corner((c & 1) != 0 ? fit.extent.max.x : fit.extent.min.x,
                            (c & 2) != 0 ? fit.extent.max.y : fit.extent.min.y,
                            (c & 4) != 0 ? fit.extent.max.z : fit.extent.min.z);
        double u = 0.0, v = 0.0;
        ElSLib::Parameters(plane, corner, u, v);
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, v);
        v1 = std::max(v1, v);
    }
    if (!(u1 - u0 > Precision::Confusion()) || !(v1 - v0 > Precision::Confusion())) {
        return fail(ErrorCode::InvalidArgument, "the plane's extent is empty");
    }
    return TopoDS_Shape(BRepBuilderAPI_MakeFace(plane, u0, u1, v0, v1).Face());
}

/// The shape's solids summarised together, and counts of what is outside them.
StepContents contents(const TopoDS_Shape& shape) {
    TopoDS_Compound solids;
    BRep_Builder builder;
    builder.MakeCompound(solids);
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
        builder.Add(solids, it.Current());
    }
    StepContents out{.solids = summarise(solids)};
    for (TopExp_Explorer it(shape, TopAbs_FACE, TopAbs_SOLID); it.More(); it.Next()) {
        ++out.looseFaces;
    }
    for (TopExp_Explorer it(shape, TopAbs_EDGE, TopAbs_FACE); it.More(); it.Next()) {
        ++out.looseEdges;
    }
    return out;
}

} // namespace

Result<StepContents> writeStep(std::span<const StepPart> parts, const std::filesystem::path& path) {
    if (parts.empty()) return fail(ErrorCode::InvalidArgument, "there is nothing to write");
    return guarded("STEP export", [&]() -> Result<StepContents> {
        std::vector<TopoDS_Shape> shapes;
        for (const StepPart& part : parts) {
            Result<TopoDS_Shape> shape = TopoDS_Shape();
            if (const auto* solid = std::get_if<const Solid*>(&part.shape)) {
                if (!BRepCheck_Analyzer((*solid)->shape().shape).IsValid()) {
                    return fail(ErrorCode::InvalidState,
                                "the solid is not valid; nothing was written");
                }
                shape = (*solid)->shape().shape;
            } else if (const auto* sketch = std::get_if<const Sketch*>(&part.shape)) {
                shape = sketchWires(**sketch);
            } else {
                shape = planeFace(*std::get<const FittedPrimitive*>(part.shape));
            }
            if (!shape) return std::unexpected(shape.error());
            shapes.push_back(std::move(*shape));
        }
        STEPControl_Writer writer; // initialises the STEP parameters set below
        Interface_Static::SetCVal("write.step.schema", "AP242DIS");
        Interface_Static::SetCVal("write.step.unit", "MM");
        // No assembly splitting: a sketch of several loops is one product, not one per loop.
        Interface_Static::SetIVal("write.step.assembly", 0);
        for (std::size_t k = 0; k < parts.size(); ++k) {
            const int before = writer.Model()->NbEntities();
            if (writer.Transfer(shapes[k], STEPControl_AsIs) != IFSelect_RetDone) {
                return fail(ErrorCode::Internal,
                            "Open CASCADE could not convert " + parts[k].name + " to STEP");
            }
            // The transfer's product, which Open CASCADE names with a number after it.
            const opencascade::handle<TCollection_HAsciiString> name =
                new TCollection_HAsciiString(parts[k].name.c_str());
            for (int i = before + 1; i <= writer.Model()->NbEntities(); ++i) {
                const auto product =
                    opencascade::handle<StepBasic_Product>::DownCast(writer.Model()->Value(i));
                if (!product.IsNull()) {
                    product->SetId(name);
                    product->SetName(name);
                }
            }
        }
        if (writer.Write(utf8(path).c_str()) != IFSelect_RetDone) {
            return fail(ErrorCode::Io, "could not write " + utf8(path));
        }
        TopoDS_Compound all;
        BRep_Builder builder;
        builder.MakeCompound(all);
        for (const TopoDS_Shape& shape : shapes) builder.Add(all, shape);
        return contents(all);
    });
}

Result<void>
writeStep(const Solid& solid, const std::filesystem::path& path, const std::string& name) {
    const StepPart part{.name = name, .shape = &solid};
    auto written = writeStep(std::span(&part, 1), path);
    if (!written) return std::unexpected(written.error());
    return {};
}

Result<std::string> writeBrep(const Solid& solid, const BrepFormat format) {
    return guarded("BREP export", [&]() -> Result<std::string> {
        std::ostringstream out(std::ios::binary);
        if (format == BrepFormat::Binary) {
            BinTools::Write(solid.shape().shape, out);
        } else {
            BRepTools::Write(solid.shape().shape, out);
        }
        if (!out) return fail(ErrorCode::Internal, "Open CASCADE could not write the solid");
        return out.str();
    });
}

Result<Solid> readBrep(const std::string& brep,
                       const BrepFormat format,
                       std::vector<SurfacePatch> faces,
                       std::shared_ptr<const FaceTessellation> display) {
    if (!display && !faces.empty()) {
        return fail(ErrorCode::InvalidArgument, "a solid needs a display mesh");
    }
    return guarded("BREP import", [&]() -> Result<Solid> {
        std::istringstream in(brep, std::ios::binary);
        TopoDS_Shape shape;
        if (format == BrepFormat::Binary) {
            BinTools::Read(shape, in);
        } else {
            BRepTools::Read(shape, in, BRep_Builder());
        }
        if (shape.IsNull()) return fail(ErrorCode::Parse, "the BREP data holds no shape");
        const SolidSummary summary = summarise(shape);
        if (summary.solidCount != 1 || !summary.closed || !summary.valid) {
            return fail(ErrorCode::Parse, "the BREP data is not one valid closed solid");
        }
        if (!faces.empty() && summary.faceCount != faces.size()) {
            return fail(ErrorCode::Parse, "the BREP data does not match the solid's faces");
        }
        auto stored = std::make_shared<SolidShape>();
        for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
            stored->shape = it.Current();
        }
        if (faces.empty()) {
            auto kernel = triangulate(stored->shape);
            if (!kernel) return std::unexpected(kernel.error());
            display = std::make_shared<const FaceTessellation>(std::move(*kernel));
        }
        return Solid(std::move(stored),
                     std::make_shared<const std::vector<SurfacePatch>>(std::move(faces)),
                     std::move(display),
                     summary);
    });
}

Result<std::vector<double>> signedDistances(const Solid& solid, const std::vector<Vec3d>& points) {
    const std::vector<SurfacePatch>& faces = solid.faces();
    const auto& [display, triangleFaces, vertexUvs] = solid.displayMap();
    // A solid with no B-spline faces is measured against its kernel faces.
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> kernelFaces;
    if (faces.empty()) TopExp::MapShapes(solid.shape().shape, TopAbs_FACE, kernelFaces);
    const std::size_t faceCount =
        faces.empty() ? static_cast<std::size_t>(kernelFaces.Extent()) : faces.size();
    if (faceCount == 0 || triangleFaces.size() != display.faceCount() ||
        vertexUvs.size() != display.vertexCount() ||
        std::ranges::any_of(triangleFaces, [&](const std::uint32_t f) { return f >= faceCount; })) {
        return fail(ErrorCode::InvalidState, "the solid's display mesh does not match its faces");
    }
    const auto corners = [&](const std::size_t t) {
        const auto v = display.face(t);
        const auto& p = display.positions();
        return std::array{p[v[0]], p[v[1]], p[v[2]]};
    };
    // Faces point outward when the display mesh, which follows their u x v, has positive volume.
    double volume6 = 0.0;
    for (std::size_t t = 0; t < display.faceCount(); ++t) {
        const auto [a, b, c] = corners(t);
        volume6 += dot(a, cross(b, c));
    }
    const double outward = volume6 >= 0.0 ? 1.0 : -1.0;
    return guarded("distance to the solid", [&]() -> Result<std::vector<double>> {
        std::vector<opencascade::handle<ShapeAnalysis_Surface>> surfaces(faceCount);
        std::vector<std::array<double, 4>> ranges(faceCount, {0.0, 1.0, 0.0, 1.0});
        std::vector<double> turn(faceCount, 1.0); // -1 where a kernel face is reversed
        const auto surfaceOf = [&](const std::size_t f) {
            if (!surfaces[f].IsNull()) return;
            if (!faces.empty()) {
                surfaces[f] = new ShapeAnalysis_Surface(toSurface(faces[f]));
                return;
            }
            const TopoDS_Face& face = TopoDS::Face(kernelFaces(static_cast<int>(f) + 1));
            surfaces[f] = new ShapeAnalysis_Surface(BRep_Tool::Surface(face));
            auto& [u0, u1, v0, v1] = ranges[f];
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            if (face.Orientation() == TopAbs_REVERSED) turn[f] = -1.0;
        };
        const SpatialIndex& index = display.spatialIndex();
        std::vector<double> out;
        out.reserve(points.size());
        for (const Vec3d& p : points) {
            const ClosestPoint near = index.closestPoint(p);
            const std::size_t f = triangleFaces[near.face];
            // Barycentric weights of the closest point give its (u, v) from the corners'.
            const auto v = display.face(near.face);
            const auto [a, b, c] = corners(near.face);
            const Vec3d e0 = b - a, e1 = c - a, e2 = near.point - a;
            const double d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1);
            const double d20 = dot(e2, e0), d21 = dot(e2, e1);
            const double den = d00 * d11 - d01 * d01;
            const double wb = den > 0.0 ? (d11 * d20 - d01 * d21) / den : 0.0;
            const double wc = den > 0.0 ? (d00 * d21 - d01 * d20) / den : 0.0;
            const auto& ua = vertexUvs[v[0]];
            const auto& ub = vertexUvs[v[1]];
            const auto& uc = vertexUvs[v[2]];
            const double u0 = ua[0] + wb * (ub[0] - ua[0]) + wc * (uc[0] - ua[0]);
            const double v0 = ua[1] + wb * (ub[1] - ua[1]) + wc * (uc[1] - ua[1]);
            surfaceOf(f);
            const gp_Pnt target(p.x, p.y, p.z);
            const gp_Pnt2d at = surfaces[f]->NextValueOfUV(gp_Pnt2d(u0, v0), target, 1e-7);
            gp_Pnt q;
            gp_Vec du, dv;
            const auto& [uMin, uMax, vMin, vMax] = ranges[f];
            surfaces[f]->Surface()->D1(
                std::clamp(at.X(), uMin, uMax), std::clamp(at.Y(), vMin, vMax), q, du, dv);
            const gp_Vec normal = du.Crossed(dv) * turn[f];
            const gp_Vec offset(q, target);
            const double side = offset.Dot(normal) * outward >= 0.0 ? 1.0 : -1.0;
            out.push_back(side * offset.Magnitude());
        }
        return out;
    });
}

Result<StepContents> readStep(const std::filesystem::path& path) {
    return guarded("STEP import", [&]() -> Result<StepContents> {
        STEPControl_Reader reader;
        if (reader.ReadFile(utf8(path).c_str()) != IFSelect_RetDone) {
            return fail(ErrorCode::Parse, "could not read " + utf8(path) + " as STEP");
        }
        reader.TransferRoots();
        return contents(reader.OneShape());
    });
}

} // namespace m2c::occt

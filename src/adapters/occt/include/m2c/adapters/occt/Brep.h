// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// B-rep solids and STEP files with Open CASCADE (ADR-003, ADR-005). Only core types cross this
// header; the kernel shape travels as the opaque SolidShape inside a Solid.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SurfacePatch.h"
#include "m2c/core/TriMesh.h"

namespace m2c::occt {

struct SewParams {
    double toleranceMm{1e-4}; ///< Edges closer than this are joined.
};

/// Makes one B-spline face per patch, sews them into one closed shell and makes an outward solid
/// of it. Fails with InvalidState unless the result is one closed solid the kernel's checker
/// accepts. The display mesh is stored with the solid as given.
[[nodiscard]] Result<Solid> makeSolid(std::vector<SurfacePatch> faces,
                                      std::shared_ptr<const FaceTessellation> display,
                                      const SewParams& params);

/// One product of a STEP file (ADR-034): a solid; a sketch, as one wire per loop; or a fitted
/// plane, as a rectangular face around its fit's extent.
struct StepPart {
    std::string name; ///< The product's name.
    std::variant<const Solid*, const Sketch*, const FittedPrimitive*> shape;
};

/// What a STEP file holds, to check an export.
struct StepContents {
    SolidSummary solids;       ///< The file's solids, summarised together.
    std::size_t looseFaces{0}; ///< Faces in no solid, such as planes.
    std::size_t looseEdges{0}; ///< Edges on no face, such as a sketch's curves.
};

/// Writes the parts as a STEP AP242 file in millimetres, each its own product, in order, and
/// returns what was written as readStep counts it. Checks the solids first and writes nothing if
/// the kernel's checker rejects one. Fails with InvalidArgument for no parts, a primitive that is
/// not a plane, or a sketch extrudeSketch would refuse for its loops (none, an open one, or a
/// curve without its points).
[[nodiscard]] Result<StepContents> writeStep(std::span<const StepPart> parts,
                                             const std::filesystem::path& path);

/// Writes the solid as a STEP AP242 file in millimetres; `name` becomes the product name. Checks
/// the solid first and writes nothing if the kernel's checker rejects it.
[[nodiscard]] Result<void>
writeStep(const Solid& solid, const std::filesystem::path& path, const std::string& name);

/// Open CASCADE's two BREP formats. Text keeps 15 significant digits, so a solid read back can
/// differ in the last bits; binary keeps every bit.
enum class BrepFormat {
    Text,
    Binary
};

/// The solid's kernel shape as BREP data, for the project file (which uses binary).
[[nodiscard]] Result<std::string> writeBrep(const Solid& solid,
                                            BrepFormat format = BrepFormat::Binary);

/// A solid from BREP data and the faces and display mesh saved with it. A solid with no B-spline
/// faces (trimPrimitives()) is given none and no display mesh: its display is the kernel's
/// triangulation again. Fails unless the data holds one valid closed solid.
[[nodiscard]] Result<Solid> readBrep(const std::string& brep,
                                     BrepFormat format,
                                     std::vector<SurfacePatch> faces,
                                     std::shared_ptr<const FaceTessellation> display);

/// Signed distance from each point to the solid's faces, positive outside: the closest display
/// triangle gives the face and, through the display map, a start point on it; Open CASCADE then
/// projects onto that face's surface (the B-spline face, or for a solid with none the kernel
/// face's own surface), staying within the face's parameter range. Runs in parallel chunks
/// (parallelChunks()), each with its own projectors, so the results are the same on any number of
/// threads. Stops with Cancelled within a chunk of `progress` being cancelled.
[[nodiscard]] Result<std::vector<double>> signedDistances(const Solid& solid,
                                                          const std::vector<Vec3d>& points,
                                                          const Progress& progress = {});

/// The kernel's triangulation of the solid's faces (within 0.1% of its diagonal of them) as one
/// mesh, welded where faces meet: neighbouring faces share their common edge's points, so the mesh
/// is closed where the solid is. For checks on the solid's shape, such as cgal::selfIntersects().
[[nodiscard]] Result<TriMesh> weldedTriangulation(const Solid& solid);

/// Reads a STEP file and summarises its solids, and counts what is outside them.
[[nodiscard]] Result<StepContents> readStep(const std::filesystem::path& path);

} // namespace m2c::occt

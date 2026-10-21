// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// B-rep solids and STEP files with Open CASCADE (ADR-003, ADR-005). Only core types cross this
// header; the kernel shape travels as the opaque SolidShape inside a Solid.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "m2c/core/Error.h"
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

/// Writes the solid as a STEP AP242 file in millimetres; `name` becomes the product name. Checks
/// the solid first and writes nothing if the kernel's checker rejects it.
[[nodiscard]] Result<void>
writeStep(const Solid& solid, const std::filesystem::path& path, const std::string& name);

/// One body of a STEP file that holds several (writeStep below): a solid, or a sketch.
struct StepBody {
    std::string name;
    std::variant<Solid, Sketch> shape;
};

/// Writes the bodies to one STEP AP242 file in millimetres, in order, each as a product of its
/// own named after the body (Fusion 360 makes each a named component). Fusion leaves out loose
/// curves (it imports faces and solids only), so a sketch is written as its loops' planar faces
/// (a surface body): every loop not inside another bounds a face and the loops directly inside it
/// are its holes. Lines and arcs stay exact; a spline is interpolated through its points. Returns
/// each body's face count. Writes nothing, failing with InvalidArgument, for no bodies or a sketch
/// with an open loop or none, or with InvalidState if the kernel's checker rejects a body.
[[nodiscard]] Result<std::vector<std::size_t>> writeStep(const std::vector<StepBody>& bodies,
                                                         const std::filesystem::path& path);

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
/// face's own surface), staying within the face's parameter range.
[[nodiscard]] Result<std::vector<double>> signedDistances(const Solid& solid,
                                                          const std::vector<Vec3d>& points);

/// Reads a STEP file and summarises everything in it together (used to check an export).
[[nodiscard]] Result<SolidSummary> readStep(const std::filesystem::path& path);

/// Reads a STEP file and summarises each body (each root shape) on its own, in the file's order
/// (used to check an export of several bodies).
[[nodiscard]] Result<std::vector<SolidSummary>> readStepBodies(const std::filesystem::path& path);

} // namespace m2c::occt

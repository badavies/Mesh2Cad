// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// B-rep solids and STEP files with Open CASCADE (ADR-003, ADR-005). Only core types cross this
// header; the kernel shape travels as the opaque SolidShape inside a Solid.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "m2c/core/Error.h"
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

/// The solid's kernel shape in Open CASCADE's BREP text format, for the project file.
[[nodiscard]] Result<std::string> writeBrep(const Solid& solid);

/// A solid from BREP text and the faces and display mesh saved with it. Fails unless the text
/// holds one valid closed solid.
[[nodiscard]] Result<Solid> readBrep(const std::string& brep,
                                     std::vector<SurfacePatch> faces,
                                     std::shared_ptr<const FaceTessellation> display);

/// Signed distance from each point to the solid's faces, positive outside: the closest display
/// triangle gives the face and, through the display map, a start point on it; Open CASCADE then
/// projects onto that face's surface.
[[nodiscard]] Result<std::vector<double>> signedDistances(const Solid& solid,
                                                          const std::vector<Vec3d>& points);

/// Reads a STEP file and summarises everything in it together (used to check an export).
[[nodiscard]] Result<SolidSummary> readStep(const std::filesystem::path& path);

} // namespace m2c::occt

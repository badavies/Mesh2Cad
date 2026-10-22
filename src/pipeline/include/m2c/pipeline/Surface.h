// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Surface build (ARCHITECTURE.md section 6): fit a closed quad cage to the scan, turn its
// Catmull-Clark limit surface into B-spline faces (OpenSubdiv adapter) and sew them into a solid
// (Open CASCADE adapter).

#include <cstddef>
#include <memory>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/adapters/osd/Subdivision.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Vec3.h"
#include "m2c/pipeline/PatchMerge.h"

namespace m2c {

using osd::CageFitParams;
using osd::CageFitResult;
using osd::extractPatches;
using osd::fitCage;
using osd::HeldPlane;
using osd::limitNormals;
using osd::PatchParams;
using osd::PatchResult;
using osd::subdivideCage;
using osd::tessellateLimit;

/// An area of the scan that is flat: the cage over it is held on the plane while fitting, so the
/// surface there is that plane (scan noise and bumps are not followed) and blends into the rest
/// at the area's edge.
struct FlatArea {
    Vec3d point;                                ///< On the plane.
    Vec3d normal;                               ///< Unit.
    std::shared_ptr<const MeshSelection> faces; ///< Faces of the scan the area covers.
};

/// The vertices of `cage` held by each area: those whose closest scan face is in its selection.
/// Fails with InvalidArgument for a selection of another size than the scan or a normal that is
/// not a unit vector.
[[nodiscard]] Result<std::vector<HeldPlane>>
heldPlanes(const QuadLayout& cage, const TriMesh& scan, const std::vector<FlatArea>& areas);

struct SurfaceBuildParams {
    bool fit{true};        ///< Fit the cage to the scan first; off = use the cage as it is.
    CageFitParams fitting; ///< Its isolation level is used for the faces too.
    /// A fit whose faces meet at a larger angle has folded the surface and is tried again with
    /// twice the smoothing, up to fitTries fits in all.
    double maxSeamAngleDeg{10.0};
    unsigned fitTries{4};
    /// Faces of the kept try that meet at a larger angle fold the surface over itself; the build
    /// then fails rather than return a solid that crosses itself.
    double maxFoldAngleDeg{90.0};
    /// Of a closed cage: edges where its faces turn by more than this (sharpCageEdges()) are
    /// sharp edges of the surface, as a part's sharp edges are where the cage was aligned to
    /// them (quad.remesh crease_angle_deg). With a scan, an edge stays only where the scan's
    /// normals under the two faces differ by more than half the angle too, so a distorted cage
    /// gets no crease on a smooth stretch. The fit, the faces and the merge keep them sharp, and
    /// the angle checks leave them out. 0 = none: the surface is smooth everywhere.
    double creaseAngleDeg{60.0};
    /// Check that the finished solid's surface does not cross itself, on the kernel's
    /// triangulation (occt::weldedTriangulation(), cgal::selfIntersects()), and fail if it does.
    /// A surface can cross itself with no fold the angle check sees, as a fit can that overshoots
    /// a sharp edge; booleans on such a solid go wrong.
    bool checkCrossing{true};
    /// Deviation-driven refinement: while the fit's 95th-percentile sample distance is above
    /// this, the fitted cage gets one Catmull-Clark step (four times the quads, same layout) and
    /// is fitted again, up to maxRefinements times. 0 = off.
    double refineToMm{0.0};
    unsigned maxRefinements{2};
    double sewToleranceMm{1e-4};
    /// A sheet: the cage is open (the scan shows one side of a thin part). It is fitted open,
    /// then closed into a wall of this thickness against its normals (thickenCage()) before the
    /// faces are made. 0 = the cage must be closed.
    double thicknessMm{0.0};
    /// Merge rectangles of cage quads into one face each (patch merging; mergeFaces()).
    bool merge{true};
    int displayGrid{4}; ///< Display triangles: 2 n^2 per face.
    /// Flat areas held on their planes (heldPlanes()), worked out again for each cage fitted, so
    /// they survive refinement. Needs fit.
    std::vector<FlatArea> flat{};
};

struct SurfaceBuildResult {
    Solid solid;
    QuadLayout
        cage; ///< The cage the faces come from (fitted unless fit was off; thickened for a sheet).
    CageFitResult fit;     ///< Zero samples when fit was off.
    double smoothing{0.0}; ///< The smoothing of the fit that was kept.
    unsigned fits{0};      ///< Fits made on the final cage (more than one when a fit folded it).
    std::size_t regularPieces{0};
    std::size_t capPieces{0};
    double maxCapErrorMm{0.0};
    SeamReport seams; ///< Gaps and normal angles where faces and pieces meet, before merging.
    std::size_t facesBeforeMerge{0}; ///< One per cage quad (or corner quad of another face).
    std::size_t mergedFaces{0};      ///< Faces made from more than one cage quad.
    std::size_t unmergedQuads{0};    ///< Quads left single because their region was no rectangle.
    /// The fit's 95th-percentile distance before each refinement; empty when none was made.
    std::vector<double> p95BeforeRefinementMm;
    std::vector<std::size_t> heldVertices; ///< Per flat area, cage vertices held in the last fit.
    std::size_t sharpEdges{0}; ///< Cage edges kept sharp (creaseAngleDeg), of the final cage.
};

/// Closes an open cage (an oriented manifold sheet) into a wall: a copy of every vertex moved
/// thicknessMm against the limit surface's normal there (limitNormals()), its faces turned
/// over, and one quad across the wall on every boundary edge. buildSurface() makes the outer
/// side, the inner side and the rim as three surfaces, so the rim edges stay sharp. The sheet's
/// vertices come first, unchanged. Fails with InvalidArgument for a closed cage or a thickness that
/// is not positive.
[[nodiscard]] Result<QuadLayout> thickenCage(const QuadLayout& sheet, double thicknessMm);

/// Fits, extracts and sews in one go. The cage must be closed, or open with thicknessMm set;
/// `scan` may be null when fit is off.
/// Fails unless the result is one valid closed solid of positive volume whose faces meet at no
/// more than maxFoldAngleDeg. Deterministic for the same inputs.
[[nodiscard]] Result<SurfaceBuildResult> buildSurface(const QuadLayout& cage,
                                                      const TriMesh* scan,
                                                      const SurfaceBuildParams& params,
                                                      const Progress& progress = {});

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

struct PlaneFit {
    Vec3d point;     ///< Area-weighted centroid of the fitted faces.
    Vec3d normal;    ///< Unit; points the same way as the faces' average outward normal.
    double rmsMm{0}; ///< RMS distance of the faces' vertices from the plane.
};

/// Least-squares plane through the selected faces (area-weighted over the triangle surfaces).
[[nodiscard]] Result<PlaneFit> fitPlane(const TriMesh& mesh, const MeshSelection& faces);

enum class AlignOrigin {
    Centroid,
    BoundsCenter,
    BoundsBottomCenter
};

/// Automatic alignment by principal axes: the surface centroid moves to the origin, the axis of
/// largest spread to X and the smallest to Z (right-handed). Axis signs follow the skew of the
/// surface, so a part comes out the same way up whatever its scanned pose. `origin` then shifts
/// the result (for example so the part sits on Z = 0).
[[nodiscard]] Result<Transform3d>
principalAxesAlignment(const TriMesh& mesh, AlignOrigin origin = AlignOrigin::Centroid);

struct DatumAlignParams {
    MeshSelection primary;                  ///< Plane that becomes Z = 0.
    std::optional<MeshSelection> secondary; ///< Plane that becomes Y = 0.
    std::optional<MeshSelection> tertiary;  ///< Plane that becomes X = 0.
    /// By default each datum face's outward normal points to the negative axis, so the part sits
    /// on the positive side of every datum plane. Flip to put it on the other side of Z = 0.
    bool flipPrimary{false};
};

struct DatumAlignResult {
    Transform3d transform;
    PlaneFit primary;
    std::optional<PlaneFit> secondary;
    std::optional<PlaneFit> tertiary;
};

/// 3-2-1 datum alignment (ARCHITECTURE.md section 6). Without a secondary plane the X direction
/// follows the principal axes; without a tertiary plane the origin keeps the part's centroid in X.
[[nodiscard]] Result<DatumAlignResult> datumAlignment(const TriMesh& mesh,
                                                      const DatumAlignParams& params);

} // namespace m2c

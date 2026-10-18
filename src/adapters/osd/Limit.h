// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the OpenSubdiv adapter: the limit surface of one cage topology as OpenSubdiv patches.

#include <cstdint>
#include <memory>
#include <vector>

#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/topologyRefiner.h>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"

namespace m2c::osd {

/// Weight of one cage vertex in a limit point.
struct VertexWeight {
    std::uint32_t vertex{0};
    double weight{0.0};
};

/// Where a patch lies: its square [u0, u0 + size] x [v0, v0 + size] of a ptex face (a cage quad,
/// or one corner quad of a cage face with another number of sides).
struct PatchDomain {
    std::uint32_t ptexFace{0};
    double u0{0.0};
    double v0{0.0};
    double size{1.0};
};

/// Feature-adaptive patches of a cage's Catmull-Clark limit surface (Gregory basis caps at
/// irregular vertices), with every patch point expressed as weights of cage vertices. Depends on
/// the topology only: positions are passed to each query. Patch parameters (s, t) are local to a
/// patch, in [0, 1]^2.
class Limit {
public:
    /// The cage must be closed, or with `open` an oriented manifold sheet, whose limit surface
    /// ends at the boundary curve of its boundary vertices; that curve passes through vertices
    /// used by one face only, with a sharp turn there.
    [[nodiscard]] static Result<std::unique_ptr<Limit>>
    create(const QuadLayout& cage, int isolationLevel, bool open = false);

    [[nodiscard]] std::size_t patchCount() const noexcept { return handles_.size(); }
    [[nodiscard]] bool isRegular(std::size_t patch) const;
    [[nodiscard]] const PatchDomain& domain(std::size_t patch) const { return domains_[patch]; }
    [[nodiscard]] std::size_t ptexFaceCount() const noexcept { return cageFaceOfPtex_.size(); }
    /// The cage face a ptex face belongs to.
    [[nodiscard]] std::uint32_t cageFace(std::uint32_t ptexFace) const {
        return cageFaceOfPtex_[ptexFace];
    }

    /// Positions of all patch points (cage vertices first) for the given cage positions.
    [[nodiscard]] std::vector<Vec3d> patchPoints(const std::vector<Vec3d>& cagePositions) const;

    /// Limit point of a patch, from patchPoints().
    [[nodiscard]] Vec3d
    evaluate(const std::vector<Vec3d>& points, std::size_t patch, double s, double t) const;

    /// The same limit point as weights of cage vertices (sorted by vertex, no repeats).
    void weights(std::size_t patch, double s, double t, std::vector<VertexWeight>& out) const;

private:
    Limit() = default;
    void basis(std::size_t patch, double s, double t, std::vector<double>& w) const;

    std::unique_ptr<OpenSubdiv::Far::TopologyRefiner> refiner_;
    std::unique_ptr<const OpenSubdiv::Far::PatchTable> patches_;
    std::unique_ptr<const OpenSubdiv::Far::StencilTableReal<double>> stencils_;
    std::vector<OpenSubdiv::Far::PatchTable::PatchHandle> handles_;
    std::vector<PatchDomain> domains_;
    std::vector<std::uint32_t> cageFaceOfPtex_;
    std::size_t cageVertices_{0};
};

} // namespace m2c::osd

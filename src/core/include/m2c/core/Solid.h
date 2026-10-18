// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/core/SurfacePatch.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

/// The CAD kernel's own shape, defined by the Open CASCADE adapter (ADR-007): code above
/// src/adapters only passes it back to that adapter.
struct SolidShape;

struct SolidSummary {
    std::size_t faceCount{0};
    std::size_t edgeCount{0};
    std::size_t vertexCount{0};
    std::size_t shellCount{0};
    std::size_t solidCount{0};
    std::size_t freeEdges{0}; ///< Edges with one face: 0 for a closed solid.
    bool closed{false};       ///< Every shell is closed (no free edges).
    bool valid{false};        ///< The kernel's checker accepts the shape.
    double volumeMm3{0.0};
    double areaMm2{0.0};
    double maxToleranceMm{0.0}; ///< Largest edge or vertex tolerance after sewing.
    Aabb bounds;
};

/// A triangulation of a solid's faces that remembers where it lies on them, so a point found on
/// a triangle can be traced back to its face and surface parameters.
struct FaceTessellation {
    TriMesh mesh;
    std::vector<std::uint32_t> triangleFaces;     ///< The face each triangle lies on.
    std::vector<std::array<double, 2>> vertexUvs; ///< (u, v) of each vertex on its face.
};

/// A closed B-rep solid built from a quad cage (ARCHITECTURE.md section 6, Surface build): the
/// kernel shape, the B-spline faces it was made from (for exact distance queries) and a
/// triangulation for display. Immutable and cheap to copy.
class Solid {
public:
    Solid(std::shared_ptr<const SolidShape> shape,
          std::shared_ptr<const std::vector<SurfacePatch>> faces,
          std::shared_ptr<const FaceTessellation> display,
          SolidSummary summary)
        : shape_(std::move(shape)), faces_(std::move(faces)), display_(std::move(display)),
          summary_(summary) {}

    [[nodiscard]] const SolidShape& shape() const noexcept { return *shape_; }
    [[nodiscard]] const std::shared_ptr<const SolidShape>& shapePtr() const noexcept {
        return shape_;
    }
    [[nodiscard]] const std::vector<SurfacePatch>& faces() const noexcept { return *faces_; }
    [[nodiscard]] const TriMesh& display() const noexcept { return display_->mesh; }
    /// The display mesh with the face and (u, v) of each of its triangles and vertices.
    [[nodiscard]] const FaceTessellation& displayMap() const noexcept { return *display_; }
    [[nodiscard]] const SolidSummary& summary() const noexcept { return summary_; }

private:
    std::shared_ptr<const SolidShape> shape_;
    std::shared_ptr<const std::vector<SurfacePatch>> faces_;
    std::shared_ptr<const FaceTessellation> display_;
    SolidSummary summary_;
};

/// Triangulates faces on an n x n grid each, vertices shared along each face's own grid only.
/// Faces made by the surface build meet exactly, so the result looks closed but is not welded.
[[nodiscard]] Result<FaceTessellation> tessellate(const std::vector<SurfacePatch>& faces, int n);

/// How well faces meet along their edges and along the knot lines inside them.
struct SeamReport {
    double maxGapMm{0.0};            ///< Largest distance between matching edge samples.
    double maxNormalAngleDeg{0.0};   ///< Largest angle between the normals on the two sides.
    std::size_t unmatchedSamples{0}; ///< Edge samples with no other face within the tolerance.
};

/// Samples every face edge at matching parameters (samplesPerEdge per half cage edge) and pairs
/// samples of different faces that lie within toleranceMm of each other; knot lines inside a
/// face are compared piece to piece. A closed surface has no unmatched samples. Deterministic.
[[nodiscard]] Result<SeamReport>
measureSeams(const std::vector<SurfacePatch>& faces, int samplesPerEdge, double toleranceMm);

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

/// Polygon control cage, mostly quads, for Catmull-Clark surfacing (ARCHITECTURE.md sections 5
/// and 6). Faces are counter-clockwise seen from outside. Immutable and cheap to copy: the
/// buffers are shared, and fitting moves vertices with withPositions(), keeping the faces.
class QuadLayout {
public:
    /// Face f has the vertices faceVertices[faceStarts[f] .. faceStarts[f + 1]). Fails when there
    /// are no faces, faceStarts does not start at 0 and grow to faceVertices.size(), a face has
    /// fewer than three vertices or repeats one, an index is out of range or a coordinate is not
    /// finite.
    [[nodiscard]] static Result<QuadLayout> create(std::vector<Vec3d> positions,
                                                   std::vector<std::uint32_t> faceStarts,
                                                   std::vector<std::uint32_t> faceVertices);

    [[nodiscard]] std::size_t vertexCount() const noexcept { return positions_->size(); }
    [[nodiscard]] std::size_t faceCount() const noexcept { return topology_->starts.size() - 1; }

    [[nodiscard]] const std::vector<Vec3d>& positions() const noexcept { return *positions_; }
    [[nodiscard]] const std::vector<std::uint32_t>& faceStarts() const noexcept {
        return topology_->starts;
    }
    [[nodiscard]] const std::vector<std::uint32_t>& faceVertices() const noexcept {
        return topology_->vertices;
    }

    /// Vertex indices of face f. Precondition: f < faceCount().
    [[nodiscard]] std::span<const std::uint32_t> face(std::size_t f) const noexcept {
        const auto& t = *topology_;
        return {t.vertices.data() + t.starts[f], t.starts[f + 1] - t.starts[f]};
    }

    /// Vertices used by other than four faces: Catmull-Clark extraordinary vertices, where the
    /// limit surface is only tangent-continuous and the surface build places cap patches.
    [[nodiscard]] const std::vector<std::uint32_t>& extraordinaryVertices() const noexcept {
        return topology_->extraordinary;
    }

    [[nodiscard]] const Aabb& bounds() const noexcept { return bounds_; }

    /// The same faces with new vertex positions (one per vertex, finite).
    [[nodiscard]] Result<QuadLayout> withPositions(std::vector<Vec3d> positions) const;

private:
    struct Topology {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint32_t> vertices;
        std::vector<std::uint32_t> extraordinary;
    };

    QuadLayout(std::shared_ptr<const std::vector<Vec3d>> positions,
               std::shared_ptr<const Topology> topology,
               const Aabb& bounds);

    std::shared_ptr<const std::vector<Vec3d>> positions_;
    std::shared_ptr<const Topology> topology_;
    Aabb bounds_;
};

/// Topology figures of a cage. A cage the surface build accepts is closed() (every edge shared by
/// exactly two faces, each vertex a single fan, faces consistently oriented).
struct CageStats {
    std::size_t vertexCount{0};
    std::size_t faceCount{0};
    std::size_t edgeCount{0};
    std::size_t boundaryEdges{0};
    std::size_t nonManifoldEdges{0};
    std::size_t nonManifoldVertices{0};
    std::size_t misorientedEdges{0}; ///< Edges both of whose faces run the same way along it.
    std::size_t components{0};
    std::map<std::size_t, std::size_t> faceDegrees; ///< Vertices per face -> number of faces.
    std::map<std::size_t, std::size_t> valences;    ///< Faces per vertex -> number of vertices.
    std::size_t extraordinaryVertices{0};

    [[nodiscard]] long long eulerCharacteristic() const noexcept {
        return static_cast<long long>(vertexCount) - static_cast<long long>(edgeCount) +
               static_cast<long long>(faceCount);
    }
    /// Every edge has one or two faces, every vertex one fan, and faces agree in direction:
    /// a closed cage, or an open sheet whose boundary is the only place edges have one face.
    [[nodiscard]] bool orientedManifold() const noexcept {
        return nonManifoldEdges == 0 && nonManifoldVertices == 0 && misorientedEdges == 0;
    }
    [[nodiscard]] bool closed() const noexcept { return boundaryEdges == 0 && orientedManifold(); }
};

[[nodiscard]] CageStats cageStats(const QuadLayout& cage);

/// Volume enclosed by a closed cage (faces fanned into triangles); positive when faces point
/// outward.
[[nodiscard]] double signedVolume(const QuadLayout& cage);

/// Each edge of the faces once, as pairs of vertex indices (a closed cage has each edge in two
/// faces, once in each direction; the lower-to-higher one is kept).
[[nodiscard]] std::vector<std::uint32_t> cageEdges(const QuadLayout& cage);

/// Edges of a closed cage where its faces turn by more than angleDeg (the angle between the two
/// faces' area vectors): where a cage aligned to a part's sharp edges runs along them. Each as
/// (lower, higher) vertex, in order. Edges without two faces are left out.
[[nodiscard]] std::vector<std::array<std::uint32_t, 2>> sharpCageEdges(const QuadLayout& cage,
                                                                       double angleDeg);

} // namespace m2c

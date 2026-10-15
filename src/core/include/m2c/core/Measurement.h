// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "m2c/core/Vec3.h"

namespace m2c {

/// What a picked point snapped to.
enum class SnapKind {
    Free,
    Surface,
    Edge,
    Vertex
};

[[nodiscard]] constexpr std::string_view toString(SnapKind kind) noexcept {
    switch (kind) {
    case SnapKind::Free:
        return "free";
    case SnapKind::Surface:
        return "surface";
    case SnapKind::Edge:
        return "edge";
    case SnapKind::Vertex:
        return "vertex";
    }
    return "free";
}

struct PickedPoint {
    Vec3d position;
    std::uint64_t objectId{0}; ///< Mesh object the point lies on; 0 for a free point.
    SnapKind snap{SnapKind::Free};
    std::uint32_t element{0}; ///< Face, vertex or face of the edge, by `snap`.
};

/// Point-to-point distance, a document object so it is saved, undoable and listed.
struct Measurement {
    PickedPoint a;
    PickedPoint b;

    [[nodiscard]] Vec3d delta() const noexcept { return b.position - a.position; }
    [[nodiscard]] double distance() const noexcept { return length(delta()); }
};

} // namespace m2c

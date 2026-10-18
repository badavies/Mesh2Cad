// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstddef>
#include <vector>

#include <CGAL/alpha_wrap_3.h>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

#include "CgalMesh.h"

namespace m2c::cgal {

Result<TriMesh> shrinkWrap(const TriMesh& input, const WrapParams& params) {
    if (!(params.alphaMm > 0.0) || !(params.offsetMm > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "alpha and offset must be positive");
    }
    return guarded("shrink-wrap", [&]() -> Result<TriMesh> {
        std::vector<Point> points;
        points.reserve(input.vertexCount());
        for (const auto& [x, y, z] : input.positions()) points.emplace_back(x, y, z);
        std::vector<std::array<std::size_t, 3>> faces;
        faces.reserve(input.faceCount());
        for (std::size_t f = 0; f < input.faceCount(); ++f) {
            const auto [a, b, c] = input.face(f);
            faces.push_back({a, b, c});
        }
        // The triangle-soup overload accepts broken, non-manifold and self-intersecting input.
        SurfaceMesh wrap;
        CGAL::alpha_wrap_3(points, faces, params.alphaMm, params.offsetMm, wrap);
        return fromSurfaceMesh(wrap);
    });
}

} // namespace m2c::cgal

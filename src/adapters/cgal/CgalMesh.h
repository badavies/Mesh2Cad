// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the CGAL adapter: conversions between TriMesh and CGAL::Surface_mesh.

#include <exception>
#include <string>

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>

#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"

namespace m2c::cgal {

using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using Point = Kernel::Point_3;
using SurfaceMesh = CGAL::Surface_mesh<Point>;

[[nodiscard]] SurfaceMesh toSurfaceMesh(const TriMesh& mesh);

/// Converts back, skipping removed elements and faces that are not triangles.
[[nodiscard]] Result<TriMesh> fromSurfaceMesh(const SurfaceMesh& mesh);

/// Runs `body`, turning any exception into an Error (no exception escapes an adapter).
template <class F>
auto guarded(const char* what, F&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string(what) + " failed: " + e.what());
    }
}

} // namespace m2c::cgal

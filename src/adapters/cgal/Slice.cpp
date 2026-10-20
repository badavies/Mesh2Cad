// SPDX-License-Identifier: GPL-3.0-or-later
#include <iterator>
#include <vector>

#include <CGAL/Polygon_mesh_slicer.h>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/core/Error.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

#include "CgalMesh.h"

namespace m2c::cgal {

Result<std::vector<std::vector<Vec3d>>>
slice(const TriMesh& input, const Vec3d& point, const Vec3d& normal) {
    if (!(length(normal) > 0.0)) {
        return fail(ErrorCode::InvalidArgument, "the section plane's normal must not be zero");
    }
    return guarded("sectioning", [&]() -> Result<std::vector<std::vector<Vec3d>>> {
        const SurfaceMesh mesh = toSurfaceMesh(input);
        const CGAL::Polygon_mesh_slicer<SurfaceMesh, Kernel> slicer(mesh);
        std::vector<std::vector<Point>> cuts;
        slicer(Kernel::Plane_3(Point(point.x, point.y, point.z),
                               Kernel::Vector_3(normal.x, normal.y, normal.z)),
               std::back_inserter(cuts));
        std::vector<std::vector<Vec3d>> out;
        out.reserve(cuts.size());
        for (const auto& cut : cuts) {
            auto& polyline = out.emplace_back();
            polyline.reserve(cut.size());
            for (const Point& p : cut) polyline.push_back({p.x(), p.y(), p.z()});
        }
        return out;
    });
}

} // namespace m2c::cgal

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/commands/JsonConvert.h"

#include <string>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

Json toJson(const Vec3d& v) {
    return Json::array({v.x, v.y, v.z});
}

Json toJson(const Transform3d& t) {
    return Json{{"rotation", t.rotation}, {"translation", toJson(t.translation)}};
}

Json toJson(const PickedPoint& p) {
    return Json{{"point", toJson(p.position)},
                {"object_id", p.objectId},
                {"snap", std::string(toString(p.snap))},
                {"element", p.element}};
}

Json toJson(const Measurement& m) {
    return Json{{"a", toJson(m.a)},
                {"b", toJson(m.b)},
                {"distance_mm", m.distance()},
                {"delta_mm", toJson(m.delta())}};
}

Json toJson(const Error& error) {
    return Json{{"code", toString(error.code)}, {"message", error.message}};
}

Json toJson(const Aabb& box) {
    return Json{{"min", toJson(box.min)}, {"max", toJson(box.max)}};
}

Json toJson(const MeshStats& stats) {
    return Json{{"vertex_count", stats.vertexCount},
                {"face_count", stats.faceCount},
                {"edge_count", stats.edgeCount},
                {"boundary_edge_count", stats.boundaryEdgeCount},
                {"non_manifold_edge_count", stats.nonManifoldEdgeCount},
                {"degenerate_face_count", stats.degenerateFaceCount},
                {"surface_area_mm2", stats.surfaceArea},
                {"signed_volume_mm3", stats.signedVolume},
                {"is_watertight", stats.isWatertight()},
                {"bounds", toJson(stats.bounds)}};
}

} // namespace m2c

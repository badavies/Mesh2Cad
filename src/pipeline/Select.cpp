// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Select.h"

#include <cmath>
#include <numbers>
#include <vector>

#include "m2c/core/MeshTopology.h"
#include "m2c/core/SpatialIndex.h"

namespace m2c {

MeshSelection allFaces(const TriMesh& mesh) {
    MeshSelection s(mesh.faceCount());
    s.invert();
    return s;
}

MeshSelection
brushSelect(const TriMesh& mesh, const Vec3d& center, const double radiusMm, const bool connected) {
    MeshSelection selection(mesh.faceCount());
    const SpatialIndex& index = mesh.spatialIndex();
    const auto inside = index.facesWithin(center, radiusMm);
    if (inside.empty()) return selection;
    if (!connected) {
        for (const std::uint32_t f : inside) selection.set(f);
        return selection;
    }
    MeshSelection candidate(mesh.faceCount());
    for (const std::uint32_t f : inside) candidate.set(f);
    const auto& [offsets, neighbours] = mesh.faceAdjacency();
    std::vector stack{index.closestPoint(center).face};
    selection.set(stack.back());
    while (!stack.empty()) {
        const std::uint32_t f = stack.back();
        stack.pop_back();
        for (std::uint32_t i = offsets[f]; i < offsets[f + 1]; ++i) {
            if (const std::uint32_t g = neighbours[i]; candidate.test(g) && !selection.test(g)) {
                selection.set(g);
                stack.push_back(g);
            }
        }
    }
    return selection;
}

Result<MeshSelection>
creaseFloodSelect(const TriMesh& mesh, const std::uint32_t seed, const double maxAngleDeg) {
    if (seed >= mesh.faceCount()) {
        return fail(ErrorCode::InvalidArgument,
                    "seed face " + std::to_string(seed) + " does not exist");
    }
    const double cosLimit = std::cos(maxAngleDeg * std::numbers::pi / 180.0);
    const auto& [offsets, neighbours] = mesh.faceAdjacency();
    MeshSelection selection(mesh.faceCount());
    selection.set(seed);
    std::vector stack{seed};
    while (!stack.empty()) {
        const std::uint32_t f = stack.back();
        stack.pop_back();
        const Vec3d nf = faceNormal(mesh, f);
        for (std::uint32_t i = offsets[f]; i < offsets[f + 1]; ++i) {
            const std::uint32_t g = neighbours[i];
            if (selection.test(g)) continue;
            if (dot(nf, faceNormal(mesh, g)) >= cosLimit) {
                selection.set(g);
                stack.push_back(g);
            }
        }
    }
    return selection;
}

Result<MeshSelection> lassoSelect(const TriMesh& mesh, const LassoParams& params) {
    if (params.polygon.size() < 3) {
        return fail(ErrorCode::InvalidArgument, "lasso polygon needs at least three points");
    }
    if (params.visibleOnly && !params.eye) {
        return fail(ErrorCode::InvalidArgument, "visible-only lasso needs the camera position");
    }
    const auto& m = params.viewProjection;
    const auto& poly = params.polygon;
    const auto insidePolygon = [&](const double x, const double y) {
        bool in = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            if (const double xi = poly[i][0], yi = poly[i][1], xj = poly[j][0], yj = poly[j][1];
                (yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                in = !in;
        }
        return in;
    };

    MeshSelection selection(mesh.faceCount());
    const auto& p = mesh.positions();
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto [a, b, c] = mesh.face(f);
        const Vec3d centroid = (p[a] + p[b] + p[c]) * (1.0 / 3.0);
        // Column-major: clip = M * (x, y, z, 1).
        const double cx = m[0] * centroid.x + m[4] * centroid.y + m[8] * centroid.z + m[12];
        const double cy = m[1] * centroid.x + m[5] * centroid.y + m[9] * centroid.z + m[13];
        const double cw = m[3] * centroid.x + m[7] * centroid.y + m[11] * centroid.z + m[15];
        if (!(cw > 0.0)) continue; // behind the camera
        if (!insidePolygon(cx / cw, cy / cw)) continue;
        if (params.eye) {
            const Vec3d toFace = centroid - *params.eye;
            if (dot(faceNormal(mesh, f), toFace) >= 0.0) continue; // facing away
            if (params.visibleOnly) {
                const double dist = length(toFace);
                const auto hit =
                    mesh.spatialIndex().raycast(*params.eye, toFace, dist * (1.0 + 1e-9));
                if (hit && hit->face != f && hit->distance < dist * (1.0 - 1e-6)) continue;
            }
        }
        selection.set(f);
    }
    return selection;
}

MeshSelection growSelection(const TriMesh& mesh, const MeshSelection& selection, const int rings) {
    MeshSelection current = selection;
    if (rings == 0 || selection.size() != mesh.faceCount()) return current;
    const auto& [offsets, neighbours] = mesh.faceAdjacency();
    const int steps = rings > 0 ? rings : -rings;
    for (int r = 0; r < steps; ++r) {
        MeshSelection next = current;
        for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
            if (rings > 0) {
                if (current.test(f)) continue;
                for (std::uint32_t i = offsets[f]; i < offsets[f + 1]; ++i) {
                    if (current.test(neighbours[i])) {
                        next.set(f);
                        break;
                    }
                }
            } else {
                if (!current.test(f)) continue;
                // A face on an open boundary (fewer than three neighbours) or next to an
                // unselected face is on the selection border.
                const std::uint32_t degree = offsets[f + 1] - offsets[f];
                bool border = degree < 3;
                for (std::uint32_t i = offsets[f]; i < offsets[f + 1] && !border; ++i) {
                    border = !current.test(neighbours[i]);
                }
                if (border) next.set(f, false);
            }
        }
        current = std::move(next);
    }
    return current;
}

} // namespace m2c

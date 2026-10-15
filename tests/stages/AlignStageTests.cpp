// SPDX-License-Identifier: GPL-3.0-or-later
// Alignment stage tests: a part moved by a known transform must return to the origin planes
// (ARCHITECTURE.md section 10).

#include <cmath>
#include <numbers>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/MeshStats.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/pipeline/Align.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

namespace {

/// Rotation by `deg` degrees about a unit axis (Rodrigues), plus a shift.
Transform3d rotation(Vec3d axis, double deg, Vec3d shift) {
    axis = axis * (1.0 / length(axis));
    const double a = deg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    const double x = axis.x, y = axis.y, z = axis.z;
    return {{t * x * x + c,
             t * x * y - s * z,
             t * x * z + s * y,
             t * x * y + s * z,
             t * y * y + c,
             t * y * z - s * x,
             t * x * z - s * y,
             t * y * z + s * x,
             t * z * z + c},
            shift};
}

/// Faces of `mesh` whose normal is within 1 degree of `n`.
MeshSelection facesFacing(const TriMesh& mesh, Vec3d n) {
    MeshSelection s(mesh.faceCount());
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        if (dot(faceNormal(mesh, f), n) > 0.9998) s.set(f);
    }
    return s;
}

} // namespace

TEST_CASE("Plane fit recovers a face of a box", "[stages][align]") {
    const TriMesh box = makeBox(4, 6, 8, 3);
    const auto plane = fitPlane(box, facesFacing(box, {0, 0, 1}));
    REQUIRE(plane.has_value());
    CHECK_THAT(plane->normal.z, WithinAbs(1.0, 1e-12));
    CHECK_THAT(plane->point.z, WithinAbs(8.0, 1e-12));
    CHECK_THAT(plane->point.x, WithinAbs(2.0, 1e-12));
    CHECK_THAT(plane->rmsMm, WithinAbs(0.0, 1e-12));
    CHECK_FALSE(fitPlane(box, MeshSelection(box.faceCount())).has_value());
}

TEST_CASE("Datum alignment puts a moved box square on the origin planes", "[stages][align]") {
    const TriMesh box = makeBox(40, 30, 20, 4);
    const Transform3d pose = rotation({1, 2, 3}, 37.0, {100, -50, 25});
    const TriMesh scan = *transformMesh(box, pose);

    DatumAlignParams p;
    p.primary = facesFacing(scan, pose.rotate({0, 0, -1}));   // bottom
    p.secondary = facesFacing(scan, pose.rotate({0, -1, 0})); // front
    p.tertiary = facesFacing(scan, pose.rotate({-1, 0, 0}));  // left
    const auto aligned = datumAlignment(scan, p);
    REQUIRE(aligned.has_value());
    const TriMesh result = *transformMesh(scan, aligned->transform);
    const Aabb b = computeStats(result).bounds;
    CHECK_THAT(b.min.x, WithinAbs(0.0, 1e-9));
    CHECK_THAT(b.min.y, WithinAbs(0.0, 1e-9));
    CHECK_THAT(b.min.z, WithinAbs(0.0, 1e-9));
    CHECK_THAT(b.max.x, WithinAbs(40.0, 1e-9));
    CHECK_THAT(b.max.y, WithinAbs(30.0, 1e-9));
    CHECK_THAT(b.max.z, WithinAbs(20.0, 1e-9));
}

TEST_CASE("Datum alignment needs non-parallel planes", "[stages][align]") {
    const TriMesh box = makeBox(4, 4, 4, 2);
    DatumAlignParams p;
    p.primary = facesFacing(box, {0, 0, -1});
    p.secondary = facesFacing(box, {0, 0, 1});
    CHECK_FALSE(datumAlignment(box, p).has_value());
}

TEST_CASE("Principal-axes alignment is independent of the scanned pose", "[stages][align]") {
    // A box with an off-centre bump so the skew fixes the axis signs.
    const TriMesh part = combine({makeBox(40, 20, 10, 4), makeSphere(3.0, 2, {35, 15, 10})});
    const auto reference = principalAxesAlignment(part, AlignOrigin::BoundsBottomCenter);
    REQUIRE(reference.has_value());
    const TriMesh a = *transformMesh(part, *reference);

    const TriMesh moved = *transformMesh(part, rotation({-2, 1, 0.5}, 123.0, {7, 8, 9}));
    const auto t = principalAxesAlignment(moved, AlignOrigin::BoundsBottomCenter);
    REQUIRE(t.has_value());
    const TriMesh b = *transformMesh(moved, *t);
    for (std::size_t v = 0; v < a.vertexCount(); ++v) {
        CHECK(length(a.positions()[v] - b.positions()[v]) < 1e-6);
    }
    // Longest extent along X, shortest along Z, sitting on Z = 0.
    const Aabb box = computeStats(a).bounds;
    CHECK(box.max.x - box.min.x > box.max.y - box.min.y);
    CHECK(box.max.y - box.min.y > box.max.z - box.min.z);
    CHECK_THAT(box.min.z, WithinAbs(0.0, 1e-9));
}

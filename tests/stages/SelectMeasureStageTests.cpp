// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/MeshTopology.h"
#include "m2c/pipeline/Measure.h"
#include "m2c/pipeline/Select.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Crease flood selects one flat side of a box", "[stages][select]") {
    const TriMesh box = makeBox(10, 10, 10, 4);        // 32 faces per side
    const auto top = creaseFloodSelect(box, 32, 20.0); // face 32 is on the +z side
    REQUIRE(top.has_value());
    CHECK(top->count() == 32);
    for (const std::uint32_t f : top->indices()) CHECK(faceNormal(box, f).z > 0.99);
    CHECK(creaseFloodSelect(box, 0, 95.0)->count() == box.faceCount());
    CHECK_FALSE(creaseFloodSelect(box, 9999, 20.0).has_value());
}

TEST_CASE("Brush selects a connected patch", "[stages][select]") {
    // Two parallel plates 0.5 mm apart: a 2 mm brush on one must not jump to the other.
    const TriMesh plates = combine({makeBox(10, 10, 0.1, 10), [] {
                                        TriMesh b = makeBox(10, 10, 0.1, 10);
                                        std::vector<Vec3d> p = b.positions();
                                        for (Vec3d& v : p) v.z += 0.6;
                                        return makeMesh(std::move(p), b.indices());
                                    }()});
    const MeshSelection connected = brushSelect(plates, {5, 5, 0.1}, 2.0, true);
    const MeshSelection loose = brushSelect(plates, {5, 5, 0.1}, 2.0, false);
    CHECK(connected.count() > 0);
    CHECK(loose.count() > connected.count());
    for (const std::uint32_t f : connected.indices()) CHECK(f < plates.faceCount() / 2);
}

TEST_CASE("Grow, shrink and select all", "[stages][select]") {
    const TriMesh box = makeBox(10, 10, 10, 4);
    MeshSelection one(box.faceCount());
    one.set(40);
    const MeshSelection grown = growSelection(box, one, 1);
    CHECK(grown.count() == 4);                   // the face and its three edge neighbours
    CHECK(growSelection(box, grown, -1) == one); // shrinking undoes the grow
    CHECK(allFaces(box).count() == box.faceCount());
}

TEST_CASE("Lasso selects faces inside a screen polygon", "[stages][select]") {
    const TriMesh box = makeBox(2, 2, 2, 4);
    LassoParams p;
    // Orthographic top view: x, y in [0, 2] map to NDC [-1, 1]; z ignored.
    p.viewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1};
    p.polygon = {{-1.1, -1.1}, {0.0, -1.1}, {0.0, 1.1}, {-1.1, 1.1}}; // left half
    p.eye = Vec3d{1, 1, 100};
    const auto front = lassoSelect(box, p);
    REQUIRE(front.has_value());
    // Top faces in the left half face the camera; the bottom and sides do not.
    CHECK(front->count() == 16);
    p.visibleOnly = true;
    CHECK(lassoSelect(box, p)->count() == 16);
    p.polygon.resize(2);
    CHECK_FALSE(lassoSelect(box, p).has_value());
}

TEST_CASE("Measurement snaps to vertices, edges and surfaces", "[stages][measure]") {
    const TriMesh box = makeBox(10, 10, 10, 1);
    const PickedPoint corner = snapToMesh(box, 1, {10.2, 10.1, 9.9}, 0.5);
    CHECK(corner.snap == SnapKind::Vertex);
    CHECK(corner.position == Vec3d{10, 10, 10});
    const PickedPoint edge = snapToMesh(box, 1, {5, 10.1, 9.8}, 0.5);
    CHECK(edge.snap == SnapKind::Edge);
    CHECK_THAT(edge.position.x, WithinAbs(5.0, 1e-12));
    CHECK_THAT(edge.position.z, WithinAbs(10.0, 1e-12));
    const PickedPoint surface = snapToMesh(box, 1, {4, 6, 10.3}, 0.5);
    CHECK(surface.snap == SnapKind::Surface);
    CHECK(surface.position == Vec3d{4, 6, 10});
    const Measurement m{corner, surface};
    CHECK_THAT(m.distance(), WithinAbs(length(Vec3d{6, 4, 0}), 1e-12));

    const auto hit = pickMesh(box, {3, 3, 50}, {0, 0, -1});
    REQUIRE(hit.has_value());
    CHECK_THAT(hit->point.z, WithinAbs(10.0, 1e-12));
}

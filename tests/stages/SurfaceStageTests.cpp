// SPDX-License-Identifier: GPL-3.0-or-later
// Surface build stage tests (issue #17): cage fitting and B-spline face extraction, checked by
// distance to known shapes, gaps between faces and the angle across cap edges.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/adapters/cgal/MeshTools.h"
#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshStats.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/pipeline/Deviation.h"
#include "m2c/pipeline/PrimitiveFit.h"
#include "m2c/pipeline/QuadRemesh.h"
#include "m2c/pipeline/Surface.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {

/// Torus cage of n x m quads around the z axis: every vertex has valence four.
QuadLayout torusCage(double R, double r, int n, int m) {
    std::vector<Vec3d> p;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            const double u = 2.0 * std::numbers::pi * i / n;
            const double v = 2.0 * std::numbers::pi * j / m;
            p.push_back({(R + r * std::cos(v)) * std::cos(u),
                         (R + r * std::cos(v)) * std::sin(u),
                         r * std::sin(v)});
        }
    }
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> verts;
    const auto at = [&](int i, int j) { return static_cast<std::uint32_t>((i % n) * m + j % m); };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            verts.insert(verts.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
            starts.push_back(static_cast<std::uint32_t>(verts.size()));
        }
    }
    return *QuadLayout::create(std::move(p), std::move(starts), std::move(verts));
}

QuadLayout cubeCage(double size) {
    std::vector<Vec3d> p{
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (Vec3d& v : p) v = v * size;
    std::vector<std::uint32_t> v{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                                 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
    return *QuadLayout::create(std::move(p), {0, 4, 8, 12, 16, 20, 24}, std::move(v));
}

/// A rounded cube (half-size 10, nearly flat across the middle of each face) with a dent 0.4 deep
/// in the middle of its top and a little noise everywhere: a flat face scanned with a defect.
TriMesh dentedBox() {
    TriMesh box = makeRoundedCube(10.0, 8.0);
    std::vector<Vec3d> p = box.positions();
    for (Vec3d& v : p) {
        if (v.z > 9.0) v.z -= 0.4 * std::exp(-(v.x * v.x + v.y * v.y) / (2.0 * 1.5 * 1.5));
    }
    return addNoise(makeMesh(std::move(p), box.indices()), 0.03);
}

/// The top's faces with all three corners in |x|, |y| < 6.
MeshSelection topFaces(const TriMesh& scan) {
    MeshSelection top(scan.faceCount());
    const auto inside = [](const Vec3d& v) {
        return v.z > 9.0 && std::abs(v.x) < 6.0 && std::abs(v.y) < 6.0;
    };
    for (std::uint32_t f = 0; f < scan.faceCount(); ++f) {
        const auto [a, b, c] = scan.face(f);
        if (inside(scan.positions()[a]) && inside(scan.positions()[b]) &&
            inside(scan.positions()[c])) {
            top.set(f);
        }
    }
    return top;
}

/// Largest distance from the plane of the limit surface's points over |x|, |y| < 4 on the top.
double topDeviation(const QuadLayout& cage, const Primitive& plane) {
    const auto limit = tessellateLimit(cage, 2, 4);
    REQUIRE(limit);
    double worst = 0.0;
    for (const Vec3d& q : limit->positions()) {
        if (q.z > 9.0 && std::abs(q.x) < 4.0 && std::abs(q.y) < 4.0) {
            worst = std::max(worst, std::abs(dot(q - plane.point, plane.direction)));
        }
    }
    return worst;
}

SeamReport seams(const std::vector<SurfacePatch>& faces, int k, double tol) {
    auto report = measureSeams(faces, k, tol);
    REQUIRE(report);
    return *report;
}

} // namespace

TEST_CASE("A regular cage gives exact faces with no caps", "[stages][surface]") {
    const QuadLayout cage = torusCage(30.0, 10.0, 24, 12);
    const auto result = extractPatches(cage, PatchParams{});
    REQUIRE(result);
    CHECK(result->faces.size() == cage.faceCount());
    CHECK(result->capPieces == 0);
    CHECK(result->regularPieces == cage.faceCount());
    for (const SurfacePatch& f : result->faces) CHECK((f.piecesU() == 1 && f.piecesV() == 1));
    // The faces are the limit surface itself.
    const auto limit = tessellateLimit(cage, 2, 4);
    REQUIRE(limit);
    double worst = 0.0;
    for (const SurfacePatch& f : result->faces) {
        for (double u : {0.0, 0.3, 0.5, 1.0}) {
            for (double v : {0.0, 0.7, 1.0}) {
                worst =
                    std::max(worst, limit->spatialIndex().closestPoint(evaluate(f, u, v)).distance);
            }
        }
    }
    CHECK(worst < 0.05); // the tessellation is only piecewise flat
    const SeamReport s = seams(result->faces, 8, 1e-6);
    CHECK(s.unmatchedSamples == 0);
    CHECK(s.maxGapMm < 1e-6);
    CHECK(s.maxNormalAngleDeg < 1e-3); // C2 everywhere
    const auto split = measureSeams(result->faces, 8, 1e-6, result->caps);
    REQUIRE(split);
    CHECK(split->maxCapNormalAngleDeg == 0.0);
    CHECK(split->maxRegularNormalAngleDeg == s.maxNormalAngleDeg);
    // Cap flags that do not fit the faces are refused.
    auto wrong = result->caps;
    wrong.pop_back();
    CHECK_FALSE(measureSeams(result->faces, 8, 1e-6, wrong));
}

TEST_CASE("Irregular corners become caps that meet their neighbours", "[stages][surface]") {
    const QuadLayout cube = cubeCage(30.0);
    for (int level : {1, 2, 3}) {
        CAPTURE(level);
        const auto result = extractPatches(cube, PatchParams{level});
        REQUIRE(result);
        CHECK(result->faces.size() == 6);
        CHECK(result->capPieces > 0);
        for (const SurfacePatch& f : result->faces) {
            CHECK(f.piecesU() == std::size_t{1} << level);
            CHECK(f.piecesV() == f.piecesU());
            CHECK(f.poles.size() == f.polesPerRow() * f.poleRows());
        }
        const SeamReport s = seams(result->faces, 16, 1e-5);
        CHECK(s.unmatchedSamples == 0);
        CHECK(s.maxGapMm < 1e-6);
        CHECK(s.maxNormalAngleDeg < 0.5); // caps fit the tangent planes along their edges
        CHECK(result->maxCapErrorMm < 0.05);
        // Only the caps bend the normal: between regular pieces the surface is C2.
        REQUIRE(result->caps.size() == result->faces.size());
        std::size_t flagged = 0;
        for (const auto& c : result->caps)
            flagged += std::size_t(std::count(c.begin(), c.end(), true));
        CHECK(flagged == result->capPieces); // caps are of the finest size, one piece each
        const auto split = measureSeams(result->faces, 16, 1e-5, result->caps);
        REQUIRE(split);
        CHECK(split->maxNormalAngleDeg == s.maxNormalAngleDeg);
        CHECK(split->maxCapNormalAngleDeg == s.maxNormalAngleDeg);
        CHECK(split->maxCapNormalAngleDeg > 1e-3);
        CHECK(split->maxRegularNormalAngleDeg < 1e-3);
    }
}

TEST_CASE("Fitting pulls the limit surface of a sphere cage onto the sphere", "[stages][surface]") {
    const double r = 20.0;
    const TriMesh sphere = makeSphere(r, 5);
    QuadRemeshParams q;
    q.targetFaces = 400;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);

    const auto radialError = [r](const TriMesh& mesh) {
        double worst = 0.0;
        for (const Vec3d& p : mesh.positions()) worst = std::max(worst, std::abs(length(p) - r));
        return worst;
    };
    const auto before = tessellateLimit(remeshed->cage, 2, 3);
    REQUIRE(before);

    const auto fitted = fitCage(remeshed->cage, sphere, CageFitParams{});
    REQUIRE(fitted);
    CHECK(fitted->samples == sphere.vertexCount());
    CHECK(fitted->rmsDistanceMm < 0.01);
    CHECK(fitted->maxDistanceMm < 0.05);
    const auto after = tessellateLimit(fitted->cage, 2, 3);
    REQUIRE(after);
    CHECK(radialError(*after) < 0.05);
    CHECK(radialError(*after) < radialError(*before) / 4);

    const auto faces = extractPatches(fitted->cage, PatchParams{});
    REQUIRE(faces);
    CHECK(faces->maxCapErrorMm < 0.01);
    const SeamReport s = seams(faces->faces, 8, 1e-5);
    CHECK(s.unmatchedSamples == 0);
    CHECK(s.maxGapMm < 1e-6);
    CHECK(s.maxNormalAngleDeg < 0.1);
}

TEST_CASE("Fitting rejects open cages and bad parameters", "[stages][surface]") {
    const QuadLayout cube = cubeCage(1.0);
    const TriMesh scan = makeSphere();
    CageFitParams bad;
    bad.isolationLevel = 0;
    CHECK_FALSE(fitCage(cube, scan, bad));
    bad = {};
    bad.smoothing = -1.0;
    CHECK_FALSE(fitCage(cube, scan, bad));
    const QuadLayout open = *QuadLayout::create(cube.positions(), {0, 4}, {0, 3, 2, 1});
    CHECK_FALSE(extractPatches(open, PatchParams{}));
}

TEST_CASE("An open cage fits a scanned sheet and gives open patches", "[stages][surface]") {
    const TriMesh cap = makeCap(20.0, std::numbers::pi / 3.0, 24);
    QuadRemeshParams q;
    q.targetFaces = 300;
    CageCloseParams close;
    close.keepOutline = true;
    const auto sheet = quadRemesh(cap, q, close);
    REQUIRE(sheet);
    CageFitParams p;
    CHECK(fitCage(sheet->cage, cap, p).error().code == ErrorCode::InvalidArgument);
    p.open = true;
    const auto fit = fitCage(sheet->cage, cap, p);
    REQUIRE(fit);
    CHECK(fit->p95DistanceMm < 0.05);
    CHECK(fit->ignoredSamples == 0);
    const auto patches = extractPatches(fit->cage, PatchParams{2, true});
    REQUIRE(patches);
    const auto seams = measureSeams(patches->faces, 8, 1e-2, patches->caps);
    REQUIRE(seams);
    CHECK(seams->maxNormalAngleDeg < 10.0);
    CHECK(seams->unmatchedSamples > 0); // the outline
    // The edge of the surface is the cap's rim, z = 10.
    double lowest = 1e300;
    for (const SurfacePatch& face : patches->faces) {
        for (int i = 0; i <= 8; ++i) lowest = std::min(lowest, evaluate(face, i / 8.0, 0.0).z);
    }
    CHECK(std::abs(lowest - 10.0) < 0.1);
    const auto finer = subdivideCage(fit->cage);
    REQUIRE(finer);
    CHECK(cageStats(*finer).boundaryEdges == 2 * cageStats(fit->cage).boundaryEdges);
}

TEST_CASE("A sphere scan becomes a valid closed solid of the right volume", "[stages][surface]") {
    const double r = 20.0;
    const TriMesh sphere = makeSphere(r, 5);
    QuadRemeshParams q;
    q.targetFaces = 400;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    const auto built = buildSurface(remeshed->cage, &sphere, SurfaceBuildParams{});
    REQUIRE(built);
    const SolidSummary& s = built->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.shellCount == 1);
    CHECK(s.solidCount == 1);
    CHECK(s.freeEdges == 0);
    CHECK(std::abs(s.bounds.max.x - r) < 0.01);
    CHECK(std::abs(s.bounds.min.z + r) < 0.01);
    CHECK(built->seams.unmatchedSamples == 0);
    CHECK(s.faceCount == built->solid.faces().size());
    const double volume = 4.0 / 3.0 * std::numbers::pi * r * r * r;
    const double area = 4.0 * std::numbers::pi * r * r;
    CHECK(std::abs(s.volumeMm3 - volume) < 0.002 * volume);
    CHECK(std::abs(s.areaMm2 - area) < 0.002 * area);
    CHECK(s.maxToleranceMm < 1e-3);
    CHECK(built->fit.samples > 0);
    CHECK(built->fit.ignoredSamples == 0);
    // The display mesh follows the faces.
    std::size_t quads = 0;
    for (const SurfacePatch& f : built->solid.faces()) quads += std::size_t{f.cellsU} * f.cellsV;
    CHECK(quads == built->facesBeforeMerge);
    CHECK(built->solid.display().faceCount() == quads * 2 * 16);
    for (const Vec3d& p : built->solid.display().positions()) CHECK(std::abs(length(p) - r) < 0.05);
    // Every display vertex is where its (u, v) puts it on the face of each triangle using it.
    const FaceTessellation& map = built->solid.displayMap();
    REQUIRE(map.triangleFaces.size() == map.mesh.faceCount());
    REQUIRE(map.vertexUvs.size() == map.mesh.vertexCount());
    double worst = 0.0;
    for (std::size_t t = 0; t < map.mesh.faceCount(); ++t) {
        const SurfacePatch& face = built->solid.faces()[map.triangleFaces[t]];
        for (const std::uint32_t v : map.mesh.face(t)) {
            const auto [u, w] = map.vertexUvs[v];
            worst = std::max(worst, length(evaluate(face, u, w) - map.mesh.positions()[v]));
        }
    }
    CHECK(worst < 1e-12);
}

TEST_CASE("A fit that folds the surface is tried again with more smoothing", "[stages][surface]") {
    const TriMesh sphere = makeSphere(20.0, 4);
    QuadRemeshParams q;
    q.targetFaces = 200;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    SurfaceBuildParams p;
    p.fitting.iterations = 1;
    // A clean fit passes the default limit at once.
    const auto once = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(once);
    CHECK(once->fits == 1);
    CHECK(once->smoothing == p.fitting.smoothing);
    CHECK(once->seams.maxNormalAngleDeg <= p.maxSeamAngleDeg);
    // A limit no fit can meet (caps always leave a small angle): every try is made, each with
    // twice the smoothing of the one before, and the last is kept.
    p.maxSeamAngleDeg = 1e-9;
    p.fitTries = 3;
    const auto retried = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(retried);
    CHECK(retried->fits == 3);
    CHECK(retried->smoothing == 4.0 * p.fitting.smoothing);
    CHECK(retried->solid.summary().valid);
    p.fitTries = 0;
    CHECK_FALSE(buildSurface(remeshed->cage, &sphere, p));
}

TEST_CASE("A surface whose faces still meet at a sharp angle gives no solid", "[stages][surface]") {
    const TriMesh sphere = makeSphere(20.0, 4);
    QuadRemeshParams q;
    q.targetFaces = 200;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    SurfaceBuildParams p;
    p.fitting.iterations = 1;
    REQUIRE(buildSurface(remeshed->cage, &sphere, p));
    // Caps always leave a small angle, so with a hard limit below it every try folds and the
    // build fails instead of keeping the last one.
    p.maxSeamAngleDeg = 1e-9;
    p.maxFoldAngleDeg = 1e-9;
    p.fitTries = 2;
    const auto folded = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE_FALSE(folded);
    CHECK(folded.error().code == ErrorCode::InvalidState);
    CHECK(folded.error().message.find("after 2 fits") != std::string::npos);
    p.maxFoldAngleDeg = 0.0;
    CHECK(buildSurface(remeshed->cage, &sphere, p).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Subdividing a cage keeps its limit surface", "[stages][surface]") {
    const QuadLayout cage = cubeCage(10.0);
    const auto finer = subdivideCage(cage);
    REQUIRE(finer);
    CHECK(finer->faceCount() == 24);
    CHECK(finer->vertexCount() == 26);
    CHECK(cageStats(*finer).closed());
    SurfaceBuildParams p;
    p.fit = false;
    p.creaseAngleDeg = 0.0; // the cube cage's smooth limit surface, not a sharp-edged box
    const auto coarse = buildSurface(cage, nullptr, p);
    const auto fine = buildSurface(*finer, nullptr, p);
    REQUIRE(coarse);
    REQUIRE(fine);
    const double volume = coarse->solid.summary().volumeMm3;
    CHECK(std::abs(fine->solid.summary().volumeMm3 - volume) < 1e-3 * volume);
    CHECK(fine->seams.maxGapMm < 1e-6);
}

TEST_CASE("A fit farther from the scan than the target is refined and fitted again",
          "[stages][surface]") {
    const TriMesh sphere = makeSphere(20.0, 4);
    QuadRemeshParams q;
    q.targetFaces = 200;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    SurfaceBuildParams p;
    p.fitting.iterations = 1;
    const auto plain = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(plain);
    CHECK(plain->p95BeforeRefinementMm.empty());
    CHECK(plain->fit.p95DistanceMm > 0.0);
    CHECK(plain->fit.p95DistanceMm <= plain->fit.maxDistanceMm);
    // A target no fit meets: refinement stops after maxRefinements steps.
    p.refineToMm = 1e-9;
    p.maxRefinements = 1;
    const auto refined = buildSurface(remeshed->cage, &sphere, p);
    REQUIRE(refined);
    REQUIRE(refined->p95BeforeRefinementMm.size() == 1);
    CHECK(refined->p95BeforeRefinementMm[0] == plain->fit.p95DistanceMm);
    CHECK(refined->cage.faceCount() == remeshed->cage.faceVertices().size());
    CHECK(refined->fit.p95DistanceMm < plain->fit.p95DistanceMm);
    CHECK(refined->solid.summary().valid);
    // A target the first fit meets makes no refinement.
    p.refineToMm = 1.0;
    CHECK(buildSurface(remeshed->cage, &sphere, p)->p95BeforeRefinementMm.empty());
    p.refineToMm = -1.0;
    CHECK(buildSurface(remeshed->cage, &sphere, p).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Limit normals face the way the cage does", "[stages][surface]") {
    const auto cube = limitNormals(cubeCage(10.0));
    REQUIRE(cube);
    for (std::size_t v = 0; v < 8; ++v) {
        const Vec3d corner = cubeCage(1.0).positions()[v] - Vec3d{0.5, 0.5, 0.5};
        CHECK(length((*cube)[v] - corner * (1.0 / length(corner))) < 1e-9);
    }
    // An open sheet, corners and boundary included.
    const QuadLayout square =
        *QuadLayout::create({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 4}, {0, 1, 2, 3});
    const auto flat = limitNormals(square);
    REQUIRE(flat);
    for (const Vec3d& n : *flat) CHECK(length(n - Vec3d{0, 0, 1}) < 1e-12);
}

TEST_CASE("Thickening a sheet closes it into a wall", "[stages][surface]") {
    // A flat 2 x 2 grid of quads facing +z.
    std::vector<Vec3d> p;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) p.push_back({10.0 * i, 10.0 * j, 0.0});
    }
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> v;
    for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 2; ++i) {
            const std::uint32_t a = 3 * j + i;
            v.insert(v.end(), {a, a + 1, a + 4, a + 3});
            starts.push_back(static_cast<std::uint32_t>(v.size()));
        }
    }
    const QuadLayout sheet = *QuadLayout::create(p, std::move(starts), std::move(v));
    const auto wall = thickenCage(sheet, 2.0);
    REQUIRE(wall);
    CHECK(wall->vertexCount() == 18);
    CHECK(wall->faceCount() == 4 + 4 + 8);
    CHECK(cageStats(*wall).closed());
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(wall->positions()[i] == p[i]);
        CHECK(wall->positions()[i + 9] == p[i] - Vec3d{0, 0, 2});
    }
    SurfaceBuildParams b;
    b.fit = false;
    const auto solid = buildSurface(*wall, nullptr, b);
    REQUIRE(solid);
    CHECK(solid->solid.summary().valid);
    CHECK(solid->solid.summary().volumeMm3 > 0.0);

    // The sheet thickened by the build: three surfaces whose edges meet, even at the corners
    // (vertices of one face), around a 20 x 20 x 2 slab.
    SurfaceBuildParams w;
    w.fit = false;
    w.thicknessMm = 2.0;
    const auto slab = buildSurface(sheet, nullptr, w);
    REQUIRE(slab);
    CHECK(slab->solid.summary().valid);
    CHECK(std::abs(slab->solid.summary().volumeMm3 - 800.0) < 1e-6);

    CHECK(thickenCage(sheet, 0.0).error().code == ErrorCode::InvalidArgument);
    CHECK(thickenCage(cubeCage(1.0), 1.0).error().code == ErrorCode::InvalidArgument);
    // A sheet needs a thickness, and only a sheet takes one.
    b.thicknessMm = 0.0;
    CHECK(buildSurface(sheet, nullptr, b).error().code == ErrorCode::InvalidArgument);
    b.thicknessMm = 1.0;
    CHECK(buildSurface(cubeCage(10.0), nullptr, b).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("A scanned cap becomes a wall of the given thickness", "[stages][surface]") {
    const double r = 20.0;
    const double t = 2.0;
    const TriMesh cap = makeCap(r, std::numbers::pi / 3.0, 24);
    QuadRemeshParams q;
    q.targetFaces = 300;
    CageCloseParams close;
    close.keepOutline = true;
    const auto sheet = quadRemesh(cap, q, close);
    REQUIRE(sheet);
    SurfaceBuildParams p;
    p.thicknessMm = t;
    const auto built = buildSurface(sheet->cage, &cap, p);
    REQUIRE(built);
    const SolidSummary& s = built->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.solidCount == 1);
    CHECK(s.freeEdges == 0);
    CHECK(s.faceCount == built->solid.faces().size());
    CHECK(cageStats(built->cage).closed());
    // Merged across the whole wall, the faces still sew at the sharp rim.
    CHECK(built->mergedFaces > 0);
    CHECK(s.faceCount < built->facesBeforeMerge / 4);
    CHECK(built->fit.p95DistanceMm < 0.05);
    CHECK(built->fit.ignoredSamples == 0);
    // The outer side stays on the sphere and the inner side lies t inside it.
    std::size_t outer = 0;
    std::size_t inner = 0;
    for (const Vec3d& v : built->solid.display().positions()) {
        if (std::abs(length(v) - r) < 0.05) ++outer;
        if (std::abs(length(v) - (r - t)) < 0.05) ++inner;
    }
    const std::size_t all = built->solid.display().vertexCount();
    CHECK(outer > all / 3);
    CHECK(inner > all / 3);
    // A spherical shell of the cap's angle: the rim strip is radial.
    const double angle = std::numbers::pi / 3.0;
    const double volume =
        2.0 / 3.0 * std::numbers::pi * (1.0 - std::cos(angle)) * (r * r * r - std::pow(r - t, 3));
    // Measured: 0.05 % over, with the rim 0.01 mm inside the scan's edge. (One closed cage rounds
    // the rim over: 3 % under, 0.45 mm inside.)
    CHECK(std::abs(s.volumeMm3 - volume) < 0.005 * volume);
    CHECK(std::abs(s.bounds.max.x - r * std::sin(angle)) < 0.05);
}

TEST_CASE("A torus cage without fitting becomes a solid with a hole", "[stages][surface]") {
    SurfaceBuildParams p;
    p.fit = false;
    const auto built = buildSurface(torusCage(30.0, 10.0, 24, 12), nullptr, p);
    REQUIRE(built);
    CHECK(built->solid.summary().valid);
    CHECK(built->solid.summary().freeEdges == 0);
    CHECK(built->solid.summary().volumeMm3 > 0.0);
    CHECK(built->capPieces == 0);
    p.fit = true;
    CHECK_FALSE(buildSurface(torusCage(30.0, 10.0, 24, 12), nullptr, p));
}

TEST_CASE("A surface that passes through itself gives no solid", "[stages][surface]") {
    // A spindle torus: its tube (radius 15) is wider than its ring (radius 10), so the inner side
    // of the tube passes through itself around the axis. The surface is smooth everywhere, so
    // no fold shows where faces meet, and the kernel's checker accepts the solid.
    SurfaceBuildParams p;
    p.fit = false;
    const QuadLayout cage = torusCage(10.0, 15.0, 16, 12);
    const auto built = buildSurface(cage, nullptr, p);
    REQUIRE_FALSE(built);
    CHECK(built.error().code == ErrorCode::InvalidState);
    CHECK(built.error().message.find("faces cross each other") != std::string::npos);

    p.checkCrossing = false;
    const auto unchecked = buildSurface(cage, nullptr, p);
    REQUIRE(unchecked);
    CHECK(unchecked->solid.summary().valid);
    CHECK(unchecked->seams.maxNormalAngleDeg < p.maxFoldAngleDeg);
    const auto crossing = occt::weldedTriangulation(unchecked->solid);
    REQUIRE(crossing);
    CHECK(computeStats(*crossing).isWatertight());
    CHECK(cgal::selfIntersects(*crossing) == true);

    // A ring torus (tube narrower than the ring) passes the check, its triangulation closed.
    p.checkCrossing = true;
    const auto ring = buildSurface(torusCage(30.0, 10.0, 24, 12), nullptr, p);
    REQUIRE(ring);
    const auto clean = occt::weldedTriangulation(ring->solid);
    REQUIRE(clean);
    CHECK(computeStats(*clean).isWatertight());
    CHECK(cgal::selfIntersects(*clean) == false);
}

TEST_CASE("A torus scan becomes a valid closed solid of the right volume", "[stages][surface]") {
    const double R = 30.0, r = 10.0;
    const TriMesh torus = makeTorus(R, r, 192, 96);
    QuadRemeshParams q;
    q.targetFaces = 600;
    const auto remeshed = quadRemesh(torus, q);
    REQUIRE(remeshed);
    const auto built = buildSurface(remeshed->cage, &torus, SurfaceBuildParams{});
    REQUIRE(built);
    const SolidSummary& s = built->solid.summary();
    CHECK(s.valid);
    CHECK(s.closed);
    CHECK(s.solidCount == 1);
    CHECK(s.freeEdges == 0);
    // The scan's facets cut the true torus by under 0.1%; the fit lies between them.
    const double volume = 2.0 * std::numbers::pi * std::numbers::pi * R * r * r;
    CHECK(std::abs(s.volumeMm3 - volume) < 0.003 * volume);
    CHECK(built->seams.unmatchedSamples == 0);
    CHECK(built->seams.maxNormalAngleDeg < 0.5);
}

TEST_CASE("The surface build gives the same faces every time", "[stages][surface]") {
    const TriMesh sphere = makeSphere(20.0, 4);
    QuadRemeshParams q;
    q.targetFaces = 300;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    const auto a = buildSurface(remeshed->cage, &sphere, SurfaceBuildParams{});
    const auto b = buildSurface(remeshed->cage, &sphere, SurfaceBuildParams{});
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->solid.faces().size() == b->solid.faces().size());
    for (std::size_t f = 0; f < a->solid.faces().size(); ++f) {
        CHECK(a->solid.faces()[f].poles == b->solid.faces()[f].poles);
    }
    CHECK(a->fit.rmsDistanceMm == b->fit.rmsDistanceMm);
    CHECK(a->solid.summary().volumeMm3 == b->solid.summary().volumeMm3);
}

TEST_CASE("Deviation reports a known offset from a solid and from a mesh", "[stages][deviation]") {
    const double r = 20.0;
    const TriMesh sphere = makeSphere(r, 5);
    QuadRemeshParams q;
    q.targetFaces = 400;
    const auto remeshed = quadRemesh(sphere, q);
    REQUIRE(remeshed);
    const auto built = buildSurface(remeshed->cage, &sphere, SurfaceBuildParams{});
    REQUIRE(built);

    for (const double offset : {0.5, -0.3}) {
        CAPTURE(offset);
        const TriMesh moved = makeSphere(r + offset, 4);
        const auto toSolid = deviationToSolid(moved, built->solid, 0.1);
        REQUIRE(toSolid);
        const DeviationStats& s = toSolid->stats();
        CHECK(s.count == moved.vertexCount());
        // The fitted solid is within about 0.01 mm of the sphere (see above).
        CHECK(std::abs(s.meanMm - offset) < 0.01);
        CHECK(std::abs(s.minMm - offset) < 0.03);
        CHECK(std::abs(s.maxMm - offset) < 0.03);
        CHECK(s.withinTolerance == (std::abs(offset) <= 0.1 ? 1.0 : 0.0));

        // To the scan itself: its facets lie up to a sagitta inside the sphere.
        const auto toMesh = deviationToMesh(moved, sphere, 0.1);
        REQUIRE(toMesh);
        CHECK(std::abs(toMesh->stats().meanMm - offset) < 0.02);
        CHECK(std::abs(toMesh->stats().minMm - offset) < 0.03);
    }
    // A point on the surface is at zero.
    const auto self = deviationToSolid(built->solid.display(), built->solid, 0.1);
    REQUIRE(self);
    CHECK(self->stats().p95AbsMm < 1e-6);
}

TEST_CASE("A flat area of the scan becomes its plane, dent and noise left out",
          "[stages][surface]") {
    const TriMesh scan = dentedBox();
    QuadRemeshParams q;
    q.targetFaces = 2400;
    const auto remeshed = quadRemesh(scan, q);
    REQUIRE(remeshed);
    const auto top = std::make_shared<const MeshSelection>(topFaces(scan));
    REQUIRE(top->count() > 100);
    std::vector<Vec3d> points;
    for (std::uint32_t f = 0; f < scan.faceCount(); ++f) {
        if (top->test(f)) points.push_back(scan.positions()[scan.face(f)[0]]);
    }
    const Primitive plane =
        fitPrimitive(Primitive{.kind = PrimitiveKind::Plane, .direction = {0, 0, 1}}, points)
            .primitive;
    REQUIRE(plane.direction.z > 0.999);

    SurfaceBuildParams p;
    const auto plain = buildSurface(remeshed->cage, &scan, p);
    REQUIRE(plain);
    CHECK(plain->heldVertices.empty());
    CHECK(topDeviation(plain->cage, plane) > 0.2); // the fit follows the dent

    p.flat.push_back(FlatArea{.point = plane.point, .normal = plane.direction, .faces = top});
    const auto flat = buildSurface(remeshed->cage, &scan, p);
    REQUIRE(flat);
    REQUIRE(flat->heldVertices.size() == 1);
    CHECK(flat->heldVertices[0] > 50);
    CHECK(topDeviation(flat->cage, plane) < 1e-6);
    const SolidSummary s = flat->solid.summary();
    CHECK(s.closed);
    CHECK(s.valid);
    // Away from the dent the fit is as close as before.
    CHECK(flat->fit.p95DistanceMm < plain->fit.p95DistanceMm * 1.5 + 0.01);

    // Held vertices are worked out again on the refined cage.
    p.refineToMm = 1e-6;
    p.maxRefinements = 1;
    const auto refined = buildSurface(remeshed->cage, &scan, p);
    REQUIRE(refined);
    CHECK(refined->heldVertices[0] > 3 * flat->heldVertices[0]);
    CHECK(topDeviation(refined->cage, plane) < 1e-6);
}

TEST_CASE("Flat areas need fitting, a selection of the scan and a unit normal",
          "[stages][surface]") {
    const TriMesh scan = dentedBox();
    QuadRemeshParams q;
    q.targetFaces = 600;
    const auto remeshed = quadRemesh(scan, q);
    REQUIRE(remeshed);
    const auto top = std::make_shared<const MeshSelection>(topFaces(scan));
    SurfaceBuildParams p;
    p.flat.push_back(FlatArea{.point = {0, 0, 10}, .normal = {0, 0, 1}, .faces = top});
    p.fit = false;
    CHECK(buildSurface(remeshed->cage, &scan, p).error().code == ErrorCode::InvalidArgument);
    p.fit = true;
    p.flat[0].normal = {0, 0, 2};
    CHECK(buildSurface(remeshed->cage, &scan, p).error().code == ErrorCode::InvalidArgument);
    p.flat[0].normal = {0, 0, 1};
    p.flat[0].faces = std::make_shared<const MeshSelection>(scan.faceCount() + 1);
    CHECK(buildSurface(remeshed->cage, &scan, p).error().code == ErrorCode::InvalidArgument);

    osd::CageFitParams f;
    f.held.push_back(
        HeldPlane{.point = {},
                  .normal = {0, 0, 1},
                  .vertices = {static_cast<std::uint32_t>(remeshed->cage.vertexCount())}});
    CHECK(fitCage(remeshed->cage, scan, f).error().code == ErrorCode::InvalidArgument);
    f.held[0] = HeldPlane{.point = {}, .normal = {0, 1, 1}, .vertices = {0}};
    CHECK(fitCage(remeshed->cage, scan, f).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("A cage's sharp edges stay sharp when subdivided", "[stages][surface]") {
    const QuadLayout cube = cubeCage(10.0);
    std::vector<std::array<std::uint32_t, 2>> creases = sharpCageEdges(cube, 60.0);
    CHECK(creases.size() == 12);
    CHECK(sharpCageEdges(cube, 95.0).empty());
    const auto finer = subdivideCage(cube, &creases);
    REQUIRE(finer);
    CHECK(creases.size() == 24); // each edge in two halves
    // Along the creases the step keeps the cube's edges: every crease vertex stays on one.
    for (const auto& [a, b] : creases) {
        for (const std::uint32_t v : {a, b}) {
            const Vec3d p = finer->positions()[v];
            int onFaces = 0;
            for (const double c : {p.x, p.y, p.z}) onFaces += c < 1e-9 || c > 10.0 - 1e-9 ? 1 : 0;
            CHECK(onFaces >= 2);
        }
    }
    // Built with its creases, the cube cage is the cube: its faces' seams leave the sharp edges
    // out, so they show no fold.
    SurfaceBuildParams p;
    p.fit = false;
    const auto box = buildSurface(cube, nullptr, p);
    REQUIRE(box);
    CHECK(box->sharpEdges == 12);
    CHECK_THAT(box->solid.summary().volumeMm3, Catch::Matchers::WithinRel(1000.0, 1e-6));
    CHECK(box->seams.maxNormalAngleDeg < 1.0);
    CHECK(box->solid.summary().faceCount == 6);
}

TEST_CASE("Faces that reach a sharp edge only at a corner show no fold there",
          "[stages][surface]") {
    // A point on a sharp edge where the top of a part (z = 0) meets its side (y = 0), with three
    // flat faces around it on each side. The middle face of each side touches the point only
    // between two smooth edges; its corner is still on the sharp edge, so the 90 degrees between
    // the two middle faces is no fold.
    const auto flat = [](Vec3d c00, Vec3d c10, Vec3d c11, Vec3d c01, const Vec3d& outward) {
        if (dot(cross(c10 - c00, c01 - c00), outward) < 0.0) std::swap(c10, c01);
        SurfacePatch face;
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                const double u = i / 3.0, v = j / 3.0;
                face.poles.push_back(c00 * ((1 - u) * (1 - v)) + c10 * (u * (1 - v)) +
                                     c11 * (u * v) + c01 * ((1 - u) * v));
            }
        }
        return face;
    };
    std::vector<SurfacePatch> faces;
    for (const auto& [side, outward] :
         {std::pair{std::function([](double x, double t) { return Vec3d{x, t, 0.0}; }),
                    Vec3d{0, 0, 1}},
          std::pair{std::function([](double x, double t) { return Vec3d{x, 0.0, -t}; }),
                    Vec3d{0, -1, 0}}}) {
        const Vec3d o{};
        faces.push_back(flat(o, side(1, 0), side(1, 1), side(0.5, 1), outward));
        faces.push_back(flat(o, side(0.5, 1), side(0, 1.5), side(-0.5, 1), outward));
        faces.push_back(flat(o, side(-0.5, 1), side(-1, 1), side(-1, 0), outward));
    }
    std::vector<std::array<bool, 4>> sharp;
    for (const SurfacePatch& face : faces) {
        const std::array corners{face.poles[0], face.poles[3], face.poles[12], face.poles[15]};
        const auto onEdge = [&](std::size_t a, std::size_t b) {
            return corners[a].y == 0.0 && corners[a].z == 0.0 && corners[b].y == 0.0 &&
                   corners[b].z == 0.0;
        };
        sharp.push_back({onEdge(0, 1), onEdge(2, 3), onEdge(0, 2), onEdge(1, 3)});
    }
    const auto seams = measureSeams(faces, 4, 1e-6, {}, sharp);
    REQUIRE(seams);
    CHECK(seams->maxNormalAngleDeg < 1.0);
}

TEST_CASE("A noisy block with a hole keeps its sharp edges", "[stages][surface]") {
    // Issue #64: a 30 x 30 x 10 block with a 12 mm hole, every vertex moved by up to 0.15 mm, in
    // a cage aligned to its edges. Fitted smooth, the surface rounds the sharp edges and bulges
    // past them.
    const TriMesh ideal = makeBlockWithHole(30, 30, 10, 6, 32, 10, 10);
    const auto distances = [&ideal](const Solid& solid) {
        auto d = occt::signedDistances(solid, ideal.positions());
        REQUIRE(d);
        for (double& x : *d) x = std::abs(x);
        std::ranges::sort(*d);
        return std::pair{d->back(), (*d)[d->size() * 95 / 100]}; // worst, 95th percentile
    };
    const QuadLayout cage = blockWithHoleCage(30, 30, 10, 6);
    // The top and bottom edges of the square and the hole, and the four corners up the sides.
    REQUIRE(sharpCageEdges(cage, 60.0).size() == 140);
    for (const auto& [scan, limitMm] :
         {std::pair{addNoise(makeBlockWithHole(30, 30, 10, 6, 24, 8, 8), 0.15), 0.15},
          // A coarse mesh, as a CAD export is: long triangles across the sides.
          std::pair{addNoise(makeBlockWithHole(30, 30, 10, 6, 4, 1, 1), 0.15), 0.3}}) {
        SurfaceBuildParams p;
        const auto sharp = buildSurface(cage, &scan, p);
        REQUIRE(sharp);
        CHECK(sharp->sharpEdges == 140);
        // Merged up to the sharp edges and the corners where they meet: four faces each for the
        // top, the bottom, the sides and the hole.
        CHECK(sharp->solid.summary().faceCount == 16);
        const auto [worst, p95] = distances(sharp->solid);
        CHECK(worst < limitMm);
        CHECK(p95 < 0.15);
        p.creaseAngleDeg = 0.0;
        const auto smooth = buildSurface(cage, &scan, p);
        REQUIRE(smooth);
        CHECK(smooth->sharpEdges == 0);
        CHECK(distances(smooth->solid).first > 0.5);
    }
}

TEST_CASE("Sharp edges of the cage that the scan does not have stay smooth", "[stages][surface]") {
    // A distorted cage, as quad.remesh can make around a hole: one vertex on the block's flat top
    // stands 4 mm proud, so its quads meet at steep angles. The scan is flat there, so no crease
    // is made there, and the fit flattens the bump.
    const TriMesh scan = addNoise(makeBlockWithHole(30, 30, 10, 6, 24, 8, 8), 0.15);
    const QuadLayout aligned = blockWithHoleCage(30, 30, 10, 6);
    std::vector<Vec3d> moved = aligned.positions();
    const auto bump = std::ranges::find_if(
        moved, [](const Vec3d& v) { return length(v - Vec3d{15.0, 4.5, 10.0}) < 1e-9; });
    REQUIRE(bump != moved.end());
    *bump = *bump + Vec3d{0.0, 0.0, 4.0};
    const auto distorted =
        QuadLayout::create(std::move(moved), aligned.faceStarts(), aligned.faceVertices());
    REQUIRE(distorted);
    REQUIRE(sharpCageEdges(*distorted, 60.0).size() > 140);
    const auto built = buildSurface(*distorted, &scan, SurfaceBuildParams{});
    REQUIRE(built);
    CHECK(built->sharpEdges == 140);
    CHECK(built->fit.p95DistanceMm < 0.2);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Surface build stage tests (issue #17): cage fitting and B-spline face extraction, checked by
// distance to known shapes, gaps between faces and the angle across cap edges.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/pipeline/Deviation.h"
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"
#include "m2c/pipeline/PatchMerge.h"

namespace m2c {

namespace {

constexpr int kSeamSamples = 8;           // samples per face edge for the seam report
constexpr double kSeamToleranceMm = 1e-2; // edge samples closer than this are the same point

/// Faces [begin, end) of a cage as a cage of their own; `remap` takes the cage's vertex indices
/// to the part's (UINT32_MAX for vertices it does not use).
Result<QuadLayout> partOf(const QuadLayout& cage,
                          const std::size_t begin,
                          const std::size_t end,
                          std::vector<std::uint32_t>& remap) {
    remap.assign(cage.vertexCount(), UINT32_MAX);
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> starts{0};
    std::vector<std::uint32_t> vertices;
    for (std::size_t f = begin; f < end; ++f) {
        for (const std::uint32_t v : cage.face(f)) {
            if (remap[v] == UINT32_MAX) {
                remap[v] = static_cast<std::uint32_t>(positions.size());
                positions.push_back(cage.positions()[v]);
            }
            vertices.push_back(remap[v]);
        }
        starts.push_back(static_cast<std::uint32_t>(vertices.size()));
    }
    return QuadLayout::create(std::move(positions), std::move(starts), std::move(vertices));
}

/// The faces of one cage, merged when asked, with their seams measured before merging.
struct Faces {
    PatchResult patches;
    SeamReport seams;
    std::size_t before{0};
    std::size_t merged{0};
    std::size_t unmerged{0};
};

Result<Faces> mergedFaces(const QuadLayout& cage,
                          PatchResult patches,
                          const SeamReport& seams,
                          const bool merge,
                          const std::vector<std::array<std::uint32_t, 2>>& fences = {}) {
    Faces out{.patches = std::move(patches), .seams = seams};
    out.before = out.patches.faces.size();
    if (merge) {
        auto merged = mergeFaces(cage, out.patches.faces, fences);
        if (!merged) return std::unexpected(merged.error());
        out.patches.faces = std::move(merged->faces);
        out.merged = merged->mergedFaces;
        out.unmerged = merged->partition.unmergedQuads;
    }
    return out;
}

/// A sheet's wall (thickenCage()) as three open surfaces: the outer side, the inner side and the
/// rim. They meet along the limit curves of the rim loops, so the rim edges stay sharp; one
/// closed cage would round them over and pull the outer side in from the scan's edge. Faces are
/// merged over the whole wall with the rim loops as fences, so the faces on either side of a rim
/// edge end at the same vertices and sew. The seams are each surface's own.
Result<Faces> wallFaces(const QuadLayout& wall,
                        const std::size_t sheetFaces,
                        const PatchParams& params,
                        const bool merge) {
    PatchResult all;
    SeamReport worst;
    const std::array<std::size_t, 4> bounds{0, sheetFaces, 2 * sheetFaces, wall.faceCount()};
    // The sheet's vertices used by one face are corners of its edge (OpenSubdiv), and of the
    // inner side's; the rim gets the same corners, so its edges are the same curves, and a sharp
    // edge straight across the wall between each pair.
    const auto n = static_cast<std::uint32_t>(wall.vertexCount() / 2);
    std::vector<std::uint32_t> uses(n, 0);
    for (std::size_t f = 0; f < sheetFaces; ++f) {
        for (const std::uint32_t v : wall.face(f)) ++uses[v];
    }
    std::vector<std::array<std::uint32_t, 2>> corners;
    for (std::uint32_t v = 0; v < n; ++v) {
        if (uses[v] == 1) corners.push_back({v, v + n});
    }
    std::vector<std::uint32_t> remap;
    for (std::size_t k = 0; k < 3; ++k) {
        auto part = partOf(wall, bounds[k], bounds[k + 1], remap);
        if (!part) return std::unexpected(part.error());
        PatchParams partParams = params;
        if (k == 2) {
            for (const auto& [a, b] : corners) {
                partParams.corners.insert(partParams.corners.end(), {remap[a], remap[b]});
                partParams.creases.push_back({remap[a], remap[b]});
            }
        }
        auto patches = extractPatches(*part, partParams);
        if (!patches) return std::unexpected(patches.error());
        auto seams = measureSeams(patches->faces, kSeamSamples, kSeamToleranceMm, patches->caps);
        if (!seams) return std::unexpected(seams.error());
        for (SurfacePatch& face : patches->faces) {
            face.cageFace += static_cast<std::uint32_t>(bounds[k]);
            all.faces.push_back(std::move(face));
        }
        all.caps.insert(all.caps.end(), patches->caps.begin(), patches->caps.end());
        all.regularPieces += patches->regularPieces;
        all.capPieces += patches->capPieces;
        all.maxCapErrorMm = std::max(all.maxCapErrorMm, patches->maxCapErrorMm);
        if (k == 2) break; // the rim's own seams include its sharp edges
        worst.maxGapMm = std::max(worst.maxGapMm, seams->maxGapMm);
        worst.maxNormalAngleDeg = std::max(worst.maxNormalAngleDeg, seams->maxNormalAngleDeg);
        worst.maxCapNormalAngleDeg =
            std::max(worst.maxCapNormalAngleDeg, seams->maxCapNormalAngleDeg);
        worst.maxRegularNormalAngleDeg =
            std::max(worst.maxRegularNormalAngleDeg, seams->maxRegularNormalAngleDeg);
        worst.unmatchedSamples += seams->unmatchedSamples;
    }
    // Fences: the rim loops (edges of rim faces that no other rim face has) and the sharp edges
    // across the rim.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> rimEdges;
    for (std::size_t f = bounds[2]; f < bounds[3]; ++f) {
        const auto face = wall.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            ++rimEdges[std::minmax(face[i], face[(i + 1) % face.size()])];
        }
    }
    std::vector<std::array<std::uint32_t, 2>> fences = corners;
    for (const auto& [edge, count] : rimEdges) {
        if (count == 1) fences.push_back({edge.first, edge.second});
    }
    return mergedFaces(wall, std::move(all), worst, merge, fences);
}

} // namespace

Result<QuadLayout> thickenCage(const QuadLayout& sheet, const double thicknessMm) {
    if (!(thicknessMm > 0.0)) return fail(ErrorCode::InvalidArgument, "thickness must be positive");
    const CageStats stats = cageStats(sheet);
    if (!stats.orientedManifold() || stats.boundaryEdges == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "only an open, consistently oriented cage can be thickened");
    }
    const auto n = static_cast<std::uint32_t>(sheet.vertexCount());
    const auto& p = sheet.positions();
    std::vector<Vec3d> normals(n);
    std::unordered_set<std::uint64_t> directed;
    const auto key = [](const std::uint32_t a, const std::uint32_t b) {
        return (std::uint64_t{a} << 32) | b;
    };
    for (std::size_t f = 0; f < sheet.faceCount(); ++f) {
        const auto face = sheet.face(f);
        Vec3d area{};
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i], b = face[(i + 1) % face.size()];
            area = area + cross(p[a], p[b]);
            directed.insert(key(a, b));
        }
        for (const std::uint32_t v : face) normals[v] = normals[v] + area;
    }
    // The limit surface's normals, not the cage's: a cage zigzags over bends tighter than its
    // quads, and its own normals there would fold the inner side.
    auto limit = limitNormals(sheet);
    if (!limit) return std::unexpected(limit.error());
    std::vector<Vec3d> positions = p;
    for (std::uint32_t v = 0; v < n; ++v) {
        const double size = length(normals[v]);
        const Vec3d d = length((*limit)[v]) > 0.0 ? (*limit)[v]
                        : size > 0.0              ? normals[v] * (1.0 / size)
                                                  : Vec3d{};
        positions.push_back(p[v] - d * thicknessMm);
    }
    std::vector<std::uint32_t> starts = sheet.faceStarts();
    std::vector<std::uint32_t> vertices = sheet.faceVertices();
    for (std::size_t f = 0; f < sheet.faceCount(); ++f) {
        const auto face = sheet.face(f);
        for (std::size_t i = face.size(); i-- > 0;) vertices.push_back(face[i] + n);
        starts.push_back(static_cast<std::uint32_t>(vertices.size()));
    }
    for (std::size_t f = 0; f < sheet.faceCount(); ++f) {
        const auto face = sheet.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::uint32_t a = face[i], b = face[(i + 1) % face.size()];
            if (directed.contains(key(b, a))) continue;
            // The sheet runs a -> b along its edge and the turned copy b' -> a', so the quad
            // across the wall runs b -> a -> a' -> b'.
            vertices.insert(vertices.end(), {b, a, a + n, b + n});
            starts.push_back(static_cast<std::uint32_t>(vertices.size()));
        }
    }
    return QuadLayout::create(std::move(positions), std::move(starts), std::move(vertices));
}

Result<SurfaceBuildResult>
buildSurface(const QuadLayout& cage, const TriMesh* scan, const SurfaceBuildParams& params) {
    if (params.fit && scan == nullptr) {
        return fail(ErrorCode::InvalidArgument, "fitting needs a scan");
    }
    if (!(params.maxSeamAngleDeg > 0.0) || !(params.maxFoldAngleDeg > 0.0) || params.fitTries < 1 ||
        !(params.refineToMm >= 0.0) || !(params.thicknessMm >= 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    "the angle limits must be positive, the fit tried at least once and the "
                    "refinement target and thickness at least 0");
    }
    const bool sheet = params.thicknessMm > 0.0;
    if (const CageStats stats = cageStats(cage); sheet == (stats.boundaryEdges == 0)) {
        return fail(ErrorCode::InvalidArgument,
                    sheet ? "a thickness needs an open cage" : "an open cage needs a thickness");
    }
    // A fit that folds the surface shows as a large angle between the normals where faces meet
    // (the limit surface is smooth, apart from the small kinks at caps). Such a fit is tried
    // again with twice the smoothing; the last try is kept if none passes.
    // A fit still farther from the scan than refineToMm (95th percentile) is refined: its cage
    // gets one Catmull-Clark step, which keeps the surface and its layout, and is fitted again.
    CageFitParams fitting = params.fitting;
    fitting.open = sheet;
    QuadLayout start = cage;
    unsigned tries = 0;
    std::vector<double> p95BeforeRefinement;
    for (;;) {
        CageFitResult fit{.cage = start,
                          .samples = 0,
                          .ignoredSamples = 0,
                          .rmsDistanceMm = 0.0,
                          .maxDistanceMm = 0.0,
                          .p95DistanceMm = 0.0,
                          .rmsBeforeRoundMm = {}};
        if (params.fit) {
            auto fitted = fitCage(start, *scan, fitting);
            if (!fitted) return std::unexpected(fitted.error());
            fit = std::move(*fitted);
        }
        ++tries;
        auto patches = extractPatches(fit.cage, PatchParams{fitting.isolationLevel, sheet});
        if (!patches) return std::unexpected(patches.error());
        auto seams = measureSeams(patches->faces, kSeamSamples, kSeamToleranceMm, patches->caps);
        if (!seams) return std::unexpected(seams.error());
        if (params.fit && seams->maxNormalAngleDeg > params.maxSeamAngleDeg &&
            tries < params.fitTries) {
            fitting.smoothing = fitting.smoothing > 0.0 ? 2.0 * fitting.smoothing : 0.01;
            continue;
        }
        if (seams->maxNormalAngleDeg > params.maxFoldAngleDeg) {
            return fail(ErrorCode::InvalidState,
                        "the surface folds over itself: faces meet at " +
                            std::to_string(std::lround(seams->maxNormalAngleDeg)) +
                            " degrees after " + std::to_string(tries) +
                            (tries == 1 ? " fit" : " fits") +
                            "; the cage has folded faces or is too coarse for thin parts");
        }
        if (params.fit && params.refineToMm > 0.0 && fit.p95DistanceMm > params.refineToMm &&
            p95BeforeRefinement.size() < params.maxRefinements) {
            auto finer = subdivideCage(fit.cage);
            if (!finer) return std::unexpected(finer.error());
            p95BeforeRefinement.push_back(fit.p95DistanceMm);
            start = std::move(*finer);
            tries = 0;
            continue;
        }
        QuadLayout used = fit.cage;
        if (sheet) {
            auto wall = thickenCage(fit.cage, params.thicknessMm);
            if (!wall) return std::unexpected(wall.error());
            used = std::move(*wall);
        }
        auto faces = sheet ? wallFaces(used,
                                       fit.cage.faceCount(),
                                       PatchParams{fitting.isolationLevel, true},
                                       params.merge)
                           : mergedFaces(used, std::move(*patches), *seams, params.merge);
        if (!faces) return std::unexpected(faces.error());
        auto& [faceSet, faceSeams, before, mergedCount, unmergedCount] = *faces;
        // The inner side is the outer one moved along its normals: it folds where the outer
        // side bends more tightly than the thickness.
        if (sheet && faceSeams.maxNormalAngleDeg > params.maxFoldAngleDeg) {
            return fail(ErrorCode::InvalidState,
                        "the wall folds over itself: faces meet at " +
                            std::to_string(std::lround(faceSeams.maxNormalAngleDeg)) +
                            " degrees; the thickness is more than the sheet's tightest bend");
        }
        auto display = tessellate(faceSet.faces, params.displayGrid);
        if (!display) return std::unexpected(display.error());
        auto solid = occt::makeSolid(std::move(faceSet.faces),
                                     std::make_shared<const FaceTessellation>(std::move(*display)),
                                     occt::SewParams{params.sewToleranceMm});
        if (!solid) return std::unexpected(solid.error());
        return SurfaceBuildResult{.solid = std::move(*solid),
                                  .cage = std::move(used),
                                  .fit = std::move(fit),
                                  .smoothing = fitting.smoothing,
                                  .fits = tries,
                                  .regularPieces = faceSet.regularPieces,
                                  .capPieces = faceSet.capPieces,
                                  .maxCapErrorMm = faceSet.maxCapErrorMm,
                                  .seams = faceSeams,
                                  .facesBeforeMerge = before,
                                  .mergedFaces = mergedCount,
                                  .unmergedQuads = unmergedCount,
                                  .p95BeforeRefinementMm = std::move(p95BeforeRefinement)};
    }
}

} // namespace m2c

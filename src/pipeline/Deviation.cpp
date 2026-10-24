// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Deviation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "m2c/adapters/occt/Brep.h"
#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/Parallel.h"
#include "m2c/core/Progress.h"
#include "m2c/core/Solid.h"
#include "m2c/core/SpatialIndex.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c {

namespace {

constexpr std::size_t kChunk = 4096; // vertices measured per parallel chunk and cancel check

} // namespace

Result<DeviationField> deviationToSolid(const TriMesh& mesh,
                                        const Solid& reference,
                                        const double toleranceMm,
                                        const Progress& progress) {
    auto distances = occt::signedDistances(reference, mesh.positions(), progress);
    if (!distances) return std::unexpected(distances.error());
    return DeviationField::create(std::move(*distances), toleranceMm);
}

Result<DeviationField> deviationToMesh(const TriMesh& mesh,
                                       const TriMesh& reference,
                                       const double toleranceMm,
                                       const Progress& progress) {
    const SpatialIndex& index = reference.spatialIndex();
    const std::vector<Vec3d>& positions = mesh.positions();
    std::vector<double> distances(positions.size());
    parallelChunks(positions.size(), kChunk, [&](const std::size_t begin, const std::size_t end) {
        if (progress.cancelled()) return;
        for (std::size_t i = begin; i < end; ++i) {
            const auto [face, distance, point] = index.closestPoint(positions[i]);
            // The side of the face holding the closest point; near an edge or a vertex the
            // offset lies in the normal cone there, so either neighbour usually agrees.
            const bool outside = dot(positions[i] - point, faceNormal(reference, face)) >= 0.0;
            distances[i] = outside ? distance : -distance;
        }
    });
    if (auto go = checkCancelled(progress, "measuring distances to the mesh"); !go)
        return std::unexpected(go.error());
    return DeviationField::create(std::move(distances), toleranceMm);
}

std::vector<DeviationHotspot> deviationHotspots(const TriMesh& mesh, const DeviationField& field) {
    const std::vector<double>& d = field.distances();
    const double tolerance = field.stats().toleranceMm;
    const std::size_t faceCount = mesh.faceCount();
    // Each face's sign (0 within tolerance) and the vertex deciding it.
    std::vector sign(faceCount, 0);
    std::vector<std::uint32_t> worst(faceCount, 0);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const auto v = mesh.face(f);
        std::uint32_t w = v[0];
        for (const std::uint32_t k : v) {
            if (std::abs(d[k]) > std::abs(d[w])) w = k;
        }
        worst[f] = w;
        if (std::abs(d[w]) > tolerance) sign[f] = d[w] > 0.0 ? 1 : -1;
    }
    const FaceAdjacency adjacency = buildFaceAdjacency(mesh);
    std::vector seen(faceCount, false);
    std::vector<DeviationHotspot> out;
    std::vector<std::uint32_t> stack;
    for (std::size_t start = 0; start < faceCount; ++start) {
        if (sign[start] == 0 || seen[start]) continue;
        DeviationHotspot h;
        h.sign = sign[start];
        seen[start] = true;
        stack.assign(1, static_cast<std::uint32_t>(start));
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            h.faces.push_back(f);
            for (std::uint32_t k = adjacency.offsets[f]; k < adjacency.offsets[f + 1]; ++k) {
                const std::uint32_t g = adjacency.neighbours[k];
                if (!seen[g] && sign[g] == h.sign) {
                    seen[g] = true;
                    stack.push_back(g);
                }
            }
        }
        std::ranges::sort(h.faces);
        std::vector<std::uint32_t> vertices;
        Vec3d weighted{};
        h.seedFace = h.faces.front();
        h.bounds = Aabb{.min = mesh.positions()[mesh.face(h.faces.front())[0]],
                        .max = mesh.positions()[mesh.face(h.faces.front())[0]]};
        for (const std::uint32_t f : h.faces) {
            const auto v = mesh.face(f);
            const double area = faceArea(mesh, f);
            const Vec3d& a = mesh.positions()[v[0]];
            const Vec3d& b = mesh.positions()[v[1]];
            const Vec3d& c = mesh.positions()[v[2]];
            h.areaMm2 += area;
            weighted = weighted + (a + b + c) * (area / 3.0);
            if (std::abs(d[worst[f]]) > std::abs(d[worst[h.seedFace]])) h.seedFace = f;
            for (const std::uint32_t k : v) {
                vertices.push_back(k);
                const Vec3d& p = mesh.positions()[k];
                h.bounds.min = {.x = std::min(h.bounds.min.x, p.x),
                                .y = std::min(h.bounds.min.y, p.y),
                                .z = std::min(h.bounds.min.z, p.z)};
                h.bounds.max = {.x = std::max(h.bounds.max.x, p.x),
                                .y = std::max(h.bounds.max.y, p.y),
                                .z = std::max(h.bounds.max.z, p.z)};
            }
        }
        h.maxSignedMm = d[worst[h.seedFace]];
        h.centroid = h.areaMm2 > 0.0 ? weighted * (1.0 / h.areaMm2) : h.bounds.min;
        std::ranges::sort(vertices);
        const auto [last, end] = std::ranges::unique(vertices);
        vertices.erase(last, end);
        double sum = 0.0;
        for (const std::uint32_t k : vertices) sum += d[k] * d[k];
        h.rmsMm = std::sqrt(sum / static_cast<double>(vertices.size()));
        out.push_back(std::move(h));
    }
    std::ranges::stable_sort(out, [](const DeviationHotspot& a, const DeviationHotspot& b) {
        if (a.areaMm2 != b.areaMm2) return a.areaMm2 > b.areaMm2;
        return a.seedFace < b.seedFace;
    });
    return out;
}

} // namespace m2c

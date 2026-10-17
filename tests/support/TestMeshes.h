// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"

namespace m2c::test {

/// Outward-facing unit cube [0,1]^3: 8 vertices, 12 triangles, area 6, volume 1, watertight.
inline std::vector<Vec3d> cubePositions() {
    return {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
}

inline std::vector<std::uint32_t> cubeIndices() {
    return {0, 2, 1, 0, 3, 2,  // bottom (-z)
            4, 5, 6, 4, 6, 7,  // top (+z)
            0, 1, 5, 0, 5, 4,  // front (-y)
            3, 7, 6, 3, 6, 2,  // back (+y)
            0, 4, 7, 0, 7, 3,  // left (-x)
            1, 2, 6, 1, 6, 5}; // right (+x)
}

inline TriMesh makeMesh(std::vector<Vec3d> positions, std::vector<std::uint32_t> indices) {
    auto mesh = TriMesh::create(std::move(positions), std::move(indices));
    REQUIRE(mesh.has_value());
    return *mesh;
}

inline TriMesh makeCube() {
    return makeMesh(cubePositions(), cubeIndices());
}

/// Box [0,sx]x[0,sy]x[0,sz] with each face split into n x n quads (2 n^2 triangles), outward.
inline TriMesh makeBox(double sx, double sy, double sz, int n = 1) {
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    std::map<std::array<long long, 3>, std::uint32_t> lookup;
    const auto vertex = [&](double x, double y, double z) {
        const std::array<long long, 3> key{
            std::llround(x * 1e6), std::llround(y * 1e6), std::llround(z * 1e6)};
        const auto [it, inserted] = lookup.try_emplace(key, static_cast<std::uint32_t>(p.size()));
        if (inserted) p.push_back({x, y, z});
        return it->second;
    };
    // origin, u axis, v axis (u x v = outward normal)
    const std::array<std::array<Vec3d, 3>, 6> faces{{
        {Vec3d{0, 0, 0}, Vec3d{0, sy, 0}, Vec3d{sx, 0, 0}},  // -z
        {Vec3d{0, 0, sz}, Vec3d{sx, 0, 0}, Vec3d{0, sy, 0}}, // +z
        {Vec3d{0, 0, 0}, Vec3d{sx, 0, 0}, Vec3d{0, 0, sz}},  // -y
        {Vec3d{0, sy, 0}, Vec3d{0, 0, sz}, Vec3d{sx, 0, 0}}, // +y
        {Vec3d{0, 0, 0}, Vec3d{0, 0, sz}, Vec3d{0, sy, 0}},  // -x
        {Vec3d{sx, 0, 0}, Vec3d{0, sy, 0}, Vec3d{0, 0, sz}}, // +x
    }};
    for (const auto& [o, u, v] : faces) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const auto at = [&](int a, int b) {
                    const Vec3d q =
                        o + u * (static_cast<double>(a) / n) + v * (static_cast<double>(b) / n);
                    return vertex(q.x, q.y, q.z);
                };
                const auto v00 = at(i, j), v10 = at(i + 1, j), v11 = at(i + 1, j + 1),
                           v01 = at(i, j + 1);
                idx.insert(idx.end(), {v00, v10, v11, v00, v11, v01});
            }
        }
    }
    return makeMesh(std::move(p), std::move(idx));
}

/// Icosphere of radius r around c, subdivided `levels` times (20 * 4^levels triangles).
inline TriMesh makeSphere(double r = 1.0, int levels = 3, Vec3d c = {}) {
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    std::vector<Vec3d> p{{-1, t, 0},
                         {1, t, 0},
                         {-1, -t, 0},
                         {1, -t, 0},
                         {0, -1, t},
                         {0, 1, t},
                         {0, -1, -t},
                         {0, 1, -t},
                         {t, 0, -1},
                         {t, 0, 1},
                         {-t, 0, -1},
                         {-t, 0, 1}};
    std::vector<std::uint32_t> f{0, 11, 5,  0, 5,  1, 0, 1, 7, 0, 7,  10, 0, 10, 11, 1, 5, 9, 5, 11,
                                 4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3,  9,  4, 3,  4,  2, 3, 2, 6, 3,
                                 6, 8,  3,  8, 9,  4, 9, 5, 2, 4, 11, 6,  2, 10, 8,  6, 7, 9, 8, 1};
    for (int l = 0; l < levels; ++l) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            const auto [it, inserted] = mid.try_emplace(key, static_cast<std::uint32_t>(p.size()));
            if (inserted) p.push_back((p[a] + p[b]) * 0.5);
            return it->second;
        };
        std::vector<std::uint32_t> next;
        for (std::size_t i = 0; i < f.size(); i += 3) {
            const auto a = f[i], b = f[i + 1], cc = f[i + 2];
            const auto ab = midpoint(a, b), bc = midpoint(b, cc), ca = midpoint(cc, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, cc, ca, bc, ab, bc, ca});
        }
        f = std::move(next);
    }
    for (Vec3d& v : p) v = c + v * (r / length(v));
    return makeMesh(std::move(p), std::move(f));
}

/// Torus around the z axis with tube centre radius R and tube radius r, n x m quads split into
/// triangles, outward. Genus 1.
inline TriMesh makeTorus(double R = 3.0, double r = 1.0, int n = 48, int m = 24) {
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    for (int i = 0; i < n; ++i) {
        const double u = 2.0 * 3.14159265358979323846 * i / n;
        for (int j = 0; j < m; ++j) {
            const double v = 2.0 * 3.14159265358979323846 * j / m;
            p.push_back({(R + r * std::cos(v)) * std::cos(u),
                         (R + r * std::cos(v)) * std::sin(u),
                         r * std::sin(v)});
        }
    }
    const auto at = [&](int i, int j) { return static_cast<std::uint32_t>((i % n) * m + j % m); };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            const auto a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
    return makeMesh(std::move(p), std::move(idx));
}

/// Moves every vertex along a random direction by up to `amount` (seeded, reproducible).
inline TriMesh addNoise(const TriMesh& mesh, double amount, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-amount, amount);
    std::vector<Vec3d> p = mesh.positions();
    for (Vec3d& v : p) v = v + Vec3d{d(rng), d(rng), d(rng)};
    return makeMesh(std::move(p), mesh.indices());
}

/// Concatenates meshes into one (separate shells).
inline TriMesh combine(const std::vector<TriMesh>& parts) {
    std::vector<Vec3d> p;
    std::vector<std::uint32_t> idx;
    for (const TriMesh& m : parts) {
        const auto base = static_cast<std::uint32_t>(p.size());
        p.insert(p.end(), m.positions().begin(), m.positions().end());
        for (const std::uint32_t i : m.indices()) idx.push_back(base + i);
    }
    return makeMesh(std::move(p), std::move(idx));
}

/// Temporary directory removed when the object goes out of scope.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mesh2cad-test-" + std::to_string(counter++) + "-" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace m2c::test

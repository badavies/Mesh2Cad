// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/pipeline/Shells.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "m2c/core/MeshSelection.h"
#include "m2c/core/MeshTopology.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

ShellReport detectShells(const TriMesh& mesh) {
    ShellReport report;
    std::uint32_t count = 0;
    report.faceShell = faceComponents(mesh, &count);
    report.shells.resize(count);
    std::vector started(count, false);
    const auto& p = mesh.positions();
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        ShellInfo& s = report.shells[report.faceShell[f]];
        s.index = report.faceShell[f];
        ++s.faceCount;
        s.areaMm2 += faceArea(mesh, f);
        for (const std::uint32_t v : mesh.face(f)) {
            if (!started[s.index]) {
                s.bounds = {.min = p[v], .max = p[v]};
                started[s.index] = true;
            }
            s.bounds.min = {.x = std::min(s.bounds.min.x, p[v].x),
                            .y = std::min(s.bounds.min.y, p[v].y),
                            .z = std::min(s.bounds.min.z, p[v].z)};
            s.bounds.max = {.x = std::max(s.bounds.max.x, p[v].x),
                            .y = std::max(s.bounds.max.y, p[v].y),
                            .z = std::max(s.bounds.max.z, p[v].z)};
        }
    }
    if (!report.shells.empty()) {
        const auto main =
            std::ranges::max_element(report.shells, [](const ShellInfo& a, const ShellInfo& b) {
                return a.faceCount < b.faceCount;
            });
        main->isMain = true;
    }
    return report;
}

MeshSelection shellFaces(const ShellReport& report, const std::vector<std::uint32_t>& shells) {
    std::vector wanted(report.shells.size(), false);
    for (const std::uint32_t s : shells) {
        if (s < wanted.size()) wanted[s] = true;
    }
    MeshSelection selection(report.faceShell.size());
    for (std::size_t f = 0; f < report.faceShell.size(); ++f) {
        if (wanted[report.faceShell[f]]) selection.set(f);
    }
    return selection;
}

MeshSelection
smallShellFaces(const ShellReport& report, const std::size_t minFaces, const double minAreaMm2) {
    std::vector<std::uint32_t> small;
    for (const ShellInfo& s : report.shells) {
        if (s.isMain) continue;
        if ((minFaces > 0 && s.faceCount < minFaces) ||
            (minAreaMm2 > 0.0 && s.areaMm2 < minAreaMm2)) {
            small.push_back(s.index);
        }
    }
    return shellFaces(report, small);
}

} // namespace m2c

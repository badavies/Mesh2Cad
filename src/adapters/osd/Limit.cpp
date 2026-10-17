// SPDX-License-Identifier: GPL-3.0-or-later
#include "Limit.h"

#include <algorithm>
#include <exception>
#include <string>

#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/ptexIndices.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyDescriptor.h>

namespace m2c::osd {

namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

namespace {

/// Point type for OpenSubdiv's stencil evaluation.
struct Point {
    Vec3d p;
    void Clear() { p = {}; }
    void AddWithWeight(const Point& src, double w) { p = p + src.p * w; }
};

} // namespace

Result<std::unique_ptr<Limit>> Limit::create(const QuadLayout& cage, int isolationLevel) {
    if (isolationLevel < 1 || isolationLevel > 4) {
        return fail(ErrorCode::InvalidArgument, "isolation level must be 1 to 4");
    }
    if (!cageStats(cage).closed()) {
        return fail(ErrorCode::InvalidArgument, "the cage must be closed");
    }
    std::vector<int> sizes(cage.faceCount());
    for (std::size_t f = 0; f < cage.faceCount(); ++f) {
        sizes[f] = static_cast<int>(cage.face(f).size());
    }
    std::vector<int> indices(cage.faceVertices().begin(), cage.faceVertices().end());
    Far::TopologyDescriptor desc;
    desc.numVertices = static_cast<int>(cage.vertexCount());
    desc.numFaces = static_cast<int>(cage.faceCount());
    desc.numVertsPerFace = sizes.data();
    desc.vertIndicesPerFace = indices.data();

    Sdc::Options sdc;
    sdc.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
    std::unique_ptr<Limit> limit(new Limit());
    try {
        limit->refiner_.reset(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
            desc,
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options(Sdc::SCHEME_CATMARK,
                                                                          sdc)));
        if (!limit->refiner_) {
            return fail(ErrorCode::InvalidArgument, "OpenSubdiv rejected the cage topology");
        }
        Far::PatchTableFactory::Options options(static_cast<unsigned>(isolationLevel));
        options.SetEndCapType(Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
        options.SetPatchPrecision<double>();
        options.useInfSharpPatch = true;
        limit->refiner_->RefineAdaptive(options.GetRefineAdaptiveOptions());
        limit->patches_.reset(Far::PatchTableFactory::Create(*limit->refiner_, options));

        Far::StencilTableFactoryReal<double>::Options stencilOptions;
        stencilOptions.generateIntermediateLevels = true;
        stencilOptions.generateOffsets = true;
        std::unique_ptr<const Far::StencilTableReal<double>> refined(
            Far::StencilTableFactoryReal<double>::Create(*limit->refiner_, stencilOptions));
        if (const auto* local = limit->patches_->GetLocalPointStencilTable<double>()) {
            limit->stencils_.reset(
                Far::StencilTableFactoryReal<double>::AppendLocalPointStencilTable(
                    *limit->refiner_, refined.get(), local));
        } else {
            limit->stencils_ = std::move(refined);
        }
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("OpenSubdiv failed: ") + e.what());
    }
    limit->cageVertices_ = cage.vertexCount();

    const Far::PtexIndices ptex(*limit->refiner_);
    for (std::uint32_t f = 0; f < cage.faceCount(); ++f) {
        const int first = ptex.GetFaceId(static_cast<int>(f));
        limit->cageFaceOfPtex_.resize(
            static_cast<std::size_t>(first + (sizes[f] == 4 ? 1 : sizes[f])), f);
    }
    const Far::PatchTable& table = *limit->patches_;
    for (int a = 0, global = 0; a < table.GetNumPatchArrays(); ++a) {
        const int cvs = table.GetPatchArrayDescriptor(a).GetNumControlVertices();
        for (int p = 0; p < table.GetNumPatches(a); ++p, ++global) {
            Far::PatchTable::PatchHandle handle{};
            handle.arrayIndex = a;
            handle.patchIndex = global;
            handle.vertIndex = p * cvs;
            limit->handles_.push_back(handle);
            const Far::PatchParam param = table.GetPatchParam(a, p);
            double u0 = 0.0, v0 = 0.0, u1 = 1.0, v1 = 1.0;
            param.Unnormalize(u0, v0);
            param.Unnormalize(u1, v1);
            limit->domains_.push_back(
                {static_cast<std::uint32_t>(param.GetFaceId()), u0, v0, u1 - u0});
        }
    }
    return limit;
}

bool Limit::isRegular(std::size_t patch) const {
    return patches_->GetPatchDescriptor(handles_[patch]).GetType() == Far::PatchDescriptor::REGULAR;
}

std::vector<Vec3d> Limit::patchPoints(const std::vector<Vec3d>& cagePositions) const {
    std::vector<Point> points(cageVertices_ +
                              static_cast<std::size_t>(stencils_->GetNumStencils()));
    for (std::size_t v = 0; v < cageVertices_; ++v) points[v].p = cagePositions[v];
    if (stencils_->GetNumStencils() > 0) { // none when the cage needs no refinement
        stencils_->UpdateValues(points.data(), points.data() + cageVertices_);
    }
    std::vector<Vec3d> out(points.size());
    std::transform(points.begin(), points.end(), out.begin(), [](const Point& q) { return q.p; });
    return out;
}

void Limit::basis(std::size_t patch, double s, double t, std::vector<double>& w) const {
    const auto& handle = handles_[patch];
    const Far::PatchParam param = patches_->GetPatchParam(handle);
    param.Unnormalize(s, t);
    w.resize(static_cast<std::size_t>(patches_->GetPatchVertices(handle).size()));
    patches_->EvaluateBasis(handle, s, t, w.data());
}

Vec3d Limit::evaluate(const std::vector<Vec3d>& points,
                      std::size_t patch,
                      double s,
                      double t) const {
    thread_local std::vector<double> w;
    basis(patch, s, t, w);
    const auto cvs = patches_->GetPatchVertices(handles_[patch]);
    Vec3d p{};
    for (int i = 0; i < cvs.size(); ++i) {
        p = p + points[static_cast<std::size_t>(cvs[i])] * w[static_cast<std::size_t>(i)];
    }
    return p;
}

void Limit::weights(std::size_t patch, double s, double t, std::vector<VertexWeight>& out) const {
    thread_local std::vector<double> w;
    basis(patch, s, t, w);
    out.clear();
    const auto cvs = patches_->GetPatchVertices(handles_[patch]);
    for (int i = 0; i < cvs.size(); ++i) {
        const double wi = w[static_cast<std::size_t>(i)];
        const auto cv = static_cast<std::size_t>(cvs[i]);
        if (cv < cageVertices_) {
            out.push_back({static_cast<std::uint32_t>(cv), wi});
            continue;
        }
        const auto stencil = stencils_->GetStencil(static_cast<int>(cv - cageVertices_));
        const int* idx = stencil.GetVertexIndices();
        const double* sw = stencil.GetWeights();
        for (int k = 0; k < stencil.GetSize(); ++k) {
            out.push_back({static_cast<std::uint32_t>(idx[k]), wi * sw[k]});
        }
    }
    std::sort(out.begin(), out.end(), [](const VertexWeight& a, const VertexWeight& b) {
        return a.vertex < b.vertex;
    });
    std::size_t n = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (n > 0 && out[n - 1].vertex == out[i].vertex) {
            out[n - 1].weight += out[i].weight;
        } else {
            out[n++] = out[i];
        }
    }
    out.resize(n);
}

} // namespace m2c::osd

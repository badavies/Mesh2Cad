// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/document/Document.h"

#include <algorithm>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

const char* DocumentObject::kind() const noexcept {
    if (mesh() != nullptr) return "mesh";
    if (selection() != nullptr) return "selection";
    if (quadLayout() != nullptr) return "quad_layout";
    if (solid() != nullptr) return "solid";
    if (deviation() != nullptr) return "deviation";
    if (primitive() != nullptr) return "primitive";
    if (sketch() != nullptr) return "sketch";
    return "measurement";
}

namespace {

/// Runs `change` inside its own step unless a step is already open.
template <class F>
auto inStep(Document& doc, const char* label, F&& change) {
    doc.beginStep(label);
    auto result = change();
    if (result) {
        doc.commitStep();
    } else {
        doc.abortStep();
    }
    return result;
}

} // namespace

// ---- Steps
// ---------------------------------------------------------------------------------------

void Document::beginStep(std::string label, std::string source) {
    if (depth_++ > 0) return;
    open_ = Snapshot{.objects = objects_,
                     .step = HistoryStep{.label = std::move(label), .source = std::move(source)}};
    dirty_ = false;
}

void Document::commitStep() {
    if (depth_ == 0) return;
    --depth_;
    if (depth_ > 0) return;
    if (dirty_) {
        undo_.push_back(std::move(open_));
        redo_.clear();
    }
    open_ = {};
    dirty_ = false;
}

void Document::abortStep() {
    if (depth_ == 0) return;
    --depth_;
    if (depth_ > 0) return;
    if (dirty_) {
        objects_ = std::move(open_.objects);
        ++revision_;
    }
    open_ = {};
    dirty_ = false;
}

void Document::changed() noexcept {
    dirty_ = true;
    ++revision_;
}

// ---- Changes
// -------------------------------------------------------------------------------------

Result<void> Document::checkData(const ObjectData& data) const {
    if (const auto* m = std::get_if<MeshData>(&data); m != nullptr && !m->mesh) {
        return fail(ErrorCode::InvalidArgument, "cannot add a null mesh");
    }
    if (const auto* s = std::get_if<SelectionData>(&data)) {
        if (!s->faces) return fail(ErrorCode::InvalidArgument, "cannot add a null selection");
        const auto it = objects_.find(s->meshId);
        if (it == objects_.end() || it->second.mesh() == nullptr) {
            return fail(ErrorCode::NotFound,
                        "selection refers to missing mesh " + std::to_string(s->meshId));
        }
        if (it->second.mesh()->mesh->faceCount() != s->faces->size()) {
            return fail(ErrorCode::InvalidArgument,
                        "selection size does not match the face count of mesh " +
                            std::to_string(s->meshId));
        }
    }
    if (const auto* d = std::get_if<DeviationData>(&data)) {
        const auto it = objects_.find(d->meshId);
        if (it == objects_.end() || it->second.mesh() == nullptr) {
            return fail(ErrorCode::NotFound,
                        "deviation refers to missing mesh " + std::to_string(d->meshId));
        }
        if (it->second.mesh()->mesh->vertexCount() != d->field.distances().size()) {
            return fail(ErrorCode::InvalidArgument,
                        "deviation size does not match the vertex count of mesh " +
                            std::to_string(d->meshId));
        }
        if (!objects_.contains(d->referenceId)) {
            return fail(ErrorCode::NotFound,
                        "deviation refers to missing object " + std::to_string(d->referenceId));
        }
    }
    return {};
}

Result<ObjectId>
Document::addMesh(std::string name, std::shared_ptr<const TriMesh> mesh, Provenance provenance) {
    return add(
        std::move(name), MeshData{.mesh = std::move(mesh), .alignment = {}}, std::move(provenance));
}

Result<ObjectId> Document::add(std::string name, ObjectData data, Provenance provenance) {
    if (auto ok = checkData(data); !ok) return std::unexpected(ok.error());
    return inStep(*this, "add", [&]() -> Result<ObjectId> {
        const ObjectId id = nextId_++;
        objects_.emplace(id,
                         DocumentObject{.id = id,
                                        .name = std::move(name),
                                        .visible = true,
                                        .data = std::move(data),
                                        .provenance = std::move(provenance)});
        changed();
        return id;
    });
}

Result<void> Document::replaceMesh(const ObjectId id,
                                   std::shared_ptr<const TriMesh> mesh,
                                   Provenance provenance,
                                   const std::optional<Transform3d>& alignment) {
    if (!mesh) return fail(ErrorCode::InvalidArgument, "cannot set a null mesh");
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.mesh() == nullptr) {
        return fail(ErrorCode::NotFound, "no mesh object with id " + std::to_string(id));
    }
    return inStep(*this, "replace", [&]() -> Result<void> {
        auto& [current, currentAlignment] = std::get<MeshData>(it->second.data);
        const bool sameFaces = current->sharedIndices() == mesh->sharedIndices();
        current = std::move(mesh);
        if (alignment) currentAlignment = *alignment;
        it->second.provenance = std::move(provenance);
        removeDependents(id, sameFaces ? Dependents::Deviations : Dependents::OfMesh);
        changed();
        return {};
    });
}

Result<void> Document::replaceSelection(const ObjectId id,
                                        std::shared_ptr<const MeshSelection> faces) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.selection() == nullptr) {
        return fail(ErrorCode::NotFound, "no selection object with id " + std::to_string(id));
    }
    const SelectionData updated{.meshId = it->second.selection()->meshId,
                                .faces = std::move(faces)};
    if (auto ok = checkData(updated); !ok) return ok;
    return inStep(*this, "select", [&]() -> Result<void> {
        it->second.data = updated;
        changed();
        return {};
    });
}

Result<void> Document::replaceMeasurement(const ObjectId id, const Measurement& measurement) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.measurement() == nullptr) {
        return fail(ErrorCode::NotFound, "no measurement object with id " + std::to_string(id));
    }
    return inStep(*this, "measure", [&]() -> Result<void> {
        it->second.data = MeasurementData{measurement};
        changed();
        return {};
    });
}

void Document::removeDependents(const ObjectId id, const Dependents which) {
    for (auto it = objects_.begin(); it != objects_.end();) {
        const DocumentObject& o = it->second;
        const bool isSelection = which != Dependents::Deviations && o.selection() != nullptr &&
                                 o.selection()->meshId == id;
        const bool isMeasurement = which == Dependents::All && o.measurement() != nullptr &&
                                   (o.measurement()->measurement.a.objectId == id ||
                                    o.measurement()->measurement.b.objectId == id);
        const bool isDeviation = o.deviation() != nullptr &&
                                 (o.deviation()->meshId == id || o.deviation()->referenceId == id);
        it = isSelection || isMeasurement || isDeviation ? objects_.erase(it) : std::next(it);
    }
}

Result<void> Document::remove(const ObjectId id) {
    if (!objects_.contains(id)) {
        return fail(ErrorCode::NotFound, "no object with id " + std::to_string(id));
    }
    return inStep(*this, "remove", [&]() -> Result<void> {
        objects_.erase(id);
        removeDependents(id, Dependents::All);
        changed();
        return {};
    });
}

Result<void> Document::rename(const ObjectId id, std::string name) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return fail(ErrorCode::NotFound, "no object with id " + std::to_string(id));
    }
    return inStep(*this, "rename", [&]() -> Result<void> {
        it->second.name = std::move(name);
        changed();
        return {};
    });
}

Result<void> Document::setVisible(const ObjectId id, const bool visible) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) {
        return fail(ErrorCode::NotFound, "no object with id " + std::to_string(id));
    }
    if (it->second.visible == visible) return {};
    return inStep(*this, "visibility", [&]() -> Result<void> {
        it->second.visible = visible;
        changed();
        return {};
    });
}

void Document::clear() {
    objects_.clear();
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<void> Document::load(std::vector<DocumentObject> objects, const ObjectId nextId) {
    ObjectTable table;
    for (DocumentObject& o : objects) {
        if (o.id == 0 || o.id >= nextId) {
            return fail(ErrorCode::InvalidArgument,
                        "object id " + std::to_string(o.id) + " is outside the id range");
        }
        if (const ObjectId id = o.id; !table.emplace(id, std::move(o)).second) {
            return fail(ErrorCode::InvalidArgument, "duplicate object id " + std::to_string(id));
        }
    }
    // Check selections against the loaded meshes.
    std::swap(objects_, table);
    for (const auto& o : std::views::values(objects_)) {
        if (auto ok = checkData(o.data); !ok) {
            std::swap(objects_, table);
            return ok;
        }
    }
    nextId_ = std::max<ObjectId>(nextId, 1); // 0 is never an object's id
    undo_.clear();
    redo_.clear();
    ++revision_;
    return {};
}

// ---- Queries
// -------------------------------------------------------------------------------------

std::optional<DocumentObject> Document::find(const ObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    return it->second;
}

std::optional<MeshObject> Document::findMesh(const ObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.mesh() == nullptr) return std::nullopt;
    return MeshObject{.id = id, .name = it->second.name, .mesh = it->second.mesh()->mesh};
}

std::vector<DocumentObject> Document::objects() const {
    std::vector<DocumentObject> result;
    result.reserve(objects_.size());
    for (const auto& object : std::views::values(objects_)) result.push_back(object);
    return result;
}

// ---- History
// -------------------------------------------------------------------------------------

Result<void> Document::undo() {
    if (depth_ > 0) return fail(ErrorCode::InvalidState, "cannot undo while a change is running");
    if (undo_.empty()) return fail(ErrorCode::InvalidState, "nothing to undo");
    auto& [topObjects, topStep] = undo_.back();
    redo_.push_back(Snapshot{.objects = std::move(objects_), .step = topStep});
    objects_ = std::move(topObjects);
    undo_.pop_back();
    ++revision_;
    return {};
}

Result<void> Document::redo() {
    if (depth_ > 0) return fail(ErrorCode::InvalidState, "cannot redo while a change is running");
    if (redo_.empty()) return fail(ErrorCode::InvalidState, "nothing to redo");
    auto& [topObjects, topStep] = redo_.back();
    undo_.push_back(Snapshot{.objects = std::move(objects_), .step = topStep});
    objects_ = std::move(topObjects);
    redo_.pop_back();
    ++revision_;
    return {};
}

std::vector<HistoryStep> Document::undoHistory() const {
    std::vector<HistoryStep> steps;
    for (const auto& [table, step] : undo_) steps.push_back(step);
    return steps;
}

std::vector<HistoryStep> Document::redoHistory() const {
    std::vector<HistoryStep> steps;
    for (const auto& it : std::views::reverse(redo_)) steps.push_back(it.step);
    return steps;
}

} // namespace m2c

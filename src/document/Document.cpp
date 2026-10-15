// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/document/Document.h"

#include <utility>

namespace m2c {

const char* DocumentObject::kind() const noexcept {
    if (mesh() != nullptr) return "mesh";
    if (selection() != nullptr) return "selection";
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
    open_ = Snapshot{objects_, HistoryStep{std::move(label), std::move(source)}};
    dirty_ = false;
}

void Document::commitStep() {
    if (depth_ == 0 || --depth_ > 0) return;
    if (dirty_) {
        undo_.push_back(std::move(open_));
        redo_.clear();
    }
    open_ = {};
    dirty_ = false;
}

void Document::abortStep() {
    if (depth_ == 0 || --depth_ > 0) return;
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
    return {};
}

Result<ObjectId>
Document::addMesh(std::string name, std::shared_ptr<const TriMesh> mesh, Provenance provenance) {
    return add(std::move(name), MeshData{std::move(mesh), {}}, std::move(provenance));
}

Result<ObjectId> Document::add(std::string name, ObjectData data, Provenance provenance) {
    if (auto ok = checkData(data); !ok) return std::unexpected(ok.error());
    return inStep(*this, "add", [&]() -> Result<ObjectId> {
        const ObjectId id = nextId_++;
        objects_.emplace(
            id, DocumentObject{id, std::move(name), true, std::move(data), std::move(provenance)});
        changed();
        return id;
    });
}

Result<void> Document::replaceMesh(ObjectId id,
                                   std::shared_ptr<const TriMesh> mesh,
                                   Provenance provenance,
                                   std::optional<Transform3d> alignment) {
    if (!mesh) return fail(ErrorCode::InvalidArgument, "cannot set a null mesh");
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.mesh() == nullptr) {
        return fail(ErrorCode::NotFound, "no mesh object with id " + std::to_string(id));
    }
    return inStep(*this, "replace", [&]() -> Result<void> {
        auto& data = std::get<MeshData>(it->second.data);
        const bool sameFaces = data.mesh->sharedIndices() == mesh->sharedIndices();
        data.mesh = std::move(mesh);
        if (alignment) data.alignment = *alignment;
        it->second.provenance = std::move(provenance);
        if (!sameFaces) removeDependents(id, true);
        changed();
        return {};
    });
}

Result<void> Document::replaceSelection(ObjectId id, std::shared_ptr<const MeshSelection> faces) {
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.selection() == nullptr) {
        return fail(ErrorCode::NotFound, "no selection object with id " + std::to_string(id));
    }
    const SelectionData updated{it->second.selection()->meshId, std::move(faces)};
    if (auto ok = checkData(updated); !ok) return ok;
    return inStep(*this, "select", [&]() -> Result<void> {
        it->second.data = updated;
        changed();
        return {};
    });
}

Result<void> Document::replaceMeasurement(ObjectId id, const Measurement& measurement) {
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

void Document::removeDependents(ObjectId meshId, bool onlySelections) {
    for (auto it = objects_.begin(); it != objects_.end();) {
        const DocumentObject& o = it->second;
        const bool isSelection = o.selection() != nullptr && o.selection()->meshId == meshId;
        const bool isMeasurement = !onlySelections && o.measurement() != nullptr &&
                                   (o.measurement()->measurement.a.objectId == meshId ||
                                    o.measurement()->measurement.b.objectId == meshId);
        it = (isSelection || isMeasurement) ? objects_.erase(it) : std::next(it);
    }
}

Result<void> Document::remove(ObjectId id) {
    if (!objects_.contains(id)) {
        return fail(ErrorCode::NotFound, "no object with id " + std::to_string(id));
    }
    return inStep(*this, "remove", [&]() -> Result<void> {
        objects_.erase(id);
        removeDependents(id, false);
        changed();
        return {};
    });
}

Result<void> Document::rename(ObjectId id, std::string name) {
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

Result<void> Document::setVisible(ObjectId id, bool visible) {
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

Result<void> Document::load(std::vector<DocumentObject> objects, ObjectId nextId) {
    ObjectTable table;
    for (DocumentObject& o : objects) {
        if (o.id == 0 || o.id >= nextId) {
            return fail(ErrorCode::InvalidArgument,
                        "object id " + std::to_string(o.id) + " is outside the id range");
        }
        const ObjectId id = o.id;
        if (!table.emplace(id, std::move(o)).second) {
            return fail(ErrorCode::InvalidArgument, "duplicate object id " + std::to_string(id));
        }
    }
    // Check selections against the loaded meshes.
    std::swap(objects_, table);
    for (const auto& [id, o] : objects_) {
        if (auto ok = checkData(o.data); !ok) {
            std::swap(objects_, table);
            return ok;
        }
    }
    nextId_ = nextId;
    undo_.clear();
    redo_.clear();
    ++revision_;
    return {};
}

// ---- Queries
// -------------------------------------------------------------------------------------

std::optional<DocumentObject> Document::find(ObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return std::nullopt;
    return it->second;
}

std::optional<MeshObject> Document::findMesh(ObjectId id) const {
    const auto it = objects_.find(id);
    if (it == objects_.end() || it->second.mesh() == nullptr) return std::nullopt;
    return MeshObject{id, it->second.name, it->second.mesh()->mesh};
}

std::vector<DocumentObject> Document::objects() const {
    std::vector<DocumentObject> result;
    result.reserve(objects_.size());
    for (const auto& [id, object] : objects_) result.push_back(object);
    return result;
}

// ---- History
// -------------------------------------------------------------------------------------

Result<void> Document::undo() {
    if (depth_ > 0) return fail(ErrorCode::InvalidState, "cannot undo while a change is running");
    if (undo_.empty()) return fail(ErrorCode::InvalidState, "nothing to undo");
    Snapshot& top = undo_.back();
    redo_.push_back(Snapshot{std::move(objects_), top.step});
    objects_ = std::move(top.objects);
    undo_.pop_back();
    ++revision_;
    return {};
}

Result<void> Document::redo() {
    if (depth_ > 0) return fail(ErrorCode::InvalidState, "cannot redo while a change is running");
    if (redo_.empty()) return fail(ErrorCode::InvalidState, "nothing to redo");
    Snapshot& top = redo_.back();
    undo_.push_back(Snapshot{std::move(objects_), top.step});
    objects_ = std::move(top.objects);
    redo_.pop_back();
    ++revision_;
    return {};
}

std::vector<HistoryStep> Document::undoHistory() const {
    std::vector<HistoryStep> steps;
    for (const Snapshot& s : undo_) steps.push_back(s.step);
    return steps;
}

std::vector<HistoryStep> Document::redoHistory() const {
    std::vector<HistoryStep> steps;
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) steps.push_back(it->step);
    return steps;
}

} // namespace m2c

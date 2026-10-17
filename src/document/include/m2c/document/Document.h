// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/MeshSelection.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Solid.h"
#include "m2c/core/Transform.h"
#include "m2c/core/TriMesh.h"

namespace m2c {

using ObjectId = std::uint64_t;

/// A triangle mesh. `alignment` accumulates the rigid transforms alignment commands applied, so
/// the original scan position can be recovered (ARCHITECTURE.md section 6, Align).
struct MeshData {
    std::shared_ptr<const TriMesh> mesh;
    Transform3d alignment{};
};

/// A face selection on one mesh object. It is removed automatically when that mesh is removed or
/// its faces change.
struct SelectionData {
    ObjectId meshId{0};
    std::shared_ptr<const MeshSelection> faces;
};

struct MeasurementData {
    Measurement measurement;
};

/// A quad control cage made from a mesh (ARCHITECTURE.md section 6, Quad layout). It stays when
/// that mesh changes or is removed; its provenance names the mesh.
struct QuadLayoutData {
    QuadLayout cage;
};

/// A B-rep solid built from a quad layout (ARCHITECTURE.md section 6, Surface build). Like a quad
/// layout it stays when its inputs change; its provenance names them.
struct SolidData {
    Solid solid;
};

using ObjectData =
    std::variant<MeshData, SelectionData, MeasurementData, QuadLayoutData, SolidData>;

/// Which command made an object, with what parameters (a JSON text, so this module needs no JSON
/// library) and from which inputs. A stage can be re-run from this record.
struct Provenance {
    std::string command;
    std::string params;
    std::vector<ObjectId> inputs;
};

struct DocumentObject {
    ObjectId id{0};
    std::string name;
    bool visible{true};
    ObjectData data;
    Provenance provenance;

    [[nodiscard]] const MeshData* mesh() const noexcept { return std::get_if<MeshData>(&data); }
    [[nodiscard]] const SelectionData* selection() const noexcept {
        return std::get_if<SelectionData>(&data);
    }
    [[nodiscard]] const MeasurementData* measurement() const noexcept {
        return std::get_if<MeasurementData>(&data);
    }
    [[nodiscard]] const QuadLayoutData* quadLayout() const noexcept {
        return std::get_if<QuadLayoutData>(&data);
    }
    [[nodiscard]] const SolidData* solid() const noexcept { return std::get_if<SolidData>(&data); }
    /// "mesh", "selection", "measurement", "quad_layout" or "solid".
    [[nodiscard]] const char* kind() const noexcept;
};

/// Convenience view of a mesh object.
struct MeshObject {
    ObjectId id{0};
    std::string name;
    std::shared_ptr<const TriMesh> mesh;
};

/// One entry of the undo history.
struct HistoryStep {
    std::string label;  ///< Usually the command name.
    std::string source; ///< Who made the change: "ui", "cli", "json", "mcp", ...
};

/// The objects the user is working on, with linear undo and redo (ARCHITECTURE.md section 5).
///
/// Changes are grouped into steps: beginStep() snapshots the object table, commitStep() pushes
/// the snapshot onto the undo stack if anything changed, abortStep() restores it. Steps nest; only
/// the outermost one counts. The command engine opens one step per mutating command, so every
/// command is one undo step whatever it changes. Tables hold shared pointers to immutable data, so
/// a snapshot costs one map copy, not a mesh copy. Ids are never reused, even after undo.
///
/// Depends on `core` only. Not thread-safe: front ends reach it through the command engine.
class Document {
public:
    // ---- Steps -------------------------------------------------------------------------------
    void beginStep(std::string label, std::string source = {});
    void commitStep();
    void abortStep();

    // ---- Changes (each also works outside a step, as its own step) ---------------------------
    /// Adds a mesh as a new object.
    [[nodiscard]] Result<ObjectId>
    addMesh(std::string name, std::shared_ptr<const TriMesh> mesh, Provenance provenance = {});

    /// Adds any object. Selections must name an existing mesh with the same face count.
    [[nodiscard]] Result<ObjectId>
    add(std::string name, ObjectData data, Provenance provenance = {});

    /// Replaces a mesh object's mesh (an edit in place, keeping id and name). Selections on it
    /// are dropped unless the new mesh shares the old face buffer.
    [[nodiscard]] Result<void> replaceMesh(ObjectId id,
                                           std::shared_ptr<const TriMesh> mesh,
                                           Provenance provenance,
                                           std::optional<Transform3d> alignment = std::nullopt);

    /// Replaces the faces of a selection object. The new selection must fit the same mesh.
    [[nodiscard]] Result<void> replaceSelection(ObjectId id,
                                                std::shared_ptr<const MeshSelection> faces);

    /// Replaces the value of a measurement object.
    [[nodiscard]] Result<void> replaceMeasurement(ObjectId id, const Measurement& measurement);

    /// Removes an object and every selection and measurement that refers to it.
    [[nodiscard]] Result<void> remove(ObjectId id);

    [[nodiscard]] Result<void> rename(ObjectId id, std::string name);
    [[nodiscard]] Result<void> setVisible(ObjectId id, bool visible);

    /// Removes every object and the whole history (used before loading a project).
    void clear();

    // ---- Queries -----------------------------------------------------------------------------
    [[nodiscard]] std::optional<DocumentObject> find(ObjectId id) const;
    [[nodiscard]] std::optional<MeshObject> findMesh(ObjectId id) const;

    /// All objects in id order.
    [[nodiscard]] std::vector<DocumentObject> objects() const;

    /// Increases with every change, undo and redo; front ends use it to know when to refresh.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] ObjectId nextId() const noexcept { return nextId_; }

    // ---- History -----------------------------------------------------------------------------
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] Result<void> undo();
    [[nodiscard]] Result<void> redo();

    /// Undo stack, oldest first.
    [[nodiscard]] std::vector<HistoryStep> undoHistory() const;
    /// Redo stack, next redo first.
    [[nodiscard]] std::vector<HistoryStep> redoHistory() const;

    /// Restores objects and the id counter after loading a project. Clears the history.
    [[nodiscard]] Result<void> load(std::vector<DocumentObject> objects, ObjectId nextId);

private:
    using ObjectTable = std::map<ObjectId, DocumentObject>;
    struct Snapshot {
        ObjectTable objects;
        HistoryStep step;
    };

    void changed() noexcept;
    void removeDependents(ObjectId meshId, bool onlySelections);
    [[nodiscard]] Result<void> checkData(const ObjectData& data) const;

    ObjectTable objects_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    ObjectId nextId_{1};
    std::uint64_t revision_{0};

    int depth_{0};
    bool dirty_{false};
    Snapshot open_;
};

} // namespace m2c

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QWidget>
#include <cstddef>
#include <memory>

#include "m2c/core/TriMesh.h"

#include "Engine.h"

class QLabel;

namespace m2c::app {

/// The Workflow panel: the pipeline's stages in order (ARCHITECTURE.md section 6), as four
/// tracks that fold away (prepare and align; organic surfacing; prismatic parts and features;
/// verify and export), each step opening its tool. Above them, the stage gate of the current
/// object (section 6, stage contracts): whether a mesh is closed, manifold and one shell, a
/// solid one valid closed solid, a deviation within its tolerance, and what to do if not.
///
/// The gate is read from the snapshot and from read-only commands (mesh.info, shells.detect)
/// run in the background once per mesh version; the panel computes no geometry itself.
class WorkflowPanel : public QWidget {
    Q_OBJECT

public:
    explicit WorkflowPanel(Engine& engine, QWidget* parent = nullptr);

    /// Shows the gate of `id` (a selection or measurement shows its mesh's).
    void setCurrent(const std::shared_ptr<const DocSnapshot>& snapshot, ObjectId id);
    /// The gate as shown, for tests: the text and whether it passes.
    [[nodiscard]] QString gateText() const;
    [[nodiscard]] bool gatePassed() const noexcept { return passed_; }

signals:
    /// A step was clicked: the command whose tool to open.
    void toolRequested(const QString& command);

private:
    void showGate(const QString& text, bool passed);
    void checkMesh(const DocumentObject& mesh);

    Engine& engine_;
    std::shared_ptr<const DocSnapshot> snapshot_;
    QLabel* gate_{nullptr};
    bool passed_{false};
    ObjectId current_{0};
    /// The mesh version last checked, and what mesh.info and shells.detect said of it.
    std::weak_ptr<const TriMesh> checked_;
    Json stats_;
    std::size_t shells_{0};
    bool checking_{false};
};

} // namespace m2c::app

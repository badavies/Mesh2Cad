// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <memory>

#include "Engine.h"
#include "Viewport.h"

class QActionGroup;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace m2c::app {

class ParamForm;

/// The desktop app's window: viewport, object list, tool form, workflow, history and log.
/// Every action becomes a command sent through the Engine, the same way scripts and agents work.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Engine& engine, QWidget* parent = nullptr);

    /// Imports meshes or opens a project, by file extension (command line and drag and drop).
    void openFiles(const QStringList& files);

    [[nodiscard]] Viewport* viewport() const noexcept { return viewport_; }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();
    void buildDocks();
    void buildToolbar();
    void onDocumentChanged(std::shared_ptr<const DocSnapshot> snapshot);
    void refreshTree();
    void onTreeSelection();
    void showTool(const QString& name) const;
    void runForm();
    void run(const QString& command, const Json& params, Engine::Done done = {}) const;

    // Interaction results from the viewport.
    void onBrushStroke(ObjectId mesh, const std::vector<Vec3d>& centers, bool subtract, bool add);
    void onLasso(const QPolygonF& ndc, bool subtract, bool add);
    void onFaceClicked(Hit hit, bool subtract, bool add);
    void onMeasured(Hit a, Hit b);
    /// Where a selection tool's result goes: into the current selection of that mesh (replace,
    /// add with Shift, subtract with Ctrl) or a new selection.
    [[nodiscard]] Json selectionTarget(ObjectId mesh, bool subtract, bool add) const;
    void adoptSelection(const Result<Json>& result);

    void newProject();
    void openProject();
    bool saveProject(bool saveAs);
    void importMeshes();
    void exportMesh();

    [[nodiscard]] ObjectId currentMesh() const;
    [[nodiscard]] ObjectId currentSelection() const;
    [[nodiscard]] Json formDefaults(const Command& command) const;

    Engine& engine_;
    std::shared_ptr<const DocSnapshot> snapshot_;
    Viewport* viewport_{nullptr};
    QTreeWidget* tree_{nullptr};
    ParamForm* form_{nullptr};
    QListWidget* history_{nullptr};
    QPlainTextEdit* log_{nullptr};
    QLabel* status_{nullptr};
    QLabel* hover_{nullptr};
    QDoubleSpinBox* brushRadius_{nullptr};
    QDoubleSpinBox* creaseAngle_{nullptr};
    QAction* undo_{nullptr};
    QAction* redo_{nullptr};
    QActionGroup* modes_{nullptr};
    QString projectPath_;
    std::uint64_t savedRevision_{0};
    ObjectId selected_{0}; ///< Object picked in the tree.
    bool updatingTree_{false};
    bool brushSized_{false}; ///< The brush follows the first mesh's size until the user sets it.
};

} // namespace m2c::app

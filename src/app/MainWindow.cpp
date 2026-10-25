// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QModelIndex>
#include <QObject>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QString>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <Qt>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <QLatin1String>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Vec3.h"
#include "m2c/document/Document.h"
#include "m2c/rpc/Session.h"

#include "Engine.h"
#include "InspectorPanel.h"
#include "ParamForm.h"
#include "ToolCatalog.h"
#include "Viewport.h"
#include "WorkflowPanel.h"

namespace m2c::app {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

QString meshFilter() {
    return QObject::tr("Meshes (*.stl *.obj *.ply);;All files (*)");
}

QString projectFilter() {
    return QObject::tr("Mesh2Cad project (*.m2c)");
}

Json toJson(const Vec3d& v) {
    return Json::array({v.x, v.y, v.z});
}

/// A column of the Objects list that is not edited in place.
class ReadOnlyDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget*
    createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const override {
        return nullptr;
    }
};

/// The user guide's first page: installed beside the program (doc/user on Windows,
/// share/mesh2cad/user on Linux), else in the source tree of a build that isn't installed.
QString userGuideIndex() {
    for (const QDir bin(QCoreApplication::applicationDirPath());
         const QString& dir : {bin.filePath(QStringLiteral("../doc/user")),
                               bin.filePath(QStringLiteral("../share/mesh2cad/user")),
                               QStringLiteral(MESH2CAD_SOURCE_DIR "/docs/user")}) {
        if (const QFileInfo index(QDir(dir).filePath(QStringLiteral("README.md"))); index.isFile())
            return index.canonicalFilePath();
    }
    return {};
}

} // namespace

MainWindow::MainWindow(Engine& engine, QWidget* parent) : QMainWindow(parent), engine_(engine) {
    setWindowTitle(tr("Mesh2Cad"));
    setAcceptDrops(true);
    viewport_ = new Viewport;
    setCentralWidget(viewport_);
    buildDocks();
    buildToolbar();
    buildMenus();

    status_ = new QLabel;
    hover_ = new QLabel;
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->setObjectName("cancelCommand");
    cancel_->setToolTip(tr("Stop the running command; the document stays as it was"));
    cancel_->hide();
    connect(cancel_, &QPushButton::clicked, &engine_, &Engine::cancel);
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(cancel_);
    statusBar()->addPermanentWidget(hover_);

    connect(&engine_, &Engine::documentChanged, this, &MainWindow::onDocumentChanged);
    connect(&engine_, &Engine::logLine, this, [this](const QString& line, const bool error) {
        if (error) {
            log_->appendHtml(
                QStringLiteral("<span style='color:#e06c6c'>%1</span>").arg(line.toHtmlEscaped()));
        } else {
            log_->appendPlainText(line);
        }
    });
    connect(&engine_, &Engine::progressChanged, this, [this](const QString& text) {
        status_->setText(tr("Running %1…").arg(text));
    });
    connect(&engine_, &Engine::busyChanged, this, [this](const bool busy, const QString& command) {
        form_->setBusy(busy);
        cancel_->setVisible(busy);
        status_->setText(busy ? tr("Running %1…").arg(command) : QString());
        if (busy) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
        } else {
            while (QApplication::overrideCursor() != nullptr) QApplication::restoreOverrideCursor();
        }
    });
    connect(&engine_, &Engine::clientsChanged, this, [this](const int count) {
        log_->appendPlainText(count > 0 ? tr("%n agent connection(s) open", nullptr, count)
                                        : tr("No agent connections"));
    });

    connect(viewport_, &Viewport::brushStroke, this, &MainWindow::onBrushStroke);
    connect(viewport_, &Viewport::lassoDrawn, this, &MainWindow::onLasso);
    connect(viewport_, &Viewport::faceClicked, this, &MainWindow::onFaceClicked);
    connect(viewport_, &Viewport::objectClicked, this, &MainWindow::onObjectClicked);
    connect(viewport_, &Viewport::measured, this, &MainWindow::onMeasured);
    connect(viewport_, &Viewport::hoverChanged, hover_, &QLabel::setText);
    connect(viewport_, &Viewport::brushRadiusChanged, brushRadius_, &QDoubleSpinBox::setValue);
    connect(viewport_, &Viewport::glUnavailable, this, [this](const QString& reason) {
        emit engine_.logLine(reason, true);
        statusBar()->showMessage(reason);
    });

    onDocumentChanged(engine_.snapshot());
    resize(1400, 900);
}

// ---- Layout -----------------------------------------------------------------------------------

void MainWindow::buildDocks() {
    setDockOptions(AnimatedDocks | AllowTabbedDocks);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Object"), tr("Info"), tr("Id")});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // The id shows first, so it can be read without hovering; the name column keeps the tree
    // lines and the visibility box. Only names are edited in place.
    tree_->header()->moveSection(2, 0);
    tree_->setItemDelegateForColumn(2, new ReadOnlyDelegate(tree_));
    tree_->setItemDelegateForColumn(1, new ReadOnlyDelegate(tree_));
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onTreeSelection);
    connect(tree_,
            &QTreeWidget::itemChanged,
            this,
            [this](const QTreeWidgetItem* item, const int column) {
                if (updatingTree_ || column != 0 || !snapshot_) return;
                const auto id = item->data(0, kIdRole).value<ObjectId>();
                const DocumentObject* o = snapshot_->find(id);
                if (o == nullptr) return;
                // Toggling a selected row shows or hides every selected row, as one step.
                if (const bool visible = item->checkState(0) == Qt::Checked; visible != o->visible)
                    run("object.show", {{"ids", rowsFor(id)}, {"visible", visible}});
                if (const std::string name = item->text(0).trimmed().toStdString();
                    !name.empty() && name != o->name)
                    run("object.rename", {{"id", id}, {"name", name}});
            });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](const QTreeWidgetItem* item, int) {
        viewport_->fitObject(item->data(0, kIdRole).value<ObjectId>());
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QTreeWidgetItem* item = tree_->itemAt(pos);
        if (item == nullptr) return;
        QMenu menu;
        fillObjectMenu(menu, item->data(0, kIdRole).value<ObjectId>());
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });
    auto* objects = new QDockWidget(tr("Objects"), this);
    objects->setObjectName("objects");
    objects->setWidget(tree_);
    addDockWidget(Qt::LeftDockWidgetArea, objects);

    // Workflow: the pipeline's tracks in order, each step opening its tool, under the current
    // object's stage gate.
    workflow_ = new WorkflowPanel(engine_);
    connect(workflow_, &WorkflowPanel::toolRequested, this, [this](const QString& command) {
        if (command == QLatin1String("mesh.import")) {
            importMeshes();
        } else if (command == QLatin1String("project.save")) {
            saveProject(false);
        } else {
            showTool(command);
        }
    });
    auto* workflowDock = new QDockWidget(tr("Workflow"), this);
    workflowDock->setObjectName("workflow");
    workflowDock->setWidget(workflow_);
    addDockWidget(Qt::LeftDockWidgetArea, workflowDock);

    form_ = new ParamForm;
    connect(form_, &ParamForm::runRequested, this, &MainWindow::runForm);
    connect(form_, &ParamForm::objectChosen, this, &MainWindow::chooseObject);
    connect(form_, &ParamForm::pointsChanged, viewport_, &Viewport::setMarkedPoints);
    connect(form_, &ParamForm::linesChanged, viewport_, &Viewport::setMarkedLines);
    connect(form_, &ParamForm::valuesChanged, this, &MainWindow::previewSection);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(form_);
    auto* tool = new QDockWidget(tr("Tool"), this);
    tool->setObjectName("tool");
    tool->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, tool);

    history_ = new QListWidget;
    history_->setToolTip(tr("Double-click a step to undo or redo to just after it"));
    // Each row holds how many steps away it is: negative to undo, positive to redo.
    connect(history_, &QListWidget::itemActivated, this, [this](const QListWidgetItem* item) {
        if (const int steps = item->data(Qt::UserRole).toInt(); steps != 0) stepHistory(steps);
    });
    auto* historyDock = new QDockWidget(tr("History"), this);
    historyDock->setObjectName("history");
    historyDock->setWidget(history_);
    addDockWidget(Qt::RightDockWidgetArea, historyDock);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    auto* logDock = new QDockWidget(tr("Log"), this);
    logDock->setObjectName("log");
    logDock->setWidget(log_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    tabifyDockWidget(historyDock, logDock);

    // Inspector: Info and the read-only tools' results, a row each, beside History and the Log.
    inspector_ = new InspectorPanel;
    connect(inspector_, &InspectorPanel::boundsClicked, viewport_, &Viewport::fitBox);
    connect(inspector_, &InspectorPanel::commandRequested, this, &MainWindow::runInspectorAction);
    auto* inspectorDock = new QDockWidget(tr("Inspector"), this);
    inspectorDock->setObjectName("inspector");
    inspectorDock->setWidget(inspector_);
    addDockWidget(Qt::BottomDockWidgetArea, inspectorDock);
    tabifyDockWidget(logDock, inspectorDock);
    tool->raise();
}

void MainWindow::fillObjectMenu(QMenu& menu, const ObjectId id) const {
    const DocumentObject* o = snapshot_ ? snapshot_->find(id) : nullptr;
    // The tools that take an object of this kind, then a rerun of the command that made it.
    if (o != nullptr) {
        for (const std::string& command : objectTools(o->kind())) {
            if (engine_.registry().find(command) == nullptr) continue;
            const QString name = QString::fromStdString(command);
            QAction* a = menu.addAction(toolInfo(command).title + QStringLiteral("…"),
                                        this,
                                        [this, name, id] { showTool(name, id); });
            a->setObjectName(name);
        }
        if (const Command* made = engine_.registry().find(o->provenance.command);
            made != nullptr && made->mutatesDocument && !o->provenance.params.empty()) {
            if (!menu.isEmpty()) menu.addSeparator();
            menu.addAction(tr("Change and run %1 again…").arg(toolInfo(made->name).title),
                           this,
                           [this, id] { showMadeWith(id); })
                ->setObjectName(QStringLiteral("rerun"));
        }
        if (!menu.isEmpty()) menu.addSeparator();
    }
    menu.addAction(tr("Fit view"), this, [this, id] { viewport_->fitObject(id); });
    // An edit (an agent's, say) can rebuild the tree while the menu is open, deleting the
    // row, so find the object's row again.
    menu.addAction(tr("Rename"), this, [this, id] {
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
            if ((*it)->data(0, kIdRole).value<ObjectId>() == id) {
                tree_->editItem(*it, 0);
                return;
            }
        }
    });
    if (o != nullptr && infoCommand(*o) != nullptr)
        menu.addAction(tr("Info"), this, [this, id] { showInfo(id); });
    menu.addSeparator();
    const std::vector<ObjectId> rows = rowsFor(id);
    menu.addAction(rows.size() > 1 ? tr("Delete %1 objects").arg(rows.size()) : tr("Delete"),
                   this,
                   [this, rows] { run("object.delete", {{"ids", rows}}); });
}

void MainWindow::buildToolbar() {
    QToolBar* bar = addToolBar(tr("Tools"));
    bar->setObjectName("tools");
    modes_ = new QActionGroup(this);
    struct ModeAction {
        QString text;
        Mode mode;
        QString key;
        QString tip;
    };
    const std::array<ModeAction, 6> modes{{
        {.text = tr("Navigate"),
         .mode = Mode::Navigate,
         .key = "1",
         .tip =
             tr("Left drag orbits, Shift or middle drag pans, wheel zooms, double-click sets the "
                "centre")},
        {.text = tr("Brush"),
         .mode = Mode::Brush,
         .key = "2",
         .tip = tr("Paint faces. Ctrl removes. [ and ] change the size. Right drag, or a drag "
                   "off the mesh, orbits")},
        {.text = tr("Lasso"),
         .mode = Mode::Lasso,
         .key = "3",
         .tip = tr("Draw around faces. Shift adds, Ctrl removes")},
        {.text = tr("Crease"),
         .mode = Mode::Crease,
         .key = "4",
         .tip = tr("Click a face to select the region up to sharp edges. Ctrl removes. Right "
                   "drag, or a drag off the mesh, orbits")},
        {.text = tr("Measure"),
         .mode = Mode::Measure,
         .key = "5",
         .tip = tr("Click two points to measure their distance. Right drag, or a drag off the "
                   "mesh, orbits")},
        {.text = tr("Shell"),
         .mode = Mode::Shell,
         .key = "6",
         .tip = tr("Click a face to select the whole connected part it is on. Ctrl removes. "
                   "Right drag, or a drag off the mesh, orbits")},
    }};
    for (const auto& [text, mode, key, tip] : modes) {
        QAction* a = bar->addAction(text);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(key));
        a->setToolTip(tip + QStringLiteral(" (%1)").arg(key));
        a->setChecked(mode == Mode::Navigate);
        modes_->addAction(a);
        connect(a, &QAction::triggered, this, [this, m = mode] { viewport_->setMode(m); });
    }
    bar->addSeparator();
    bar->addWidget(new QLabel(tr(" Brush ")));
    brushRadius_ = new QDoubleSpinBox;
    brushRadius_->setRange(0.001, 1e6);
    brushRadius_->setDecimals(3);
    brushRadius_->setSuffix(tr(" mm"));
    brushRadius_->setValue(viewport_->brushRadius());
    connect(brushRadius_, &QDoubleSpinBox::valueChanged, viewport_, &Viewport::setBrushRadius);
    connect(brushRadius_, &QDoubleSpinBox::editingFinished, this, [this] { brushSized_ = true; });
    bar->addWidget(brushRadius_);
    bar->addWidget(new QLabel(tr("  Crease ")));
    creaseAngle_ = new QDoubleSpinBox;
    creaseAngle_->setRange(0.5, 180.0);
    creaseAngle_->setSuffix(QStringLiteral("°"));
    creaseAngle_->setValue(viewport_->creaseAngle());
    connect(creaseAngle_, &QDoubleSpinBox::valueChanged, viewport_, &Viewport::setCreaseAngle);
    bar->addWidget(creaseAngle_);
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New project"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("&Open project…"), QKeySequence::Open, this, &MainWindow::openProject);
    file->addAction(tr("&Save project"), QKeySequence::Save, this, [this] { saveProject(false); });
    file->addAction(
        tr("Save project &as…"), QKeySequence::SaveAs, this, [this] { saveProject(true); });
    file->addSeparator();
    file->addAction(
        tr("&Import mesh…"), QKeySequence(tr("Ctrl+I")), this, &MainWindow::importMeshes);
    file->addAction(tr("&Export mesh…"), QKeySequence(tr("Ctrl+E")), this, &MainWindow::exportMesh);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undo_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { run("doc.undo", {}); });
    redo_ = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, [this] { run("doc.redo", {}); });
    edit->addSeparator();
    edit->addAction(tr("Select &all faces"), QKeySequence(tr("Ctrl+A")), this, [this] {
        if (const ObjectId mesh = currentMesh()) {
            Json p = selectionTarget(mesh, false, false);
            p["id"] = mesh;
            run("select.all", p, [this](const Result<Json>& r) { adoptSelection(r); });
        }
    });
    const auto onSelection = [this](const char* command, const Json& extra = Json::object()) {
        return [this, command, extra] {
            if (const ObjectId sel = currentSelection()) {
                Json p = extra;
                p["selection_id"] = sel;
                run(command, p);
            } else {
                status_->setText(tr("Pick a selection in the Objects list first"));
            }
        };
    };
    edit->addAction(tr("&Invert selection"),
                    QKeySequence(tr("Ctrl+Shift+I")),
                    this,
                    onSelection("select.invert"));
    edit->addAction(
        tr("&Grow selection"), QKeySequence(tr("Ctrl+=")), this, onSelection("select.grow"));
    edit->addAction(
        tr("S&hrink selection"), QKeySequence(tr("Ctrl+-")), this, onSelection("select.shrink"));
    edit->addAction(tr("&Delete selected faces"),
                    QKeySequence(Qt::Key_Delete),
                    this,
                    onSelection("faces.delete"));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("&Fit all"), QKeySequence(tr("F")), viewport_, &Viewport::fitAll);
    view->addSeparator();
    view->addAction(tr("Front"), QKeySequence(tr("Ctrl+1")), this, [this] {
        viewport_->setStandardView(-90, 0);
    });
    view->addAction(tr("Right"), QKeySequence(tr("Ctrl+3")), this, [this] {
        viewport_->setStandardView(0, 0);
    });
    view->addAction(tr("Top"), QKeySequence(tr("Ctrl+7")), this, [this] {
        viewport_->setStandardView(-90, 89.9);
    });
    view->addAction(tr("Isometric"), QKeySequence(tr("Ctrl+0")), this, [this] {
        viewport_->setStandardView(-45, 35.264);
    });
    view->addSeparator();
    QAction* wire = view->addAction(tr("&Wireframe"));
    wire->setCheckable(true);
    wire->setShortcut(QKeySequence(tr("W")));
    connect(wire, &QAction::toggled, viewport_, &Viewport::setWireframe);
    QAction* back = view->addAction(tr("Show &back faces in red"));
    back->setCheckable(true);
    back->setChecked(true);
    connect(back, &QAction::toggled, viewport_, &Viewport::setShowBackFaces);
    QAction* palette = view->addAction(tr("&Colour-blind-safe deviation colours"));
    palette->setCheckable(true);
    connect(palette, &QAction::toggled, viewport_, &Viewport::setColorBlindPalette);
    view->addSeparator();
    for (const QDockWidget* dock : findChildren<QDockWidget*>())
        view->addAction(dock->toggleViewAction());

    // Tools: every command by the task it serves, titled for people with the command's name
    // beside it, each opening its generated form.
    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    std::map<QString, QMenu*> groups;
    for (const QString& group : toolGroups()) groups[group] = tools->addMenu(group);
    std::vector<const Command*> commands = engine_.registry().list();
    std::ranges::stable_sort(
        commands, std::ranges::less{}, [](const Command* c) { return toolInfo(c->name).order; });
    for (const Command* c : commands) {
        const auto [title, group, order] = toolInfo(c->name);
        if (group.isEmpty()) continue;
        QMenu*& menu = groups[group];
        if (menu == nullptr) menu = tools->addMenu(group);
        const QString name = QString::fromStdString(c->name);
        QAction* a = menu->addAction(title + QStringLiteral("…\t") + name);
        a->setObjectName(name);
        a->setToolTip(QString::fromStdString(c->summary));
        a->setStatusTip(QString::fromStdString(c->summary));
        connect(a, &QAction::triggered, this, [this, name] { showTool(name); });
    }
    for (QMenu* menu : std::views::values(groups)) {
        if (menu->isEmpty()) menu->menuAction()->setVisible(false);
    }

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* guideAction = help->addAction(tr("User guide"), this, [this] {
        auto* guide = findChild<QTextBrowser*>(QStringLiteral("userGuide"));
        if (guide == nullptr) {
            const QString index = userGuideIndex();
            if (index.isEmpty()) {
                QDesktopServices::openUrl(QUrl(QStringLiteral(
                    "https://github.com/badavies/Mesh2Cad/blob/main/docs/user/README.md")));
                return;
            }
            // Its own window, which shows the Markdown pages and follows the links between them.
            guide = new QTextBrowser(this);
            guide->setObjectName(QStringLiteral("userGuide"));
            guide->setWindowFlag(Qt::Window);
            guide->setWindowTitle(tr("Mesh2Cad user guide"));
            guide->setOpenExternalLinks(true);
            guide->setSource(QUrl::fromLocalFile(index));
            guide->resize(900, 700);
        }
        guide->show();
        guide->raise();
        guide->activateWindow();
    });
    guideAction->setShortcut(QKeySequence::HelpContents);
    help->addAction(tr("Connect an AI agent…"), this, [this] {
        QMessageBox::information(
            this,
            tr("Connect an AI agent"),
            tr("<p>While Mesh2Cad is open, scripts and AI agents can drive it through "
               "<b>mesh2cad-mcp</b> (for Claude Desktop and other MCP clients) or any JSON-RPC "
               "client on the local socket.</p>"
               "<p>Session file: <code>%1</code><br>Session log: <code>%2</code></p>"
               "<p>Changes made by an agent appear in the History panel with their source, "
               "and Edit &gt; Undo reverts them.</p>")
                .arg(QString::fromStdString(defaultSessionFile().string()).toHtmlEscaped(),
                     engine_.sessionLogPath().toHtmlEscaped()));
    });
    help->addAction(tr("About Mesh2Cad"), this, [this] {
        QMessageBox::about(this,
                           tr("About Mesh2Cad"),
                           tr("<b>Mesh2Cad %1</b><p>Scan to CAD. GPL-3.0-or-later.</p>")
                               .arg(QStringLiteral(MESH2CAD_VERSION)));
    });
}

// ---- Document ---------------------------------------------------------------------------------

void MainWindow::onDocumentChanged(std::shared_ptr<const DocSnapshot> snapshot) {
    snapshot_ = std::move(snapshot);
    if (selected_ != 0 && snapshot_->find(selected_) == nullptr) selected_ = 0;
    viewport_->setSnapshot(snapshot_);
    if (!brushSized_) {
        for (const DocumentObject& o : snapshot_->objects) {
            if (o.mesh() == nullptr) continue;
            const auto& [min, max] = o.mesh()->mesh->bounds();
            brushRadius_->setValue(length(max - min) * 0.02);
            brushSized_ = true;
            break;
        }
    }
    showCurrent();
    refreshTree();
    form_->setObjects(snapshot_);
    previewSection(); // a plane primitive it cuts with may have changed

    history_->clear();
    // A first row for the state before every step, so a double-click can undo them all.
    const auto done = static_cast<int>(snapshot_->undo.size());
    auto* start = new QListWidgetItem(tr("(start)"));
    start->setData(Qt::UserRole, -done);
    history_->addItem(start);
    for (int i = 0; i < done; ++i) {
        const auto& [label, source] = snapshot_->undo[static_cast<std::size_t>(i)];
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   [%2]")
                .arg(QString::fromStdString(label), QString::fromStdString(source)));
        item->setData(Qt::UserRole, i + 1 - done);
        history_->addItem(item);
    }
    for (std::size_t i = 0; i < snapshot_->redo.size(); ++i) {
        const auto& [label, source] = snapshot_->redo[i];
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   [%2]  (undone)")
                .arg(QString::fromStdString(label), QString::fromStdString(source)));
        item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        item->setData(Qt::UserRole, static_cast<int>(i) + 1);
        history_->addItem(item);
    }
    history_->setCurrentRow(done);
    undo_->setEnabled(!snapshot_->undo.empty());
    redo_->setEnabled(!snapshot_->redo.empty());
    undo_->setText(snapshot_->undo.empty()
                       ? tr("&Undo")
                       : tr("&Undo %1").arg(QString::fromStdString(snapshot_->undo.back().label)));
    redo_->setText(snapshot_->redo.empty()
                       ? tr("&Redo")
                       : tr("&Redo %1").arg(QString::fromStdString(snapshot_->redo.front().label)));

    const QString name =
        projectPath_.isEmpty() ? tr("Untitled") : QFileInfo(projectPath_).fileName();
    setWindowTitle(QStringLiteral("%1%2 - Mesh2Cad")
                       .arg(name, snapshot_->revision != savedRevision_ ? "*" : ""));
}

void MainWindow::refreshTree() {
    updatingTree_ = true;
    tree_->clear();
    std::map<ObjectId, QTreeWidgetItem*> meshes;
    const auto makeItem = [](const DocumentObject& o, const QString& info) {
        auto* item = new QTreeWidgetItem(
            {QString::fromStdString(o.name), info, QStringLiteral("#%1").arg(o.id)});
        item->setData(0, kIdRole, QVariant::fromValue(o.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setCheckState(0, o.visible ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(0,
                         QStringLiteral("#%1 %2, made by %3")
                             .arg(o.id)
                             .arg(QString::fromLatin1(o.kind()),
                                  o.provenance.command.empty()
                                      ? QObject::tr("import")
                                      : QString::fromStdString(o.provenance.command)));
        return item;
    };
    for (const DocumentObject& o : snapshot_->objects) {
        if (const MeshData* m = o.mesh()) {
            auto* item = makeItem(o, tr("%L1 faces").arg(m->mesh->faceCount()));
            tree_->addTopLevelItem(item);
            meshes[o.id] = item;
        } else if (const QuadLayoutData* q = o.quadLayout()) {
            tree_->addTopLevelItem(makeItem(o, tr("%L1 quads").arg(q->cage.faceCount())));
        } else if (const SolidData* d = o.solid()) {
            tree_->addTopLevelItem(makeItem(o, tr("%L1 faces").arg(d->solid.summary().faceCount)));
        } else if (const PrimitiveData* p = o.primitive()) {
            tree_->addTopLevelItem(makeItem(o, tr("P95 %1 mm").arg(p->fit.p95Mm, 0, 'f', 3)));
        }
    }
    for (const DocumentObject& o : snapshot_->objects) {
        QTreeWidgetItem* item = nullptr;
        ObjectId parent = 0;
        if (const SelectionData* s = o.selection()) {
            item = makeItem(o, tr("%L1 faces").arg(s->faces->count()));
            parent = s->meshId;
        } else if (const MeasurementData* m = o.measurement()) {
            item = makeItem(o, QStringLiteral("%1 mm").arg(m->measurement.distance(), 0, 'f', 3));
            parent = m->measurement.a.objectId;
        } else if (const DeviationData* d = o.deviation()) {
            item = makeItem(o, tr("RMS %1 mm").arg(d->field.stats().rmsMm, 0, 'f', 3));
            parent = d->meshId;
        } else if (const SketchData* k = o.sketch()) {
            const auto loops = static_cast<int>(k->sketch.loops.size());
            item = makeItem(o, loops == 1 ? tr("1 loop") : tr("%1 loops").arg(loops));
            parent = o.provenance.inputs.empty() ? 0 : o.provenance.inputs.front();
        } else {
            continue;
        }
        if (const auto it = meshes.find(parent); it != meshes.end()) {
            it->second->addChild(item);
            it->second->setExpanded(true);
        } else {
            tree_->addTopLevelItem(item);
        }
    }
    // Select the rows selected before, the first of them current; selected_ may have been set
    // on its own (a new selection adopted from a tool).
    std::erase_if(selectedIds_,
                  [this](const ObjectId id) { return snapshot_->find(id) == nullptr; });
    if (selected_ != 0 && (selectedIds_.empty() || selectedIds_.front() != selected_))
        selectedIds_.assign(1, selected_);
    if (selected_ == 0) selectedIds_.clear();
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if (const auto id = (*it)->data(0, kIdRole).value<ObjectId>(); id == selected_)
            tree_->setCurrentItem(*it);
    }
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if (std::ranges::find(selectedIds_, (*it)->data(0, kIdRole).value<ObjectId>()) !=
            selectedIds_.end())
            (*it)->setSelected(true);
    }
    updatingTree_ = false;
}

void MainWindow::onTreeSelection() {
    if (updatingTree_) return;
    // Keep the order rows were selected in: those still selected, then the new ones.
    std::vector<ObjectId> now;
    for (const QTreeWidgetItem* item : tree_->selectedItems())
        now.push_back(item->data(0, kIdRole).value<ObjectId>());
    std::erase_if(selectedIds_,
                  [&](const ObjectId id) { return std::ranges::find(now, id) == now.end(); });
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if (const auto id = (*it)->data(0, kIdRole).value<ObjectId>();
            (*it)->isSelected() && std::ranges::find(selectedIds_, id) == selectedIds_.end())
            selectedIds_.push_back(id);
    }
    const ObjectId before = selected_;
    selected_ = selectedIds_.empty() ? 0 : selectedIds_.front();
    form_->setSelectedObjects(selectedIds_);
    showCurrent();
    // A deviation picked in the list shows its statistics and hotspots.
    if (const DocumentObject* o = snapshot_ ? snapshot_->find(selected_) : nullptr;
        o != nullptr && o->deviation() != nullptr && selected_ != before)
        showInfo(selected_, true);
}

std::vector<ObjectId> MainWindow::rowsFor(const ObjectId id) const {
    if (std::ranges::find(selectedIds_, id) == selectedIds_.end()) return {id};
    return selectedIds_;
}

void MainWindow::showCurrent() const {
    viewport_->setCurrent(currentMesh(), currentSelection(), selected_);
    workflow_->setCurrent(snapshot_, selected_ != 0 ? selected_ : currentMesh());
}

void MainWindow::chooseObject(const ObjectId id) {
    selected_ = id;
    selectedIds_.assign(1, id);
    refreshTree();
    showCurrent();
}

ObjectId MainWindow::currentMesh() const {
    if (!snapshot_) return 0;
    if (const DocumentObject* o = snapshot_->find(selected_)) {
        if (o->mesh() != nullptr) return o->id;
        if (const SelectionData* s = o->selection()) return s->meshId;
        if (const DeviationData* d = o->deviation()) return d->meshId;
        if (const MeasurementData* m = o->measurement()) {
            if (m->measurement.a.objectId != 0) return m->measurement.a.objectId;
        }
    }
    // Otherwise the newest visible mesh.
    for (const DocumentObject& o : std::views::reverse(snapshot_->objects)) {
        if (o.mesh() != nullptr && o.visible) return o.id;
    }
    return 0;
}

ObjectId MainWindow::currentSelection() const {
    if (!snapshot_) return 0;
    const DocumentObject* o = snapshot_->find(selected_);
    return o != nullptr && o->selection() != nullptr ? o->id : 0;
}

// ---- Commands ---------------------------------------------------------------------------------

const char* MainWindow::infoCommand(const DocumentObject& o) {
    return o.mesh() != nullptr        ? "mesh.info"
           : o.solid() != nullptr     ? "solid.info"
           : o.deviation() != nullptr ? "deviation.info"
                                      : nullptr;
}

void MainWindow::showInfo(const ObjectId id, const bool background) const {
    const DocumentObject* o = snapshot_ ? snapshot_->find(id) : nullptr;
    const char* info = o != nullptr ? infoCommand(*o) : nullptr;
    if (info == nullptr) return;
    engine_.run(
        info,
        {{"id", id}},
        [this, info](const Result<Json>& r) {
            if (r) inspect(QString::fromLatin1(info), *r);
        },
        background);
}

void MainWindow::inspect(const QString& command, const Json& result) const {
    inspector_->showResult(command, result);
    if (auto* dock = findChild<QDockWidget*>("inspector")) {
        dock->show();
        dock->raise();
    }
}

void MainWindow::runInspectorAction(const QString& command, const Json& params) {
    const ObjectId mesh = params.value("id", ObjectId{0});
    run(command, params, [this, command, mesh](const Result<Json>& r) {
        if (!r) {
            QMessageBox::warning(this, command, QString::fromStdString(r.error().message));
            return;
        }
        adoptSelection(r);
        if (command == "deviation.compute") {
            // The new deviation, with its hotspots now selections.
            showInfo(r->value("id", ObjectId{0}));
        } else if (command != "shells.select") {
            // Keeping or splitting renumbers the shells, so list them again.
            run("shells.detect", {{"id", mesh}}, [this](const Result<Json>& shells) {
                if (shells) inspect(QStringLiteral("shells.detect"), *shells);
            });
            viewport_->update();
        }
    });
}

void MainWindow::run(const QString& command, const Json& params, Engine::Done done) const {
    engine_.run(command, params, std::move(done));
}

Json MainWindow::formDefaults(const Command& command) const {
    Json values = Json::object();
    const Json properties = command.paramsSchema.value("properties", Json::object());
    std::vector<std::string> required;
    if (command.paramsSchema.contains("required")) {
        required = command.paramsSchema["required"].get<std::vector<std::string>>();
    }
    const auto isRequired = [&](const std::string& k) {
        return std::ranges::find(required, k) != required.end();
    };
    // The selected object of a kind, else the newest one.
    const auto pick = [this](const std::string& kind) -> ObjectId {
        if (!snapshot_) return 0;
        if (const DocumentObject* o = snapshot_->find(selected_); o != nullptr && o->kind() == kind)
            return o->id;
        for (const DocumentObject& o : std::views::reverse(snapshot_->objects)) {
            if (o.kind() == kind) return o.id;
        }
        return 0;
    };
    // Object fields the command needs, from the kinds in their schema: a mesh is the current one,
    // a selection only the current one, anything else the selected or newest of its kind.
    for (const auto& [key, prop] : properties.items()) {
        if (!prop.contains("x-object") || !(isRequired(key) || key == "id" || key == "mesh_id"))
            continue;
        const Json& x = prop["x-object"];
        const Json kinds = x.is_array() ? x : Json::array({x});
        ObjectId id = 0;
        for (std::size_t i = 0; i < kinds.size() && id == 0; ++i) {
            if (const std::string kind = kinds[i].get<std::string>(); kind == "any") {
                id = selected_;
            } else if (kind == "selection") {
                id = currentSelection();
            } else if (kind == "mesh") {
                id = i == 0 ? currentMesh() : 0; // a second-choice mesh would be the current one
            } else {
                id = pick(kind);
            }
        }
        if (id != 0) values[key] = id;
    }
    if (properties.contains("path") && command.name == "project.save" && !projectPath_.isEmpty()) {
        values["path"] = projectPath_.toStdString();
    }
    return values;
}

void MainWindow::showTool(const QString& name, const ObjectId forObject) const {
    const Command* c = engine_.registry().find(name.toStdString());
    if (c == nullptr) return;
    Json values = formDefaults(*c);
    if (const DocumentObject* o = snapshot_ ? snapshot_->find(forObject) : nullptr) {
        // The field of the object's kind: id, else a required one, else any; a list takes it
        // as its one item.
        std::vector<std::string> required;
        if (c->paramsSchema.contains("required"))
            required = c->paramsSchema["required"].get<std::vector<std::string>>();
        std::string best;
        Json item;
        int bestRank = 3;
        const Json properties = c->paramsSchema.value("properties", Json::object());
        for (const auto& [key, prop] : properties.items()) {
            const bool list = !prop.contains("x-object") && prop.contains("items") &&
                              prop["items"].contains("x-object");
            const Json x = list ? prop["items"]["x-object"] : prop.value("x-object", Json());
            const Json kinds = x.is_array() ? x : Json::array({x});
            if (std::ranges::find(kinds, Json(o->kind())) == kinds.end()) continue;
            const int rank = key == "id"                                          ? 0
                             : std::ranges::find(required, key) != required.end() ? 1
                                                                                  : 2;
            if (rank < bestRank) {
                bestRank = rank;
                best = key;
                item = list ? Json::array({o->id}) : Json(o->id);
            }
        }
        if (!best.empty()) values[best] = item;
    }
    form_->setCommand(c, values);
    if (auto* dock = findChild<QDockWidget*>("tool")) {
        dock->show();
        dock->raise();
    }
}

void MainWindow::showMadeWith(const ObjectId id) const {
    const DocumentObject* o = snapshot_ ? snapshot_->find(id) : nullptr;
    const Command* c = o != nullptr ? engine_.registry().find(o->provenance.command) : nullptr;
    if (c == nullptr) return;
    const Json params = Json::parse(o->provenance.params, nullptr, false);
    form_->setCommand(c, params.is_object() ? params : Json::object());
    if (auto* dock = findChild<QDockWidget*>("tool")) {
        dock->show();
        dock->raise();
    }
}

std::optional<SketchPlane> MainWindow::sectionPlane(const Json& params) const {
    // As sketch.section reads it; the command checks it again when it runs.
    const auto vec = [&](const char* key) -> std::optional<Vec3d> {
        const Json v = params.value(key, Json());
        if (!v.is_array() || v.size() != 3 ||
            !std::ranges::all_of(v, [](const Json& x) { return x.is_number(); }))
            return std::nullopt;
        return Vec3d{.x = v[0].get<double>(), .y = v[1].get<double>(), .z = v[2].get<double>()};
    };
    SketchPlane plane;
    if (const std::string name = params.value("plane", ""); name == "yz") {
        plane.u = {.x = 0, .y = 1, .z = 0};
        plane.v = {.x = 0, .y = 0, .z = 1};
    } else if (name == "zx") {
        plane.u = {.x = 0, .y = 0, .z = 1};
        plane.v = {.x = 1, .y = 0, .z = 0};
    } else if (name.empty() && params.contains("plane_id")) {
        const DocumentObject* o =
            snapshot_ ? snapshot_->find(params.value("plane_id", ObjectId{0})) : nullptr;
        if (o == nullptr || o->primitive() == nullptr ||
            o->primitive()->fit.surface.kind != PrimitiveKind::Plane)
            return std::nullopt;
        plane =
            planeThrough(o->primitive()->fit.surface.point, o->primitive()->fit.surface.direction);
    } else if (name.empty()) {
        const auto point = vec("point");
        const auto normal = vec("normal");
        if (!point || !normal || !(length(*normal) > 0.0)) return std::nullopt;
        plane = planeThrough(*point, *normal);
    }
    plane.origin = plane.origin + plane.normal() * params.value("offset_mm", 0.0);
    return plane;
}

void MainWindow::previewSection() const {
    std::optional<SketchPlane> plane;
    if (const Command* c = form_->command(); c != nullptr && c->name == "sketch.section") {
        if (const auto params = form_->params()) plane = sectionPlane(*params);
    }
    viewport_->setPreviewPlane(plane);
}

void MainWindow::stepHistory(const int steps) const {
    // One command per step, run in order, as pressing Undo or Redo that many times would.
    for (int i = 0; i < std::abs(steps); ++i) run(steps < 0 ? "doc.undo" : "doc.redo", {});
}

void MainWindow::runForm() {
    const Command* c = form_->command();
    if (c == nullptr) return;
    const auto params = form_->params();
    if (!params) {
        QMessageBox::warning(
            this, QString::fromStdString(c->name), QString::fromStdString(params.error().message));
        return;
    }
    const QString name = QString::fromStdString(c->name);
    run(name, *params, [this, name](const Result<Json>& r) {
        if (!r) {
            QMessageBox::warning(this, name, QString::fromStdString(r.error().message));
            return;
        }
        adoptSelection(r);
        // Read-only tools report in the Inspector; a new deviation shows its statistics there.
        if (const Command* command = engine_.registry().find(name.toStdString());
            command != nullptr && !command->mutatesDocument) {
            inspect(name, *r);
        } else if (name == "deviation.compute") {
            showInfo(r->value("id", ObjectId{0}));
        }
        if (name == "mesh.wrap" || name == "shells.split") viewport_->update();
    });
}

Json MainWindow::selectionTarget(const ObjectId mesh, const bool subtract, const bool add) const {
    const ObjectId sel = currentSelection();
    if (const DocumentObject* o = snapshot_ ? snapshot_->find(sel) : nullptr;
        o != nullptr && o->selection() != nullptr && o->selection()->meshId == mesh) {
        return Json{{"selection_id", sel},
                    {"mode",
                     subtract ? "subtract"
                     : add    ? "add"
                              : "replace"}};
    }
    if (subtract) return {}; // nothing to remove from
    return Json{{"name", "Selection"}};
}

void MainWindow::adoptSelection(const Result<Json>& result) {
    if (result && result->is_object() && result->contains("selection_id")) {
        chooseObject((*result)["selection_id"].get<ObjectId>());
        form_->setSelectedObjects(selectedIds_); // the open form takes it, as from a click
    }
}

void MainWindow::onBrushStroke(ObjectId mesh,
                               const std::vector<Vec3d>& centers,
                               const bool subtract,
                               bool) {
    // A brush adds by default; Ctrl removes.
    Json p = selectionTarget(mesh, subtract, true);
    if (p.is_null()) return;
    Json list = Json::array();
    for (const Vec3d& c : centers) list.push_back(toJson(c));
    p["id"] = mesh;
    p["centers"] = list;
    p["radius_mm"] = viewport_->brushRadius();
    run("select.brush", p, [this](const Result<Json>& r) { adoptSelection(r); });
}

void MainWindow::onLasso(const QPolygonF& ndc, const bool subtract, const bool add) {
    const ObjectId mesh = currentMesh();
    if (mesh == 0) return;
    Json p = selectionTarget(mesh, subtract, add);
    if (p.is_null()) return;
    Json vp = Json::array();
    for (const double v : viewport_->viewProjection().m) vp.push_back(v);
    Json polygon = Json::array();
    for (const QPointF& point : ndc) polygon.push_back({point.x(), point.y()});
    p["id"] = mesh;
    p["view_projection"] = vp;
    p["polygon"] = polygon;
    p["eye"] = toJson(viewport_->eye());
    p["visible_only"] = true;
    run("select.lasso", p, [this](const Result<Json>& r) { adoptSelection(r); });
}

void MainWindow::onObjectClicked(const Hit& hit) {
    // With fillet.fit open a click on a solid marks an edge to round (a point near it);
    // otherwise it selects what was clicked, which also fills the open form.
    const DocumentObject* o = snapshot_ ? snapshot_->find(hit.objectId) : nullptr;
    if (o == nullptr) return;
    if (const Command* c = form_->command();
        c != nullptr && c->name == "fillet.fit" && o->solid() != nullptr) {
        form_->appendPoint("edge_points", hit.point);
        return;
    }
    // With quad.remesh open a click on its mesh adds a point to the last guide.
    if (const Command* c = form_->command(); c != nullptr && c->name == "quad.remesh") {
        if (const auto params = form_->params();
            params && o->mesh() != nullptr && params->value("id", ObjectId{0}) == o->id) {
            form_->appendPoint("guides", hit.point);
            return;
        }
    }
    chooseObject(hit.objectId);
    form_->setSelectedObjects(selectedIds_);
}

void MainWindow::onFaceClicked(Hit hit, const bool subtract, const bool add) {
    Json p = selectionTarget(hit.meshId, subtract, add || !subtract);
    if (p.is_null()) return;
    p["id"] = hit.meshId;
    p["face"] = hit.face;
    const bool shell = viewport_->mode() == Mode::Shell;
    if (!shell) p["max_angle_deg"] = viewport_->creaseAngle();
    run(shell ? "select.shell" : "select.crease", p, [this](const Result<Json>& r) {
        adoptSelection(r);
    });
}

void MainWindow::onMeasured(Hit a, Hit b) const {
    // Points snap to a vertex or edge within a few pixels of the click.
    run("measure.distance",
        {{"a", {{"id", a.meshId}, {"point", toJson(a.point)}}},
         {"b", {{"id", b.meshId}, {"point", toJson(b.point)}}},
         {"snap_mm", std::min(viewport_->snapMm(a.point), viewport_->snapMm(b.point))}},
        [this](const Result<Json>& r) {
            if (r && r->contains("distance_mm")) {
                status_->setText(
                    tr("Distance %1 mm").arg((*r)["distance_mm"].get<double>(), 0, 'f', 3));
            }
        });
}

// ---- Files ------------------------------------------------------------------------------------

void MainWindow::openFiles(const QStringList& files) {
    for (const QString& f : files) {
        if (f.endsWith(QLatin1String(".m2c"), Qt::CaseInsensitive)) {
            run("project.open", {{"path", f.toStdString()}}, [this, f](const Result<Json>& r) {
                if (!r) {
                    QMessageBox::warning(
                        this, tr("Open project"), QString::fromStdString(r.error().message));
                    return;
                }
                projectPath_ = f;
                savedRevision_ = snapshot_ ? snapshot_->revision : 0;
                viewport_->fitAll();
                onDocumentChanged(snapshot_);
            });
        } else {
            run("mesh.import", {{"path", f.toStdString()}}, [this](const Result<Json>& r) {
                if (!r) {
                    QMessageBox::warning(
                        this, tr("Import"), QString::fromStdString(r.error().message));
                } else {
                    chooseObject((*r)["id"].get<ObjectId>());
                    form_->setSelectedObjects(selectedIds_);
                }
            });
        }
    }
}

void MainWindow::newProject() {
    if (snapshot_ && !snapshot_->objects.empty() && snapshot_->revision != savedRevision_) {
        const auto answer = QMessageBox::question(this,
                                                  tr("New project"),
                                                  tr("Discard unsaved changes?"),
                                                  QMessageBox::Discard | QMessageBox::Cancel,
                                                  QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) return;
    }
    run("project.new", {}, [this](const Result<Json>& r) {
        if (!r) return;
        projectPath_.clear();
        savedRevision_ = snapshot_ ? snapshot_->revision : 0;
        onDocumentChanged(snapshot_);
    });
}

void MainWindow::openProject() {
    const QString file =
        QFileDialog::getOpenFileName(this, tr("Open project"), {}, projectFilter());
    if (!file.isEmpty()) openFiles({file});
}

bool MainWindow::saveProject(const bool saveAs) {
    QString file = projectPath_;
    if (saveAs || file.isEmpty()) {
        file = QFileDialog::getSaveFileName(this, tr("Save project"), file, projectFilter());
        if (file.isEmpty()) return false;
        if (!file.endsWith(QLatin1String(".m2c"), Qt::CaseInsensitive)) file += ".m2c";
    }
    const std::uint64_t revision = snapshot_ ? snapshot_->revision : 0;
    run("project.save",
        {{"path", file.toStdString()}},
        [this, file, revision](const Result<Json>& r) {
            if (!r) {
                QMessageBox::warning(
                    this, tr("Save project"), QString::fromStdString(r.error().message));
                return;
            }
            projectPath_ = file;
            savedRevision_ = revision;
            onDocumentChanged(snapshot_);
            status_->setText(tr("Saved %1").arg(file));
        });
    return true;
}

void MainWindow::importMeshes() {
    const QStringList files =
        QFileDialog::getOpenFileNames(this, tr("Import mesh"), {}, meshFilter());
    openFiles(files);
}

void MainWindow::exportMesh() {
    const ObjectId mesh = currentMesh();
    if (mesh == 0) return;
    const QString file = QFileDialog::getSaveFileName(this, tr("Export mesh"), {}, meshFilter());
    if (file.isEmpty()) return;
    run("mesh.export",
        {{"id", mesh}, {"path", file.toStdString()}},
        [this, file](const Result<Json>& r) {
            if (!r)
                QMessageBox::warning(this, tr("Export"), QString::fromStdString(r.error().message));
            else
                status_->setText(tr("Exported %1").arg(file));
        });
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) files << url.toLocalFile();
    }
    openFiles(files);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (snapshot_ && !snapshot_->objects.empty() && snapshot_->revision != savedRevision_) {
        const auto answer =
            QMessageBox::question(this,
                                  tr("Quit"),
                                  tr("Save changes before closing?"),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                  QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save) {
            // Saving runs on the engine thread; close once it is written.
            if (!saveProject(false)) {
                event->ignore();
                return;
            }
            event->ignore();
            run("doc.list", {}, [this](const Result<Json>&) {
                if (snapshot_ && savedRevision_ == snapshot_->revision) close();
            });
            return;
        }
    }
    event->accept();
}

} // namespace m2c::app

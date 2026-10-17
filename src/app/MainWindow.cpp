// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

#include "ParamForm.h"

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

QString groupTitle(const std::string& prefix) {
    static const std::map<std::string, QString> titles{
        {"mesh", QObject::tr("Mesh")},
        {"holes", QObject::tr("Holes")},
        {"shells", QObject::tr("Shells")},
        {"select", QObject::tr("Select")},
        {"faces", QObject::tr("Faces")},
        {"align", QObject::tr("Align")},
        {"plane", QObject::tr("Fit")},
        {"measure", QObject::tr("Measure")},
        {"quad", QObject::tr("Quad layout")},
        {"surface", QObject::tr("Surface")},
        {"solid", QObject::tr("Solid")},
        {"deviation", QObject::tr("Deviation")},
        {"object", QObject::tr("Object")},
        {"project", QObject::tr("Project")},
    };
    const auto it = titles.find(prefix);
    return it != titles.end() ? it->second : QString::fromStdString(prefix);
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
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(hover_);

    connect(&engine_, &Engine::documentChanged, this, &MainWindow::onDocumentChanged);
    connect(&engine_, &Engine::logLine, this, [this](const QString& line, bool error) {
        if (error) {
            log_->appendHtml(
                QStringLiteral("<span style='color:#e06c6c'>%1</span>").arg(line.toHtmlEscaped()));
        } else {
            log_->appendPlainText(line);
        }
    });
    connect(&engine_, &Engine::busyChanged, this, [this](bool busy, const QString& command) {
        form_->setBusy(busy);
        status_->setText(busy ? tr("Running %1…").arg(command) : QString());
        if (busy) {
            QApplication::setOverrideCursor(Qt::BusyCursor);
        } else {
            while (QApplication::overrideCursor() != nullptr) QApplication::restoreOverrideCursor();
        }
    });
    connect(&engine_, &Engine::clientsChanged, this, [this](int count) {
        log_->appendPlainText(count > 0 ? tr("%n agent connection(s) open", nullptr, count)
                                        : tr("No agent connections"));
    });

    connect(viewport_, &Viewport::brushStroke, this, &MainWindow::onBrushStroke);
    connect(viewport_, &Viewport::lassoDrawn, this, &MainWindow::onLasso);
    connect(viewport_, &Viewport::faceClicked, this, &MainWindow::onFaceClicked);
    connect(viewport_, &Viewport::measured, this, &MainWindow::onMeasured);
    connect(viewport_, &Viewport::hoverChanged, hover_, &QLabel::setText);
    connect(viewport_, &Viewport::brushRadiusChanged, brushRadius_, &QDoubleSpinBox::setValue);

    onDocumentChanged(engine_.snapshot());
    resize(1400, 900);
}

// ---- Layout -----------------------------------------------------------------------------------

void MainWindow::buildDocks() {
    setDockOptions(AnimatedDocks | AllowTabbedDocks);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Object"), tr("Info")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onTreeSelection);
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updatingTree_ || column != 0 || !snapshot_) return;
        const auto id = item->data(0, kIdRole).value<ObjectId>();
        const DocumentObject* o = snapshot_->find(id);
        if (o == nullptr) return;
        const bool visible = item->checkState(0) == Qt::Checked;
        if (visible != o->visible) run("object.show", {{"id", id}, {"visible", visible}});
        const std::string name = item->text(0).trimmed().toStdString();
        if (!name.empty() && name != o->name) run("object.rename", {{"id", id}, {"name", name}});
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        viewport_->fitObject(item->data(0, kIdRole).value<ObjectId>());
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = tree_->itemAt(pos);
        if (item == nullptr) return;
        const auto id = item->data(0, kIdRole).value<ObjectId>();
        QMenu menu;
        menu.addAction(tr("Fit view"), this, [this, id] { viewport_->fitObject(id); });
        menu.addAction(tr("Rename"), this, [this, item] { tree_->editItem(item, 0); });
        menu.addAction(tr("Info"), this, [this, id] {
            run("mesh.info", {{"id", id}}, [this](const Result<Json>& r) {
                if (r) log_->appendPlainText(QString::fromStdString(r->dump(2)));
            });
        });
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id] { run("object.delete", {{"id", id}}); });
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });
    auto* objects = new QDockWidget(tr("Objects"), this);
    objects->setObjectName("objects");
    objects->setWidget(tree_);
    addDockWidget(Qt::LeftDockWidgetArea, objects);

    // Workflow: the usual order of M0/M1 tools, each opening its form.
    auto* workflow = new QWidget;
    auto* steps = new QVBoxLayout(workflow);
    const auto addStep = [&](const QString& text, const std::function<void()>& action) {
        auto* button = new QPushButton(text);
        button->setStyleSheet("text-align: left; padding: 4px 8px;");
        connect(button, &QPushButton::clicked, this, action);
        steps->addWidget(button);
    };
    addStep(tr("1  Import scan…"), [this] { importMeshes(); });
    addStep(tr("2  Repair"), [this] { showTool("mesh.repair"); });
    addStep(tr("3  Fill holes"), [this] { showTool("holes.fill"); });
    addStep(tr("4  Remove small shells"), [this] { showTool("shells.clean"); });
    addStep(tr("5  Smooth"), [this] { showTool("mesh.smooth"); });
    addStep(tr("6  Simplify"), [this] { showTool("mesh.simplify"); });
    addStep(tr("7  Align to principal axes"), [this] { showTool("align.principal"); });
    addStep(tr("    or align to datums"), [this] { showTool("align.datum"); });
    addStep(tr("8  Save project…"), [this] { saveProject(false); });
    steps->addStretch(1);
    auto* workflowDock = new QDockWidget(tr("Workflow"), this);
    workflowDock->setObjectName("workflow");
    workflowDock->setWidget(workflow);
    addDockWidget(Qt::LeftDockWidgetArea, workflowDock);

    form_ = new ParamForm;
    connect(form_, &ParamForm::runRequested, this, &MainWindow::runForm);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(form_);
    auto* tool = new QDockWidget(tr("Tool"), this);
    tool->setObjectName("tool");
    tool->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, tool);

    history_ = new QListWidget;
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
    tool->raise();
}

void MainWindow::buildToolbar() {
    QToolBar* bar = addToolBar(tr("Tools"));
    bar->setObjectName("tools");
    modes_ = new QActionGroup(this);
    const struct {
        QString text;
        Mode mode;
        QString key;
        QString tip;
    } modes[] = {
        {tr("Navigate"),
         Mode::Navigate,
         "1",
         tr("Left drag orbits, Shift or middle drag pans, wheel zooms, double-click sets the "
            "centre")},
        {tr("Brush"),
         Mode::Brush,
         "2",
         tr("Paint faces. Ctrl removes. [ and ] change the size. Right drag orbits")},
        {tr("Lasso"), Mode::Lasso, "3", tr("Draw around faces. Shift adds, Ctrl removes")},
        {tr("Crease"),
         Mode::Crease,
         "4",
         tr("Click a face to select the region up to sharp edges. Ctrl removes")},
        {tr("Measure"), Mode::Measure, "5", tr("Click two points to measure their distance")},
    };
    for (const auto& m : modes) {
        QAction* a = bar->addAction(m.text);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(m.key));
        a->setToolTip(m.tip + QStringLiteral(" (%1)").arg(m.key));
        a->setChecked(m.mode == Mode::Navigate);
        modes_->addAction(a);
        const Mode mode = m.mode;
        connect(a, &QAction::triggered, this, [this, mode] { viewport_->setMode(mode); });
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
    const auto onSelection = [this](const char* command, Json extra = Json::object()) {
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
    for (QDockWidget* dock : findChildren<QDockWidget*>())
        view->addAction(dock->toggleViewAction());

    // Tools: every command, grouped by prefix, each opening its generated form.
    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    std::map<std::string, QMenu*> groups;
    for (const Command* c : engine_.registry().list()) {
        const std::string prefix = c->name.substr(0, c->name.find('.'));
        if (prefix == "doc" || prefix == "commands") continue;
        QMenu*& group = groups[prefix];
        if (group == nullptr) group = tools->addMenu(groupTitle(prefix));
        QAction* a = group->addAction(QString::fromStdString(c->name));
        a->setToolTip(QString::fromStdString(c->summary));
        a->setStatusTip(QString::fromStdString(c->summary));
        const QString name = QString::fromStdString(c->name);
        connect(a, &QAction::triggered, this, [this, name] { showTool(name); });
    }

    QMenu* help = menuBar()->addMenu(tr("&Help"));
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
            const Aabb& b = o.mesh()->mesh->bounds();
            brushRadius_->setValue(length(b.max - b.min) * 0.02);
            brushSized_ = true;
            break;
        }
    }
    viewport_->setCurrent(currentMesh(), currentSelection());
    refreshTree();

    history_->clear();
    for (const HistoryStep& s : snapshot_->undo) {
        history_->addItem(
            QStringLiteral("%1   [%2]")
                .arg(QString::fromStdString(s.label), QString::fromStdString(s.source)));
    }
    for (const HistoryStep& s : snapshot_->redo) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   [%2]  (undone)")
                .arg(QString::fromStdString(s.label), QString::fromStdString(s.source)));
        item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        history_->addItem(item);
    }
    if (!snapshot_->undo.empty())
        history_->setCurrentRow(static_cast<int>(snapshot_->undo.size()) - 1);
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
        auto* item = new QTreeWidgetItem({QString::fromStdString(o.name), info});
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
        } else {
            continue;
        }
        const auto it = meshes.find(parent);
        if (it != meshes.end()) {
            it->second->addChild(item);
            it->second->setExpanded(true);
        } else {
            tree_->addTopLevelItem(item);
        }
    }
    if (selected_ != 0) {
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
            if ((*it)->data(0, kIdRole).value<ObjectId>() == selected_) {
                tree_->setCurrentItem(*it);
                break;
            }
        }
    }
    updatingTree_ = false;
}

void MainWindow::onTreeSelection() {
    if (updatingTree_) return;
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    selected_ = items.isEmpty() ? 0 : items.front()->data(0, kIdRole).value<ObjectId>();
    viewport_->setCurrent(currentMesh(), currentSelection());
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
    for (auto it = snapshot_->objects.rbegin(); it != snapshot_->objects.rend(); ++it) {
        if (it->mesh() != nullptr && it->visible) return it->id;
    }
    return 0;
}

ObjectId MainWindow::currentSelection() const {
    if (!snapshot_) return 0;
    const DocumentObject* o = snapshot_->find(selected_);
    return (o != nullptr && o->selection() != nullptr) ? o->id : 0;
}

// ---- Commands ---------------------------------------------------------------------------------

void MainWindow::run(const QString& command, const Json& params, Engine::Done done) {
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
        return std::find(required.begin(), required.end(), k) != required.end();
    };
    if (properties.contains("id")) {
        const ObjectId id = command.name.rfind("object.", 0) == 0 ? selected_ : currentMesh();
        if (id != 0) values["id"] = id;
    }
    if (properties.contains("selection_id") && isRequired("selection_id") && currentSelection()) {
        values["selection_id"] = currentSelection();
    }
    if (properties.contains("path") && command.name == "project.save" && !projectPath_.isEmpty()) {
        values["path"] = projectPath_.toStdString();
    }
    return values;
}

void MainWindow::showTool(const QString& name) {
    const Command* c = engine_.registry().find(name.toStdString());
    if (c == nullptr) return;
    form_->setCommand(c, formDefaults(*c));
    if (auto* dock = findChild<QDockWidget*>("tool")) {
        dock->show();
        dock->raise();
    }
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
        // Read-only tools report through the log.
        const Command* command = engine_.registry().find(name.toStdString());
        if (command != nullptr && !command->mutatesDocument) {
            log_->appendPlainText(name + QStringLiteral(": ") + QString::fromStdString(r->dump(2)));
        }
        if (name == "mesh.wrap" || name == "shells.split") viewport_->update();
    });
}

Json MainWindow::selectionTarget(ObjectId mesh, bool subtract, bool add) const {
    const ObjectId sel = currentSelection();
    const DocumentObject* o = snapshot_ ? snapshot_->find(sel) : nullptr;
    if (o != nullptr && o->selection() != nullptr && o->selection()->meshId == mesh) {
        return Json{{"selection_id", sel},
                    {"mode", subtract ? "subtract" : (add ? "add" : "replace")}};
    }
    if (subtract) return Json(); // nothing to remove from
    return Json{{"name", "Selection"}};
}

void MainWindow::adoptSelection(const Result<Json>& result) {
    if (result && result->is_object() && result->contains("selection_id")) {
        selected_ = (*result)["selection_id"].get<ObjectId>();
        refreshTree();
        viewport_->setCurrent(currentMesh(), currentSelection());
    }
}

void MainWindow::onBrushStroke(ObjectId mesh,
                               const std::vector<Vec3d>& centers,
                               bool subtract,
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

void MainWindow::onLasso(const QPolygonF& ndc, bool subtract, bool add) {
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

void MainWindow::onFaceClicked(Hit hit, bool subtract, bool add) {
    Json p = selectionTarget(hit.meshId, subtract, add || !subtract);
    if (p.is_null()) return;
    p["id"] = hit.meshId;
    p["face"] = hit.face;
    p["max_angle_deg"] = viewport_->creaseAngle();
    run("select.crease", p, [this](const Result<Json>& r) { adoptSelection(r); });
}

void MainWindow::onMeasured(Hit a, Hit b) {
    run("measure.distance",
        {{"a", {{"id", a.meshId}, {"point", toJson(a.point)}}},
         {"b", {{"id", b.meshId}, {"point", toJson(b.point)}}}},
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
                    selected_ = (*r)["id"].get<ObjectId>();
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

bool MainWindow::saveProject(bool saveAs) {
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

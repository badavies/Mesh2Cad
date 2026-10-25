// SPDX-License-Identifier: GPL-3.0-or-later
#include "WorkflowPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <Qt>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/document/Document.h"

#include "Engine.h"
#include "ToolCatalog.h"

namespace m2c::app {

namespace {

/// One line of a track: its steps side by side, each a label and the command it opens.
using Row = std::vector<std::pair<QString, const char*>>;

struct Track {
    QString title;
    std::vector<Row> rows;
};

std::vector<Track> tracks() {
    return {
        {QObject::tr("1  Prepare and align"),
         {{{QObject::tr("Import scan…"), "mesh.import"}},
          {{QObject::tr("Repair"), "mesh.repair"},
           {QObject::tr("Select spikes and tunnels"), "select.defects"}},
          {{QObject::tr("Fill holes"), "holes.fill"}},
          {{QObject::tr("List shells"), "shells.detect"},
           {QObject::tr("Remove small shells"), "shells.clean"}},
          {{QObject::tr("Smooth"), "mesh.smooth"},
           {QObject::tr("Simplify"), "mesh.simplify"},
           {QObject::tr("Remesh"), "mesh.remesh"}},
          {{QObject::tr("Align: principal axes"), "align.principal"},
           {QObject::tr("symmetry"), "align.symmetry"},
           {QObject::tr("datums"), "align.datum"}}}},
        {QObject::tr("2  Path A: organic surfacing"),
         {{{QObject::tr("Quad remesh"), "quad.remesh"}},
          {{QObject::tr("Build surface"), "surface.build"}}}},
        {QObject::tr("3  Path B: prismatic parts and features"),
         {{{QObject::tr("Segment"), "mesh.segment"}},
          {{QObject::tr("Fit primitive"), "primitive.fit"},
           {QObject::tr("Fit plane"), "plane.fit"},
           {QObject::tr("Fit box"), "primitive.box"}},
          {{QObject::tr("Align primitives"), "primitive.align"}},
          {{QObject::tr("Trim into a solid"), "primitive.trim"},
           {QObject::tr("Section sketch"), "sketch.section"}},
          {{QObject::tr("Extrude"), "solid.extrude"}, {QObject::tr("Revolve"), "solid.revolve"}},
          {{QObject::tr("Boolean"), "solid.boolean"}},
          {{QObject::tr("Fit fillet"), "fillet.fit"}}}},
        {QObject::tr("4  Verify and export"),
         {{{QObject::tr("Compute deviation"), "deviation.compute"},
           {QObject::tr("Report"), "deviation.report"}},
          {{QObject::tr("Export STEP"), "solid.export"},
           {QObject::tr("Export mesh"), "solid.export.mesh"}},
          {{QObject::tr("Save project…"), "project.save"}}}},
    };
}

/// "1 shell", "3 shells".
QString counted(const std::size_t n, const QString& one, const QString& many) {
    return QStringLiteral("%L1 %2").arg(n).arg(n == 1 ? one : many);
}

} // namespace

WorkflowPanel::WorkflowPanel(Engine& engine, QWidget* parent) : QWidget(parent), engine_(engine) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    gate_ = new QLabel;
    gate_->setObjectName(QStringLiteral("gate"));
    gate_->setWordWrap(true);
    gate_->setTextFormat(Qt::PlainText);
    layout->addWidget(gate_);

    auto* steps = new QWidget;
    auto* column = new QVBoxLayout(steps);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    for (const auto& [title, rows] : tracks()) {
        // A header that folds its track away.
        auto* header = new QToolButton;
        header->setText(title);
        header->setCheckable(true);
        header->setChecked(true);
        header->setArrowType(Qt::DownArrow);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setAutoRaise(true);
        header->setStyleSheet(QStringLiteral("font-weight: bold;"));
        auto* body = new QWidget;
        auto* lines = new QVBoxLayout(body);
        lines->setContentsMargins(12, 0, 0, 4);
        lines->setSpacing(2);
        connect(header, &QToolButton::toggled, body, [header, body](const bool open) {
            header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            body->setVisible(open);
        });
        for (const Row& row : rows) {
            auto* line = new QHBoxLayout;
            line->setSpacing(2);
            for (const auto& [text, command] : row) {
                auto* button = new QPushButton(text);
                button->setObjectName(QString::fromLatin1(command));
                button->setStyleSheet(QStringLiteral("text-align: left; padding: 3px 6px;"));
                if (const Command* c = engine_.registry().find(command)) {
                    button->setToolTip(QStringLiteral("%1 (%2)").arg(
                        QString::fromStdString(c->summary), QString::fromLatin1(command)));
                }
                const QString name = QString::fromLatin1(command);
                connect(button, &QPushButton::clicked, this, [this, name] {
                    emit toolRequested(name);
                });
                line->addWidget(button, row.size() == 1 ? 1 : 0);
            }
            if (row.size() > 1) line->addStretch(1);
            lines->addLayout(line);
        }
        column->addWidget(header);
        column->addWidget(body);
    }
    column->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(steps);
    layout->addWidget(scroll, 1);
    setCurrent(nullptr, 0);
}

void WorkflowPanel::showGate(const QString& text, const bool passed) {
    passed_ = passed;
    gate_->setText(text);
    const QString color = passed ? QStringLiteral("60, 154, 95") : QStringLiteral("208, 138, 44");
    gate_->setStyleSheet(
        QStringLiteral("QLabel { border-left: 4px solid rgb(%1); padding: 4px 6px; "
                       "background-color: rgba(%1, 40); }")
            .arg(color));
}

QString WorkflowPanel::gateText() const {
    return gate_->text();
}

void WorkflowPanel::setCurrent(const std::shared_ptr<const DocSnapshot>& snapshot,
                               const ObjectId id) {
    snapshot_ = snapshot;
    const DocumentObject* o = snapshot ? snapshot->find(id) : nullptr;
    // A selection or measurement is judged by its mesh.
    if (o != nullptr && o->selection() != nullptr) o = snapshot->find(o->selection()->meshId);
    if (o != nullptr && o->measurement() != nullptr)
        o = snapshot->find(o->measurement()->measurement.a.objectId);
    current_ = o != nullptr ? o->id : 0;
    if (o == nullptr) {
        showGate(tr("Import a scan to start (step 1)."), false);
    } else if (o->mesh() != nullptr) {
        checkMesh(*o);
    } else if (const SolidData* solid = o->solid()) {
        const SolidSummary& s = solid->solid.summary();
        QStringList problems;
        if (!s.valid) problems << tr("the kernel's check fails");
        if (s.freeEdges > 0)
            problems << tr("%1: not closed")
                            .arg(counted(s.freeEdges, tr("free edge"), tr("free edges")));
        if (s.solidCount != 1)
            problems << tr("%1, not one").arg(counted(s.solidCount, tr("solid"), tr("solids")));
        if (problems.isEmpty()) {
            showGate(tr("Solid #%1: one valid closed solid of %2. Ready for booleans, deviation "
                        "and STEP export.")
                         .arg(o->id)
                         .arg(counted(s.faceCount, tr("face"), tr("faces"))),
                     true);
        } else {
            showGate(
                tr("Solid #%1 is not ready to export: %2.").arg(o->id).arg(problems.join("; ")),
                false);
        }
    } else if (const DeviationData* deviation = o->deviation()) {
        const DeviationStats& s = deviation->field.stats();
        const QString within = QString::number(s.withinTolerance * 100.0, 'f', 1);
        if (s.p95AbsMm <= s.toleranceMm) {
            showGate(tr("Deviation #%1: P95 %2 mm, within the %3 mm tolerance (%4% of points "
                        "within). Ready to export.")
                         .arg(o->id)
                         .arg(s.p95AbsMm, 0, 'f', 3)
                         .arg(s.toleranceMm, 0, 'g', 6)
                         .arg(within),
                     true);
        } else {
            showGate(tr("Deviation #%1: P95 %2 mm, over the %3 mm tolerance (%4% of points "
                        "within). Refine the layout or the fits where the hotspots are, then "
                        "measure again.")
                         .arg(o->id)
                         .arg(s.p95AbsMm, 0, 'f', 3)
                         .arg(s.toleranceMm, 0, 'g', 6)
                         .arg(within),
                     false);
        }
    } else if (const QuadLayoutData* q = o->quadLayout()) {
        showGate(tr("Quad layout #%1, %2: next, Build surface.")
                     .arg(o->id)
                     .arg(counted(q->cage.faceCount(), tr("quad"), tr("quads"))),
                 true);
    } else if (const PrimitiveData* p = o->primitive()) {
        showGate(tr("Primitive #%1 fits its region with P95 %2 mm: next, Align primitives or "
                    "Trim into a solid.")
                     .arg(o->id)
                     .arg(p->fit.p95Mm, 0, 'f', 3),
                 true);
    } else if (const SketchData* k = o->sketch()) {
        showGate(tr("Sketch #%1, %2: next, Extrude or Revolve.")
                     .arg(o->id)
                     .arg(counted(k->sketch.loops.size(), tr("loop"), tr("loops"))),
                 true);
    } else {
        showGate(QString::fromStdString(o->name), true);
    }
}

void WorkflowPanel::checkMesh(const DocumentObject& o) {
    const std::shared_ptr<const TriMesh>& mesh = o.mesh()->mesh;
    if (!stats_.is_null() && checked_.lock() == mesh) {
        // Repair's gate: closed, manifold and one shell.
        QStringList problems;
        const auto count = [this](const char* key) { return stats_.value(key, std::size_t{0}); };
        if (const auto n = count("boundary_edge_count"); n > 0)
            problems << tr("%1 around holes: Fill holes")
                            .arg(counted(n, tr("open edge"), tr("open edges")));
        if (const auto n = count("non_manifold_edge_count"); n > 0)
            problems << tr("%1: Repair")
                            .arg(counted(n, tr("non-manifold edge"), tr("non-manifold edges")));
        if (const auto n = count("degenerate_face_count"); n > 0)
            problems
                << tr("%1: Repair").arg(counted(n, tr("degenerate face"), tr("degenerate faces")));
        if (shells_ > 1)
            problems << tr("%1: Remove small shells, or keep the main one")
                            .arg(counted(shells_, tr("shell"), tr("shells")));
        if (problems.isEmpty()) {
            showGate(tr("Mesh #%1 is closed, manifold and one shell: ready for Quad remesh or "
                        "Segment.")
                         .arg(o.id),
                     true);
        } else {
            showGate(tr("Mesh #%1 is not ready for surfacing:\n• %2")
                         .arg(o.id)
                         .arg(problems.join(QStringLiteral("\n• "))),
                     false);
        }
        return;
    }
    showGate(tr("Checking mesh #%1…").arg(o.id), false);
    if (checking_) return; // the check under way looks again when it ends
    // Once per mesh version, in the background: no busy cursor, nothing in the log.
    checking_ = true;
    const QPointer<WorkflowPanel> self(this);
    const std::weak_ptr<const TriMesh> version = mesh;
    const Json params{{"id", o.id}};
    engine_.run(
        QStringLiteral("mesh.info"),
        params,
        [self, version, params](const Result<Json>& info) {
            if (!self) return;
            if (!info) {
                self->checking_ = false;
                return;
            }
            self->engine_.run(
                QStringLiteral("shells.detect"),
                params,
                [self, version, stats = info->value("stats", Json::object())](
                    const Result<Json>& shells) {
                    if (!self) return;
                    self->checking_ = false;
                    if (shells) {
                        self->checked_ = version;
                        self->stats_ = stats;
                        self->shells_ = shells->value("shells", Json::array()).size();
                    }
                    self->setCurrent(self->snapshot_, self->current_);
                },
                true);
        },
        true);
}

} // namespace m2c::app

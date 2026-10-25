// SPDX-License-Identifier: GPL-3.0-or-later
#include "InspectorPanel.h"

#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantList>
#include <Qt>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ToolCatalog.h"

namespace m2c::app {

namespace {

constexpr int kBoundsRole = Qt::UserRole;    ///< min x, y, z, max x, y, z of the row's part
constexpr int kShellRole = Qt::UserRole + 1; ///< a shell row's index
constexpr qsizetype kLongText = 120;

/// "area_mm2" reads "Area (mm²)": spaced words, the unit in brackets.
QString label(std::string key) {
    static constexpr std::array<std::pair<std::string_view, const char*>, 4> units{{
        {"_mm2", " (mm²)"},
        {"_mm3", " (mm³)"},
        {"_mm", " (mm)"},
        {"_deg", " (°)"},
    }};
    QString unit;
    for (const auto& [suffix, text] : units) {
        if (key.ends_with(suffix)) {
            key.resize(key.size() - suffix.size());
            unit = QString::fromUtf8(text);
            break;
        }
    }
    QString words = QString::fromStdString(key).replace('_', ' ');
    if (!words.isEmpty()) words[0] = words[0].toUpper();
    return words + unit;
}

QString valueText(const std::string& key, const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? QObject::tr("Yes") : QObject::tr("No");
    if (v.is_number_float()) {
        if (key == "within_tolerance") return QString::number(100 * v.get<double>(), 'f', 1) + "%";
        return QString::number(v.get<double>(), 'g', 6);
    }
    if (v.is_string()) return QString::fromStdString(v.get<std::string>());
    if (v.is_null()) return QObject::tr("none");
    if (v.is_array() && std::ranges::all_of(v, [](const Json& x) { return x.is_number(); })) {
        QStringList parts;
        for (const Json& x : v) parts << valueText(key, x);
        return parts.join(QStringLiteral(", "));
    }
    return QString::fromStdString(v.dump());
}

/// Whether a value is something to fix: open, non-manifold or degenerate parts, an invalid
/// solid, a deviation whose 95th percentile is past its tolerance, or a reported error.
bool problem(const std::string& key, const Json& v, const Json& in) {
    static constexpr std::array<std::string_view, 4> counts{
        "boundary_edge_count", "non_manifold_edge_count", "degenerate_face_count", "free_edges"};
    if (std::ranges::find(counts, key) != counts.end()) return v.is_number() && v.get<double>() > 0;
    if (key == "is_watertight" || key == "valid") return v.is_boolean() && !v.get<bool>();
    if (key == "p95_abs_mm" && v.is_number() && in.contains("tolerance_mm"))
        return v.get<double>() > in["tolerance_mm"].get<double>();
    return key == "error" || key == "warning" || key == "warnings";
}

std::optional<Aabb> boundsOf(const Json& part) {
    const Json box = part.value("bounds", Json());
    const auto corner = [&](const char* name) -> std::optional<Vec3d> {
        const Json c = box.is_object() ? box.value(name, Json()) : Json();
        if (!c.is_array() || c.size() != 3) return std::nullopt;
        return Vec3d{.x = c[0].get<double>(), .y = c[1].get<double>(), .z = c[2].get<double>()};
    };
    const auto min = corner("min");
    const auto max = corner("max");
    if (!min || !max) return std::nullopt;
    return Aabb{.min = *min, .max = *max};
}

/// A part's first few numbers, for its row: "Area (mm²) 12.5, Face count 40".
QString summary(const Json& part) {
    QStringList parts;
    for (const auto& [key, v] : part.items()) {
        if (!v.is_number() || key == "index" || key == "seed_face") continue;
        parts << label(key) + ' ' + valueText(key, v);
        if (parts.size() == 3) break;
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    title_ = new QLabel(tr("Info and the results of read-only tools show here."));
    title_->setWordWrap(true);
    layout->addWidget(title_);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Name"), tr("Value")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->setUniformRowHeights(true);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](const QTreeWidgetItem* item) {
        const QVariantList b = item->data(0, kBoundsRole).toList();
        if (b.size() != 6) return;
        emit boundsClicked(
            Aabb{.min = {.x = b[0].toDouble(), .y = b[1].toDouble(), .z = b[2].toDouble()},
                 .max = {.x = b[3].toDouble(), .y = b[4].toDouble(), .z = b[5].toDouble()}});
    });
    layout->addWidget(tree_, 1);

    // A shell list's actions.
    shellActions_ = new QWidget;
    auto* shells = new QHBoxLayout(shellActions_);
    shells->setContentsMargins(0, 0, 0, 0);
    auto* select = new QPushButton(tr("Select checked"));
    select->setObjectName("selectShells");
    select->setToolTip(tr("Make one selection of the checked shells"));
    select->setEnabled(false);
    connect(select, &QPushButton::clicked, this, [this] {
        emit commandRequested(
            {"shells.select", {{"id", result_.value("id", 0)}, {"shells", checkedShells()}}});
    });
    connect(tree_, &QTreeWidget::itemChanged, select, [this, select] {
        select->setEnabled(!checkedShells().empty());
    });
    auto* keep = new QPushButton(tr("Keep main"));
    keep->setObjectName("keepMain");
    keep->setToolTip(tr("Delete every shell except the main (largest) one"));
    connect(keep, &QPushButton::clicked, this, [this] {
        emit commandRequested({"shells.keep", {{"id", result_.value("id", 0)}}});
    });
    auto* split = new QPushButton(tr("Split others off"));
    split->setObjectName("splitShells");
    split->setToolTip(tr("Make every shell except the main one a mesh of its own"));
    connect(split, &QPushButton::clicked, this, [this] {
        emit commandRequested({"shells.split", {{"id", result_.value("id", 0)}}});
    });
    shells->addWidget(select);
    shells->addWidget(keep);
    shells->addWidget(split);
    shellActions_->hide();
    layout->addWidget(shellActions_);

    // A deviation's: measure again with its settings, making each hotspot a selection.
    selectHotspots_ = new QPushButton(tr("Select hotspots"));
    selectHotspots_->setObjectName("selectHotspots");
    selectHotspots_->setToolTip(tr("Measure again with the same reference and tolerance, making "
                                   "each hotspot a selection, ready for Fit primitive"));
    connect(selectHotspots_, &QPushButton::clicked, this, [this] {
        const Json stats = result_.value("stats", Json::object());
        emit commandRequested({"deviation.compute",
                               {{"id", result_.value("mesh_id", 0)},
                                {"reference_id", result_.value("reference_id", 0)},
                                {"tolerance_mm", stats.value("tolerance_mm", 0.1)},
                                {"max_hotspots", result_.value("hotspots", Json::array()).size()},
                                {"select_hotspots", true}}});
    });
    selectHotspots_->hide();
    layout->addWidget(selectHotspots_);

    auto* copy = new QPushButton(tr("Copy as JSON"));
    copy->setObjectName("copy");
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(QString::fromStdString(result_.dump(2)));
    });
    layout->addWidget(copy, 0, Qt::AlignRight);
}

void InspectorPanel::showResult(const QString& command, const Json& result) {
    command_ = command;
    result_ = result;
    QString title = QStringLiteral("<b>%1</b>").arg(toolInfo(command.toStdString()).title);
    if (result.contains("name") && result["name"].is_string())
        title += QStringLiteral(": ") + QString::fromStdString(result["name"].get<std::string>());
    if (result.contains("id") && result["id"].is_number())
        title += QStringLiteral(" (#%1)").arg(result["id"].get<ObjectId>());
    title_->setText(title);

    const QSignalBlocker quiet(tree_);
    tree_->clear();
    if (result.is_object()) {
        for (const auto& [key, value] : result.items()) {
            if (key != "id" && key != "name") addRow(nullptr, key, value, result);
        }
    } else {
        addRow(nullptr, "result", result, Json::object());
    }
    shellActions_->setVisible(command == "shells.detect" && result.contains("shells"));
    selectHotspots_->setVisible(command == "deviation.info" &&
                                !result.value("hotspots", Json::array()).empty());
    if (auto* select = findChild<QPushButton*>("selectShells")) select->setEnabled(false);
}

void InspectorPanel::addRow(QTreeWidgetItem* parent,
                            const std::string& key,
                            const Json& value,
                            const Json& in) {
    auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
    item->setText(0, label(key));
    if (value.is_object()) {
        for (const auto& [k, v] : value.items()) addRow(item, k, v, value);
        item->setExpanded(parent == nullptr);
        return;
    }
    if (value.is_array() && !value.empty() && value.front().is_object()) {
        // A list of parts: a row each, under their name in the singular, with their bounds.
        item->setText(1, QString::number(value.size()));
        item->setExpanded(parent == nullptr);
        const QString one = label(key.ends_with('s') ? key.substr(0, key.size() - 1) : key);
        const bool shells = key == "shells" && command_ == "shells.detect";
        for (std::size_t i = 0; i < value.size(); ++i) {
            const Json& part = value[i];
            auto* row = new QTreeWidgetItem(item);
            QString name = one + ' ' + QString::number(part.value("index", i + 1));
            if (part.contains("sign"))
                name += QStringLiteral(" (%1)").arg(valueText("", part["sign"]));
            if (part.value("is_main", false)) name += tr(" (main)");
            row->setText(0, name);
            row->setText(1, summary(part));
            for (const auto& [k, v] : part.items()) addRow(row, k, v, part);
            if (const auto box = boundsOf(part)) {
                row->setData(
                    0,
                    kBoundsRole,
                    QVariantList{
                        box->min.x, box->min.y, box->min.z, box->max.x, box->max.y, box->max.z});
                row->setToolTip(0, tr("Click to look at it"));
            }
            if (shells) {
                row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
                row->setCheckState(0, Qt::Unchecked);
                row->setData(0, kShellRole, part.value("index", static_cast<int>(i)));
            }
        }
        return;
    }
    QString text = valueText(key, value);
    if (text.size() > kLongText) {
        item->setToolTip(1, text);
        text = text.left(kLongText) + QStringLiteral("…");
    }
    item->setText(1, text);
    if (problem(key, value, in)) {
        QFont bold = item->font(0);
        bold.setBold(true);
        for (const int column : {0, 1}) {
            item->setForeground(column, QBrush(QColor(208, 138, 44)));
            item->setFont(column, bold);
        }
    }
}

std::vector<int> InspectorPanel::checkedShells() const {
    std::vector<int> out;
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if ((*it)->data(0, kShellRole).isValid() && (*it)->checkState(0) == Qt::Checked)
            out.push_back((*it)->data(0, kShellRole).toInt());
    }
    return out;
}

} // namespace m2c::app

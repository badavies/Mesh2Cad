// SPDX-License-Identifier: GPL-3.0-or-later
#include "ParamForm.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPoint>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <QLatin1String>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/Vec3.h"
#include "m2c/document/Document.h"

#include "Engine.h"

namespace m2c::app {

namespace {

QString label(const std::string& key) {
    QString text = QString::fromStdString(key);
    text.replace('_', ' ');
    if (text.endsWith(QLatin1String(" mm"))) text = text.left(text.size() - 3) + " (mm)";
    if (text.endsWith(QLatin1String(" deg")))
        text = text.left(text.size() - 4) + QStringLiteral(" (°)");
    if (!text.isEmpty()) text[0] = text[0].toUpper();
    return text;
}

QString valueText(const Json& v) {
    if (v.is_string()) return QString::fromStdString(v.get<std::string>());
    if (v.is_array() && std::ranges::all_of(v, [](const Json& x) { return x.is_number(); })) {
        QStringList parts;
        for (const Json& x : v) parts << QString::number(x.get<double>(), 'g', 12);
        return parts.join(", ");
    }
    if (v.is_number_integer()) return QString::number(v.get<long long>());
    if (v.is_number()) return QString::number(v.get<double>(), 'g', 12);
    return QString::fromStdString(v.dump());
}

/// The object kinds an "x-object" offers: one kind or a list; empty for any.
std::vector<std::string> kindsOf(const Json& x) {
    std::vector<std::string> kinds;
    for (const Json& kind : x.is_array() ? x : Json::array({x})) {
        if (kind != "any") kinds.push_back(kind.get<std::string>());
    }
    return kinds;
}

/// One item of a list of objects, with each property's ids as 1, for a placeholder:
/// [{"plane_id":1,"selection_id":1}].
QString example(const Json& items) {
    Json item = Json::object();
    for (const auto& [key, prop] : items["properties"].items()) {
        const std::string type = prop.value("type", "");
        item[key] = type == "integer" || type == "number" ? Json(1)
                    : type == "boolean"                   ? Json(true)
                    : type == "array"                     ? Json::array()
                                                          : Json("…");
    }
    return QString::fromStdString(Json::array({item}).dump());
}

/// A tuning parameter: optional, with a default, and not objects (an object, a list of them, or
/// a list of items made of them such as surface.build's flat areas).
bool isAdvanced(const Json& prop) {
    if (!prop.contains("default") || prop.contains("x-object")) return false;
    const Json items = prop.value("items", Json::object());
    return !items.contains("x-object") &&
           !std::ranges::any_of(items.value("properties", Json::object()),
                                [](const Json& p) { return p.contains("x-object"); });
}

/// The range a number may take, from the schema's minimum and maximum (exclusive ones a step
/// inside); unbounded sides are as large as a spin box holds.
std::pair<double, double> limits(const Json& prop, const bool integer) {
    const double step = integer ? 1.0 : 1e-6;
    double low = integer ? std::numeric_limits<int>::min() : -1e9;
    double high = integer ? std::numeric_limits<int>::max() : 1e9;
    if (prop.contains("minimum")) low = prop["minimum"].get<double>();
    if (prop.contains("exclusiveMinimum")) low = prop["exclusiveMinimum"].get<double>() + step;
    if (prop.contains("maximum")) high = prop["maximum"].get<double>();
    if (prop.contains("exclusiveMaximum")) high = prop["exclusiveMaximum"].get<double>() - step;
    return {low, high};
}

/// A spin box for millimetres and other numbers: up to six decimals, shown without trailing
/// zeros (0.1, not 0.100000).
class NumberBox : public QDoubleSpinBox {
public:
    NumberBox() {
        setDecimals(6);
        setStepType(AdaptiveDecimalStepType);
        QLocale numbers = locale();
        numbers.setNumberOptions(numbers.numberOptions() | QLocale::OmitGroupSeparator);
        setLocale(numbers);
    }
    [[nodiscard]] QString textFromValue(const double value) const override {
        return locale().toString(value, 'g', 12);
    }
};

} // namespace

ParamForm::ParamForm(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    title_ = new QLabel(tr("Choose a tool from the Tools menu or the workflow."));
    title_->setWordWrap(true);
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    fieldsHost_ = new QWidget;
    fields_ = new QFormLayout(fieldsHost_);
    fields_->setContentsMargins(0, 4, 0, 4);
    // Tuning parameters, folded away; open or closed stays as the person left it.
    advancedToggle_ = new QToolButton;
    advancedToggle_->setObjectName(QStringLiteral("advanced"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle_->setArrowType(Qt::RightArrow);
    advancedToggle_->setAutoRaise(true);
    advancedToggle_->hide();
    advancedHost_ = new QWidget;
    advanced_ = new QFormLayout(advancedHost_);
    advanced_->setContentsMargins(12, 0, 0, 4);
    advancedHost_->hide();
    connect(advancedToggle_, &QToolButton::toggled, this, [this](const bool open) {
        advancedOpen_ = open;
        advancedToggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        advancedHost_->setVisible(open);
    });
    run_ = new QPushButton(tr("Run"));
    run_->setDefault(true);
    run_->setEnabled(false);
    connect(run_, &QPushButton::clicked, this, &ParamForm::runRequested);
    reset_ = new QPushButton(tr("Reset to defaults"));
    reset_->setObjectName(QStringLiteral("reset"));
    reset_->setToolTip(
        tr("Put every setting back to its default; the objects chosen and points picked stay"));
    reset_->setEnabled(false);
    connect(reset_, &QPushButton::clicked, this, &ParamForm::resetToDefaults);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(reset_);
    buttons->addWidget(run_, 1);
    layout->addWidget(title_);
    layout->addWidget(summary_);
    layout->addWidget(fieldsHost_);
    layout->addWidget(advancedToggle_);
    layout->addWidget(advancedHost_);
    layout->addLayout(buttons);
    layout->addStretch(1);
}

void ParamForm::setCommand(const Command* command, const Json& values) {
    command_ = command;
    editors_.clear();
    while (fields_->rowCount() > 0) fields_->removeRow(0);
    while (advanced_->rowCount() > 0) advanced_->removeRow(0);
    advancedToggle_->hide();
    advancedHost_->hide();
    emit pointsChanged({});
    emit linesChanged({});
    if (command == nullptr) {
        title_->setText(tr("Choose a tool from the Tools menu or the workflow."));
        summary_->clear();
        run_->setEnabled(false);
        reset_->setEnabled(false);
        return;
    }
    title_->setText(QString::fromStdString(command->name));
    summary_->setText(QString::fromStdString(command->summary));
    run_->setEnabled(true);
    reset_->setEnabled(true);

    const Json& schema = command->paramsSchema;
    std::vector<std::string> required;
    if (schema.contains("required")) required = schema["required"].get<std::vector<std::string>>();
    const Json properties = schema.value("properties", Json::object());
    // The fields a person sets first lead, the tuning parameters the command has defaults for
    // follow under Advanced (JSON objects here keep their keys sorted, so the schema's own order
    // is lost).
    std::vector<std::string> keys;
    for (const auto& [key, prop] : properties.items()) keys.push_back(key);
    const auto rank = [&](const std::string& key) {
        const Json& prop = properties[key];
        const bool object = prop.contains("x-object") ||
                            (prop.contains("items") && prop["items"].contains("x-object"));
        const bool needed = std::ranges::find(required, key) != required.end();
        return key == "id"        ? 0
               : object && needed ? 1
               : needed           ? 2
               : object           ? 3
               : key == "name"    ? 5
               : isAdvanced(prop) ? 6
                                  : 4;
    };
    std::ranges::stable_sort(keys, std::ranges::less{}, rank);
    bool openAdvanced = false;
    int advancedCount = 0;
    for (const std::string& key : keys) {
        const Json& prop = properties[key];
        Field field;
        field.required = std::ranges::find(required, key) != required.end();
        field.advanced = !field.required && isAdvanced(prop);
        if (prop.contains("default")) field.fallback = prop["default"];
        const QString tip = QString::fromStdString(prop.value("description", ""));
        if (field.advanced) {
            ++advancedCount;
            if (values.contains(key) && values[key] != field.fallback) openAdvanced = true;
        }
        QWidget* row = makeEditor(key, prop, field, values);
        row->setToolTip(tip);
        QFormLayout* layout = field.advanced ? advanced_ : fields_;
        layout->addRow(label(key) + (field.required ? " *" : ""), row);
        editors_[key] = field;
    }
    if (advancedCount > 0) {
        advancedToggle_->setText(tr("Advanced (%1)").arg(advancedCount));
        advancedToggle_->show();
        // Opened for values passed in that differ from the defaults, without changing what the
        // person chose.
        const bool open = advancedOpen_ || openAdvanced;
        const QSignalBlocker quiet(advancedToggle_);
        advancedToggle_->setChecked(open);
        advancedToggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        advancedHost_->setVisible(open);
    }
    // Any edit says so, for previews of what the command will do.
    for (QWidget* host : {fieldsHost_, advancedHost_}) {
        for (auto* w : host->findChildren<QLineEdit*>())
            connect(w, &QLineEdit::textChanged, this, &ParamForm::valuesChanged);
        for (auto* w : host->findChildren<QComboBox*>())
            connect(w, &QComboBox::currentIndexChanged, this, &ParamForm::valuesChanged);
        for (auto* w : host->findChildren<QAbstractSpinBox*>()) {
            if (auto* d = qobject_cast<QDoubleSpinBox*>(w))
                connect(d, &QDoubleSpinBox::valueChanged, this, &ParamForm::valuesChanged);
            else if (auto* i = qobject_cast<QSpinBox*>(w))
                connect(i, &QSpinBox::valueChanged, this, &ParamForm::valuesChanged);
        }
        for (auto* w : host->findChildren<QCheckBox*>())
            connect(w, &QCheckBox::toggled, this, &ParamForm::valuesChanged);
    }
    emit valuesChanged();
}

QWidget*
ParamForm::makeEditor(const std::string& key, const Json& prop, Field& field, const Json& values) {
    const std::string type = prop.value("type", "");
    const QString name = QString::fromStdString(key);
    const bool has = values.contains(key);
    const Json& start = has ? values[key] : field.fallback;
    const Json& items = prop.value("items", Json::object());

    if (type == "boolean") {
        // Checked as the command's default; sent only when changed from it.
        auto* box = new QCheckBox;
        box->setObjectName(name);
        box->setChecked(start.is_boolean() && start.get<bool>());
        field.type = "boolean";
        field.editor = box;
        return box;
    }
    if (prop.contains("x-object")) {
        field.kinds = kindsOf(prop["x-object"]);
        auto* combo = new QComboBox;
        combo->setObjectName(name);
        field.type = "object";
        field.editor = combo;
        fillObjects(field, has ? values[key].get<ObjectId>() : 0);
        connect(combo, &QComboBox::activated, this, [this, combo] {
            if (const auto id = combo->currentData().value<ObjectId>(); id != 0)
                emit objectChosen(id);
        });
        return combo;
    }
    if (type == "string" && prop.contains("enum")) {
        auto* combo = new QComboBox;
        combo->setObjectName(name);
        combo->addItem(prop.contains("default") ? tr("Default (%1)").arg(valueText(prop["default"]))
                                                : tr("None"));
        for (const Json& option : prop["enum"]) combo->addItem(valueText(option));
        if (has) combo->setCurrentText(valueText(values[key]));
        field.type = "enum";
        field.editor = combo;
        return combo;
    }
    if (type == "array" && items.value("type", "") == "string" && items.contains("enum")) {
        // A set of options (mesh.segment's kinds): a box for each, checked as the default.
        auto* row = new QWidget;
        row->setObjectName(name);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        for (const Json& option : items["enum"]) {
            auto* box = new QCheckBox(valueText(option));
            box->setObjectName(QString::fromStdString(option.get<std::string>()));
            box->setChecked(start.is_array() && std::ranges::find(start, option) != start.end());
            h->addWidget(box);
            field.options.emplace_back(option.get<std::string>(), box);
        }
        h->addStretch(1);
        field.type = "options";
        field.editor = row;
        return row;
    }
    if ((type == "integer" || type == "number") && prop.contains("default")) {
        // A spin box within the schema's limits, starting at the default.
        const auto [low, high] = limits(prop, type == "integer");
        field.minimum = low;
        field.maximum = high;
        if (type == "integer") {
            auto* box = new QSpinBox;
            box->setRange(static_cast<int>(low), static_cast<int>(high));
            box->setValue(start.is_number() ? start.get<int>() : 0);
            field.editor = box;
        } else {
            auto* box = new NumberBox;
            box->setRange(low, high);
            box->setValue(start.is_number() ? start.get<double>() : 0.0);
            field.editor = box;
        }
        field.editor->setObjectName(name);
        field.type = type == "integer" ? "spin" : "dspin";
        return field.editor;
    }

    auto* edit = new QLineEdit;
    edit->setObjectName(name);
    field.editor = edit;
    if (has) edit->setText(valueText(values[key]));
    if (prop.contains("default"))
        edit->setPlaceholderText(tr("default %1").arg(valueText(prop["default"])));
    else if (!field.required)
        edit->setPlaceholderText(tr("optional"));
    if (type == "integer" || type == "number" || type == "string") {
        field.type = type;
        if (type != "string") {
            // Typed, so it can stay empty for the command to choose; held to the limits.
            const auto [low, high] = limits(prop, type == "integer");
            field.minimum = low;
            field.maximum = high;
        }
    } else if (type == "array" && items.value("type", "") == "number") {
        field.type = "vec3";
        if (edit->placeholderText().isEmpty()) edit->setPlaceholderText(QStringLiteral("x, y, z"));
    } else if (type == "array" && items.contains("x-object")) {
        // A list of objects by id, typed as their numbers: "12, 13, 14".
        field.type = "ids";
        const Json& x = items["x-object"];
        field.kinds = kindsOf(x);
        const QString kind = x.is_string() && x != "any"
                                 ? QString::fromStdString(x.get<std::string>()).replace('_', ' ')
                                 : tr("object");
        const QString hint = tr("%1 ids, e.g. 3, 4, 5").arg(kind);
        edit->setPlaceholderText(field.required ? hint : tr("optional: %1").arg(hint));
    } else if (type == "array" && items.value("type", "") == "array" &&
               items.value("items", Json::object()).value("type", "") == "number") {
        // A list of points, picked in the viewport or typed as JSON.
        field.type = "points";
        return pointsRow(edit, false);
    } else if (const Json point = items.value("items", Json());
               type == "array" && items.value("type", "") == "array" && point.is_object() &&
               point.value("type", "") == "array" &&
               point.value("items", Json::object()).value("type", "") == "number") {
        // A list of lines of points (quad.remesh's guides), picked the same way.
        field.type = "lines";
        return pointsRow(edit, true);
    } else {
        field.type = "json";
        if (type == "array" && items.contains("properties")) {
            // A list of objects: show one, so the shape to type is plain.
            edit->setPlaceholderText(tr("JSON, e.g. %1").arg(example(items)));
            for (const auto& [itemKey, itemProp] : items["properties"].items()) {
                if (!itemProp.contains("x-object")) {
                    field.items.clear();
                    break;
                }
                field.items.emplace_back(itemKey, kindsOf(itemProp["x-object"]));
            }
        } else if (!prop.contains("default")) {
            edit->setPlaceholderText(tr("JSON"));
        }
        // Every item is objects (surface.build's flat areas): build it from the rows picked in
        // the Objects list, with Add and Clear buttons.
        if (!field.items.empty()) return itemsRow(key, field, edit);
    }
    if (key == "path") return pathRow(prop, edit);
    return edit;
}

QWidget* ParamForm::pointsRow(QLineEdit* edit, const bool lines) {
    edit->setPlaceholderText(lines ? tr("click in the view, or JSON [[[x, y, z], ...], ...]")
                                   : tr("click in the view, or JSON [[x, y, z], ...]"));
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* count = new QLabel;
    count->setObjectName(QStringLiteral("count"));
    auto* undo = new QToolButton;
    undo->setText(tr("Undo last"));
    undo->setObjectName(QStringLiteral("undoPoint"));
    auto* clear = new QToolButton;
    clear->setText(tr("Clear"));
    clear->setObjectName(QStringLiteral("clear"));
    connect(clear, &QToolButton::clicked, edit, &QLineEdit::clear);
    const auto parse = [edit] {
        const Json list = Json::parse(edit->text().toStdString(), nullptr, false);
        return list.is_array() ? list : Json::array();
    };
    const auto show = [edit](const Json& list) {
        edit->setText(list.empty() ? QString() : QString::fromStdString(list.dump()));
    };
    // The last point goes; with lines, from the last line, and an emptied line with it.
    connect(undo, &QToolButton::clicked, edit, [parse, show, lines] {
        Json list = parse();
        if (lines) {
            while (!list.empty() && (!list.back().is_array() || list.back().empty()))
                list.erase(list.end() - 1);
            if (!list.empty()) list.back().erase(list.back().end() - 1);
            if (!list.empty() && list.back().empty()) list.erase(list.end() - 1);
        } else if (!list.empty()) {
            list.erase(list.end() - 1);
        }
        show(list);
    });
    QToolButton* next = nullptr;
    if (lines) {
        // Clicks go on the last line; New line starts another.
        next = new QToolButton;
        next->setText(tr("New line"));
        next->setObjectName(QStringLiteral("newLine"));
        connect(next, &QToolButton::clicked, edit, [parse, show] {
            Json list = parse();
            if (!list.empty() && list.back().is_array() && !list.back().empty())
                list.push_back(Json::array());
            show(list);
        });
    }
    // The count, and the markers in the viewport, follow the list as it is picked or typed.
    const auto changed = [this, parse, count, undo, next, lines] {
        const auto point = [](const Json& p) -> std::optional<Vec3d> {
            if (!p.is_array() || p.size() != 3 ||
                !std::ranges::all_of(p, [](const Json& x) { return x.is_number(); }))
                return std::nullopt;
            return Vec3d{p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
        };
        std::vector<std::vector<Vec3d>> found;
        for (const Json& item : parse()) {
            if (!lines) {
                if (const auto p = point(item)) found.push_back({*p});
                continue;
            }
            found.emplace_back();
            for (const Json& p : item.is_array() ? item : Json::array()) {
                if (const auto q = point(p)) found.back().push_back(*q);
            }
        }
        std::vector<Vec3d> points;
        for (const auto& line : found) points.insert(points.end(), line.begin(), line.end());
        const QString n = points.size() == 1 ? tr("1 point") : tr("%1 points").arg(points.size());
        std::erase_if(found, [](const auto& line) { return line.empty(); });
        count->setText(!lines              ? n
                       : found.size() == 1 ? tr("1 line, %1").arg(n)
                                           : tr("%1 lines, %2").arg(found.size()).arg(n));
        undo->setEnabled(!points.empty());
        if (next != nullptr) next->setEnabled(!points.empty());
        emit pointsChanged(points);
        emit linesChanged(lines ? found : std::vector<std::vector<Vec3d>>{});
    };
    connect(edit, &QLineEdit::textChanged, this, changed);
    changed();
    h->addWidget(edit, 1);
    h->addWidget(count);
    if (next != nullptr) h->addWidget(next);
    h->addWidget(undo);
    h->addWidget(clear);
    return row;
}

QWidget* ParamForm::pathRow(const Json& prop, QLineEdit* edit) {
    // A file chooser next to path fields, showing the files the command takes; commands that
    // write files ask for a new one.
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QToolButton;
    browse->setText(QStringLiteral("…"));
    browse->setObjectName(QStringLiteral("browse"));
    const bool saving = command_->writesFiles;
    const QString filter = fileFilter(prop);
    browse->setProperty("saving", saving);
    browse->setProperty("filter", filter);
    connect(browse, &QToolButton::clicked, this, [this, edit, saving, filter] {
        const QString file =
            saving ? QFileDialog::getSaveFileName(this, tr("Save"), edit->text(), filter)
                   : QFileDialog::getOpenFileName(this, tr("Open"), edit->text(), filter);
        if (!file.isEmpty()) edit->setText(file);
    });
    h->addWidget(edit, 1);
    h->addWidget(browse);
    return row;
}

void ParamForm::setObjects(std::shared_ptr<const DocSnapshot> snapshot) {
    snapshot_ = std::move(snapshot);
    for (const Field& field : std::views::values(editors_)) {
        if (field.type == "object") {
            fillObjects(field,
                        static_cast<QComboBox*>(field.editor)->currentData().value<ObjectId>());
        }
    }
}

void ParamForm::setSelectedObjects(const std::vector<ObjectId>& ids) {
    picked_ = ids;
    if (!snapshot_) return;
    const auto fits = [this](const Field& field, const ObjectId id) {
        const DocumentObject* o = snapshot_->find(id);
        return o != nullptr && (field.kinds.empty() ||
                                std::ranges::find(field.kinds, o->kind()) != field.kinds.end());
    };
    // Single fields in order: id first, then the rest by name (the schema's own order is lost,
    // as JSON objects here keep their keys sorted).
    std::vector<std::string> keys;
    if (editors_.contains("id")) keys.emplace_back("id");
    for (const std::string& key : std::views::keys(editors_)) {
        if (key != "id") keys.push_back(key);
    }
    // Fields of named kinds take their objects before a field of any kind; of those only id is
    // filled (focus_id, highlight_id and the like are left to the person).
    std::ranges::stable_partition(
        keys, [this](const std::string& key) { return !editors_.at(key).kinds.empty(); });
    std::vector<ObjectId> used;
    for (const std::string& key : keys) {
        const Field& field = editors_.at(key);
        if (field.type == "object" && field.kinds.empty() && key != "id") continue;
        if (field.type == "ids") {
            QStringList matching;
            for (const ObjectId id : ids) {
                if (fits(field, id)) matching << QString::number(id);
            }
            if (!matching.isEmpty())
                static_cast<QLineEdit*>(field.editor)->setText(matching.join(QStringLiteral(", ")));
        } else if (field.type == "object") {
            for (const ObjectId id : ids) {
                if (!fits(field, id) || std::ranges::find(used, id) != used.end()) continue;
                auto* combo = static_cast<QComboBox*>(field.editor);
                if (const int index = combo->findData(QVariant::fromValue(id)); index >= 0) {
                    combo->setCurrentIndex(index);
                    used.push_back(id);
                    break;
                }
            }
        }
    }
}

QWidget* ParamForm::itemsRow(const std::string& key, const Field& field, QLineEdit* edit) {
    QStringList names;
    for (const auto& kinds : std::views::values(field.items)) {
        QStringList either;
        for (const std::string& kind : kinds)
            either << QString::fromStdString(kind).replace('_', ' ');
        names << (either.isEmpty() ? tr("an object") : tr("a %1").arg(either.join(tr(" or "))));
    }
    const QString pick =
        tr("Select %1 in the Objects list (Ctrl+click), then Add").arg(names.join(tr(" and ")));
    // Keeps the example typed JSON shows.
    edit->setPlaceholderText(
        tr("Pick %1, then Add; or %2").arg(names.join(tr(" and ")), edit->placeholderText()));
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* add = new QToolButton;
    add->setText(tr("Add"));
    add->setObjectName(QStringLiteral("add"));
    add->setToolTip(pick);
    connect(add, &QToolButton::clicked, this, [this, key, add, pick] {
        if (!addPicked(key))
            QToolTip::showText(add->mapToGlobal(QPoint(0, add->height())), pick, add);
    });
    auto* clear = new QToolButton;
    clear->setText(tr("Clear"));
    clear->setObjectName(QStringLiteral("clear"));
    connect(clear, &QToolButton::clicked, edit, &QLineEdit::clear);
    h->addWidget(edit, 1);
    h->addWidget(add);
    h->addWidget(clear);
    return row;
}

void ParamForm::resetToDefaults() {
    if (command_ == nullptr) return;
    // Every setting goes back to its default; the objects chosen and points picked stay.
    Json keep = Json::object();
    for (const auto& [key, field] : editors_) {
        if (field.type == "object") {
            if (const auto id =
                    static_cast<QComboBox*>(field.editor)->currentData().value<ObjectId>();
                id != 0)
                keep[key] = id;
        } else if (field.type == "ids" || field.type == "points" || field.type == "lines") {
            if (const QString text = static_cast<QLineEdit*>(field.editor)->text(); !text.isEmpty())
                keep[key] = text.toStdString();
        }
    }
    setCommand(command_, keep);
}

bool ParamForm::addPicked(const std::string& key) {
    const auto it = editors_.find(key);
    if (it == editors_.end() || it->second.items.empty() || !snapshot_) return false;
    const Field& field = it->second;
    const auto fits = [this](const std::vector<std::string>& kinds, const ObjectId id) {
        const DocumentObject* o = snapshot_->find(id);
        return o != nullptr &&
               (kinds.empty() || std::ranges::find(kinds, o->kind()) != kinds.end());
    };
    // Each property takes the first picked object of its kinds, in the order they were picked.
    Json item = Json::object();
    std::vector<ObjectId> used;
    for (const auto& [itemKey, kinds] : field.items) {
        for (const ObjectId id : picked_) {
            if (fits(kinds, id) && std::ranges::find(used, id) == used.end()) {
                item[itemKey] = id;
                used.push_back(id);
                break;
            }
        }
    }
    // One not picked is the newest object of its kinds made from a picked one, or that a picked
    // one was made from, so a selection alone brings the plane fitted to it and the other way
    // round.
    const auto related = [this](const DocumentObject& o, const ObjectId id) {
        const DocumentObject* other = snapshot_->find(id);
        return std::ranges::find(o.provenance.inputs, id) != o.provenance.inputs.end() ||
               (other != nullptr && std::ranges::find(other->provenance.inputs, o.id) !=
                                        other->provenance.inputs.end());
    };
    for (const auto& [itemKey, kinds] : field.items) {
        if (item.contains(itemKey) || used.empty()) continue;
        for (const DocumentObject& o : std::views::reverse(snapshot_->objects)) {
            if (!fits(kinds, o.id) || std::ranges::find(used, o.id) != used.end()) continue;
            if (std::ranges::any_of(used, [&](const ObjectId id) { return related(o, id); })) {
                item[itemKey] = o.id;
                used.push_back(o.id);
                break;
            }
        }
    }
    if (item.size() != field.items.size()) return false;
    auto* edit = static_cast<QLineEdit*>(field.editor);
    Json list = Json::parse(edit->text().toStdString(), nullptr, false);
    if (!list.is_array()) list = Json::array();
    if (std::ranges::find(list, item) == list.end()) list.push_back(item);
    edit->setText(QString::fromStdString(list.dump()));
    return true;
}

bool ParamForm::appendPoint(const std::string& key, const Vec3d& point) {
    const auto it = editors_.find(key);
    if (it == editors_.end() || (it->second.type != "points" && it->second.type != "lines"))
        return false;
    auto* edit = static_cast<QLineEdit*>(it->second.editor);
    Json points = Json::parse(edit->text().toStdString(), nullptr, false);
    if (!points.is_array()) points = Json::array();
    // A line field takes the point on its last line.
    if (it->second.type == "lines") {
        if (points.empty() || !points.back().is_array()) points.push_back(Json::array());
        points.back().push_back(Json::array({point.x, point.y, point.z}));
    } else {
        points.push_back(Json::array({point.x, point.y, point.z}));
    }
    edit->setText(QString::fromStdString(points.dump()));
    return true;
}

void ParamForm::fillObjects(const Field& field, const ObjectId chosen) const {
    auto* combo = static_cast<QComboBox*>(field.editor);
    combo->clear();
    QStringList names;
    for (const std::string& kind : field.kinds)
        names << QString::fromStdString(kind).replace('_', ' ');
    const QString prompt =
        names.isEmpty() ? tr("Choose an object") : tr("Choose a %1").arg(names.join(tr(" or ")));
    combo->addItem(field.required ? prompt : tr("None"), QVariant::fromValue(ObjectId{0}));
    if (!snapshot_) return;
    for (const DocumentObject& o : snapshot_->objects) {
        if (!field.kinds.empty() && std::ranges::find(field.kinds, o.kind()) == field.kinds.end())
            continue;
        combo->addItem(QStringLiteral("%1 (#%2)").arg(QString::fromStdString(o.name)).arg(o.id),
                       QVariant::fromValue(o.id));
        if (o.id == chosen) combo->setCurrentIndex(combo->count() - 1);
    }
}

Result<Json> ParamForm::params() const {
    Json out = Json::object();
    for (const auto& [key, field] : editors_) {
        const auto bad = [&key](const QString& why) {
            return fail(ErrorCode::InvalidArgument, (label(key) + ": " + why).toStdString());
        };
        // Settings at the command's default are left out, so its own default applies.
        const auto put = [&](const Json& v) {
            if (v != field.fallback) out[key] = v;
        };
        if (field.type == "boolean") {
            put(static_cast<QCheckBox*>(field.editor)->isChecked());
            continue;
        }
        if (field.type == "spin") {
            put(static_cast<QSpinBox*>(field.editor)->value());
            continue;
        }
        if (field.type == "dspin") {
            const double v = static_cast<QDoubleSpinBox*>(field.editor)->value();
            if (!field.fallback.is_number() ||
                std::abs(v - field.fallback.get<double>()) > 1e-9 * std::max(1.0, std::abs(v)))
                out[key] = v;
            continue;
        }
        if (field.type == "options") {
            Json chosen = Json::array();
            for (const auto& [option, box] : field.options) {
                if (box->isChecked()) chosen.push_back(option);
            }
            if (chosen.empty()) return bad(tr("choose at least one"));
            put(chosen);
            continue;
        }
        if (field.type == "object") {
            if (const auto id =
                    static_cast<QComboBox*>(field.editor)->currentData().value<ObjectId>();
                id != 0) {
                out[key] = id;
            } else if (field.required) {
                return bad(tr("required"));
            }
            continue;
        }
        if (field.type == "enum") {
            if (const auto* combo = static_cast<QComboBox*>(field.editor);
                combo->currentIndex() > 0)
                out[key] = combo->currentText().toStdString();
            continue;
        }
        const QString text = static_cast<QLineEdit*>(field.editor)->text().trimmed();
        if (text.isEmpty()) {
            if (field.required) return bad(tr("required"));
            continue;
        }
        bool ok = false;
        const auto inRange = [&](const double v) -> Result<void> {
            if (v < field.minimum) return bad(tr("at least %1").arg(field.minimum));
            if (v > field.maximum) return bad(tr("at most %1").arg(field.maximum));
            return {};
        };
        if (field.type == "integer") {
            const long long v = text.toLongLong(&ok);
            if (!ok) return bad(tr("expected a whole number"));
            if (auto in = inRange(static_cast<double>(v)); !in) return std::unexpected(in.error());
            out[key] = v;
        } else if (field.type == "number") {
            const double v = text.toDouble(&ok);
            if (!ok) return bad(tr("expected a number"));
            if (auto in = inRange(v); !in) return std::unexpected(in.error());
            out[key] = v;
        } else if (field.type == "string") {
            out[key] = text.toStdString();
        } else if (field.type == "ids") {
            Json list = Json::array();
            for (const QString& part :
                 text.split(QRegularExpression(R"([\[\],\s]+)"), Qt::SkipEmptyParts)) {
                const long long v = part.toLongLong(&ok);
                if (!ok || v < 0) return bad(tr("expected object ids separated by commas"));
                list.push_back(v);
            }
            out[key] = list;
        } else if (field.type == "vec3") {
            Json list = Json::array();
            for (const QString& part :
                 text.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)) {
                const double v = part.toDouble(&ok);
                if (!ok) return bad(tr("expected numbers separated by commas"));
                list.push_back(v);
            }
            out[key] = list;
        } else {
            Json v = Json::parse(text.toStdString(), nullptr, false);
            if (v.is_discarded()) return bad(tr("not valid JSON"));
            // A line started with New line but not picked yet is left out.
            if (field.type == "lines" && v.is_array()) {
                Json kept = Json::array();
                for (const Json& line : v) {
                    if (!line.is_array() || !line.empty()) kept.push_back(line);
                }
                if (kept.empty() && !field.required) continue;
                v = std::move(kept);
            }
            out[key] = v;
        }
    }
    return out;
}

void ParamForm::setBusy(const bool busy) const {
    run_->setEnabled(!busy && command_ != nullptr);
    reset_->setEnabled(!busy && command_ != nullptr);
    run_->setText(busy ? tr("Working…") : tr("Run"));
}

QString ParamForm::fileFilter(const Json& property) {
    QString all = tr("All files (*)");
    if (!property.contains("x-file-types") || !property["x-file-types"].is_array()) return all;
    QStringList names;
    QStringList patterns;
    for (const Json& type : property["x-file-types"]) {
        if (!type.is_string()) continue;
        const QString extension = QString::fromStdString(type.get<std::string>());
        names.append(extension.toUpper());
        patterns.append(QStringLiteral("*.") + extension);
    }
    if (patterns.isEmpty()) return all;
    return QStringLiteral("%1 (%2);;%3")
        .arg(names.join(QStringLiteral(", ")), patterns.join(QLatin1Char(' ')), all);
}

} // namespace m2c::app

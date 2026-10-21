// SPDX-License-Identifier: GPL-3.0-or-later
#include "ParamForm.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <Qt>
#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

#include <QLatin1String>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"

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
    run_ = new QPushButton(tr("Run"));
    run_->setDefault(true);
    run_->setEnabled(false);
    connect(run_, &QPushButton::clicked, this, &ParamForm::runRequested);
    layout->addWidget(title_);
    layout->addWidget(summary_);
    layout->addWidget(fieldsHost_);
    layout->addWidget(run_);
    layout->addStretch(1);
}

void ParamForm::setCommand(const Command* command, const Json& values) {
    command_ = command;
    editors_.clear();
    while (fields_->rowCount() > 0) fields_->removeRow(0);
    if (command == nullptr) {
        title_->setText(tr("Choose a tool from the Tools menu or the workflow."));
        summary_->clear();
        run_->setEnabled(false);
        return;
    }
    title_->setText(QString::fromStdString(command->name));
    summary_->setText(QString::fromStdString(command->summary));
    run_->setEnabled(true);

    const Json& schema = command->paramsSchema;
    std::vector<std::string> required;
    if (schema.contains("required")) required = schema["required"].get<std::vector<std::string>>();
    for (const Json properties = schema.value("properties", Json::object());
         const auto& [key, prop] : properties.items()) {
        Field field;
        field.required = std::ranges::find(required, key) != required.end();
        const std::string type = prop.value("type", "");
        const QString tip = QString::fromStdString(prop.value("description", ""));
        const bool has = values.contains(key);
        QString placeholder;
        if (prop.contains("default"))
            placeholder = tr("default %1").arg(valueText(prop["default"]));
        else if (!field.required)
            placeholder = tr("optional");

        if (type == "boolean") {
            auto* combo = new QComboBox;
            combo->addItem(
                prop.contains("default")
                    ? tr("Default (%1)").arg(prop["default"].get<bool>() ? tr("yes") : tr("no"))
                    : tr("Default"));
            combo->addItem(tr("Yes"));
            combo->addItem(tr("No"));
            if (has) combo->setCurrentIndex(values[key].get<bool>() ? 1 : 2);
            field.type = "boolean";
            field.editor = combo;
            combo->setObjectName(QString::fromStdString(key));
        } else if (prop.contains("x-object")) {
            const Json& x = prop["x-object"];
            for (const Json& kind : x.is_array() ? x : Json::array({x})) {
                if (kind != "any") field.kinds.push_back(kind.get<std::string>());
            }
            auto* combo = new QComboBox;
            combo->setObjectName(QString::fromStdString(key));
            field.type = "object";
            field.editor = combo;
            fillObjects(field, has ? values[key].get<ObjectId>() : 0);
            connect(combo, &QComboBox::activated, this, [this, combo] {
                if (const auto id = combo->currentData().value<ObjectId>(); id != 0)
                    emit objectChosen(id);
            });
        } else if (type == "string" && prop.contains("enum")) {
            auto* combo = new QComboBox;
            combo->addItem(prop.contains("default")
                               ? tr("Default (%1)").arg(valueText(prop["default"]))
                               : tr("None"));
            for (const Json& option : prop["enum"]) combo->addItem(valueText(option));
            if (has) combo->setCurrentText(valueText(values[key]));
            field.type = "enum";
            field.editor = combo;
            combo->setObjectName(QString::fromStdString(key));
        } else {
            auto* edit = new QLineEdit;
            edit->setPlaceholderText(placeholder);
            if (has) edit->setText(valueText(values[key]));
            if (type == "integer" || type == "number" || type == "string") {
                field.type = type;
            } else if (type == "array" && prop.contains("items") &&
                       prop["items"].value("type", "") == "number") {
                field.type = "vec3";
                if (placeholder.isEmpty()) edit->setPlaceholderText(QStringLiteral("x, y, z"));
            } else if (type == "array" && prop.contains("items") &&
                       prop["items"].contains("x-object")) {
                // A list of objects by id, typed as their numbers: "12, 13, 14".
                field.type = "ids";
                const Json& x = prop["items"]["x-object"];
                const QString kind =
                    x.is_string() && x != "any"
                        ? QString::fromStdString(x.get<std::string>()).replace('_', ' ')
                        : tr("object");
                const QString hint = tr("%1 ids, e.g. 3, 4, 5").arg(kind);
                edit->setPlaceholderText(field.required ? hint : tr("optional: %1").arg(hint));
            } else {
                field.type = "json";
                if (type == "array" && prop.contains("items") &&
                    prop["items"].contains("properties")) {
                    // A list of objects: show one, so the shape to type is plain.
                    edit->setPlaceholderText(tr("JSON, e.g. %1").arg(example(prop["items"])));
                } else if (placeholder.isEmpty() || placeholder == tr("optional")) {
                    edit->setPlaceholderText(tr("JSON"));
                }
            }
            field.editor = edit;
            edit->setObjectName(QString::fromStdString(key));
            if (key == "path") {
                // A file chooser next to path fields, showing the files the command takes;
                // commands that write files ask for a new one.
                auto* row = new QWidget;
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(0, 0, 0, 0);
                auto* browse = new QToolButton;
                browse->setText(QStringLiteral("…"));
                browse->setObjectName(QStringLiteral("browse"));
                const bool saving = command->writesFiles;
                const QString filter = fileFilter(prop);
                browse->setProperty("saving", saving);
                browse->setProperty("filter", filter);
                connect(browse, &QToolButton::clicked, this, [this, edit, saving, filter] {
                    const QString file =
                        saving
                            ? QFileDialog::getSaveFileName(this, tr("Save"), edit->text(), filter)
                            : QFileDialog::getOpenFileName(this, tr("Open"), edit->text(), filter);
                    if (!file.isEmpty()) edit->setText(file);
                });
                h->addWidget(edit, 1);
                h->addWidget(browse);
                fields_->addRow(label(key) + (field.required ? " *" : ""), row);
                row->setToolTip(tip);
                editors_[key] = field;
                continue;
            }
        }
        field.editor->setToolTip(tip);
        fields_->addRow(label(key) + (field.required ? " *" : ""), field.editor);
        editors_[key] = field;
    }
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
        if (field.type == "boolean") {
            if (const int index = static_cast<QComboBox*>(field.editor)->currentIndex(); index > 0)
                out[key] = index == 1;
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
        if (field.type == "integer") {
            const long long v = text.toLongLong(&ok);
            if (!ok) return bad(tr("expected a whole number"));
            out[key] = v;
        } else if (field.type == "number") {
            const double v = text.toDouble(&ok);
            if (!ok) return bad(tr("expected a number"));
            out[key] = v;
        } else if (field.type == "string") {
            out[key] = text.toStdString();
        } else if (field.type == "ids") {
            Json list = Json::array();
            for (const QString& part :
                 text.split(QRegularExpression("[\\[\\],\\s]+"), Qt::SkipEmptyParts)) {
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
            const Json v = Json::parse(text.toStdString(), nullptr, false);
            if (v.is_discarded()) return bad(tr("not valid JSON"));
            out[key] = v;
        }
    }
    return out;
}

void ParamForm::setBusy(const bool busy) const {
    run_->setEnabled(!busy && command_ != nullptr);
    run_->setText(busy ? tr("Working…") : tr("Run"));
}

QString ParamForm::fileFilter(const Json& property) {
    const QString all = tr("All files (*)");
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

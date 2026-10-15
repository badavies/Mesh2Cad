// SPDX-License-Identifier: GPL-3.0-or-later
#include "ParamForm.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

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
    if (v.is_array() &&
        std::all_of(v.begin(), v.end(), [](const Json& x) { return x.is_number(); })) {
        QStringList parts;
        for (const Json& x : v) parts << QString::number(x.get<double>(), 'g', 12);
        return parts.join(", ");
    }
    if (v.is_number_integer()) return QString::number(v.get<long long>());
    if (v.is_number()) return QString::number(v.get<double>(), 'g', 12);
    return QString::fromStdString(v.dump());
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
    summary_->setStyleSheet("color: palette(mid);");
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
    const Json properties = schema.value("properties", Json::object());
    for (const auto& [key, prop] : properties.items()) {
        Field field;
        field.required = std::find(required.begin(), required.end(), key) != required.end();
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
        } else if (type == "string" && prop.contains("enum")) {
            auto* combo = new QComboBox;
            combo->addItem(prop.contains("default")
                               ? tr("Default (%1)").arg(valueText(prop["default"]))
                               : tr("Default"));
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
            } else {
                field.type = "json";
                if (placeholder.isEmpty() || placeholder == tr("optional")) {
                    edit->setPlaceholderText(tr("JSON"));
                }
            }
            field.editor = edit;
            edit->setObjectName(QString::fromStdString(key));
            if (key == "path") {
                // A file chooser next to path fields; saving commands ask for a new file.
                auto* row = new QWidget;
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(0, 0, 0, 0);
                auto* browse = new QToolButton;
                browse->setText(QStringLiteral("…"));
                const bool saving = command->name.find("export") != std::string::npos ||
                                    command->name.find("save") != std::string::npos;
                const bool project = command->name.rfind("project.", 0) == 0;
                connect(browse, &QToolButton::clicked, this, [this, edit, saving, project] {
                    const QString filter =
                        project ? tr("Mesh2Cad project (*.m2c)") : tr("Meshes (*.stl *.obj *.ply)");
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

Result<Json> ParamForm::params() const {
    Json out = Json::object();
    for (const auto& [key, field] : editors_) {
        const auto bad = [&key](const QString& why) {
            return fail(ErrorCode::InvalidArgument, (label(key) + ": " + why).toStdString());
        };
        if (field.type == "boolean") {
            const int index = static_cast<QComboBox*>(field.editor)->currentIndex();
            if (index > 0) out[key] = index == 1;
            continue;
        }
        if (field.type == "enum") {
            const auto* combo = static_cast<QComboBox*>(field.editor);
            if (combo->currentIndex() > 0) out[key] = combo->currentText().toStdString();
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
            out[key] = v;
        }
    }
    return out;
}

void ParamForm::setBusy(bool busy) {
    run_->setEnabled(!busy && command_ != nullptr);
    run_->setText(busy ? tr("Working…") : tr("Run"));
}

} // namespace m2c::app

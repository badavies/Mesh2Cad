// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <map>
#include <string>

#include "m2c/commands/Command.h"

class QFormLayout;
class QLabel;
class QPushButton;

namespace m2c::app {

/// A form for one command, generated from its JSON schema, so every command gets a UI without
/// hand-written dialogs and the UI can never offer something an agent cannot do.
///
/// Empty fields are left out of the parameters, so the command's own defaults apply (shown as
/// placeholder text).
class ParamForm : public QWidget {
    Q_OBJECT

public:
    explicit ParamForm(QWidget* parent = nullptr);

    /// Shows the form for `command`, with `values` filled in (for example the current mesh id).
    void setCommand(const Command* command, const Json& values = Json::object());
    [[nodiscard]] const Command* command() const noexcept { return command_; }

    /// Reads the fields. Fails with a message naming the bad field.
    [[nodiscard]] Result<Json> params() const;

    void setBusy(bool busy);

signals:
    void runRequested();

private:
    struct Field {
        std::string type; ///< integer, number, boolean, string, enum, vec3, json
        QWidget* editor{nullptr};
        bool required{false};
    };

    const Command* command_{nullptr};
    QLabel* title_{nullptr};
    QLabel* summary_{nullptr};
    QWidget* fieldsHost_{nullptr};
    QFormLayout* fields_{nullptr};
    QPushButton* run_{nullptr};
    std::map<std::string, Field> editors_;
};

} // namespace m2c::app

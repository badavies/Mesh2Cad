// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"

#include "Engine.h"

class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace m2c::app {

/// A form for one command, generated from its JSON schema, so every command gets a UI without
/// hand-written dialogs and the UI can never offer something an agent cannot do.
///
/// Empty fields are left out of the parameters, so the command's own defaults apply (shown as
/// placeholder text). A parameter whose schema has "x-object" is a drop-down of the document's
/// objects of those kinds.
class ParamForm : public QWidget {
    Q_OBJECT

public:
    explicit ParamForm(QWidget* parent = nullptr);

    /// Shows the form for `command`, with `values` filled in (for example the current mesh id).
    void setCommand(const Command* command, const Json& values = Json::object());
    /// The objects the drop-downs offer; called whenever the document changes. Choices are kept
    /// while their objects exist.
    void setObjects(std::shared_ptr<const DocSnapshot> snapshot);
    [[nodiscard]] const Command* command() const noexcept { return command_; }
    /// Fills object fields from objects selected in the Objects list, in selection order: an id
    /// list gets the selected ids of its kinds, and single object fields take the selected ids
    /// of their kinds one each, id first and then by name (so solid.boolean's id, then tool_id).
    /// A field none of the selected objects fits is left as it is.
    void setSelectedObjects(const std::vector<ObjectId>& ids);
    /// Adds an item to a list-of-objects field (surface.build's flat) from the objects selected
    /// in the Objects list, as its Add button does: each property takes the first selected
    /// object of its kinds; one none fits takes the newest object of its kinds made from a
    /// selected one or that a selected one was made from (the plane fitted to a selection, or
    /// the selection a plane was fitted to). False, changing nothing, when a property stays
    /// empty or the form has no such field.
    bool addPicked(const std::string& key);
    /// Adds a point to a list-of-points field (fillet.fit's edge_points), as a click in the
    /// viewport does. False when the form has no such field.
    bool appendPoint(const std::string& key, const Vec3d& point);

    /// Reads the fields. Fails with a message naming the bad field.
    [[nodiscard]] Result<Json> params() const;

    void setBusy(bool busy) const;

    /// The file dialog filter for a path parameter, from its "x-file-types" (extensions without
    /// the dot), e.g. "STEP, STP (*.step *.stp);;All files (*)". The first entry is the default,
    /// so a typed name gets the right extension.
    [[nodiscard]] static QString fileFilter(const Json& property);

signals:
    void runRequested();
    /// The person picked an object in a drop-down.
    void objectChosen(m2c::ObjectId id);

private:
    struct Field {
        std::string
            type; ///< integer, number, boolean, string, enum, vec3, ids, points, json, object
        QWidget* editor{nullptr};
        bool required{false};
        std::vector<std::string> kinds; ///< object, ids: the kinds offered; empty for any
        /// json, a list whose items are all objects: each item property and its kinds.
        std::vector<std::pair<std::string, std::vector<std::string>>> items;
    };

    void fillObjects(const Field& field, ObjectId chosen) const;
    void addItemsRow(const std::string& key, Field& field, QLineEdit* edit, const QString& tip);

    const Command* command_{nullptr};
    QLabel* title_{nullptr};
    QLabel* summary_{nullptr};
    QWidget* fieldsHost_{nullptr};
    QFormLayout* fields_{nullptr};
    QPushButton* run_{nullptr};
    std::map<std::string, Field> editors_;
    std::shared_ptr<const DocSnapshot> snapshot_;
    std::vector<ObjectId> picked_; ///< Selected in the Objects list, in the order picked.
};

} // namespace m2c::app

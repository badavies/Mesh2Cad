// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "m2c/core/TriMesh.h"

#include "Engine.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace m2c::app {

/// The Inspector: a read-only command's result (mesh.info, solid.info, deviation.info,
/// shells.detect, ...) as rows of names and values instead of raw JSON, with units in the
/// names, problems (open edges, an invalid solid, a deviation past its tolerance) in amber and
/// long text cut short. A row of a list of parts with bounds (deviation hotspots, shells) moves
/// the view to that part when clicked. A shell list selects the checked shells, keeps the main
/// one or splits the others off; a deviation selects its hotspots. Those are commands it asks
/// for, so nothing is computed here.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);

    /// Shows `result`, the answer of `command`.
    void showResult(const QString& command, const Json& result);
    [[nodiscard]] const QString& command() const noexcept { return command_; }
    [[nodiscard]] const Json& result() const noexcept { return result_; }
    [[nodiscard]] QTreeWidget* tree() const noexcept { return tree_; }
    /// The shells checked in a shells.detect result, by index.
    [[nodiscard]] std::vector<int> checkedShells() const;

signals:
    /// A row with bounds was clicked: the box to look at.
    void boundsClicked(const m2c::Aabb& box);
    /// One of the result's actions was pressed: the command to run.
    void commandRequested(const QString& command, const m2c::Json& params);

private:
    void addRow(QTreeWidgetItem* parent, const std::string& key, const Json& value, const Json& in);

    QLabel* title_{nullptr};
    QTreeWidget* tree_{nullptr};
    QWidget* shellActions_{nullptr};
    QPushButton* selectHotspots_{nullptr};
    QString command_;
    Json result_;
};

} // namespace m2c::app

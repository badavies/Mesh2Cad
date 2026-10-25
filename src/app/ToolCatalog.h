// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <string>
#include <vector>

namespace m2c::app {

/// What the app calls a command: a title a person reads ("Fill holes") and the group of the
/// Tools menu it is in, by the task it serves ("Mesh cleanup"). The command keeps its own name
/// as its id. A command the catalogue does not know shows its name under its first dotted
/// part, so a new command still appears.
struct ToolInfo {
    QString title;
    QString group;
    int order{0}; ///< Its place in the menu: the catalogue's order, unknown commands last.
};

[[nodiscard]] ToolInfo toolInfo(const std::string& command);

/// The Tools menu's groups in menu order. Commands hidden from the menu (doc.*,
/// commands.list) have an empty group.
[[nodiscard]] const std::vector<QString>& toolGroups();

} // namespace m2c::app

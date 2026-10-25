// SPDX-License-Identifier: GPL-3.0-or-later
#include "ToolCatalog.h"

#include <QObject>
#include <QString>
#include <algorithm>
#include <string>
#include <vector>

namespace m2c::app {

namespace {

enum Group : unsigned char {
    Cleanup,
    Selection,
    Alignment,
    Primitives,
    Solids,
    Surfacing,
    Inspection,
    Files,
    Objects,
    Hidden
};

struct Entry {
    const char* command;
    const char* title;
    Group group;
};

/// In menu order within each group.
const std::vector<Entry>& catalogue() {
    static const std::vector<Entry> entries{
        {"mesh.repair", QT_TR_NOOP("Repair mesh"), Cleanup},
        {"select.defects", QT_TR_NOOP("Select spikes and tunnels"), Cleanup},
        {"holes.fill", QT_TR_NOOP("Fill holes"), Cleanup},
        {"faces.delete", QT_TR_NOOP("Delete selected faces"), Cleanup},
        {"shells.detect", QT_TR_NOOP("List shells"), Cleanup},
        {"shells.clean", QT_TR_NOOP("Remove small shells"), Cleanup},
        {"shells.keep", QT_TR_NOOP("Keep the main shell"), Cleanup},
        {"shells.select", QT_TR_NOOP("Select shells"), Cleanup},
        {"shells.split", QT_TR_NOOP("Split shells into objects"), Cleanup},
        {"mesh.smooth", QT_TR_NOOP("Smooth"), Cleanup},
        {"mesh.simplify", QT_TR_NOOP("Simplify"), Cleanup},
        {"mesh.remesh", QT_TR_NOOP("Remesh evenly"), Cleanup},
        {"mesh.wrap", QT_TR_NOOP("Shrink-wrap"), Cleanup},
        {"mesh.flip", QT_TR_NOOP("Flip faces"), Cleanup},
        {"select.all", QT_TR_NOOP("Select all faces"), Selection},
        {"select.brush", QT_TR_NOOP("Brush select"), Selection},
        {"select.lasso", QT_TR_NOOP("Lasso select"), Selection},
        {"select.crease", QT_TR_NOOP("Select up to sharp edges"), Selection},
        {"select.shell", QT_TR_NOOP("Select a shell"), Selection},
        {"select.grow", QT_TR_NOOP("Grow selection"), Selection},
        {"select.shrink", QT_TR_NOOP("Shrink selection"), Selection},
        {"select.invert", QT_TR_NOOP("Invert selection"), Selection},
        {"align.principal", QT_TR_NOOP("Align to principal axes"), Alignment},
        {"align.symmetry", QT_TR_NOOP("Align to symmetry plane"), Alignment},
        {"align.datum", QT_TR_NOOP("Align to datums (3-2-1)"), Alignment},
        {"align.transform", QT_TR_NOOP("Move by a transform"), Alignment},
        {"align.reset", QT_TR_NOOP("Reset alignment"), Alignment},
        {"mesh.segment", QT_TR_NOOP("Segment into primitives"), Primitives},
        {"primitive.fit", QT_TR_NOOP("Fit primitive"), Primitives},
        {"plane.fit", QT_TR_NOOP("Fit plane"), Primitives},
        {"primitive.align", QT_TR_NOOP("Align primitives"), Primitives},
        {"primitive.box", QT_TR_NOOP("Fit box"), Primitives},
        {"primitive.solid", QT_TR_NOOP("Primitive to solid"), Primitives},
        {"primitive.trim", QT_TR_NOOP("Trim primitives into a solid"), Primitives},
        {"sketch.section", QT_TR_NOOP("Section sketch"), Solids},
        {"solid.extrude", QT_TR_NOOP("Extrude sketch"), Solids},
        {"solid.revolve", QT_TR_NOOP("Revolve sketch"), Solids},
        {"solid.boolean", QT_TR_NOOP("Boolean"), Solids},
        {"fillet.fit", QT_TR_NOOP("Fit fillet"), Solids},
        {"quad.remesh", QT_TR_NOOP("Quad remesh"), Surfacing},
        {"surface.build", QT_TR_NOOP("Build surface"), Surfacing},
        {"measure.distance", QT_TR_NOOP("Measure distance"), Inspection},
        {"deviation.compute", QT_TR_NOOP("Compute deviation"), Inspection},
        {"deviation.info", QT_TR_NOOP("Deviation info"), Inspection},
        {"deviation.report", QT_TR_NOOP("Deviation report (CSV)"), Inspection},
        {"mesh.info", QT_TR_NOOP("Mesh info"), Inspection},
        {"solid.info", QT_TR_NOOP("Solid info"), Inspection},
        {"mesh.pick", QT_TR_NOOP("Pick a point"), Inspection},
        {"view.render", QT_TR_NOOP("Render a picture"), Inspection},
        {"mesh.import", QT_TR_NOOP("Import mesh"), Files},
        {"mesh.export", QT_TR_NOOP("Export mesh"), Files},
        {"solid.export", QT_TR_NOOP("Export STEP"), Files},
        {"solid.export.mesh", QT_TR_NOOP("Export solid as a mesh"), Files},
        {"project.new", QT_TR_NOOP("New project"), Files},
        {"project.open", QT_TR_NOOP("Open project"), Files},
        {"project.save", QT_TR_NOOP("Save project"), Files},
        {"object.show", QT_TR_NOOP("Show or hide objects"), Objects},
        {"object.rename", QT_TR_NOOP("Rename object"), Objects},
        {"object.delete", QT_TR_NOOP("Delete objects"), Objects},
        {"commands.list", "", Hidden},
    };
    return entries;
}

} // namespace

const std::vector<QString>& toolGroups() {
    static const std::vector<QString> groups{QObject::tr("Mesh cleanup"),
                                             QObject::tr("Selection"),
                                             QObject::tr("Alignment"),
                                             QObject::tr("Primitives and segmentation"),
                                             QObject::tr("Sketches and solids"),
                                             QObject::tr("Surfacing"),
                                             QObject::tr("Inspection"),
                                             QObject::tr("Files"),
                                             QObject::tr("Objects")};
    return groups;
}

ToolInfo toolInfo(const std::string& command) {
    if (command.starts_with("doc.")) return {QString::fromStdString(command), {}};
    const auto& entries = catalogue();
    if (const auto it = std::ranges::find(entries, command, &Entry::command); it != entries.end()) {
        if (it->group == Hidden) return {QString::fromStdString(command), {}};
        return {QObject::tr(it->title),
                toolGroups()[it->group],
                static_cast<int>(it - entries.begin())};
    }
    // Unknown: its name, under its prefix with a capital.
    QString prefix = QString::fromStdString(command.substr(0, command.find('.')));
    if (!prefix.isEmpty()) prefix[0] = prefix[0].toUpper();
    return {QString::fromStdString(command), prefix, static_cast<int>(entries.size())};
}

} // namespace m2c::app

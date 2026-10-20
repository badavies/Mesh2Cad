// SPDX-License-Identifier: GPL-3.0-or-later
// project.save, project.open, project.new.

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/core/Measurement.h"
#include "m2c/core/Primitive.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/Solid.h"
#include "m2c/document/Document.h"
#include "m2c/io/Project.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

void addProjectCommands(std::vector<Command>& out) {
    Command save = makeCommand(
        "project.save",
        "Save every object (meshes, selections, measurements, alignments, quad layouts, solids, "
        "deviations, primitives and sketches) to a project file (.m2c).",
        R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})",
        false,
        [](const CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(path, requireString(params, "path"));
            ProjectData project;
            project.nextId = context.document.nextId();
            for (const DocumentObject& o : context.document.objects()) {
                ProjectObject p;
                p.id = o.id;
                p.name = o.name;
                p.visible = o.visible;
                p.madeBy = o.provenance.command;
                p.madeWith = o.provenance.params;
                p.inputs = o.provenance.inputs;
                if (const MeshData* m = o.mesh()) {
                    p.data = ProjectMesh{.mesh = m->mesh, .alignment = m->alignment};
                } else if (const SelectionData* s = o.selection()) {
                    p.data = ProjectSelection{.meshId = s->meshId, .faces = s->faces};
                } else if (const MeasurementData* d = o.measurement()) {
                    p.data = d->measurement;
                } else if (const QuadLayoutData* q = o.quadLayout()) {
                    p.data = q->cage;
                } else if (const SolidData* solid = o.solid()) {
                    p.data = solid->solid;
                } else if (const PrimitiveData* primitive = o.primitive()) {
                    p.data = primitive->fit;
                } else if (const SketchData* sketch = o.sketch()) {
                    p.data = sketch->sketch;
                } else {
                    const auto& [meshId, referenceId, field] = *o.deviation();
                    p.data = ProjectDeviation{
                        .meshId = meshId, .referenceId = referenceId, .field = field};
                }
                project.objects.push_back(std::move(p));
            }
            M2C_TRY(written, writeProject(project, std::filesystem::path(*path)));
            return Json{{"path", *path}, {"object_count", project.objects.size()}};
        });
    save.writesFiles = true;
    out.push_back(std::move(save));

    Command open = makeCommand(
        "project.open",
        "Open a project file, replacing the current document. Clears the undo history.",
        R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})",
        true,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(path, requireString(params, "path"));
            M2C_TRY(project, readProject(std::filesystem::path(*path)));
            std::vector<DocumentObject> objects;
            for (auto& [id, name, visible, data, madeBy, madeWith, inputs] : project->objects) {
                DocumentObject o;
                o.id = id;
                o.name = std::move(name);
                o.visible = visible;
                o.provenance = Provenance{.command = std::move(madeBy),
                                          .params = std::move(madeWith),
                                          .inputs = std::move(inputs)};
                if (const auto* m = std::get_if<ProjectMesh>(&data)) {
                    o.data = MeshData{.mesh = m->mesh, .alignment = m->alignment};
                } else if (const auto* s = std::get_if<ProjectSelection>(&data)) {
                    o.data = SelectionData{.meshId = s->meshId, .faces = s->faces};
                } else if (const auto* d = std::get_if<Measurement>(&data)) {
                    o.data = MeasurementData{*d};
                } else if (auto* q = std::get_if<QuadLayout>(&data)) {
                    o.data = QuadLayoutData{std::move(*q)};
                } else if (auto* solid = std::get_if<Solid>(&data)) {
                    o.data = SolidData{std::move(*solid)};
                } else if (const auto* primitive = std::get_if<FittedPrimitive>(&data)) {
                    o.data = primitiveData(*primitive);
                } else if (auto* sketch = std::get_if<Sketch>(&data)) {
                    o.data = SketchData{std::move(*sketch)};
                } else {
                    auto& [meshId, referenceId, field] = std::get<ProjectDeviation>(data);
                    o.data = DeviationData{
                        .meshId = meshId, .referenceId = referenceId, .field = std::move(field)};
                }
                objects.push_back(std::move(o));
            }
            const std::size_t count = objects.size();
            M2C_TRY(loaded, context.document.load(std::move(objects), project->nextId));
            return Json{{"path", *path}, {"object_count", count}};
        });
    open.opensUndoStep = false;
    out.push_back(std::move(open));

    Command clear = makeCommand("project.new",
                                "Start an empty document. Clears the undo history.",
                                R"({"type":"object","properties":{},"additionalProperties":false})",
                                true,
                                [](CommandContext& context, const Json&) -> Result<Json> {
                                    context.document.clear();
                                    return Json::object();
                                });
    clear.opensUndoStep = false;
    out.push_back(std::move(clear));
}

} // namespace m2c::cmd

// SPDX-License-Identifier: GPL-3.0-or-later
// project.save, project.open, project.new.

#include <filesystem>
#include <string>

#include "m2c/io/Project.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

void addProjectCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "project.save",
        "Save every object (meshes, selections, measurements, alignments) to a project file "
        "(.m2c).",
        R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
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
                    p.data = ProjectMesh{m->mesh, m->alignment};
                } else if (const SelectionData* s = o.selection()) {
                    p.data = ProjectSelection{s->meshId, s->faces};
                } else if (const MeasurementData* d = o.measurement()) {
                    p.data = d->measurement;
                } else {
                    return fail(ErrorCode::Unsupported,
                                std::string("project files cannot hold a ") + o.kind() +
                                    " yet (object " + std::to_string(o.id) + ")");
                }
                project.objects.push_back(std::move(p));
            }
            M2C_TRY(written, writeProject(project, std::filesystem::path(*path)));
            return Json{{"path", *path}, {"object_count", project.objects.size()}};
        }));

    Command open =
        makeCommand("project.open",
                    "Open a project file, replacing the current document. Clears the undo history.",
                    R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})",
                    true,
                    [](CommandContext& context, const Json& params) -> Result<Json> {
                        M2C_TRY(path, requireString(params, "path"));
                        M2C_TRY(project, readProject(std::filesystem::path(*path)));
                        std::vector<DocumentObject> objects;
                        for (ProjectObject& p : project->objects) {
                            DocumentObject o;
                            o.id = p.id;
                            o.name = std::move(p.name);
                            o.visible = p.visible;
                            o.provenance = Provenance{
                                std::move(p.madeBy), std::move(p.madeWith), std::move(p.inputs)};
                            if (auto* m = std::get_if<ProjectMesh>(&p.data)) {
                                o.data = MeshData{m->mesh, m->alignment};
                            } else if (auto* s = std::get_if<ProjectSelection>(&p.data)) {
                                o.data = SelectionData{s->meshId, s->faces};
                            } else {
                                o.data = MeasurementData{std::get<Measurement>(p.data)};
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

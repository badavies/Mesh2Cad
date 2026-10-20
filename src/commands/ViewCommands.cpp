// SPDX-License-Identifier: GPL-3.0-or-later
// view.render: a picture of the document for agents, which see no viewport.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/DeviationColors.h"
#include "m2c/core/Error.h"
#include "m2c/core/Raster.h"
#include "m2c/core/Sketch.h"
#include "m2c/document/Document.h"
#include "m2c/io/Png.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

// The app's viewport colours (src/app/Viewport.cpp).
constexpr Rgb kBackground{0.17, 0.18, 0.20};
constexpr Rgb kMesh{0.80, 0.82, 0.86};
constexpr Rgb kSolid{0.55, 0.66, 0.82};
constexpr Rgb kPrimitive{0.56, 0.78, 0.60};
constexpr Rgb kBack{0.72, 0.30, 0.30};
constexpr Rgb kCage{0.95, 0.60, 0.20};
constexpr Rgb kSketch{0.30, 0.85, 0.95};
constexpr Rgb kHighlight{1.00, 0.82, 0.25};

struct View {
    Vec3d from;
    Vec3d up;
};

std::optional<View> standardView(const std::string& name) {
    const Vec3d z{0, 0, 1}, y{0, 1, 0};
    if (name == "iso") return View{{1, -1, 1}, z};
    if (name == "front") return View{{0, -1, 0}, z};
    if (name == "back") return View{{0, 1, 0}, z};
    if (name == "left") return View{{-1, 0, 0}, z};
    if (name == "right") return View{{1, 0, 0}, z};
    if (name == "top") return View{{0, 0, 1}, y};
    if (name == "bottom") return View{{0, 0, -1}, y};
    return std::nullopt;
}

void grow(Aabb& box, const Aabb& more) {
    box.min = {std::min(box.min.x, more.min.x),
               std::min(box.min.y, more.min.y),
               std::min(box.min.z, more.min.z)};
    box.max = {std::max(box.max.x, more.max.x),
               std::max(box.max.y, more.max.y),
               std::max(box.max.z, more.max.z)};
}

Aabb pointsBox(const std::vector<Vec3d>& points) {
    Aabb box{.min = points.front(), .max = points.front()};
    for (const Vec3d& p : points) grow(box, Aabb{.min = p, .max = p});
    return box;
}

/// The newest visible deviation of a mesh as vertex colours, or none.
std::vector<Rgb> deviationColors(const std::vector<DocumentObject>& objects, ObjectId mesh) {
    const DeviationData* newest = nullptr;
    for (const DocumentObject& o : objects) {
        if (o.visible && o.deviation() != nullptr && o.deviation()->meshId == mesh) {
            newest = o.deviation();
        }
    }
    std::vector<Rgb> out;
    if (newest == nullptr) return out;
    const DeviationStats& stats = newest->field.stats();
    const double range = deviationRange(stats.p95AbsMm, stats.toleranceMm);
    for (const double d : newest->field.distances()) {
        const Rgb8 c = deviationColor(d, stats.toleranceMm, range, false);
        out.push_back({c.r / 255.0, c.g / 255.0, c.b / 255.0});
    }
    return out;
}

} // namespace

void addViewCommands(std::vector<Command>& out) {
    out.push_back(makeCommand(
        "view.render",
        "Draw the visible objects to a PNG file, as the app's viewport shows them, so an agent "
        "can see the document: meshes grey (red where a back face shows, deviation colours "
        "when a deviation of the mesh is visible), solids blue, primitives green, quad layout "
        "edges orange and sketches cyan over everything. selection_id marks a selection's "
        "faces orange and highlight_id draws one object yellow. The view is orthographic and "
        "framed on what is drawn. Needs no GPU or display. Over MCP the picture is also "
        "returned as an image.",
        R"({
            "type":"object",
            "properties":{
                "path":{"type":"string","x-file":{"save":true,"types":["png"]},"description":"PNG file to write."},
                "view":{"type":"string","enum":["iso","front","back","left","right","top","bottom"],"default":"iso"},
                "direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"From the part towards the camera, instead of a standard view."},
                "width":{"type":"integer","minimum":16,"maximum":4096,"default":800},
                "height":{"type":"integer","minimum":16,"maximum":4096,"default":600},
                "selection_id":{"type":"integer","x-object":"selection","minimum":0,"description":"A selection whose faces are marked."},
                "highlight_id":{"type":"integer","x-object":"any","minimum":0,"description":"An object drawn in yellow."}
            },
            "required":["path"],"additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(path, requireString(params, "path"));
            M2C_TRY(viewName, optionalString(params, "view"));
            const auto chosen = standardView(viewName->value_or("iso"));
            if (!chosen) {
                return fail(ErrorCode::InvalidArgument,
                            "view must be iso, front, back, left, right, top or bottom");
            }
            View view = *chosen;
            if (params.contains("direction")) {
                M2C_TRY(from, requireVec3(params, "direction"));
                if (!(length(*from) > 0.0)) {
                    return fail(ErrorCode::InvalidArgument, "direction must not be zero");
                }
                // Up is +Z unless the camera looks along it.
                const bool vertical = std::abs(from->z) > 0.99 * length(*from);
                view = View{*from, vertical ? Vec3d{0, 1, 0} : Vec3d{0, 0, 1}};
            }
            M2C_TRY(width, optionalInteger(params, "width", 800, 16, 4096));
            M2C_TRY(height, optionalInteger(params, "height", 600, 16, 4096));
            M2C_TRY(selection, optionalSelection(context, params, "selection_id"));
            M2C_TRY(highlight, optionalId(params, "highlight_id"));

            const std::vector<DocumentObject> objects = context.document.objects();
            std::optional<Aabb> box;
            const auto add = [&](const Aabb& b) {
                if (box) {
                    grow(*box, b);
                } else {
                    box = b;
                }
            };
            std::vector<SketchLines> sketches(objects.size());
            for (std::size_t i = 0; i < objects.size(); ++i) {
                const DocumentObject& o = objects[i];
                if (!o.visible) continue;
                if (const MeshData* m = o.mesh()) add(m->mesh->bounds());
                if (const SolidData* s = o.solid()) add(s->solid.display().bounds());
                if (const PrimitiveData* p = o.primitive(); p && p->display)
                    add(p->display->bounds());
                if (const QuadLayoutData* q = o.quadLayout()) add(q->cage.bounds());
                if (const SketchData* s = o.sketch()) {
                    sketches[i] = sketchLines(s->sketch);
                    if (!sketches[i].points.empty()) add(pointsBox(sketches[i].points));
                }
            }
            if (!box) return fail(ErrorCode::InvalidState, "nothing visible to draw");

            Raster raster(static_cast<int>(*width),
                          static_cast<int>(*height),
                          *box,
                          view.from,
                          view.up,
                          kBackground);
            const double diagonal = length(box->max - box->min);
            Json drawn = Json::array();
            const auto color = [&](const DocumentObject& o, Rgb base) {
                return *highlight && **highlight == o.id ? kHighlight : base;
            };
            // Meshes, then solids and primitives over the scan they were fitted to, then edges,
            // then sketches over everything (a section lies inside its mesh).
            for (const DocumentObject& o : objects) {
                const MeshData* m = o.mesh();
                if (!o.visible || m == nullptr) continue;
                std::vector<std::uint8_t> marks;
                if (*selection && (*selection)->meshId == o.id) {
                    marks.resize(m->mesh->faceCount());
                    for (std::size_t f = 0; f < marks.size(); ++f) {
                        marks[f] = (*selection)->faces->test(f) ? 1 : 0;
                    }
                }
                std::vector<Rgb> colors = deviationColors(objects, o.id);
                if (*highlight && **highlight == o.id) colors.clear();
                raster.triangles(m->mesh->positions(),
                                 m->mesh->indices(),
                                 color(o, kMesh),
                                 kBack,
                                 colors,
                                 marks);
                drawn.push_back(o.id);
            }
            for (const DocumentObject& o : objects) {
                if (!o.visible) continue;
                const TriMesh* surface = nullptr;
                Rgb base = kSolid;
                if (const SolidData* s = o.solid()) surface = &s->solid.display();
                if (const PrimitiveData* p = o.primitive()) {
                    surface = p->display.get();
                    base = kPrimitive;
                }
                if (surface == nullptr) continue;
                // A primitive's patch is one-sided: draw its back in its own colour too.
                const Rgb c = color(o, base);
                raster.triangles(surface->positions(),
                                 surface->indices(),
                                 c,
                                 o.primitive() != nullptr ? c : kBack,
                                 {},
                                 {},
                                 1e-3 * diagonal);
                drawn.push_back(o.id);
            }
            for (const DocumentObject& o : objects) {
                if (!o.visible || o.quadLayout() == nullptr) continue;
                const QuadLayout& cage = o.quadLayout()->cage;
                raster.lines(cage.positions(), cageEdges(cage), color(o, kCage), false);
                drawn.push_back(o.id);
            }
            for (std::size_t i = 0; i < objects.size(); ++i) {
                if (!objects[i].visible || sketches[i].points.empty()) continue;
                raster.lines(
                    sketches[i].points, sketches[i].segments, color(objects[i], kSketch), true);
                drawn.push_back(objects[i].id);
            }
            M2C_TRY(written, writePng(raster.image(), *path));
            return Json{{"path", *path},
                        {"width", *width},
                        {"height", *height},
                        {"view_direction", toJson(view.from * (1.0 / length(view.from)))},
                        {"drawn_ids", std::move(drawn)},
                        {"image", {{"path", *path}, {"mime_type", "image/png"}}}};
        }));
}

} // namespace m2c::cmd

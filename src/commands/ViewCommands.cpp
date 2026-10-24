// SPDX-License-Identifier: GPL-3.0-or-later
// view.render: a picture of the document for agents, which see no viewport.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/commands/JsonConvert.h"
#include "m2c/core/Base64.h"
#include "m2c/core/DeviationColors.h"
#include "m2c/core/DeviationField.h"
#include "m2c/core/Error.h"
#include "m2c/core/QuadLayout.h"
#include "m2c/core/Raster.h"
#include "m2c/core/SelectionColors.h"
#include "m2c/core/Sketch.h"
#include "m2c/core/TriMesh.h"
#include "m2c/core/Vec3.h"
#include "m2c/document/Document.h"
#include "m2c/io/Png.h"

#include "detail/Params.h"
#include "detail/Register.h"

namespace m2c::cmd {

namespace {

// The app's viewport colours (src/app/Viewport.cpp).
constexpr Rgb kBackground{.r = 0.17, .g = 0.18, .b = 0.20};
constexpr Rgb kMesh{.r = 0.80, .g = 0.82, .b = 0.86};
constexpr Rgb kSolid{.r = 0.55, .g = 0.66, .b = 0.82};
constexpr Rgb kPrimitive{.r = 0.56, .g = 0.78, .b = 0.60};
constexpr Rgb kBack{.r = 0.72, .g = 0.30, .b = 0.30};
constexpr Rgb kCage{.r = 0.95, .g = 0.60, .b = 0.20};
constexpr Rgb kSketch{.r = 0.30, .g = 0.85, .b = 0.95};
constexpr Rgb kHighlight{.r = 1.00, .g = 0.82, .b = 0.25};
constexpr Rgb kLabel{.r = 0.85, .g = 0.86, .b = 0.88};

struct View {
    Vec3d from;
    Vec3d up;
};

std::optional<View> standardView(const std::string& name) {
    constexpr Vec3d z{.x = 0, .y = 0, .z = 1}, y{.x = 0, .y = 1, .z = 0};
    if (name == "iso") return View{.from = {.x = 1, .y = -1, .z = 1}, .up = z};
    if (name == "front") return View{.from = {.x = 0, .y = -1, .z = 0}, .up = z};
    if (name == "back") return View{.from = {.x = 0, .y = 1, .z = 0}, .up = z};
    if (name == "left") return View{.from = {.x = -1, .y = 0, .z = 0}, .up = z};
    if (name == "right") return View{.from = {.x = 1, .y = 0, .z = 0}, .up = z};
    if (name == "top") return View{.from = {.x = 0, .y = 0, .z = 1}, .up = y};
    if (name == "bottom") return View{.from = {.x = 0, .y = 0, .z = -1}, .up = y};
    return std::nullopt;
}

void grow(Aabb& box, const Aabb& more) {
    box.min = {.x = std::min(box.min.x, more.min.x),
               .y = std::min(box.min.y, more.min.y),
               .z = std::min(box.min.z, more.min.z)};
    box.max = {.x = std::max(box.max.x, more.max.x),
               .y = std::max(box.max.y, more.max.y),
               .z = std::max(box.max.z, more.max.z)};
}

Aabb pointsBox(const std::vector<Vec3d>& points) {
    Aabb box{.min = points.front(), .max = points.front()};
    for (const Vec3d& p : points) grow(box, Aabb{.min = p, .max = p});
    return box;
}

/// The newest visible deviation of a mesh as vertex colours, or none.
std::vector<Rgb> deviationColors(const std::vector<DocumentObject>& objects, const ObjectId mesh) {
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
        out.push_back({.r = c.r / 255.0, .g = c.g / 255.0, .b = c.b / 255.0});
    }
    return out;
}

/// The bounds of any object, for focus_id: a selection's faces, a deviation's mesh, a
/// measurement's two points. None when it has nothing to frame (an empty selection).
std::optional<Aabb> objectBounds(const Document& document, const DocumentObject& o) {
    if (const MeshData* m = o.mesh()) return m->mesh->bounds();
    if (const SolidData* s = o.solid()) return s->solid.display().bounds();
    if (const PrimitiveData* p = o.primitive(); p && p->display) return p->display->bounds();
    if (const QuadLayoutData* q = o.quadLayout()) return q->cage.bounds();
    if (const SketchData* k = o.sketch()) {
        const SketchLines lines = sketchLines(k->sketch);
        if (lines.points.empty()) return std::nullopt;
        return pointsBox(lines.points);
    }
    if (const MeasurementData* m = o.measurement())
        return pointsBox({m->measurement.a.position, m->measurement.b.position});
    const ObjectId meshId = o.selection() != nullptr   ? o.selection()->meshId
                            : o.deviation() != nullptr ? o.deviation()->meshId
                                                       : 0;
    const auto mesh = document.find(meshId);
    if (!mesh || mesh->mesh() == nullptr) return std::nullopt;
    const TriMesh& m = *mesh->mesh()->mesh;
    if (o.deviation() != nullptr) return m.bounds();
    std::vector<Vec3d> points;
    for (const std::uint32_t f : o.selection()->faces->indices()) {
        for (std::size_t k = 0; k < 3; ++k)
            points.push_back(m.positions()[m.indices()[std::size_t{3} * f + k]]);
    }
    if (points.empty()) return std::nullopt;
    return pointsBox(points);
}

/// What view.render draws, gathered once and drawn into each picture (four for a quad).
struct Scene {
    std::vector<DocumentObject> objects;
    std::vector<SketchLines> sketches; ///< One per object, empty for anything but a sketch.
    std::vector<SelectionRef> marked;  ///< Mark k + 2 is marked[k]; the current one is mark 1.
    std::vector<Rgb> markColors;
    bool current{false}; ///< The last of `marked` is selection_id.
    std::optional<ObjectId> highlight;
    double diagonal{0.0};
};

/// Draws the scene as the app does and returns the ids drawn: meshes, then solids and
/// primitives over the scan they were fitted to, then edges, then sketches over everything (a
/// section lies inside its mesh).
Json draw(Raster& raster, const Scene& scene) {
    Json drawn = Json::array();
    const auto color = [&](const DocumentObject& o, const Rgb& base) {
        return scene.highlight && *scene.highlight == o.id ? kHighlight : base;
    };
    const std::vector<DocumentObject>& objects = scene.objects;
    for (const DocumentObject& o : objects) {
        const MeshData* m = o.mesh();
        if (!o.visible || m == nullptr) continue;
        // Later marks win, so selection_id (last) shows over the listed selections.
        std::vector<std::uint8_t> marks;
        for (std::size_t k = 0; k < scene.marked.size(); ++k) {
            if (scene.marked[k].meshId != o.id) continue;
            marks.resize(m->mesh->faceCount());
            const bool current = scene.current && k + 1 == scene.marked.size();
            const auto mark = static_cast<std::uint8_t>(current ? 1 : k + 2);
            for (const std::uint32_t f : scene.marked[k].faces->indices()) marks[f] = mark;
        }
        std::vector<Rgb> colors = deviationColors(objects, o.id);
        if (scene.highlight && *scene.highlight == o.id) colors.clear();
        raster.triangles(m->mesh->positions(),
                         m->mesh->indices(),
                         color(o, kMesh),
                         kBack,
                         colors,
                         marks,
                         0.0,
                         scene.markColors);
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
                         1e-3 * scene.diagonal);
        drawn.push_back(o.id);
    }
    for (const DocumentObject& o : objects) {
        if (!o.visible || o.quadLayout() == nullptr) continue;
        const QuadLayout& cage = o.quadLayout()->cage;
        raster.lines(cage.positions(), cageEdges(cage), color(o, kCage), false);
        drawn.push_back(o.id);
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i].visible || scene.sketches[i].points.empty()) continue;
        raster.lines(
            scene.sketches[i].points, scene.sketches[i].segments, color(objects[i], kSketch), true);
        drawn.push_back(objects[i].id);
    }
    return drawn;
}

Json unitJson(const Vec3d& v) {
    return toJson(v * (1.0 / length(v)));
}

} // namespace

void addViewCommands(std::vector<Command>& out) {
    Command render = makeCommand(
        "view.render",
        "Draw the visible objects as the app's viewport shows them, so an agent can see the "
        "document: meshes grey (red where a back face shows, deviation colours when a deviation "
        "of the mesh is visible), solids blue, primitives green, quad layout edges orange and "
        "sketches cyan over everything. selection_id marks a selection's faces orange, "
        "selection_ids marks each listed selection in its own colour (the app's, listed in "
        "color_legend), and highlight_id draws one object yellow. The view is orthographic and "
        "framed on what is drawn, or on focus_id with 20% padding; view quad is a 2 x 2 sheet of "
        "front, right, top and iso, labelled, all at one scale (framing sphere). With path the "
        "PNG is written there; without it nothing is written and the result carries the PNG as "
        "image.data_base64. Needs no GPU or display. Over MCP the picture is also returned as an "
        "image.",
        R"({
            "type":"object",
            "properties":{
                "path":{"type":"string","x-file-types":["png"],"description":"PNG file to write; without it the picture is returned in the result instead."},
                "view":{"type":"string","enum":["iso","front","back","left","right","top","bottom","quad"],"default":"iso","description":"A standard view, or quad: front, right, top and iso in one picture (width and height must be even)."},
                "direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"From the part towards the camera, instead of a standard view."},
                "width":{"type":"integer","minimum":16,"maximum":4096,"default":800},
                "height":{"type":"integer","minimum":16,"maximum":4096,"default":600},
                "framing":{"type":"string","enum":["box","sphere"],"default":"box","description":"box fits the view tightly; sphere frames the bounding sphere, the same scale and centre from every direction (as each quad panel does)."},
                "focus_id":{"type":"integer","x-object":"any","minimum":0,"description":"Frame this object's bounds with 20% padding, still drawing everything visible."},
                "axes":{"type":"boolean","default":false,"description":"Draw an X (red), Y (green), Z (blue) triad in the bottom-left corner."},
                "selection_id":{"type":"integer","x-object":"selection","minimum":0,"description":"A selection whose faces are marked orange."},
                "selection_ids":{"type":"array","items":{"type":"integer","x-object":"selection","minimum":0},"maxItems":254,"description":"Selections whose faces are marked, each in its own colour; selection_id wins where they overlap."},
                "highlight_id":{"type":"integer","x-object":"any","minimum":0,"description":"An object drawn in yellow."}
            },
            "additionalProperties":false})",
        false,
        [](CommandContext& context, const Json& params) -> Result<Json> {
            M2C_TRY(path, optionalString(params, "path"));
            M2C_TRY(viewName, optionalString(params, "view"));
            const bool quad = viewName->value_or("iso") == "quad";
            const auto chosen = standardView(quad ? "iso" : viewName->value_or("iso"));
            if (!chosen) {
                return fail(ErrorCode::InvalidArgument,
                            "view must be iso, front, back, left, right, top, bottom or quad");
            }
            View view = *chosen;
            if (params.contains("direction")) {
                if (quad) return fail(ErrorCode::InvalidArgument, "a quad view takes no direction");
                M2C_TRY(from, requireVec3(params, "direction"));
                if (!(length(*from) > 0.0)) {
                    return fail(ErrorCode::InvalidArgument, "direction must not be zero");
                }
                // Up is +Z unless the camera looks along it.
                const bool vertical = std::abs(from->z) > 0.99 * length(*from);
                view = View{.from = *from,
                            .up = vertical ? Vec3d{.x = 0, .y = 1, .z = 0}
                                           : Vec3d{.x = 0, .y = 0, .z = 1}};
            }
            M2C_TRY(width, optionalInteger(params, "width", 800, 16, 4096));
            M2C_TRY(height, optionalInteger(params, "height", 600, 16, 4096));
            if (quad && (*width % 2 != 0 || *height % 2 != 0 || *width < 32 || *height < 32)) {
                return fail(ErrorCode::InvalidArgument,
                            "a quad view needs an even width and height of at least 32");
            }
            M2C_TRY(framingName, optionalString(params, "framing"));
            if (*framingName && **framingName != "box" && **framingName != "sphere")
                return fail(ErrorCode::InvalidArgument, "framing must be box or sphere");
            const Framing framing =
                quad || framingName->value_or("box") == "sphere" ? Framing::Sphere : Framing::Box;
            M2C_TRY(axes, optionalBool(params, "axes", false));
            M2C_TRY(focusId, optionalId(params, "focus_id"));
            M2C_TRY(selection, optionalSelection(context, params, "selection_id"));
            M2C_TRY(highlight, optionalId(params, "highlight_id"));

            Scene scene;
            scene.highlight = *highlight;
            // Marks: 1 is selection_id in orange, 2 onwards the listed selections' colours.
            scene.markColors.push_back(kCurrentSelectionColor);
            Json legend = Json::array();
            if (params.contains("selection_ids")) {
                const Json& ids = params["selection_ids"];
                if (!ids.is_array() || ids.size() > 254) {
                    return fail(ErrorCode::InvalidArgument,
                                "selection_ids must list at most 254 selections");
                }
                for (const Json& id : ids) {
                    M2C_TRY(ref, requireSelection(context, Json{{"id", id}}, "id"));
                    scene.markColors.push_back(selectionColor(ref->id));
                    scene.marked.push_back(std::move(*ref));
                }
            }
            if (*selection) {
                scene.marked.push_back(**selection);
                scene.markColors.push_back(kCurrentSelectionColor);
                scene.current = true;
            }
            for (std::size_t k = 0; k < scene.marked.size(); ++k) {
                const auto object = context.document.find(scene.marked[k].id);
                const Rgb& c = scene.markColors[k + 1];
                legend.push_back({{"id", scene.marked[k].id},
                                  {"name", object ? object->name : std::string()},
                                  {"rgb", Json::array({c.r, c.g, c.b})}});
            }

            scene.objects = context.document.objects();
            const std::vector<DocumentObject>& objects = scene.objects;
            std::optional<Aabb> box;
            const auto add = [&](const Aabb& b) {
                if (box) {
                    grow(*box, b);
                } else {
                    box = b;
                }
            };
            scene.sketches.resize(objects.size());
            for (std::size_t i = 0; i < objects.size(); ++i) {
                const DocumentObject& o = objects[i];
                if (!o.visible) continue;
                if (const MeshData* m = o.mesh()) add(m->mesh->bounds());
                if (const SolidData* s = o.solid()) add(s->solid.display().bounds());
                if (const PrimitiveData* p = o.primitive(); p && p->display)
                    add(p->display->bounds());
                if (const QuadLayoutData* q = o.quadLayout()) add(q->cage.bounds());
                if (const SketchData* s = o.sketch()) {
                    scene.sketches[i] = sketchLines(s->sketch);
                    if (!scene.sketches[i].points.empty()) add(pointsBox(scene.sketches[i].points));
                }
            }
            if (!box) return fail(ErrorCode::InvalidState, "nothing visible to draw");
            scene.diagonal = length(box->max - box->min);
            Aabb frame = *box;
            if (*focusId) {
                const auto focus = context.document.find(**focusId);
                if (!focus) {
                    return fail(ErrorCode::NotFound,
                                "no object with id " + std::to_string(**focusId));
                }
                const auto bounds = objectBounds(context.document, *focus);
                if (!bounds) {
                    return fail(ErrorCode::InvalidArgument,
                                "object " + std::to_string(**focusId) + " has nothing to frame");
                }
                // 20% padding: 10% of the size on each side, at least a millimetre in all.
                const Vec3d centre = (bounds->min + bounds->max) * 0.5;
                const Vec3d half = (bounds->max - bounds->min) * 0.6;
                const double floor = std::max(0.5, 0.1 * length(half));
                const Vec3d pad{.x = std::max(half.x, floor),
                                .y = std::max(half.y, floor),
                                .z = std::max(half.z, floor)};
                frame = Aabb{.min = centre - pad, .max = centre + pad};
            }

            Image picture;
            Json drawn;
            Json result = Json::object();
            if (!quad) {
                Raster raster(static_cast<int>(*width),
                              static_cast<int>(*height),
                              frame,
                              view.from,
                              view.up,
                              kBackground,
                              framing);
                drawn = draw(raster, scene);
                if (*axes) raster.axisTriad();
                picture = raster.image();
                result["view_direction"] = unitJson(view.from);
            } else {
                // Front, right / top, iso; each panel is the single view at half the size.
                const int w = static_cast<int>(*width) / 2, h = static_cast<int>(*height) / 2;
                picture = Image{.width = 2 * w,
                                .height = 2 * h,
                                .rgb = std::vector<std::uint8_t>(12 * static_cast<std::size_t>(w) *
                                                                 static_cast<std::size_t>(h))};
                Json panels = Json::array();
                constexpr std::array<const char*, 4> names{"front", "right", "top", "iso"};
                for (std::size_t k = 0; k < names.size(); ++k) {
                    const View v = *standardView(names[k]);
                    Raster raster(w, h, frame, v.from, v.up, kBackground, Framing::Sphere);
                    drawn = draw(raster, scene);
                    if (*axes) raster.axisTriad();
                    std::string label = names[k];
                    for (char& c : label) c = static_cast<char>(c - 'a' + 'A');
                    raster.label(4, 4, label, kLabel, h >= 200 ? 2 : 1);
                    const int left = (k % 2 == 0 ? 0 : w), top = (k < 2 ? 0 : h);
                    const Image& panel = raster.image();
                    for (int y = 0; y < h; ++y) {
                        std::copy_n(panel.rgb.begin() + 3 * static_cast<std::ptrdiff_t>(y) * w,
                                    3 * w,
                                    picture.rgb.begin() +
                                        3 * (static_cast<std::ptrdiff_t>(top + y) * 2 * w + left));
                    }
                    panels.push_back({{"view", names[k]},
                                      {"x", left},
                                      {"y", top},
                                      {"width", w},
                                      {"height", h},
                                      {"view_direction", unitJson(v.from)}});
                }
                result["panels"] = std::move(panels);
            }
            result["width"] = *width;
            result["height"] = *height;
            result["drawn_ids"] = std::move(drawn);
            result["color_legend"] = std::move(legend);
            if (*path) {
                M2C_TRY(written, writePng(picture, **path));
                result["path"] = **path;
                result["image"] = {{"path", **path}, {"mime_type", "image/png"}};
            } else {
                M2C_TRY(png, encodePng(picture));
                result["image"] = {{"data_base64", encodeBase64(*png)}, {"mime_type", "image/png"}};
            }
            return result;
        });
    // Only with a path (ADR-031, amended): without one the picture comes back in the result.
    render.writesFiles = true;
    out.push_back(std::move(render));
}

} // namespace m2c::cmd

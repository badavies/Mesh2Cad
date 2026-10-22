// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the desktop window the way a person does (mouse strokes in the viewport) and the way an
// agent does (JSON-RPC on the local socket), and checks both land in the same document.

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QStringList>
#include <QSurfaceFormat>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <thread>
#include <utility>

#include "m2c/core/SelectionColors.h"
#include "m2c/io/MeshIO.h"
#include "m2c/rpc/Session.h"

#include "Engine.h"
#include "MainWindow.h"
#include "ParamForm.h"
#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::app;
using namespace m2c::test;

namespace {

QApplication& application() {
    static int argc = 1;
    static char name[] = "m2c_app_tests";
    static char* argv[] = {name, nullptr};
    static QApplication* app = [] {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        QSurfaceFormat::setDefaultFormat(format);
        return new QApplication(argc, argv);
    }();
    return *app;
}

/// Processes events until `done` holds (or a timeout).
template <class F>
bool waitFor(F done, int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void mouse(QWidget* w,
           QEvent::Type type,
           QPointF pos,
           Qt::MouseButtons buttons,
           Qt::KeyboardModifiers mods = {}) {
    const Qt::MouseButton button = (type == QEvent::MouseMove) ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, pos, w->mapToGlobal(pos), button, buttons, mods);
    QApplication::sendEvent(w, &event);
}

void drag(QWidget* w, const std::vector<QPointF>& path, Qt::KeyboardModifiers mods = {}) {
    mouse(w, QEvent::MouseButtonPress, path.front(), Qt::LeftButton, mods);
    for (std::size_t i = 1; i < path.size(); ++i) {
        mouse(w, QEvent::MouseMove, path[i], Qt::LeftButton, mods);
    }
    mouse(w, QEvent::MouseButtonRelease, path.back(), Qt::NoButton, mods);
}

/// Saves the viewport when M2C_TEST_SCREENSHOT_DIR is set, to look at what the tests drew.
void screenshot(Viewport* view, const char* name) {
    const QByteArray dir = qgetenv("M2C_TEST_SCREENSHOT_DIR");
    if (dir.isEmpty()) return;
    QApplication::processEvents();
    view->grabFramebuffer().save(QString::fromUtf8(dir) + "/" + name + ".png");
}

std::size_t countKind(const DocSnapshot& s, const char* kind) {
    return static_cast<std::size_t>(
        std::count_if(s.objects.begin(), s.objects.end(), [&](const auto& o) {
            return std::string(o.kind()) == kind;
        }));
}

/// A closed, bumpy sphere standing in for a large organic scan: 2 * segments * (rings - 1)
/// triangles.
TriMesh makeBumpySphere(double radius, std::uint32_t rings, std::uint32_t segments) {
    std::vector<Vec3d> positions;
    positions.reserve(2 + std::size_t{rings - 1} * segments);
    const auto at = [&](double theta, double phi) {
        const double r = radius * (1.0 + 0.03 * std::sin(7 * theta) * std::cos(5 * phi) +
                                   0.01 * std::sin(23 * theta) * std::sin(19 * phi));
        return Vec3d{r * std::sin(theta) * std::cos(phi),
                     r * std::sin(theta) * std::sin(phi),
                     r * std::cos(theta)};
    };
    positions.push_back(at(0.0, 0.0));
    for (std::uint32_t i = 1; i < rings; ++i) {
        for (std::uint32_t j = 0; j < segments; ++j) {
            positions.push_back(
                at(std::numbers::pi * i / rings, 2 * std::numbers::pi * j / segments));
        }
    }
    positions.push_back(at(std::numbers::pi, 0.0));
    const auto ring = [&](std::uint32_t i, std::uint32_t j) {
        return 1 + (i - 1) * segments + j % segments;
    };
    const auto south = static_cast<std::uint32_t>(positions.size() - 1);
    std::vector<std::uint32_t> indices;
    indices.reserve(6 * std::size_t{segments} * (rings - 1));
    for (std::uint32_t j = 0; j < segments; ++j) {
        indices.insert(indices.end(), {0, ring(1, j), ring(1, j + 1)});
        for (std::uint32_t i = 1; i + 1 < rings; ++i) {
            indices.insert(indices.end(), {ring(i, j), ring(i + 1, j), ring(i + 1, j + 1)});
            indices.insert(indices.end(), {ring(i, j), ring(i + 1, j + 1), ring(i, j + 1)});
        }
        indices.insert(indices.end(), {ring(rings - 1, j), south, ring(rings - 1, j + 1)});
    }
    return makeMesh(std::move(positions), std::move(indices));
}

} // namespace

TEST_CASE("The desktop app selects, measures and serves agents", "[app]") {
    application();
    const TempDir dir;
    // Keep the session file out of the real per-user location.
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto ball = dir.path() / "ball.stl";
    REQUIRE(writeMesh(makeSphere(10.0, 4), ball).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));

    window.openFiles({QString::fromStdString(ball.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    Viewport* view = window.viewport();
    view->fitAll();
    QApplication::processEvents();

    // The mesh is drawn: the centre pixel is not the background.
    const QImage frame = view->grabFramebuffer();
    REQUIRE_FALSE(frame.isNull());
    const QColor centre = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    CHECK(centre.red() > 80);

    const QPointF c(view->width() / 2.0, view->height() / 2.0);

    SECTION("a brush stroke is one selection and one undo step") {
        view->setMode(Mode::Brush);
        view->setBrushRadius(2.0);
        const auto steps = engine.snapshot()->undo.size();
        drag(view, {c, c + QPointF(10, 0), c + QPointF(20, 0), c + QPointF(30, 5)});
        REQUIRE(waitFor(
            [&] { return !engine.busy() && countKind(*engine.snapshot(), "selection") == 1; }));
        CHECK(engine.snapshot()->undo.size() == steps + 1);
        CHECK(engine.snapshot()->undo.back().label == "select.brush");
        CHECK(engine.snapshot()->undo.back().source == "ui");
        const DocumentObject* sel = nullptr;
        for (const auto& o : engine.snapshot()->objects) {
            if (o.selection() != nullptr) sel = &o;
        }
        REQUIRE(sel != nullptr);
        const std::size_t first = sel->selection()->faces->count();
        CHECK(first > 0);

        // A second stroke adds to the same selection; Ctrl+stroke removes.
        drag(view, {c + QPointF(-40, -40), c + QPointF(-30, -40)});
        REQUIRE(waitFor([&] { return !engine.busy(); }));
        CHECK(countKind(*engine.snapshot(), "selection") == 1);
        drag(view,
             {c, c + QPointF(10, 0), c + QPointF(20, 0), c + QPointF(30, 5)},
             Qt::ControlModifier);
        REQUIRE(waitFor([&] { return !engine.busy(); }));
        for (const auto& o : engine.snapshot()->objects) {
            if (o.selection() != nullptr) CHECK(o.selection()->faces->count() < first + 200);
        }
        screenshot(view, "brush");
    }

    SECTION("a lasso selects only the faces facing the camera") {
        view->setMode(Mode::Lasso);
        const double r = view->height() * 0.45;
        drag(view,
             {c + QPointF(-r, -r), c + QPointF(r, -r), c + QPointF(r, r), c + QPointF(-r, r)});
        REQUIRE(waitFor(
            [&] { return !engine.busy() && countKind(*engine.snapshot(), "selection") == 1; }));
        for (const auto& o : engine.snapshot()->objects) {
            if (const auto* s = o.selection()) {
                const std::size_t faces = s->faces->size();
                CHECK(s->faces->count() > faces / 5);
                CHECK(s->faces->count() < faces * 3 / 5);
            }
        }
        screenshot(view, "lasso");
        if (const QByteArray d = qgetenv("M2C_TEST_SCREENSHOT_DIR"); !d.isEmpty()) {
            window.grab().save(QString::fromUtf8(d) + "/window.png");
        }
    }

    SECTION("two clicks measure a distance") {
        view->setMode(Mode::Measure);
        drag(view, {c + QPointF(-30, 0)});
        drag(view, {c + QPointF(30, 0)});
        REQUIRE(waitFor(
            [&] { return !engine.busy() && countKind(*engine.snapshot(), "measurement") == 1; }));
        for (const auto& o : engine.snapshot()->objects) {
            if (const auto* m = o.measurement()) {
                CHECK(m->measurement.distance() > 0.5);
                CHECK(m->measurement.distance() < 20.0);
                CHECK(m->measurement.a.objectId != 0);
            }
        }
        screenshot(view, "measure");
    }

    SECTION("an agent on the local socket edits the same document") {
        REQUIRE(engine.startLocalServer().startsWith("Listening"));
        const auto info = readSessionFile(defaultSessionFile());
        REQUIRE(info.has_value());
        const ObjectId meshId = engine.snapshot()->objects.front().id;
        QStringList log;
        QObject::connect(
            &engine, &Engine::logLine, [&](const QString& line, bool) { log.append(line); });
        const std::string exported = (dir.path() / "agent.stl").string();
        // Big enough that smoothing it takes far longer than the GUI takes to look.
        const std::string bigPath = (dir.path() / "big.stl").string();
        REQUIRE(writeMesh(makeBumpySphere(10.0, 300, 600), bigPath).has_value());
        // An agent command still running when the GUI next looks gets its "running" line.
        engine.setRunningNotice(std::chrono::milliseconds(0));
        std::atomic<bool> finished{false};
        Json listed;
        Json rejected;
        std::thread agent([&] {
            auto client = LocalClient::connect(info->endpoint);
            if (client) {
                const auto call = [&](int id, const char* method, const Json& params) {
                    (void)(*client)->writeLine(
                        Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}
                            .dump());
                    // Skip doc.changed notifications.
                    for (;;) {
                        auto line = (*client)->readLine();
                        if (!line) return Json();
                        Json j = Json::parse(*line, nullptr, false);
                        if (j.contains("id")) return j;
                    }
                };
                rejected = call(1, "doc.list", Json::object());
                call(2, "rpc.auth", {{"token", info->token}, {"source", "mcp"}});
                call(3, "select.all", {{"id", meshId}});
                listed = call(4, "doc.list", Json::object());
                call(5, "mesh.export", {{"id", meshId}, {"path", exported}});
                const Json big = call(6, "mesh.import", {{"path", bigPath}});
                call(7, "mesh.smooth", {{"id", big["result"].value("id", 0)}, {"iterations", 100}});
            }
            finished = true;
        });
        const bool done = waitFor([&] { return finished.load(); });
        agent.join();
        REQUIRE(done);
        CHECK(rejected["error"]["code"] == RpcServer::kUnauthorized);
        CHECK(listed["result"]["objects"].size() == 2);
        REQUIRE(waitFor([&] { return countKind(*engine.snapshot(), "selection") == 1; }));
        CHECK(engine.snapshot()->undo.back().source == "mcp");
        // The export changes nothing in the document, but the person still sees the file it wrote;
        // the read-only doc.list stays out of the log.
        const QString writeLine = QStringLiteral("[mcp] mesh.export  #%1").arg(meshId);
        REQUIRE(waitFor([&] {
            return std::ranges::any_of(log,
                                       [&](const QString& l) { return l.startsWith(writeLine); });
        }));
        CHECK(std::ranges::any_of(
            log, [&](const QString& l) { return l.endsWith(QString::fromStdString(exported)); }));
        CHECK_FALSE(std::ranges::any_of(
            log, [](const QString& l) { return l.startsWith("[mcp] doc.list"); }));
        // The smoothing of the big mesh is still running when the GUI looks: it is announced, then
        // logged again when it finishes.
        const auto smoothed = [](const QString& l) { return l.startsWith("[mcp] mesh.smooth  #"); };
        REQUIRE(waitFor([&] { return std::ranges::any_of(log, smoothed); }));
        const auto announced = log.indexOf(QStringLiteral("[mcp] mesh.smooth  running..."));
        CHECK(announced >= 0);
        CHECK(announced < std::ranges::find_if(log, smoothed) - log.begin());
    }

    SECTION("an agent and the person see a long command's stages") {
        REQUIRE(engine.startLocalServer().startsWith("Listening"));
        const auto info = readSessionFile(defaultSessionFile());
        REQUIRE(info.has_value());
        const ObjectId meshId = engine.snapshot()->objects.front().id;
        QStringList shown;      // what the status bar was told
        const QObject listener; // ends the connection before `shown` goes
        QObject::connect(&engine, &Engine::progressChanged, &listener, [&shown](const QString& t) {
            shown << t;
        });
        std::atomic<bool> finished{false};
        std::vector<Json> lines; // everything the agent read, in order
        std::thread agent([&] {
            if (auto client = LocalClient::connect(info->endpoint)) {
                const auto call = [&](int id, const char* method, const Json& params) {
                    (void)(*client)->writeLine(
                        Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}
                            .dump());
                    for (;;) {
                        auto line = (*client)->readLine();
                        if (!line) return;
                        lines.push_back(Json::parse(*line, nullptr, false));
                        if (lines.back().contains("id")) return;
                    }
                };
                call(1, "rpc.auth", {{"token", info->token}});
                call(2, "mesh.simplify", {{"id", meshId}, {"target_faces", 200}});
            }
            finished = true;
        });
        const bool done = waitFor([&] { return finished.load(); });
        agent.join();
        REQUIRE(done);
        // The stage comes before the reply.
        REQUIRE(lines.size() >= 3);
        CHECK(lines[1]["method"] == "rpc.progress");
        CHECK(lines[1]["params"]["id"] == 2);
        CHECK(lines[1]["params"]["stage"] == "simplifying the mesh");
        CHECK(lines.back()["id"] == 2);
        CHECK(lines.back().contains("result"));
        REQUIRE(waitFor([&] { return !shown.isEmpty(); }));
        CHECK(shown.front() == "mesh.simplify: simplifying the mesh");
    }
}

TEST_CASE("The viewport draws solids, cages and deviation colours", "[app][surface]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto ball = dir.path() / "ball.stl";
    REQUIRE(writeMesh(makeSphere(20.0, 4), ball).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(ball.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;

    // Runs a command as the UI does and returns its result.
    const auto run = [&](const char* command, const Json& params) {
        Json out;
        bool done = false;
        engine.run(command, params, [&](const Result<Json>& r) {
            if (r) out = *r;
            done = true;
        });
        REQUIRE(waitFor([&] { return done && !engine.busy(); }, 120000));
        REQUIRE(out.contains("id"));
        return out["id"].get<ObjectId>();
    };
    const ObjectId cage = run("quad.remesh", {{"id", mesh}, {"target_faces", 300}});
    const ObjectId solid = run("surface.build", {{"id", cage}, {"mesh_id", mesh}});
    const ObjectId deviation =
        run("deviation.compute", {{"id", mesh}, {"reference_id", solid}, {"tolerance_mm", 0.5}});

    // Info on the deviation logs its statistics, not an error.
    auto* log = window.findChild<QPlainTextEdit*>();
    REQUIRE(log != nullptr);
    QStringList lines;
    QObject::connect(&engine, &Engine::logLine, log, [&lines](const QString& line, bool failed) {
        if (failed) lines << line;
    });
    window.showInfo(deviation);
    REQUIRE(waitFor([&] { return log->toPlainText().contains("\"stats\""); }));
    CHECK(log->toPlainText().contains("\"rms_mm\""));
    CHECK(lines.isEmpty());

    Viewport* view = window.viewport();
    view->fitAll();
    const auto centre = [&] {
        QApplication::processEvents();
        const QImage frame = view->grabFramebuffer();
        return frame.pixelColor(frame.width() / 2, frame.height() / 2);
    };

    // The scan and the solid lie on each other; hide one at a time to see the other.
    run("object.show", {{"id", solid}, {"visible", false}});

    // The fitted sphere is within tolerance everywhere, so the scan is drawn in the neutral
    // colour: green in the standard palette, grey in the colour-blind-safe one.
    QColor c = centre();
    CHECK(c.green() > c.red() + 40);
    CHECK(c.green() > c.blue() + 40);
    screenshot(view, "deviation");
    view->setColorBlindPalette(true);
    c = centre();
    CHECK(std::abs(c.green() - c.red()) < 20);
    CHECK(std::abs(c.green() - c.blue()) < 20);
    view->setColorBlindPalette(false);

    // With the scan hidden the solid shows, in its own blue.
    run("object.show", {{"id", mesh}, {"visible", false}});
    run("object.show", {{"id", solid}, {"visible", true}});
    c = centre();
    CHECK(c.blue() > c.red() + 20);
    screenshot(view, "solid");

    // With the solid hidden too only the cage's orange edges are left.
    run("object.show", {{"id", solid}, {"visible", false}});
    QApplication::processEvents();
    const QImage frame = view->grabFramebuffer();
    std::size_t orange = 0;
    for (int y = 0; y < frame.height(); y += 2) {
        for (int x = 0; x < frame.width(); x += 2) {
            const QColor p = frame.pixelColor(x, y);
            if (p.red() > 200 && p.green() > 120 && p.green() < 180 && p.blue() < 90) ++orange;
        }
    }
    CHECK(orange > 500);
    screenshot(view, "cage");
}

TEST_CASE("The viewport draws fitted primitives", "[app][fit]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto can = dir.path() / "can.stl";
    REQUIRE(writeMesh(makeCylinder(10.0, 30.0, 96, 30), can).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(can.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;
    const auto run = [&](const char* command, const Json& params) {
        Json out;
        bool done = false;
        engine.run(command, params, [&](const Result<Json>& r) {
            if (r) out = *r;
            done = true;
        });
        REQUIRE(waitFor([&] { return done && !engine.busy(); }, 120000));
        return out;
    };
    const Json regions = run("mesh.segment", {{"id", mesh}})["regions"];
    REQUIRE(regions.size() == 3);

    // Every row shows its object's id; the name column stays the bare name.
    auto* tree = window.findChild<QTreeWidget*>();
    REQUIRE(tree != nullptr);
    std::set<ObjectId> listed;
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        const auto id = (*it)->data(0, Qt::UserRole + 1).value<ObjectId>();
        const DocumentObject* o = engine.snapshot()->find(id);
        REQUIRE(o != nullptr);
        CHECK((*it)->text(2) == QStringLiteral("#%1").arg(id));
        CHECK((*it)->text(0) == QString::fromStdString(o->name));
        listed.insert(id);
    }
    CHECK(listed.size() == engine.snapshot()->objects.size());
    // Renaming in place sets exactly the typed name.
    tree->topLevelItem(0)->setText(0, "Renamed can");
    REQUIRE(waitFor(
        [&] { return !engine.busy() && engine.snapshot()->find(mesh)->name == "Renamed can"; }));
    CHECK(tree->topLevelItem(0)->text(2) == QStringLiteral("#%1").arg(mesh));
    const Json fit = run("primitive.fit", {{"selection_id", regions[2]["selection_id"]}});
    REQUIRE(fit["kind"] == "cylinder");
    CHECK(countKind(*engine.snapshot(), "primitive") == 1);

    // With the scan hidden the cylinder, clipped to its region, shows in the primitives' green.
    run("object.show", {{"id", mesh}, {"visible", false}});
    Viewport* view = window.viewport();
    view->fitAll();
    QApplication::processEvents();
    const QImage frame = view->grabFramebuffer();
    const QColor c = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    CHECK(c.green() > c.red() + 20);
    CHECK(c.green() > c.blue() + 20);
    screenshot(view, "primitive");

    // A section of the can is listed under it and drawn as a cyan circle over the cylinder.
    const Json section = run("sketch.section", {{"id", mesh}, {"plane", "xy"}, {"offset_mm", 15}});
    const auto sketchId = section["id"].get<ObjectId>();
    REQUIRE(tree->topLevelItemCount() >= 1);
    const QTreeWidgetItem* canItem = tree->topLevelItem(0);
    REQUIRE(canItem->childCount() >= 1);
    const QTreeWidgetItem* sketchItem = canItem->child(canItem->childCount() - 1);
    CHECK(sketchItem->text(0) == "Section");
    CHECK(sketchItem->text(1) == "1 loop");
    view->fitAll();
    QApplication::processEvents();
    const QImage drawn = view->grabFramebuffer();
    bool cyan = false;
    for (int y = 0; y < drawn.height() && !cyan; ++y) {
        for (int x = 0; x < drawn.width() && !cyan; ++x) {
            const QColor p = drawn.pixelColor(x, y);
            cyan = p.blue() > 200 && p.green() > 180 && p.red() < 120;
        }
    }
    CHECK(cyan);
    screenshot(view, "sketch");

    // primitive.solid offers only primitives, the fitted cylinder first; choosing one selects it.
    QAction* tool = nullptr;
    for (QAction* a : window.findChildren<QAction*>()) {
        if (a->text() == "primitive.solid") tool = a;
    }
    REQUIRE(tool != nullptr);
    tool->trigger();
    auto* form = window.findChild<ParamForm*>();
    REQUIRE(form != nullptr);
    auto* id = form->findChild<QComboBox*>("id");
    REQUIRE(id != nullptr);
    REQUIRE(id->count() == 2); // "Choose a primitive" and the cylinder
    CHECK(id->currentData().value<ObjectId>() == fit["id"].get<ObjectId>());
    id->setCurrentIndex(1);
    emit id->activated(1);
    QApplication::processEvents();
    REQUIRE(tree->currentItem() != nullptr);
    CHECK(tree->currentItem()->data(0, Qt::UserRole + 1).value<ObjectId>() ==
          fit["id"].get<ObjectId>());

    // solid.extrude offers the sketch, and follows the document: a deleted sketch goes.
    for (QAction* a : window.findChildren<QAction*>()) {
        if (a->text() == "solid.extrude") a->trigger();
    }
    id = form->findChild<QComboBox*>("id");
    REQUIRE(id != nullptr);
    CHECK(id->currentData().value<ObjectId>() == sketchId);
    run("object.delete", {{"id", sketchId}});
    CHECK(waitFor([&] { return id->count() == 1; }));
    CHECK_FALSE(form->params().has_value()); // id is required
}

TEST_CASE("The viewport colours each selection", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto box = dir.path() / "box.stl";
    REQUIRE(writeMesh(makeBox(30, 20, 10, 4), box).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(box.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;
    Json regions;
    bool done = false;
    engine.run("mesh.segment", {{"id", mesh}}, [&](const Result<Json>& r) {
        if (r) regions = (*r)["regions"];
        done = true;
    });
    REQUIRE(waitFor([&] { return done && !engine.busy(); }));
    REQUIRE(regions.size() == 6);

    // One region current, the five others visible: each pixel is matched to the colour it is
    // closest to once shading is divided out (shading scales all three channels alike).
    Viewport* view = window.viewport();
    const auto current = regions[0]["selection_id"].get<ObjectId>();
    view->setCurrent(mesh, current);
    view->fitAll();
    QApplication::processEvents();
    const QImage frame = view->grabFramebuffer();
    const auto chroma = [](double r, double g, double b) {
        const double top = std::max({r, g, b, 1e-9});
        return std::array<double, 3>{r / top, g / top, b / top};
    };
    const Rgb base{0.80, 0.82, 0.86};
    const auto onMesh = [&](const Rgb& c) {
        return chroma(0.2 * base.r + 0.8 * c.r, 0.2 * base.g + 0.8 * c.g, 0.2 * base.b + 0.8 * c.b);
    };
    std::vector<std::pair<ObjectId, std::array<double, 3>>> expected{
        {current, onMesh(kCurrentSelectionColor)}};
    for (const Json& r : regions) {
        const auto id = r["selection_id"].get<ObjectId>();
        if (id != current) expected.emplace_back(id, onMesh(selectionColor(id)));
    }
    std::map<ObjectId, int> counts;
    for (int y = 0; y < frame.height(); y += 2) {
        for (int x = 0; x < frame.width(); x += 2) {
            const QColor p = frame.pixelColor(x, y);
            if (std::max({p.red(), p.green(), p.blue()}) < 60) continue; // background, dark
            const auto c = chroma(p.redF(), p.greenF(), p.blueF());
            for (const auto& [id, e] : expected) {
                if (std::abs(c[0] - e[0]) + std::abs(c[1] - e[1]) + std::abs(c[2] - e[2]) < 0.06)
                    ++counts[id];
            }
        }
    }
    screenshot(view, "selection-colours");
    CHECK(counts[current] > 200);
    int others = 0;
    for (const auto& [id, n] : counts) {
        if (id != current && n > 200) ++others;
    }
    CHECK(others >= 2); // at least two other faces of the box are in view
}

TEST_CASE("Selecting rows in the Objects list fills the open form", "[app][fit]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto can = dir.path() / "can.stl";
    REQUIRE(writeMesh(makeCylinder(10.0, 30.0, 96, 30), can).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(can.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;
    const auto run = [&](const char* command, const Json& params) {
        Json out;
        bool done = false;
        engine.run(command, params, [&](const Result<Json>& r) {
            if (r) out = *r;
            done = true;
        });
        REQUIRE(waitFor([&] { return done && !engine.busy(); }, 120000));
        return out;
    };
    const Json regions =
        run("mesh.segment", {{"id", mesh}, {"create_primitives", true}})["regions"];
    REQUIRE(regions.size() == 3);
    std::vector<ObjectId> primitives;
    for (const Json& r : regions) primitives.push_back(r["primitive_id"].get<ObjectId>());
    const auto first = run("primitive.solid", {{"id", primitives[2]}})["id"].get<ObjectId>();
    const auto second = run("primitive.solid", {{"id", primitives[2]}})["id"].get<ObjectId>();

    auto* tree = window.findChild<QTreeWidget*>();
    REQUIRE(tree != nullptr);
    const auto row = [&](ObjectId id) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
            if ((*it)->data(0, Qt::UserRole + 1).value<ObjectId>() == id) return *it;
        }
        return nullptr;
    };
    // Selects rows one after another, as Ctrl+click does.
    const auto select = [&](const std::vector<ObjectId>& ids) {
        tree->clearSelection();
        for (const ObjectId id : ids) {
            QTreeWidgetItem* item = row(id);
            REQUIRE(item != nullptr);
            item->setSelected(true);
        }
        QApplication::processEvents();
    };
    const auto open = [&](const char* tool) {
        for (QAction* a : window.findChildren<QAction*>()) {
            if (a->text() == tool) a->trigger();
        }
        return window.findChild<ParamForm*>();
    };

    // primitive.trim takes the primitives in the order they were picked.
    ParamForm* form = open("primitive.trim");
    REQUIRE(form != nullptr);
    select({primitives[2], primitives[0], primitives[1]});
    auto* ids = form->findChild<QLineEdit*>("primitive_ids");
    REQUIRE(ids != nullptr);
    CHECK(ids->text() ==
          QStringLiteral("%1, %2, %3").arg(primitives[2]).arg(primitives[0]).arg(primitives[1]));
    const auto params = form->params();
    REQUIRE(params.has_value());
    CHECK(params->at("primitive_ids") ==
          Json::array({primitives[2], primitives[0], primitives[1]}));
    // A mesh fills the mesh field and leaves the primitives alone.
    const QString before = ids->text();
    select({mesh});
    CHECK(ids->text() == before);
    CHECK(form->findChild<QComboBox*>("mesh_id")->currentData().value<ObjectId>() == mesh);

    // solid.boolean takes the first solid as id and the second as tool_id.
    form = open("solid.boolean");
    select({second, first});
    CHECK(form->findChild<QComboBox*>("id")->currentData().value<ObjectId>() == second);
    CHECK(form->findChild<QComboBox*>("tool_id")->currentData().value<ObjectId>() == first);

    // Hiding one of three selected rows hides all three, as one step.
    select({primitives[0], primitives[1], primitives[2]});
    const auto steps = engine.snapshot()->undo.size();
    row(primitives[1])->setCheckState(0, Qt::Unchecked);
    REQUIRE(waitFor([&] { return !engine.busy() && engine.snapshot()->undo.size() == steps + 1; }));
    for (const ObjectId id : primitives) CHECK_FALSE(engine.snapshot()->find(id)->visible);
    CHECK(engine.snapshot()->undo.back().label == "object.show");
    // The selection survives the rebuilt list.
    CHECK(tree->selectedItems().size() == 3);
}

TEST_CASE("Clicks pick solids and primitives, and mark fillet edges", "[app][fit]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto can = dir.path() / "can.stl";
    REQUIRE(writeMesh(makeCylinder(10.0, 30.0, 96, 30), can).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(can.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;
    const auto run = [&](const char* command, const Json& params) {
        Json out;
        bool done = false;
        engine.run(command, params, [&](const Result<Json>& r) {
            if (r) out = *r;
            done = true;
        });
        REQUIRE(waitFor([&] { return done && !engine.busy(); }, 120000));
        return out;
    };
    const Json regions =
        run("mesh.segment", {{"id", mesh}, {"create_primitives", true}})["regions"];
    REQUIRE(regions.size() == 3);
    std::vector<ObjectId> primitives;
    for (const Json& r : regions) primitives.push_back(r["primitive_id"].get<ObjectId>());
    const auto box =
        run("primitive.box", {{"size_mm", {20, 20, 20}}, {"center", {60, 0, 15}}})["id"]
            .get<ObjectId>();
    const Json hidden{{"ids", {mesh, primitives[0], primitives[1], primitives[2]}},
                      {"visible", false}};
    run("object.show", hidden);

    // With only the box showing, the centre of the view is on it.
    Viewport* view = window.viewport();
    view->fitAll();
    QApplication::processEvents();
    const QPointF c(view->width() / 2.0, view->height() / 2.0);
    const auto hit = view->pickObject(c);
    REQUIRE(hit.has_value());
    CHECK(hit->objectId == box);
    CHECK(hit->meshId == 0);
    const Vec3d local = hit->point - Vec3d{60, 0, 15};
    CHECK(std::abs(std::max({std::abs(local.x), std::abs(local.y), std::abs(local.z)}) - 10.0) <
          1e-3);

    // The brush still paints meshes only: a stroke over the solid selects nothing.
    const std::size_t selections = countKind(*engine.snapshot(), "selection");
    view->setMode(Mode::Brush);
    drag(view, {c, c + QPointF(10, 0), c + QPointF(20, 0)});
    QApplication::processEvents();
    CHECK_FALSE(engine.busy());
    CHECK(countKind(*engine.snapshot(), "selection") == selections);
    view->setMode(Mode::Navigate);

    // With fillet.fit open, two clicks on the solid are two edge points; Clear empties them.
    for (QAction* a : window.findChildren<QAction*>()) {
        if (a->text() == "fillet.fit") a->trigger();
    }
    auto* form = window.findChild<ParamForm*>();
    REQUIRE(form != nullptr);
    const QPointF a = c + QPointF(-20, -10), b = c + QPointF(25, 15);
    const auto pa = view->pickObject(a), pb = view->pickObject(b);
    REQUIRE(pa.has_value());
    REQUIRE(pb.has_value());
    drag(view, {a});
    drag(view, {b});
    auto* points = form->findChild<QLineEdit*>("edge_points");
    REQUIRE(points != nullptr);
    const Json picked = Json::parse(points->text().toStdString());
    REQUIRE(picked.size() == 2);
    CHECK(picked[0] == Json::array({pa->point.x, pa->point.y, pa->point.z}));
    CHECK(picked[1] == Json::array({pb->point.x, pb->point.y, pb->point.z}));
    form->findChild<QToolButton*>("clear")->click();
    CHECK(points->text().isEmpty());

    // With another tool open, a click on a primitive selects its row.
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == "solid.info") action->trigger();
    }
    run("object.show", {{"id", box}, {"visible", false}});
    run("object.show", {{"id", primitives[2]}, {"visible", true}});
    view->fitAll();
    QApplication::processEvents();
    const auto onCan = view->pickObject(c);
    REQUIRE(onCan.has_value());
    CHECK(onCan->objectId == primitives[2]);
    drag(view, {c});
    QApplication::processEvents();
    auto* tree = window.findChild<QTreeWidget*>();
    REQUIRE(tree->currentItem() != nullptr);
    CHECK(tree->currentItem()->data(0, Qt::UserRole + 1).value<ObjectId>() == primitives[2]);
}

TEST_CASE("Tool forms are generated from command schemas", "[app]") {
    application();
    CommandRegistry registry;
    REQUIRE(registerBuiltinCommands(registry).has_value());
    ParamForm form;
    // Object fields are drop-downs of the document's objects of the kinds the schema names.
    auto snapshot = std::make_shared<DocSnapshot>();
    snapshot->objects.push_back({.id = 7,
                                 .name = "scan",
                                 .data = MeshData{std::make_shared<const TriMesh>(makeCube())},
                                 .provenance = {}});
    snapshot->objects.push_back({.id = 8, .name = "other", .data = MeshData{}, .provenance = {}});
    snapshot->objects.push_back(
        {.id = 9, .name = "Section", .data = SketchData{}, .provenance = {}});
    form.setObjects(snapshot);

    form.setCommand(registry.find("mesh.simplify"), {{"id", 7}});
    const auto* mesh = form.findChild<QComboBox*>("id");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->count() == 3); // "Choose a mesh", scan and other: no sketch
    CHECK(mesh->currentText() == "scan (#7)");
    auto params = form.params();
    REQUIRE(params.has_value());
    CHECK(*params == Json{{"id", 7}}); // empty fields are left to the command's defaults

    form.findChild<QLineEdit*>("target_ratio")->setText("0.25");
    form.findChild<QComboBox*>("preserve_borders")->setCurrentIndex(2); // "No"
    params = form.params();
    REQUIRE(params.has_value());
    CHECK((*params)["target_ratio"] == 0.25);
    CHECK((*params)["preserve_borders"] == false);

    form.findChild<QLineEdit*>("target_faces")->setText("many");
    CHECK_FALSE(form.params().has_value());

    form.setCommand(registry.find("select.brush"));
    CHECK_FALSE(form.params().has_value());               // id and radius are required
    form.findChild<QComboBox*>("id")->setCurrentIndex(2); // "other"
    form.findChild<QLineEdit*>("radius_mm")->setText("2");
    form.findChild<QLineEdit*>("center")->setText("1, 2.5 3");
    form.findChild<QComboBox*>("mode")->setCurrentText("subtract");
    params = form.params();
    REQUIRE(params.has_value());
    CHECK((*params)["center"] == Json::array({1.0, 2.5, 3.0}));
    CHECK((*params)["mode"] == "subtract");
    form.findChild<QLineEdit*>("centers")->setText("[[0,0,0],[1,1,1]]");
    CHECK(form.params()->at("centers").size() == 2);
    CHECK((*params)["id"] == 8);

    // A list of ids is typed as numbers, with or without brackets; a list of objects shows one.
    form.setCommand(registry.find("primitive.trim"));
    auto* ids = form.findChild<QLineEdit*>("primitive_ids");
    REQUIRE(ids != nullptr);
    CHECK(ids->placeholderText().startsWith("primitive ids"));
    form.findChild<QComboBox*>("mesh_id")->setCurrentIndex(1);
    ids->setText("12, 13 14");
    CHECK(form.params()->at("primitive_ids") == Json::array({12, 13, 14}));
    ids->setText("[12,13]");
    CHECK(form.params()->at("primitive_ids") == Json::array({12, 13}));
    ids->setText("12, top");
    CHECK_FALSE(form.params().has_value());
    form.setCommand(registry.find("surface.build"));
    CHECK(form.findChild<QLineEdit*>("flat")->placeholderText() ==
          R"(JSON, e.g. [{"plane_id":1,"selection_id":1}])");

    // An optional object field starts at None and is left out.
    form.setCommand(registry.find("sketch.section"), {{"id", 7}});
    CHECK(form.findChild<QComboBox*>("plane_id")->currentText() == "None");
    CHECK_FALSE(form.params()->contains("plane_id"));

    // A path field browses for the files its command takes, and asks for a new file when the
    // command writes one: a STEP export must not default to meshes.
    const auto browse = [&](const char* command) {
        form.setCommand(registry.find(command));
        const auto* button = form.findChild<QToolButton*>("browse");
        REQUIRE(button != nullptr);
        return std::pair{button->property("filter").toString(),
                         button->property("saving").toBool()};
    };
    CHECK(browse("solid.export") ==
          std::pair{QStringLiteral("STEP, STP (*.step *.stp);;All files (*)"), true});
    CHECK(browse("mesh.import") ==
          std::pair{QStringLiteral("STL, OBJ, PLY (*.stl *.obj *.ply);;All files (*)"), false});
    CHECK(browse("view.render").second); // writes a picture, so a save dialog
    CHECK(browse("project.open").first.startsWith("M2C (*.m2c)"));
}

// The M0 gate (docs/ARCHITECTURE.md section 12): a 10M-triangle scan loads and orbits at 30 fps or
// better. Hidden from normal runs; see tests/README.md for how to run it on a Release build.
TEST_CASE("A 10M-triangle scan loads and orbits at 30 fps", "[.][perf][app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto scan = dir.path() / "scan.ply";
    std::size_t triangles = 0;
    {
        const TriMesh mesh = makeBumpySphere(60.0, 1600, 3200);
        triangles = mesh.faceCount();
        REQUIRE(triangles >= 10'000'000);
        REQUIRE(writeMesh(mesh, scan).has_value());
    }

    Engine engine;
    MainWindow window(engine);
    window.resize(1600, 1000);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    Viewport* view = window.viewport();
    std::size_t frames = 0;
    QObject::connect(view, &QOpenGLWidget::frameSwapped, [&] { ++frames; });
    // Asks for a frame and spins the event loop until it is on screen.
    const auto nextFrame = [&] {
        const std::size_t before = frames;
        view->update();
        return waitFor([&] { return frames > before; }, 60000);
    };

    QElapsedTimer clock;
    clock.start();
    window.openFiles({QString::fromStdString(scan.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; },
                    600000));
    view->fitAll();
    REQUIRE(nextFrame());
    const double loadSeconds = static_cast<double>(clock.elapsed()) / 1000.0;

    // Full resolution, camera still.
    constexpr int kStillFrames = 30;
    clock.restart();
    for (int i = 0; i < kStillFrames; ++i) REQUIRE(nextFrame());
    const double stillFps = kStillFrames / (static_cast<double>(clock.elapsed()) / 1000.0);

    // Orbiting draws the coarse proxy, built in the background after loading.
    clock.restart();
    REQUIRE(waitFor([&] { return view->proxiesReady(); }, 600000));
    const double proxySeconds = static_cast<double>(clock.elapsed()) / 1000.0;

    constexpr int kFrames = 120;
    const QPointF c(view->width() / 2.0, view->height() / 2.0);
    mouse(view, QEvent::MouseButtonPress, c, Qt::LeftButton);
    clock.restart();
    for (int i = 1; i <= kFrames; ++i) {
        mouse(view, QEvent::MouseMove, c + QPointF(3.0 * i, 0.5 * i), Qt::LeftButton);
        REQUIRE(nextFrame());
    }
    const double orbitFps = kFrames / (static_cast<double>(clock.elapsed()) / 1000.0);
    mouse(
        view, QEvent::MouseButtonRelease, c + QPointF(3.0 * kFrames, 0.5 * kFrames), Qt::NoButton);

    std::cout << "perf: " << triangles << " triangles; load " << loadSeconds << " s; still "
              << stillFps << " fps; proxy ready " << proxySeconds << " s later; orbit " << orbitFps
              << " fps\n";
    CHECK(orbitFps >= 30.0);
}

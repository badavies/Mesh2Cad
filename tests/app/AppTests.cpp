// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the desktop window the way a person does (mouse strokes in the viewport) and the way an
// agent does (JSON-RPC on the local socket), and checks both land in the same document.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "m2c/core/SelectionColors.h"
#include "m2c/core/Sketch.h"
#include "m2c/io/MeshIO.h"
#include "m2c/rpc/Session.h"

#include "Camera.h"
#include "Engine.h"
#include "InspectorPanel.h"
#include "MainWindow.h"
#include "ParamForm.h"
#include "TestMeshes.h"
#include "ToolCatalog.h"
#include "Viewport.h"
#include "WorkflowPanel.h"

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

    SECTION("crease and measure orbit when dragged off the mesh, as the brush does") {
        const QPointF corner(20, 20); // the sphere fills the middle of the view only
        REQUIRE_FALSE(view->pickObject(corner).has_value());
        for (const Mode mode : {Mode::Crease, Mode::Shell, Mode::Measure, Mode::Brush}) {
            view->setMode(mode);
            const Mat4d before = view->viewProjection();
            drag(view, {corner, corner + QPointF(40, 0), corner + QPointF(80, 10)});
            QApplication::processEvents();
            CHECK(view->viewProjection().m != before.m);
        }
        CHECK_FALSE(engine.busy());
        CHECK(countKind(*engine.snapshot(), "selection") == 0);
        CHECK(countKind(*engine.snapshot(), "measurement") == 0);
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

    SECTION("the person and an agent cancel a long command") {
        // Measuring a big scan against another takes long enough to be stopped part way.
        const std::string bigPath = (dir.path() / "big.stl").string();
        REQUIRE(writeMesh(makeBumpySphere(10.0, 300, 600), bigPath).has_value());
        for (int i = 0; i < 2; ++i) {
            window.openFiles({QString::fromStdString(bigPath)});
            REQUIRE(waitFor([&] { return !engine.busy(); }));
        }
        const auto objects = engine.snapshot()->objects;
        REQUIRE(objects.size() == 3);
        const Json measure{{"id", objects[1].id}, {"reference_id", objects[2].id}};
        auto* cancel = window.findChild<QPushButton*>("cancelCommand");
        REQUIRE(cancel != nullptr);
        CHECK_FALSE(cancel->isVisible());

        // The person presses Cancel once the command says what it is doing.
        const QObject listener;
        bool shownWhileRunning = false;
        QObject::connect(&engine, &Engine::progressChanged, &listener, [&] {
            shownWhileRunning = cancel->isVisible();
            cancel->click();
        });
        std::optional<Result<Json>> result;
        engine.run("deviation.compute", measure, [&](const Result<Json>& r) { result = r; });
        REQUIRE(waitFor([&] { return result.has_value(); }));
        CHECK(shownWhileRunning);
        REQUIRE_FALSE(result->has_value());
        CHECK(result->error().code == ErrorCode::Cancelled);
        REQUIRE(waitFor([&] { return !engine.busy(); }));
        CHECK_FALSE(cancel->isVisible());
        CHECK(countKind(*engine.snapshot(), "deviation") == 0);
        QObject::disconnect(&engine, &Engine::progressChanged, &listener, nullptr);

        // An agent sends rpc.cancel for its request when it hears the request's first stage.
        REQUIRE(engine.startLocalServer().startsWith("Listening"));
        const auto info = readSessionFile(defaultSessionFile());
        REQUIRE(info.has_value());
        std::atomic<bool> finished{false};
        std::vector<Json> lines;
        std::thread agent([&] {
            if (auto client = LocalClient::connect(info->endpoint)) {
                const auto send = [&](const Json& m) { (void)(*client)->writeLine(m.dump()); };
                send(Json{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "rpc.auth"},
                          {"params", {{"token", info->token}}}});
                send(Json{{"jsonrpc", "2.0"},
                          {"id", 2},
                          {"method", "deviation.compute"},
                          {"params", measure}});
                for (;;) {
                    auto line = (*client)->readLine();
                    if (!line) break;
                    lines.push_back(Json::parse(*line, nullptr, false));
                    if (lines.back().value("method", Json()) == "rpc.progress") {
                        send(Json{{"jsonrpc", "2.0"},
                                  {"id", 3},
                                  {"method", "rpc.cancel"},
                                  {"params", {{"id", 2}}}});
                    }
                    if (lines.back().value("id", Json()) == 2) break;
                }
            }
            finished = true;
        });
        const bool done = waitFor([&] { return finished.load(); });
        agent.join();
        REQUIRE(done);
        const auto reply = [&](int id) {
            const auto it = std::ranges::find_if(
                lines, [id](const Json& j) { return j.value("id", Json()) == id; });
            return it != lines.end() ? *it : Json();
        };
        CHECK(reply(3)["result"]["cancelled"] == true);
        CHECK(reply(2)["error"]["code"] == RpcServer::kCancelled);
        CHECK(reply(2)["error"]["message"].get<std::string>().starts_with("cancelled while"));
        CHECK(countKind(*engine.snapshot(), "deviation") == 0);
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

    // Info on the deviation shows its statistics in the Inspector, not an error.
    auto* inspector = window.findChild<InspectorPanel*>();
    REQUIRE(inspector != nullptr);
    QStringList lines;
    QObject::connect(
        &engine, &Engine::logLine, inspector, [&lines](const QString& line, bool failed) {
            if (failed) lines << line;
        });
    window.showInfo(deviation);
    REQUIRE(waitFor([&] { return inspector->command() == "deviation.info"; }));
    CHECK(inspector->result()["stats"].contains("rms_mm"));
    CHECK(inspector->tree()->findItems("Rms (mm)", Qt::MatchExactly | Qt::MatchRecursive).size() ==
          1);
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

    // Every row shows its object's id, first, so it can be read without hovering; the name
    // column stays the bare name.
    auto* tree = window.findChild<QTreeWidget*>();
    REQUIRE(tree != nullptr);
    CHECK(tree->header()->visualIndex(2) == 0);
    CHECK(tree->header()->visualIndex(0) == 1);
    // The Tools menu's groups start with a capital, like every other menu item.
    int groups = 0;
    for (const QAction* menu : window.menuBar()->actions()) {
        if (menu->text() != "&Tools") continue;
        for (const QAction* group : menu->menu()->actions()) {
            CAPTURE(group->text().toStdString());
            CHECK(group->text().front().isUpper());
            ++groups;
        }
    }
    CHECK(groups == static_cast<int>(toolGroups().size())); // by task, not by prefix
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
        if (a->objectName() == "primitive.solid") tool = a;
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
        if (a->objectName() == "solid.extrude") a->trigger();
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

namespace {

SketchCurve line(Vec2d a, Vec2d b) {
    return SketchCurve{.kind = CurveKind::Line, .points = {a, b}};
}

/// A closed loop of a line, an arc, a spline and a line, on a tilted plane; with `open`, also
/// an open chain of two lines.
Sketch kindsSketch(bool open) {
    Sketch sketch{.plane = planeThrough({1, 2, 3}, {1, 1, 2}), .loops = {}};
    sketch.loops.push_back(SketchLoop{{
        line({0, 0}, {20, 0}),
        SketchCurve{.kind = CurveKind::Arc,
                    .points = {},
                    .centre = {20, 10},
                    .radiusMm = 10,
                    .startRad = -std::numbers::pi / 2,
                    .sweepRad = std::numbers::pi},
        SketchCurve{.kind = CurveKind::Spline, .points = {{20, 20}, {10, 25}, {0, 20}}},
        line({0, 20}, {0, 0}),
    }});
    if (open) sketch.loops.push_back(SketchLoop{{line({30, 0}, {40, 0}), line({40, 0}, {40, 10})}});
    return sketch;
}

std::shared_ptr<const DocSnapshot> sketchSnapshot(const Sketch& sketch, bool visible) {
    auto snapshot = std::make_shared<DocSnapshot>();
    snapshot->objects.push_back(DocumentObject{.id = 1,
                                               .name = "Sketch",
                                               .visible = visible,
                                               .data = SketchData{sketch},
                                               .provenance = {}});
    snapshot->revision = visible ? 1 : 2;
    return snapshot;
}

} // namespace

TEST_CASE("Sketches show curve kinds, the normal and open chain ends", "[app]") {
    const Sketch sketch = kindsSketch(true);
    const SketchScene scene = buildSketchScene(sketch);
    REQUIRE(scene.colors.size() == scene.points.size());
    REQUIRE(scene.segments.size() % 2 == 0);

    // Each kind its own colour, the arrow (its last three segments) another.
    std::set<std::array<float, 3>> curveColors;
    for (std::size_t s = 0; s + 6 < scene.segments.size(); s += 2) {
        const QVector3D& a = scene.colors[scene.segments[s]];
        CHECK(a == scene.colors[scene.segments[s + 1]]);
        curveColors.insert({a.x(), a.y(), a.z()});
    }
    CHECK(curveColors.size() == 3);
    for (const CurveKind kind : {CurveKind::Line, CurveKind::Arc, CurveKind::Spline}) {
        const QVector3D c = sketchCurveColor(kind);
        CHECK(curveColors.contains({c.x(), c.y(), c.z()}));
    }
    const QVector3D arrowColor = scene.colors[scene.segments.back()];
    CHECK_FALSE(curveColors.contains({arrowColor.x(), arrowColor.y(), arrowColor.z()}));

    // Two dots for the open chain, at its ends; none for the closed loop.
    REQUIRE(scene.openEnds.size() == 2);
    const auto near = [](const Vec3d& a, const Vec3d& b) { return length(a - b) < 1e-9; };
    CHECK(near(scene.openEnds[0], sketch.plane.toWorld({40, 10})));
    CHECK(near(scene.openEnds[1], sketch.plane.toWorld({30, 0})));
    CHECK(buildSketchScene(kindsSketch(false)).openEnds.empty());

    // The arrow points along the plane normal, from the origin.
    const Vec3d arrow = scene.arrowTip - scene.arrowTail;
    CHECK(near(scene.arrowTail, sketch.plane.origin));
    CHECK(dot(arrow * (1.0 / length(arrow)), sketch.plane.normal()) > 0.999);

    // Drawn: the arrow, the dots and the curves show while the sketch is visible, none of them
    // once it is hidden.
    application();
    Viewport view;
    view.resize(800, 600);
    view.show();
    REQUIRE(waitFor([&] { return view.isVisible(); }));
    const auto count = [&] {
        view.fitAll();
        QApplication::processEvents();
        const QImage frame = view.grabFramebuffer();
        std::map<std::string, int> n;
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                if (x < 120 && y > frame.height() - 120) continue; // the axis triad
                const QColor p = frame.pixelColor(x, y);
                if (p.red() > 200 && p.green() < 90 && p.blue() < 90) ++n["dot"];
                if (p.red() > 225 && p.green() > 225 && p.blue() > 225) ++n["arrow"];
                if (p.green() > 200 && p.red() > 150 && p.blue() < 110) ++n["line"];
            }
        }
        return n;
    };
    view.setSnapshot(sketchSnapshot(sketch, true));
    std::map<std::string, int> shown = count();
    screenshot(&view, "sketch-kinds");
    CHECK(shown["dot"] > 20);
    CHECK(shown["arrow"] > 10);
    CHECK(shown["line"] > 10);
    view.setSnapshot(sketchSnapshot(sketch, false));
    std::map<std::string, int> hidden = count();
    CHECK(hidden["dot"] == 0);
    CHECK(hidden["arrow"] == 0);
    CHECK(hidden["line"] == 0);
}

TEST_CASE("Hovering reads the deviation, and markers show its extremes", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    // The scan, and the same box 0.25 mm lower as the reference: the scan's top lies 0.25 mm
    // outside it.
    const TriMesh scan = makeBox(30, 20, 10, 4);
    std::vector<Vec3d> lowered = scan.positions();
    for (Vec3d& p : lowered) p.z -= 0.25;
    const auto reference = TriMesh::create(lowered, scan.indices());
    REQUIRE(reference.has_value());
    const auto scanPath = dir.path() / "scan.stl";
    const auto referencePath = dir.path() / "reference.stl";
    REQUIRE(writeMesh(scan, scanPath).has_value());
    REQUIRE(writeMesh(*reference, referencePath).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(scanPath.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    window.openFiles({QString::fromStdString(referencePath.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 2; }));
    const ObjectId scanId = engine.snapshot()->objects[0].id;
    const ObjectId referenceId = engine.snapshot()->objects[1].id;
    const auto run = [&](const char* command, const Json& params) {
        Json out;
        bool done = false;
        engine.run(command, params, [&](const Result<Json>& r) {
            if (r) out = *r;
            done = true;
        });
        REQUIRE(waitFor([&] { return done && !engine.busy(); }));
        return out;
    };
    const auto deviationId =
        run("deviation.compute", {{"id", scanId}, {"reference_id", referenceId}})["id"]
            .get<ObjectId>();
    run("object.show", {{"id", referenceId}, {"visible", false}});
    const DeviationField field = engine.snapshot()->find(deviationId)->deviation()->field;
    const std::vector<double>& d = field.distances();

    Viewport* view = window.viewport();
    view->setMode(Mode::Measure);
    view->setStandardView(-45, 35.264);
    view->fitAll();
    QApplication::processEvents();
    view->grabFramebuffer();
    QString hovered;
    QObject::connect(view, &Viewport::hoverChanged, [&](const QString& t) { hovered = t; });
    // Hovers the vertex at `p` (the widget position it projects to, as Viewport::project).
    const auto hover = [&](const Vec3d& p) {
        const Mat4d vp = view->viewProjection();
        const double x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
        const double y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
        const double w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
        hovered.clear();
        mouse(view,
              QEvent::MouseMove,
              QPointF((x / w + 1.0) * 0.5 * view->width(), (1.0 - y / w) * 0.5 * view->height()),
              Qt::NoButton);
    };

    // A vertex in the middle of the top, where it and its neighbours are all 0.25 mm out.
    const auto& positions = scan.positions();
    const auto top = static_cast<std::size_t>(std::ranges::find(positions, Vec3d{15, 10, 10}) -
                                              positions.begin());
    REQUIRE(top < positions.size());
    hover(positions[top]);
    INFO(hovered.toStdString());
    CHECK(hovered.endsWith(QString::asprintf(" | dev: %+.3f mm", d[top])));
    CHECK(hovered.endsWith(" | dev: +0.250 mm"));

    // The markers sit at the first vertices with the largest and the smallest distance.
    const auto largest = static_cast<std::size_t>(std::ranges::max_element(d) - d.begin());
    const auto smallest = static_cast<std::size_t>(std::ranges::min_element(d) - d.begin());
    std::vector<DeviationMarker> markers = view->deviationMarkers();
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].largest);
    CHECK(markers[0].vertex == largest);
    CHECK(markers[0].point == positions[largest]);
    CHECK(markers[0].valueMm == d[largest]);
    CHECK_FALSE(markers[1].largest);
    CHECK(markers[1].vertex == smallest);
    CHECK(markers[1].point == positions[smallest]);
    CHECK(markers[1].valueMm == d[smallest]);
    screenshot(view, "deviation-markers");

    // With the deviation hidden: no markers, and no deviation in the hover text.
    run("object.show", {{"id", deviationId}, {"visible", false}});
    QApplication::processEvents();
    view->grabFramebuffer();
    CHECK(view->deviationMarkers().empty());
    hover(positions[top]);
    CHECK(hovered.startsWith("Object"));
    CHECK_FALSE(hovered.contains("dev:"));
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
            if (a->objectName() == tool) a->trigger();
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
    // A 3D mouse hit-tests the same objects along a ray of its own: from the eye, through the
    // centre of the view.
    const auto along = view->pickRay(view->eye(), view->camera().forward());
    REQUIRE(along.has_value());
    CHECK(along->objectId == box);
    CHECK(length(along->point - hit->point) < 1e-6);

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
        if (a->objectName() == "fillet.fit") a->trigger();
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
        if (action->objectName() == "solid.info") action->trigger();
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

    // A boolean is a check box at its default, sent only when changed from it.
    auto* borders = form.findChild<QCheckBox*>("preserve_borders");
    REQUIRE(borders != nullptr);
    CHECK(borders->isChecked()); // default true
    form.findChild<QLineEdit*>("target_ratio")->setText("0.25");
    borders->setChecked(false);
    params = form.params();
    REQUIRE(params.has_value());
    CHECK((*params)["target_ratio"] == 0.25);
    CHECK((*params)["preserve_borders"] == false);
    // A typed number is held to the schema's limits (target_ratio is at most 1).
    form.findChild<QLineEdit*>("target_ratio")->setText("2");
    CHECK_FALSE(form.params().has_value());
    form.findChild<QLineEdit*>("target_ratio")->clear();

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
    auto* flat = form.findChild<QLineEdit*>("flat");
    REQUIRE(flat != nullptr);
    CHECK(flat->placeholderText().endsWith(R"(JSON, e.g. [{"plane_id":1,"selection_id":1}])"));

    // surface.build's flat areas are built from rows picked in the Objects list: Add makes an
    // item of a selection and a plane, in either order, and either alone brings the other when
    // the plane was fitted to the selection.
    snapshot->objects.push_back({.id = 10,
                                 .name = "Top",
                                 .data = SelectionData{.meshId = 7, .faces = nullptr},
                                 .provenance = {}});
    snapshot->objects.push_back(
        {.id = 11,
         .name = "Plane fit",
         .data = PrimitiveData{},
         .provenance = {.command = "plane.fit", .params = "", .inputs = {10, 7}}});
    snapshot->objects.push_back({.id = 12,
                                 .name = "Side",
                                 .data = SelectionData{.meshId = 7, .faces = nullptr},
                                 .provenance = {}});
    snapshot->objects.push_back(
        {.id = 13, .name = "Plane", .data = PrimitiveData{}, .provenance = {}});
    form.setObjects(snapshot);
    form.setCommand(registry.find("surface.build"));
    flat = form.findChild<QLineEdit*>("flat");
    auto* add = form.findChild<QToolButton*>("add");
    REQUIRE(add != nullptr);
    form.setSelectedObjects({13, 12});
    add->click();
    CHECK(Json::parse(flat->text().toStdString()) ==
          Json::parse(R"([{"selection_id":12,"plane_id":13}])"));
    form.setSelectedObjects({10});
    add->click();
    add->click(); // the same area again is not listed twice
    CHECK(Json::parse(flat->text().toStdString()) ==
          Json::parse(R"([{"selection_id":12,"plane_id":13},{"selection_id":10,"plane_id":11}])"));
    form.setSelectedObjects({11}); // a plane alone brings the selection it was fitted to
    CHECK(form.addPicked("flat"));
    CHECK(Json::parse(flat->text().toStdString()).size() == 2);
    // Nothing that fits changes nothing.
    form.setSelectedObjects({7});
    CHECK_FALSE(form.addPicked("flat"));
    form.setSelectedObjects({12}); // no plane was fitted to this one
    CHECK_FALSE(form.addPicked("flat"));
    CHECK(Json::parse(flat->text().toStdString()).size() == 2);
    form.findChild<QToolButton*>("clear")->click();
    CHECK(flat->text().isEmpty());

    // Fields come in the order a person fills them, with the tuning parameters that have
    // defaults under Advanced, closed until opened; Reset puts them back and keeps the objects.
    form.setCommand(registry.find("surface.build"), {{"id", 9}, {"mesh_id", 7}});
    const auto order = [&] {
        QStringList names;
        for (const auto* layout : form.findChildren<QFormLayout*>()) {
            for (int i = 0; i < layout->rowCount(); ++i) {
                if (const auto* item = layout->itemAt(i, QFormLayout::FieldRole))
                    names << item->widget()->objectName();
            }
        }
        return names;
    };
    const QStringList names = order();
    REQUIRE(names.size() >= 4);
    CHECK(names.mid(0, 2) == QStringList{"id", "mesh_id"});
    auto* advanced = form.findChild<QToolButton*>("advanced");
    REQUIRE(advanced != nullptr);
    CHECK(advanced->text().startsWith("Advanced"));
    CHECK_FALSE(advanced->isChecked());
    CHECK(form.findChild<QSpinBox*>("fairing")->value() == 5); // the default
    // Values passed in that differ from the defaults open Advanced.
    form.setCommand(registry.find("surface.build"), {{"fairing", 2}});
    CHECK(form.findChild<QToolButton*>("advanced")->isChecked());
    CHECK(form.findChild<QSpinBox*>("fairing")->value() == 2);
    // Numbers with defaults are spin boxes, shown as typed and sent only when changed.
    form.setCommand(registry.find("mesh.smooth"), {{"id", 7}});
    auto* iterations = form.findChild<QSpinBox*>("iterations");
    auto* strength = form.findChild<QDoubleSpinBox*>("strength");
    REQUIRE(iterations != nullptr);
    REQUIRE(strength != nullptr);
    CHECK(strength->text() == "0.5");
    CHECK(*form.params() == Json{{"id", 7}});
    iterations->setValue(25);
    strength->setValue(0.25);
    form.findChild<QCheckBox*>("preserve_sharp_edges")->setChecked(true);
    CHECK(*form.params() ==
          Json{{"id", 7}, {"iterations", 25}, {"strength", 0.25}, {"preserve_sharp_edges", true}});
    form.findChild<QPushButton*>("reset")->click();
    CHECK(*form.params() == Json{{"id", 7}});

    // A list of options is a box for each, checked as the default.
    form.setCommand(registry.find("mesh.segment"), {{"id", 7}});
    auto* kinds = form.findChild<QWidget*>("kinds");
    REQUIRE(kinds != nullptr);
    auto* sphere = kinds->findChild<QCheckBox*>("sphere");
    REQUIRE(sphere != nullptr);
    CHECK(kinds->findChild<QCheckBox*>("plane")->isChecked());
    CHECK_FALSE(sphere->isChecked());
    CHECK_FALSE(form.params()->contains("kinds"));
    sphere->setChecked(true);
    CHECK(form.params()->at("kinds") == Json::array({"plane", "cylinder", "sphere"}));
    for (auto* box : kinds->findChildren<QCheckBox*>()) box->setChecked(false);
    CHECK_FALSE(form.params().has_value()); // at least one kind

    // A list of points counts them; Undo last takes the newest off, and the viewport is told.
    form.setCommand(registry.find("fillet.fit"));
    std::vector<Vec3d> marked;
    QObject::connect(
        &form, &ParamForm::pointsChanged, [&](const std::vector<Vec3d>& p) { marked = p; });
    REQUIRE(form.appendPoint("edge_points", {1, 2, 3}));
    REQUIRE(form.appendPoint("edge_points", {4, 5, 6}));
    CHECK(form.findChild<QLabel*>("count")->text() == "2 points");
    CHECK(marked.size() == 2);
    form.findChild<QToolButton*>("undoPoint")->click();
    CHECK(form.findChild<QLabel*>("count")->text() == "1 point");
    CHECK(Json::parse(form.findChild<QLineEdit*>("edge_points")->text().toStdString()) ==
          Json::parse("[[1,2,3]]"));
    REQUIRE(marked.size() == 1);
    CHECK(marked[0].z == 3.0);
    form.findChild<QPushButton*>("reset")->click(); // picked points are not settings
    CHECK(form.findChild<QLabel*>("count")->text() == "1 point");
    form.setCommand(registry.find("mesh.info"));
    CHECK(marked.empty());

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
TEST_CASE("The camera follows a 3D mouse's camera-to-world transform", "[app]") {
    Camera camera;
    camera.target = {.x = 10, .y = 20, .z = 5};
    camera.distance = 80;
    camera.sceneRadius = 50;
    camera.setView(30, 20);
    const auto close = [](const Vec3d& a, const Vec3d& b) { return length(a - b) < 1e-9; };
    const auto column = [](const Mat4d& m, int c) {
        return Vec3d{.x = m.at(0, c), .y = m.at(1, c), .z = m.at(2, c)};
    };

    // Columns right, up, back and eye, as the Navigation Library expects.
    const Mat4d m = camera.cameraToWorld();
    CHECK(close(column(m, 0), camera.right()));
    CHECK(close(column(m, 1), camera.up()));
    CHECK(close(column(m, 2), camera.forward() * -1.0));
    CHECK(close(column(m, 3), camera.eye()));
    CHECK(std::abs(dot(column(m, 0), column(m, 1))) < 1e-12);

    SECTION("its own transform changes nothing") {
        Camera c = camera;
        c.setCameraToWorld(m);
        CHECK(std::abs(c.yawDeg - 30) < 1e-9);
        CHECK(std::abs(c.pitchDeg - 20) < 1e-9);
        CHECK(std::abs(c.distance - 80) < 1e-9);
        CHECK(close(c.target, camera.target));
    }
    SECTION("moving along the line of sight keeps the target") {
        Mat4d moved = m;
        const Vec3d eye = camera.eye() + camera.forward() * 30.0;
        moved.at(0, 3) = eye.x, moved.at(1, 3) = eye.y, moved.at(2, 3) = eye.z;
        Camera c = camera;
        c.setCameraToWorld(moved);
        CHECK(close(c.eye(), eye));
        CHECK(std::abs(c.distance - 50) < 1e-9);
        CHECK(close(c.target, camera.target));
    }
    SECTION("turning looks the new way from the new eye, around the old target") {
        Camera other;
        other.target = {.x = -40, .y = 5, .z = 60};
        other.setView(100, -10);
        Camera c = camera;
        c.setCameraToWorld(other.cameraToWorld());
        CHECK(std::abs(c.yawDeg - 100) < 1e-9);
        CHECK(std::abs(c.pitchDeg + 10) < 1e-9);
        CHECK(close(c.eye(), other.eye()));
        CHECK(std::abs(dot(camera.target - c.target, c.forward())) < 1e-9);
    }
    SECTION("roll is dropped") {
        // Turn the right and up axes 20 degrees around the line of sight.
        const double a = 20 * std::numbers::pi / 180;
        const Vec3d r = camera.right() * std::cos(a) + camera.up() * std::sin(a);
        const Vec3d u = camera.up() * std::cos(a) - camera.right() * std::sin(a);
        Mat4d rolled = m;
        rolled.at(0, 0) = r.x, rolled.at(1, 0) = r.y, rolled.at(2, 0) = r.z;
        rolled.at(0, 1) = u.x, rolled.at(1, 1) = u.y, rolled.at(2, 1) = u.z;
        Camera c = camera;
        c.setCameraToWorld(rolled);
        CHECK(std::abs(c.yawDeg - 30) < 1e-9);
        CHECK(std::abs(c.pitchDeg - 20) < 1e-9);
        CHECK(std::abs(c.right().z) < 1e-12);
    }
    SECTION("upside down past straight up, it stops looking down from above") {
        Mat4d over = m;
        for (int row = 0; row < 3; ++row) {
            over.at(row, 0) = -m.at(row, 0);
            over.at(row, 1) = -m.at(row, 1);
        }
        Camera c = camera;
        c.setCameraToWorld(over);
        CHECK(c.pitchDeg == 89.9);
        CHECK(std::abs(c.yawDeg - 30) < 1e-9);
    }
    SECTION("looking straight down, the right axis gives the heading") {
        Mat4d down; // right +X, up +Y, back +Z
        down.at(2, 3) = 100;
        Camera c = camera;
        c.setCameraToWorld(down);
        CHECK(std::abs(c.yawDeg + 90) < 1e-9);
        CHECK(c.pitchDeg == 89.9);
        CHECK(close(c.eye(), {.x = 0, .y = 0, .z = 100}));
    }
}

TEST_CASE("The workflow lists every track and shows the stage gate", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto ball = dir.path() / "ball.stl";
    const auto cap = dir.path() / "cap.stl";
    REQUIRE(writeMesh(makeSphere(10.0, 3), ball).has_value());
    REQUIRE(writeMesh(makeCap(10.0, 1.0, 6), cap).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    auto* workflow = window.findChild<WorkflowPanel*>();
    REQUIRE(workflow != nullptr);
    CHECK(workflow->gateText().startsWith("Import a scan"));

    // Every track's steps are there, past M1: surfacing, prismatic tools, deviation and export.
    for (const char* step : {"holes.fill",
                             "align.symmetry",
                             "quad.remesh",
                             "surface.build",
                             "mesh.segment",
                             "primitive.trim",
                             "solid.boolean",
                             "fillet.fit",
                             "deviation.compute",
                             "solid.export"}) {
        INFO(step);
        CHECK(workflow->findChild<QPushButton*>(step) != nullptr);
    }
    workflow->findChild<QPushButton*>("surface.build")->click();
    CHECK(window.findChild<ParamForm*>()->command()->name == "surface.build");

    // A closed sphere passes the repair gate; an open cap does not, and says why. The check runs
    // in the background, without a busy cursor or a log line.
    QStringList log;
    QObject::connect(&engine, &Engine::logLine, [&](const QString& line, bool) { log << line; });
    window.openFiles({QString::fromStdString(ball.string())});
    REQUIRE(waitFor([&] { return workflow->gateText().contains("closed, manifold"); }));
    CHECK(workflow->gatePassed());
    window.openFiles({QString::fromStdString(cap.string())});
    REQUIRE(waitFor([&] { return workflow->gateText().contains("not ready"); }));
    CHECK_FALSE(workflow->gatePassed());
    CHECK(workflow->gateText().contains("open edges around holes: Fill holes"));
    REQUIRE(waitFor([&] { return !engine.busy(); }));
    CHECK_FALSE(std::ranges::any_of(log, [](const QString& l) {
        return l.contains("mesh.info") || l.contains("shells.detect");
    }));

    // The Tools menu groups commands by task and titles them, with the command's name beside.
    QMenu* tools = nullptr;
    for (QAction* a : window.menuBar()->actions()) {
        if (a->text() == "&Tools") tools = a->menu();
    }
    REQUIRE(tools != nullptr);
    QStringList groups;
    for (QAction* a : tools->actions()) {
        if (a->isVisible()) groups << a->text();
    }
    CHECK(groups.contains("Mesh cleanup"));
    CHECK(groups.contains("Surfacing"));
    CHECK_FALSE(groups.contains("Holes")); // no one-item groups by prefix
    QAction* fill = nullptr;
    for (QAction* a : window.findChildren<QAction*>()) {
        if (a->objectName() == "holes.fill") fill = a;
    }
    REQUIRE(fill != nullptr);
    CHECK(fill->text() == QStringLiteral("Fill holes…\tholes.fill"));
}

TEST_CASE("Object menus offer the tools for their kind, and History jumps to a step", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    const auto ball = dir.path() / "ball.stl";
    REQUIRE(writeMesh(makeSphere(10.0, 3), ball).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(ball.string())});
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
    const auto layout =
        run("quad.remesh", {{"id", mesh}, {"target_faces", 300}})["id"].get<ObjectId>();
    auto* form = window.findChild<ParamForm*>();
    const auto menuFor = [&](ObjectId id) {
        auto menu = std::make_unique<QMenu>();
        window.fillObjectMenu(*menu, id);
        return menu;
    };
    const auto action = [](const QMenu& menu, const char* name) -> QAction* {
        for (QAction* a : menu.actions()) {
            if (a->objectName() == name) return a;
        }
        return nullptr;
    };

    // A mesh offers its tools; a quad layout Build surface, with the layout filled in.
    auto meshMenu = menuFor(mesh);
    CHECK(action(*meshMenu, "holes.fill") != nullptr);
    CHECK(action(*meshMenu, "sketch.section") != nullptr);
    CHECK(action(*meshMenu, "surface.build") == nullptr);
    auto layoutMenu = menuFor(layout);
    QAction* build = action(*layoutMenu, "surface.build");
    REQUIRE(build != nullptr);
    CHECK(build->text() == QStringLiteral("Build surface…"));
    build->trigger();
    REQUIRE(form->command() != nullptr);
    CHECK(form->command()->name == "surface.build");
    CHECK(form->findChild<QComboBox*>("id")->currentData().value<ObjectId>() == layout);

    // The layout was made by quad.remesh: its menu opens that form with the same parameters.
    QAction* rerun = action(*layoutMenu, "rerun");
    REQUIRE(rerun != nullptr);
    CHECK(rerun->text() == QStringLiteral("Change and run Quad remesh again…"));
    rerun->trigger();
    CHECK(form->command()->name == "quad.remesh");
    CHECK(form->findChild<QSpinBox*>("target_faces")->value() == 300);
    CHECK(form->findChild<QComboBox*>("id")->currentData().value<ObjectId>() == mesh);
    CHECK(form->findChild<QToolButton*>("advanced")->isChecked()); // target_faces is changed

    // History: a first row for the start, then each step; a double-click undoes or redoes to it.
    run("mesh.smooth", {{"id", mesh}});
    run("mesh.flip", {{"id", mesh}});
    auto* history = window.findChild<QListWidget*>();
    REQUIRE(history != nullptr);
    const std::size_t steps = engine.snapshot()->undo.size();
    REQUIRE(history->count() == static_cast<int>(steps) + 1);
    CHECK(history->item(0)->text() == "(start)");
    emit history->itemActivated(history->item(static_cast<int>(steps) - 2)); // two back
    REQUIRE(waitFor([&] { return !engine.busy() && engine.snapshot()->redo.size() == 2; }));
    CHECK(engine.snapshot()->undo.size() == steps - 2);
    CHECK(engine.snapshot()->redo.front().label == "mesh.smooth");
    REQUIRE(history->count() == static_cast<int>(steps) + 1);
    emit history->itemActivated(history->item(history->count() - 1)); // redo both
    REQUIRE(waitFor([&] { return !engine.busy() && engine.snapshot()->redo.empty(); }));
    CHECK(engine.snapshot()->undo.size() == steps);
    emit history->itemActivated(history->item(0)); // back to the start
    REQUIRE(waitFor([&] { return !engine.busy() && engine.snapshot()->undo.empty(); }));
    CHECK(countKind(*engine.snapshot(), "mesh") == 0);
}

TEST_CASE("The Inspector lists shells and hotspots, and the Shell tool picks a part", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    // Two balls in one mesh, the second smaller and off to the side; and a bumpy ball.
    const TriMesh big = makeSphere(10.0, 3);
    const TriMesh small = makeSphere(5.0, 3, {.x = 40.0, .y = 0.0, .z = 0.0});
    std::vector<Vec3d> positions = big.positions();
    positions.insert(positions.end(), small.positions().begin(), small.positions().end());
    std::vector<std::uint32_t> indices = big.indices();
    for (const std::uint32_t i : small.indices())
        indices.push_back(i + static_cast<std::uint32_t>(big.positions().size()));
    const auto pair = dir.path() / "pair.stl";
    const auto bumpy = dir.path() / "bumpy.stl";
    REQUIRE(writeMesh(makeMesh(std::move(positions), std::move(indices)), pair).has_value());
    REQUIRE(writeMesh(makeBumpySphere(10.0, 30, 60), bumpy).has_value());

    Engine engine;
    MainWindow window(engine);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitFor([&] { return window.viewport()->isVisible(); }));
    window.openFiles({QString::fromStdString(pair.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 1; }));
    const ObjectId mesh = engine.snapshot()->objects.front().id;
    auto* inspector = window.findChild<InspectorPanel*>();
    REQUIRE(inspector != nullptr);
    Viewport* view = window.viewport();

    // List shells, from the Workflow, runs into the Inspector: a row per shell, the main one
    // marked, each with a box to check.
    window.findChild<QPushButton*>("shells.detect")->click();
    window.findChild<ParamForm*>()->runRequested();
    REQUIRE(waitFor([&] { return !engine.busy() && inspector->command() == "shells.detect"; }));
    const auto shellRows =
        inspector->tree()->findItems("Shell ", Qt::MatchStartsWith | Qt::MatchRecursive);
    REQUIRE(shellRows.size() == 2);
    CHECK(shellRows[0]->text(0) == "Shell 0 (main)");
    CHECK(shellRows[1]->text(0) == "Shell 1");
    CHECK(shellRows[1]->text(1).startsWith("Area (mm²)"));

    // Clicking a shell looks at it: the small ball fills the view, so the centre is on it.
    view->fitAll();
    emit inspector->tree()->itemClicked(shellRows[1], 0);
    view->setMode(Mode::Shell);
    const QPointF c(view->width() / 2.0, view->height() / 2.0);
    drag(view, {c});
    REQUIRE(
        waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "selection") == 1; }));
    CHECK(engine.snapshot()->undo.back().label == "select.shell");
    const auto selected = [&] {
        for (const auto& o : engine.snapshot()->objects) {
            if (o.selection() != nullptr) return o.selection()->faces->count();
        }
        return std::size_t{0};
    };
    CHECK(selected() == small.faceCount());

    // Checked shells become one selection; Keep main deletes the rest and lists them again.
    auto* selectShells = inspector->findChild<QPushButton*>("selectShells");
    REQUIRE(selectShells != nullptr);
    CHECK_FALSE(selectShells->isEnabled());
    shellRows[0]->setCheckState(0, Qt::Checked);
    CHECK(selectShells->isEnabled());
    CHECK(inspector->checkedShells() == std::vector<int>{0});
    selectShells->click();
    REQUIRE(
        waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "selection") == 2; }));
    CHECK(selected() == big.faceCount());
    inspector->findChild<QPushButton*>("keepMain")->click();
    REQUIRE(waitFor([&] {
        return !engine.busy() && inspector->result().value("shells", Json::array()).size() == 1;
    }));
    CHECK(engine.snapshot()->find(mesh)->mesh()->mesh->faceCount() == big.faceCount());

    // A deviation picked in the Objects list shows its hotspots; each looks at its patch, and
    // Select hotspots measures again, making each a selection.
    window.openFiles({QString::fromStdString(bumpy.string())});
    REQUIRE(waitFor([&] { return !engine.busy() && countKind(*engine.snapshot(), "mesh") == 2; }));
    const ObjectId scan = engine.snapshot()->objects.back().id;
    bool done = false;
    engine.run("deviation.compute",
               {{"id", scan}, {"reference_id", mesh}, {"tolerance_mm", 0.05}},
               [&](const Result<Json>&) { done = true; });
    REQUIRE(waitFor([&] { return done && !engine.busy(); }));
    auto* objects = window.findChild<QTreeWidget*>();
    for (QTreeWidgetItemIterator it(objects); *it != nullptr; ++it) {
        if ((*it)->text(0).endsWith("(deviation)")) objects->setCurrentItem(*it);
    }
    REQUIRE(waitFor([&] { return !engine.busy() && inspector->command() == "deviation.info"; }));
    const auto hotspots =
        inspector->tree()->findItems("Hotspot ", Qt::MatchStartsWith | Qt::MatchRecursive);
    REQUIRE_FALSE(hotspots.empty());
    const Mat4d before = view->viewProjection();
    emit inspector->tree()->itemClicked(hotspots.front(), 0);
    CHECK(view->viewProjection().m != before.m);
    const std::size_t selections = countKind(*engine.snapshot(), "selection");
    auto* selectHotspots = inspector->findChild<QPushButton*>("selectHotspots");
    REQUIRE(selectHotspots->isVisibleTo(inspector));
    selectHotspots->click();
    REQUIRE(waitFor([&] {
        return !engine.busy() && countKind(*engine.snapshot(), "deviation") == 2 &&
               inspector->command() == "deviation.info";
    }));
    CHECK(countKind(*engine.snapshot(), "selection") ==
          selections + static_cast<std::size_t>(hotspots.size()));
}

TEST_CASE("Help opens the user guide and follows its links", "[app]") {
    application();
    const TempDir dir;
    qputenv("XDG_RUNTIME_DIR", QByteArray::fromStdString(dir.path().string()));
    qputenv("LOCALAPPDATA", QByteArray::fromStdString(dir.path().string()));
    Engine engine;
    MainWindow window(engine);
    QAction* help = nullptr;
    for (QAction* a : window.findChildren<QAction*>()) {
        if (a->text() == "User guide") help = a;
    }
    REQUIRE(help != nullptr);
    help->trigger();
    // Not installed, so it is the source tree's guide.
    auto* guide = window.findChild<QTextBrowser*>("userGuide");
    REQUIRE(guide != nullptr);
    CHECK(guide->isVisible());
    CHECK(guide->toPlainText().startsWith("Mesh2Cad user guide"));
    guide->setSource(guide->source().resolved(QUrl("export.md")));
    CHECK(guide->toPlainText().startsWith("Export"));
    help->trigger(); // the same window again
    CHECK(window.findChildren<QTextBrowser*>("userGuide").size() == 1);
}

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

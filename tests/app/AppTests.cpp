// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the desktop window the way a person does (mouse strokes in the viewport) and the way an
// agent does (JSON-RPC on the local socket), and checks both land in the same document.

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <atomic>
#include <chrono>
#include <thread>

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
    }
}

TEST_CASE("Tool forms are generated from command schemas", "[app]") {
    application();
    CommandRegistry registry;
    REQUIRE(registerBuiltinCommands(registry).has_value());
    ParamForm form;

    form.setCommand(registry.find("mesh.simplify"), {{"id", 7}});
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
    CHECK_FALSE(form.params().has_value()); // id and radius are required
    form.findChild<QLineEdit*>("id")->setText("1");
    form.findChild<QLineEdit*>("radius_mm")->setText("2");
    form.findChild<QLineEdit*>("center")->setText("1, 2.5 3");
    form.findChild<QComboBox*>("mode")->setCurrentText("subtract");
    params = form.params();
    REQUIRE(params.has_value());
    CHECK((*params)["center"] == Json::array({1.0, 2.5, 3.0}));
    CHECK((*params)["mode"] == "subtract");
    form.findChild<QLineEdit*>("centers")->setText("[[0,0,0],[1,1,1]]");
    CHECK(form.params()->at("centers").size() == 2);
}

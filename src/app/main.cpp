// SPDX-License-Identifier: GPL-3.0-or-later
// mesh2cad-app: the desktop app (ARCHITECTURE.md section 9).
//
//   mesh2cad-app [files...]                 open meshes (.stl .obj .ply) or a project (.m2c)
//   mesh2cad-app --screenshot out.png FILE  render FILE, save the viewport and quit (CI smoke test)

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QImage>
#include <QObject>
#include <QString>
#include <QSurfaceFormat>
#include <QTimer>

#include "Engine.h"
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // The viewport needs OpenGL 3.3 core; set it before the application creates any context.
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Mesh2Cad"));
    QApplication::setOrganizationName(QStringLiteral("Mesh2Cad"));
    QApplication::setApplicationVersion(QStringLiteral(MESH2CAD_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Mesh2Cad desktop app"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption screenshot(
        QStringLiteral("screenshot"),
        QStringLiteral("Render the given files, save the viewport to <png> and quit."),
        QStringLiteral("png"));
    const QCommandLineOption noServer(QStringLiteral("no-agent-socket"),
                                      QStringLiteral("Do not accept script or agent connections."));
    parser.addOption(screenshot);
    parser.addOption(noServer);
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Meshes to import or a project to open."));
    parser.process(app);

    m2c::app::Engine engine;
    m2c::app::MainWindow window(engine);
    if (!parser.isSet(noServer) && !parser.isSet(screenshot)) {
        emit engine.logLine(engine.startLocalServer(), false);
    }
    if (!engine.sessionLogPath().isEmpty()) {
        emit engine.logLine(QStringLiteral("Session log: %1").arg(engine.sessionLogPath()), false);
    }
    window.show();
    window.openFiles(parser.positionalArguments());

    if (parser.isSet(screenshot)) {
        const QString out = parser.value(screenshot);
        auto* poll = new QTimer(&app);
        int waited = 0;
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll]() mutable {
            waited += 200;
            if (engine.busy() && waited < 60000) return;
            poll->stop();
            window.viewport()->fitAll();
            QTimer::singleShot(300, &app, [&] {
                const QImage image = window.viewport()->grabFramebuffer();
                if (image.isNull()) {
                    qCritical("--screenshot: the viewport has no picture (no OpenGL 3.3 context?)");
                    QApplication::exit(1);
                } else if (!image.save(out)) {
                    qCritical("--screenshot: could not write %s", qPrintable(out));
                    QApplication::exit(1);
                } else {
                    QApplication::exit(0);
                }
            });
        });
        poll->start(200);
    }
    return QApplication::exec();
}

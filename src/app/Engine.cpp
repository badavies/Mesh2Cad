// SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <Qt>
#include <QtGlobal>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "m2c/commands/Command.h"
#include "m2c/core/Error.h"
#include "m2c/document/Document.h"
#include "m2c/rpc/RpcServer.h"
#include "m2c/rpc/Session.h"

namespace m2c::app {

namespace {

/// One line for the log panel: "[source] command  summary".
QString summarize(const std::string& source, const std::string& command, const Result<Json>& r) {
    QString line = QStringLiteral("[%1] %2").arg(QString::fromStdString(source),
                                                 QString::fromStdString(command));
    if (!r) {
        return line + QStringLiteral("  failed: ") + QString::fromStdString(r.error().message);
    }
    if (const Json& j = *r; j.is_object()) {
        if (j.contains("id") && j["id"].is_number()) {
            line += QStringLiteral("  #%1").arg(j["id"].get<ObjectId>());
        }
        if (j.contains("selection_id")) {
            line += QStringLiteral("  selection #%1").arg(j["selection_id"].get<ObjectId>());
        }
        if (j.contains("face_count") && j["face_count"].is_number()) {
            line += QStringLiteral("  %1 faces").arg(j["face_count"].get<std::uint64_t>());
        }
        if (j.contains("stats") && j["stats"].contains("face_count")) {
            line += QStringLiteral("  %1 faces").arg(j["stats"]["face_count"].get<std::uint64_t>());
        }
        if (j.contains("distance_mm") && j["distance_mm"].is_number()) {
            line += QStringLiteral("  %1 mm").arg(j["distance_mm"].get<double>(), 0, 'f', 3);
        }
        if (j.contains("path") && j["path"].is_string()) {
            line +=
                QStringLiteral("  -> %1").arg(QString::fromStdString(j["path"].get<std::string>()));
        }
    }
    return line;
}

/// Whether a command wrote a file: one that may write files did when its result names the path
/// (view.render without a path returns its picture instead; ADR-031).
bool wroteFile(const Command* c, const Json& result) {
    return c->writesFiles && result.is_object() && result.contains("path");
}

} // namespace

const DocumentObject* DocSnapshot::find(const ObjectId id) const {
    for (const DocumentObject& o : objects) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

Engine::Engine(QObject* parent) : QObject(parent) {
    if (const auto added = registerBuiltinCommands(registry_); !added) {
        qFatal("registering commands failed: %s", added.error().message.c_str());
    }
    uiContext_ = std::make_unique<CommandContext>(document_);
    uiContext_->source = "ui";
    uiContext_->sessionLog = [this](const Json& entry) { writeLog(entry); };
    uiContext_->progress = [this](std::string_view stage, double fraction) {
        showProgress(uiContext_->currentCommand, stage, fraction);
    };

    // Session log: every change from every source, one JSON object per line, replayable.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions";
    QDir().mkpath(dir);
    log_ = std::make_unique<QFile>(
        dir + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".jsonl");
    if (!log_->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) log_.reset();

    snapshot_ = std::make_shared<DocSnapshot>();
    worker_ = std::thread([this] { workerLoop(); });
    connect(&server_, &QLocalServer::newConnection, this, &Engine::onNewConnection);
}

Engine::~Engine() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    worker_.join();
    // Sockets are children of the server and emit `disconnected` while it is destroyed, after
    // this object's members are gone; detach them first.
    for (const auto& c : std::views::values(connections_)) c->socket->disconnect(this);
    connections_.clear();
    server_.close();
    if (!sessionFile_.empty()) {
        // Remove the file only if it is still ours (another app instance may have replaced it).
        if (const auto info = readSessionFile(sessionFile_); info && info->token == token_) {
            std::error_code ignored;
            std::filesystem::remove(sessionFile_, ignored);
        }
    }
}

QString Engine::sessionLogPath() const {
    return log_ ? log_->fileName() : QString();
}

void Engine::post(std::function<void()> job) {
    {
        const std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Engine::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
        publish();
    }
}

void Engine::publish() {
    if (document_.revision() == published_) return;
    published_ = document_.revision();
    auto snap = std::make_shared<DocSnapshot>();
    snap->objects = document_.objects();
    snap->revision = document_.revision();
    snap->undo = document_.undoHistory();
    snap->redo = document_.redoHistory();
    const std::uint64_t revision = snap->revision;
    QMetaObject::invokeMethod(
        this,
        [this, snap = std::shared_ptr<const DocSnapshot>(std::move(snap)), revision] {
            snapshot_ = snap;
            emit documentChanged(snap);
            // Tell every connected client, so an agent sees edits made by the person (and the
            // other way round).
            const Json event{{"jsonrpc", "2.0"},
                             {"method", "doc.changed"},
                             {"params", {{"revision", revision}}}};
            const QByteArray line = QByteArray::fromStdString(event.dump() + "\n");
            for (const auto& c : std::views::values(connections_)) {
                if (c->authenticated) c->socket->write(line);
            }
        },
        Qt::QueuedConnection);
}

void Engine::showProgress(const std::string& command,
                          const std::string_view stage,
                          const double fraction) {
    QString text = QStringLiteral("%1: %2").arg(QString::fromStdString(command),
                                                QString::fromStdString(std::string(stage)));
    if (fraction >= 0.0) text += QStringLiteral(" (%1%)").arg(qRound(fraction * 100.0));
    QMetaObject::invokeMethod(
        this,
        [this, text] {
            busyShown_ = true; // so the status clears when the command finishes
            emit progressChanged(text);
        },
        Qt::QueuedConnection);
}

void Engine::writeLog(const Json& entry) const {
    if (!log_) return;
    Json line = entry;
    line["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    log_->write(QByteArray::fromStdString(line.dump() + "\n"));
    log_->flush();
}

void Engine::run(const QString& command, const Json& params, Done done) {
    ++pending_;
    busyShown_ = true;
    emit busyChanged(true, command);
    const std::string name = command.toStdString();
    post([this, name, params, done = std::move(done)] {
        const auto started = std::chrono::steady_clock::now();
        Result<Json> result = registry_.execute(*uiContext_, name, params);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        publish(); // the UI sees the new document before the callback runs
        const Command* c = registry_.find(name);
        const bool quiet = result && c != nullptr && !c->mutatesDocument && !wroteFile(c, *result);
        QString line = summarize("ui", name, result);
        if (ms >= 100)
            line += QStringLiteral("  (%1 s)").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 1);
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result), done, line, quiet, name] {
                --pending_;
                if (!quiet) emit logLine(line, !result.has_value());
                busyShown_ = pending_ > 0;
                emit busyChanged(busyShown_, QString::fromStdString(name));
                if (done) done(result);
            },
            Qt::QueuedConnection);
    });
}

// ---- Local socket ----------------------------------------------------------------------------

QString Engine::startLocalServer() {
    token_ = makeSessionToken();
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = QStringLiteral("mesh2cad-%1").arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(name);
    if (!server_.listen(name)) {
        return QStringLiteral("Agents cannot connect: %1").arg(server_.errorString());
    }
    sessionFile_ = defaultSessionFile();
    const SessionInfo info{.endpoint = server_.fullServerName().toStdString(),
                           .token = token_,
                           .processId = QCoreApplication::applicationPid()};
    if (const auto written = writeSessionFile(info, sessionFile_); !written) {
        sessionFile_.clear();
        return QStringLiteral("Agents cannot connect: %1")
            .arg(QString::fromStdString(written.error().message));
    }
    return QStringLiteral("Listening for scripts and AI agents (mesh2cad-mcp) on %1")
        .arg(server_.fullServerName());
}

void Engine::onNewConnection() {
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        const int id = nextConnection_++;
        const auto c = std::make_shared<Connection>();
        c->socket = socket;
        c->context = std::make_unique<CommandContext>(document_);
        c->context->source = "json";
        c->context->sessionLog = [this](const Json& entry) { writeLog(entry); };
        // The client hears its own commands' stages (rpc.progress) before their replies; the
        // person sees them in the status bar. publish() sends doc.changed to every client.
        c->rpc =
            std::make_unique<RpcServer>(registry_, *c->context, token_, [this, id](const Json& n) {
                if (n.value("method", Json()) != "rpc.progress") return;
                const Json p = n.value("params", Json::object());
                showProgress(p.value("command", std::string()),
                             p.value("stage", std::string()),
                             p.value("fraction", -1.0));
                const QByteArray line = QByteArray::fromStdString(n.dump() + "\n");
                QMetaObject::invokeMethod(
                    this,
                    [this, id, line] {
                        if (const auto conn = connections_.find(id); conn != connections_.end())
                            conn->second->socket->write(line);
                    },
                    Qt::QueuedConnection);
            });
        connections_[id] = c;
        connect(socket, &QLocalSocket::readyRead, this, [this, id] { onReadyRead(id); });
        connect(socket, &QLocalSocket::disconnected, this, [this, id] { onDisconnected(id); });
        emit clientsChanged(static_cast<int>(connections_.size()));
    }
}

void Engine::onReadyRead(int connectionId) {
    const auto it = connections_.find(connectionId);
    if (it == connections_.end()) return;
    const std::shared_ptr<Connection> c = it->second;
    c->buffer += c->socket->readAll();
    if (c->buffer.size() > static_cast<qsizetype>(64U << 20U)) { // a runaway client; drop it
        c->socket->disconnectFromServer();
        return;
    }
    for (qsizetype nl = c->buffer.indexOf('\n'); nl >= 0; nl = c->buffer.indexOf('\n')) {
        std::string line = c->buffer.left(nl).toStdString();
        c->buffer.remove(0, nl + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        ++pending_;
        post([this, c, line = std::move(line), connectionId] {
            const Json request = Json::parse(line, nullptr, false);
            const bool named =
                request.is_object() && request.contains("method") && request["method"].is_string();
            const std::string method = named ? request["method"].get<std::string>() : "";
            const Command* command = registry_.find(method);
            // A command the log will show still running after a while says so, so a long one
            // does not look hung.
            const auto finished = std::make_shared<std::atomic<bool>>(false);
            const bool givesPath = named && request.contains("params") &&
                                   request["params"].is_object() &&
                                   request["params"].contains("path");
            if (command != nullptr &&
                (command->mutatesDocument || (command->writesFiles && givesPath))) {
                const QString running = QStringLiteral("[%1] %2  running...")
                                            .arg(QString::fromStdString(c->context->source),
                                                 QString::fromStdString(method));
                QMetaObject::invokeMethod(
                    this,
                    [this, finished, running] {
                        QTimer::singleShot(runningNotice_, this, [this, finished, running] {
                            if (!finished->load()) emit logLine(running, false);
                        });
                    },
                    Qt::QueuedConnection);
            }
            const auto started = std::chrono::steady_clock::now();
            const std::string reply = c->rpc->handleLine(line);
            finished->store(true);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
            c->authenticated = c->rpc->authenticated();
            // Log what the client did, so the person can follow an agent's work.
            QString logText;
            bool failed = false;
            if (named) {
                // A notification has no reply; the parse then gives a discarded value.
                const Json response = Json::parse(reply, nullptr, false);
                failed = response.is_object() && response.contains("error");
                const Json result =
                    !failed && response.is_object() ? response.value("result", Json()) : Json();
                if (command != nullptr &&
                    (command->mutatesDocument || wroteFile(command, result) || failed)) {
                    const Result<Json> r =
                        failed ? Result<Json>(
                                     std::unexpected(Error{.code = ErrorCode::InvalidArgument,
                                                           .message = response["error"].value(
                                                               "message", std::string("error"))}))
                               : Result<Json>(result);
                    logText = summarize(c->context->source, method, r);
                    if (ms >= 100) {
                        logText += QStringLiteral("  (%1 s)")
                                       .arg(static_cast<double>(ms) / 1000.0, 0, 'f', 1);
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, connectionId, reply, logText, failed] {
                    --pending_;
                    // A UI command that finished first left the busy state on for this one.
                    if (pending_ == 0 && busyShown_) {
                        busyShown_ = false;
                        emit busyChanged(false, QString());
                    }
                    if (!logText.isEmpty()) emit logLine(logText, failed);
                    if (const auto conn = connections_.find(connectionId);
                        conn != connections_.end() && !reply.empty()) {
                        conn->second->socket->write(QByteArray::fromStdString(reply + "\n"));
                    }
                },
                Qt::QueuedConnection);
        });
    }
}

void Engine::onDisconnected(const int connectionId) {
    const auto it = connections_.find(connectionId);
    if (it == connections_.end()) return;
    it->second->socket->deleteLater();
    // The engine thread may still hold the connection for a queued request; the shared pointer
    // keeps its context alive until that request finishes.
    connections_.erase(it);
    emit clientsChanged(static_cast<int>(connections_.size()));
}

} // namespace m2c::app

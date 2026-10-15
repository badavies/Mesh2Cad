// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "m2c/commands/Command.h"
#include "m2c/rpc/RpcServer.h"
#include "m2c/rpc/Session.h"

class QFile;
class QLocalSocket;

namespace m2c::app {

/// What the UI draws and lists: a copy of the object table (shared pointers, so cheap) taken by
/// the engine thread after every change. The UI never touches the Document itself.
struct DocSnapshot {
    std::vector<DocumentObject> objects;
    std::uint64_t revision{0};
    std::vector<HistoryStep> undo; ///< Oldest first.
    std::vector<HistoryStep> redo; ///< Next redo first.

    [[nodiscard]] const DocumentObject* find(ObjectId id) const;
};

/// Owns the document and the command registry and runs every command on one worker thread, in
/// order, whoever sent it: the UI, or a script or AI agent over the local socket (ARCHITECTURE.md
/// section 8). The UI stays responsive while a long tool runs, and commands from different
/// sources can never interleave inside one undo step.
///
/// The local socket speaks JSON-RPC, one message per line, and needs the per-session token from
/// the session file. `mesh2cad-mcp` finds a running app through that file.
class Engine : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(const Result<Json>&)>;

    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Queues a command from the UI. `done` runs on the UI thread with the result.
    void run(const QString& command, const Json& params, Done done = {});

    /// Starts listening for local connections and writes the session file. Returns a message
    /// for the log either way; failure only means agents cannot connect.
    QString startLocalServer();

    [[nodiscard]] const CommandRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] std::shared_ptr<const DocSnapshot> snapshot() const { return snapshot_; }
    [[nodiscard]] bool busy() const noexcept { return pending_ > 0; }
    [[nodiscard]] QString sessionLogPath() const;

signals:
    void documentChanged(std::shared_ptr<const m2c::app::DocSnapshot> snapshot);
    /// A finished command, from any source: a short line for the log panel.
    void logLine(const QString& line, bool error);
    void busyChanged(bool busy, const QString& command);
    void clientsChanged(int count);

private:
    struct Connection {
        QLocalSocket* socket{nullptr};
        std::unique_ptr<CommandContext> context; // engine thread only
        std::unique_ptr<RpcServer> rpc;          // engine thread only
        QByteArray buffer;                       // UI thread only
        std::atomic<bool> authenticated{false};  // written by the engine thread
    };

    void post(std::function<void()> job);
    void workerLoop();
    /// Sends a new snapshot to the UI if the document changed since the last one.
    void publish();
    void writeLog(const Json& entry);
    void onNewConnection();
    void onReadyRead(int connectionId);
    void onDisconnected(int connectionId);

    // Engine thread state.
    Document document_;
    CommandRegistry registry_;
    std::unique_ptr<CommandContext> uiContext_;
    std::uint64_t published_{0};

    // Worker queue.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{false};
    std::thread worker_;

    // UI thread state.
    std::shared_ptr<const DocSnapshot> snapshot_;
    int pending_{0};
    QLocalServer server_;
    std::string token_;
    std::filesystem::path sessionFile_;
    std::map<int, std::shared_ptr<Connection>> connections_;
    int nextConnection_{1};
    std::unique_ptr<QFile> log_;
};

} // namespace m2c::app

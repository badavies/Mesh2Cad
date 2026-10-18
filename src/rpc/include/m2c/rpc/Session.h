// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "m2c/core/Error.h"

namespace m2c {

/// How to reach a running desktop app: its local socket (a named pipe on Windows) and the
/// per-session token. The app writes this file when it starts and removes it when it quits; only
/// the current user can read it (ARCHITECTURE.md section 8: no network listener, local
/// connections need a per-session token).
struct SessionInfo {
    std::string endpoint; ///< Full socket path, or \\.\pipe\name on Windows.
    std::string token;
    long long processId{0};
};

/// Per-user location of the session file.
[[nodiscard]] std::filesystem::path defaultSessionFile();

[[nodiscard]] Result<void> writeSessionFile(const SessionInfo& info,
                                            const std::filesystem::path& path);
[[nodiscard]] Result<SessionInfo> readSessionFile(const std::filesystem::path& path);

/// A random 128-bit token as hex, from std::random_device.
[[nodiscard]] std::string makeSessionToken();

/// Line-based client for the app's local socket.
class LocalClient {
public:
    ~LocalClient();
    LocalClient(const LocalClient&) = delete;
    LocalClient& operator=(const LocalClient&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<LocalClient>> connect(const std::string& endpoint);

    [[nodiscard]] Result<void> writeLine(const std::string& line) const;
    /// Next line from the server, without the newline.
    [[nodiscard]] Result<std::string> readLine();

private:
    LocalClient() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string pending_;
};

} // namespace m2c

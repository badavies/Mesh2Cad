// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/rpc/Session.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstring>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

namespace m2c {

std::filesystem::path defaultSessionFile() {
    std::filesystem::path base;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
#else
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        base = runtime;
    } else if (const char* home = std::getenv("HOME")) {
        base = std::filesystem::path(home) / ".cache";
    }
#endif
    if (base.empty()) base = std::filesystem::temp_directory_path();
    return base / "mesh2cad" / "session.json";
}

Result<void> writeSessionFile(const SessionInfo& info, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file) return fail(ErrorCode::Io, "cannot write session file '" + path.string() + "'");
        file << nlohmann::json{{"endpoint", info.endpoint},
                               {"token", info.token},
                               {"pid", info.processId}}
                    .dump(2);
    }
#ifndef _WIN32
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR); // per-user locations on Windows are already private
#endif
    return {};
}

Result<SessionInfo> readSessionFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return fail(ErrorCode::NotFound,
                    "no running Mesh2Cad app found (no session file at '" + path.string() + "')");
    }
    const auto j = nlohmann::json::parse(file, nullptr, false);
    if (!j.is_object() || !j.contains("endpoint") || !j["endpoint"].is_string() ||
        !j.contains("token") || !j["token"].is_string() ||
        (j.contains("pid") && !j["pid"].is_number_integer())) {
        return fail(ErrorCode::Parse, "session file '" + path.string() + "' is malformed");
    }
    return SessionInfo{.endpoint = j["endpoint"].get<std::string>(),
                       .token = j["token"].get<std::string>(),
                       .processId = j.value("pid", 0LL)};
}

std::string makeSessionToken() {
    std::random_device rd;
    std::string hex;
    for (int i = 0; i < 4; ++i) {
        constexpr std::array digits{
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        const unsigned v = rd();
        for (unsigned k = 0; k < 8; ++k) hex.push_back(digits[(v >> (4U * k)) & 0xFU]);
    }
    return hex;
}

// ---- LocalClient ----------------------------------------------------------------------------

#ifdef _WIN32
struct LocalClient::Impl {
    HANDLE pipe{INVALID_HANDLE_VALUE};
    ~Impl() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }
};

Result<std::unique_ptr<LocalClient>> LocalClient::connect(const std::string& endpoint) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, endpoint.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, endpoint.c_str(), -1, wide.data(), n);
    const HANDLE pipe = CreateFileW(
        wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::Io, "cannot connect to the Mesh2Cad app at '" + endpoint + "'");
    }
    std::unique_ptr<LocalClient> client(new LocalClient());
    client->impl_ = std::make_unique<Impl>();
    client->impl_->pipe = pipe;
    return client;
}

Result<void> LocalClient::writeLine(const std::string& line) const {
    const std::string data = line + "\n";
    DWORD written = 0;
    if (!WriteFile(impl_->pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
        written != data.size()) {
        return fail(ErrorCode::Io, "lost the connection to the Mesh2Cad app");
    }
    return {};
}

Result<std::string> LocalClient::readLine() {
    while (true) {
        if (const auto nl = pending_.find('\n'); nl != std::string::npos) {
            std::string line = pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            return line;
        }
        std::array<char, 65536> buffer{};
        DWORD got = 0;
        if (!ReadFile(
                impl_->pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) ||
            got == 0) {
            return fail(ErrorCode::Io, "lost the connection to the Mesh2Cad app");
        }
        pending_.append(buffer.data(), got);
    }
}
#else
struct LocalClient::Impl {
    int fd{-1};
    ~Impl() {
        if (fd >= 0) ::close(fd);
    }
};

Result<std::unique_ptr<LocalClient>> LocalClient::connect(const std::string& endpoint) {
    sockaddr_un address{};
    if (endpoint.size() >= sizeof(address.sun_path)) {
        return fail(ErrorCode::InvalidArgument, "socket path is too long: " + endpoint);
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fail(ErrorCode::Io, "cannot create a socket");
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return fail(ErrorCode::Io, "cannot connect to the Mesh2Cad app at '" + endpoint + "'");
    }
    std::unique_ptr<LocalClient> client(new LocalClient());
    client->impl_ = std::make_unique<Impl>();
    client->impl_->fd = fd;
    return client;
}

Result<void> LocalClient::writeLine(const std::string& line) const {
    const std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(impl_->fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return fail(ErrorCode::Io, "lost the connection to the Mesh2Cad app");
        sent += static_cast<std::size_t>(n);
    }
    return {};
}

Result<std::string> LocalClient::readLine() {
    while (true) {
        const auto nl = pending_.find('\n');
        if (nl != std::string::npos) {
            std::string line = pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            return line;
        }
        std::array<char, 65536> buffer{};
        const auto n = ::recv(impl_->fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) return fail(ErrorCode::Io, "lost the connection to the Mesh2Cad app");
        pending_.append(buffer.data(), static_cast<std::size_t>(n));
    }
}
#endif

LocalClient::~LocalClient() = default;

} // namespace m2c

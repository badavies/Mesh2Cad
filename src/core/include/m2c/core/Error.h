// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <expected>
#include <string>
#include <utility>

namespace m2c {

/// Categories of failure that cross module boundaries. Front ends map these to exit codes,
/// JSON-RPC error codes and MCP errors, so add a value only when callers need to tell it apart.
enum class ErrorCode {
    InvalidArgument, ///< A parameter is missing, has the wrong type, or is out of range.
    InvalidState,    ///< The request is valid but not possible now (for example nothing to undo).
    NotFound,        ///< A named command, object or file does not exist.
    Io,              ///< Reading or writing a file failed.
    Parse,           ///< The file content is malformed.
    Unsupported,     ///< Valid request for something not implemented.
    Internal,        ///< A bug: an exception or invariant violation we did not expect.
};

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] constexpr const char* toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::InvalidState:
        return "invalid_state";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::Io:
        return "io";
    case ErrorCode::Parse:
        return "parse";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Internal:
        return "internal";
    }
    return "internal";
}

/// Shorthand for `return std::unexpected(Error{code, message})`.
[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected<Error>(Error{code, std::move(message)});
}

} // namespace m2c

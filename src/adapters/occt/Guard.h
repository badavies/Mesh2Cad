// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private to the Open CASCADE adapter: running kernel code with its exceptions turned into errors.

#include <exception>
#include <mutex>
#include <string>

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>

#include "m2c/core/Error.h"

namespace m2c::occt {

inline const char* message(const Standard_Failure& e) {
#if OCC_VERSION_HEX >= 0x080000
    return e.what(); // Standard_Failure is a std::exception from OCCT 8
#else
    return e.GetMessageString();
#endif
}

/// Open CASCADE reports progress through its default messenger, which prints to standard output;
/// the CLI and the JSON channel use standard output for results, so it is silenced once.
inline void silenceKernel() {
    static std::once_flag once;
    std::call_once(once, [] { Message::DefaultMessenger()->ChangePrinters().Clear(); });
}

/// Runs f, turning kernel exceptions into errors.
template <class F>
auto guarded(const char* what, F&& f) -> decltype(f()) {
    silenceKernel();
    try {
        return f();
    } catch (const Standard_Failure& e) {
        return fail(ErrorCode::Internal,
                    std::string(what) + " failed in Open CASCADE: " + message(e));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string(what) + " failed: " + e.what());
    }
}

} // namespace m2c::occt

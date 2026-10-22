// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "m2c/core/Error.h"

namespace m2c {

/// Asks a running operation to stop. Copies share one flag, so a front end keeps a copy and
/// cancels from any thread while the operation checks it on its own. A default-constructed token
/// can never be cancelled.
class CancelToken {
public:
    CancelToken() = default;

    /// A token that cancel() can set.
    [[nodiscard]] static CancelToken make() {
        CancelToken token;
        token.flag_ = std::make_shared<std::atomic<bool>>(false);
        return token;
    }

    /// Asks the operation to stop; does nothing on a default-constructed token.
    void cancel() const noexcept {
        if (flag_) flag_->store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return flag_ && flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

/// What a long operation is told and asked while it runs: it reports its stage ("fitting the
/// cage") and the fraction of that stage done, from 0 to 1 or negative when not known, and it
/// stops at its next checkpoint once the token is cancelled. Reports are made on the thread doing
/// the work. Both parts may be empty.
class Progress {
public:
    using Report = std::function<void(std::string_view stage, double fraction)>;

    Progress() = default;
    Progress(Report report, CancelToken cancel)
        : report_(std::move(report)), cancel_(std::move(cancel)) {}
    /// Any callable taking (stage, fraction), with no token.
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, Progress> &&
                 std::is_invocable_v<F&, std::string_view, double>)
    Progress(F&& report) // implicit: a lambda is a Progress
        : report_(std::forward<F>(report)) {}

    /// Whether anyone listens to the reports.
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(report_); }
    void operator()(std::string_view stage, double fraction) const {
        if (report_) report_(stage, fraction);
    }
    [[nodiscard]] bool cancelled() const noexcept { return cancel_.cancelled(); }
    [[nodiscard]] const CancelToken& token() const noexcept { return cancel_; }

private:
    Report report_;
    CancelToken cancel_;
};

/// Reports the stage if anyone listens.
inline void report(const Progress& progress, std::string_view stage, double fraction = -1.0) {
    progress(stage, fraction);
}

/// Fails with Cancelled ("cancelled while <what>") once the operation was asked to stop. Long
/// loops call it every so often; a caller passes the failure on and changes nothing.
[[nodiscard]] inline Result<void> checkCancelled(const Progress& progress, std::string_view what) {
    if (!progress.cancelled()) return {};
    return fail(ErrorCode::Cancelled, "cancelled while " + std::string(what));
}

/// Reports the stage, then checkCancelled(): the start of a stage is where it can stop.
[[nodiscard]] inline Result<void>
checkpoint(const Progress& progress, std::string_view stage, double fraction = -1.0) {
    report(progress, stage, fraction);
    return checkCancelled(progress, stage);
}

} // namespace m2c

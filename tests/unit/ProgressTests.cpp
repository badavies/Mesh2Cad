// SPDX-License-Identifier: GPL-3.0-or-later
// Progress reports and cancel tokens (core/Progress.h).

#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "m2c/core/Error.h"
#include "m2c/core/Progress.h"

using namespace m2c;

TEST_CASE("A cancel token stops a long operation at its next checkpoint", "[progress]") {
    // An empty Progress reports nowhere and is never cancelled.
    const Progress none;
    CHECK_FALSE(none);
    CHECK_FALSE(none.cancelled());
    CHECK(checkpoint(none, "anything").has_value());

    // A lambda is a Progress with no token: it hears every stage and cannot be cancelled.
    std::vector<std::string> heard;
    const Progress listening = [&heard](std::string_view stage, double) {
        heard.emplace_back(stage);
    };
    CHECK(listening);
    CHECK(checkpoint(listening, "stage one", 0.5).has_value());
    CHECK(heard == std::vector<std::string>{"stage one"});
    CancelToken().cancel(); // a default token ignores cancel()
    CHECK_FALSE(CancelToken().cancelled());

    // Copies of a made token share its flag, across threads too.
    const CancelToken token = CancelToken::make();
    const Progress progress([&heard](std::string_view stage, double) { heard.emplace_back(stage); },
                            token);
    CHECK(checkCancelled(progress, "fitting").has_value());
    std::thread([copy = token] { copy.cancel(); }).join();
    CHECK(progress.cancelled());
    CHECK(progress.token().cancelled());
    const auto stopped = checkpoint(progress, "making the faces");
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().code == ErrorCode::Cancelled);
    CHECK(stopped.error().message == "cancelled while making the faces");
    CHECK(heard.back() == "making the faces"); // reported before it stopped
    CHECK(std::string(toString(ErrorCode::Cancelled)) == "cancelled");
}

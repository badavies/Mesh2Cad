// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic parallel loops (core/Parallel.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "m2c/core/Parallel.h"

using namespace m2c;

TEST_CASE("Parallel chunks are the same for any thread count", "[parallel]") {
    const auto chunksOf = [](std::size_t count, std::size_t chunk) {
        std::mutex mutex;
        std::vector<std::pair<std::size_t, std::size_t>> seen;
        std::vector<int> visits(count, 0);
        parallelChunks(count, chunk, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) ++visits[i]; // each index in one chunk
            const std::scoped_lock lock(mutex);
            seen.emplace_back(begin, end);
        });
        CHECK(std::ranges::all_of(visits, [](int v) { return v == 1; }));
        std::ranges::sort(seen);
        return seen;
    };
    using Chunks = std::vector<std::pair<std::size_t, std::size_t>>;
    CHECK(chunksOf(10, 4) == Chunks{{0, 4}, {4, 8}, {8, 10}});
    CHECK(chunksOf(8, 4) == Chunks{{0, 4}, {4, 8}});
    CHECK(chunksOf(3, 0) == Chunks{{0, 1}, {1, 2}, {2, 3}}); // a chunk of 0 counts as 1
    CHECK(chunksOf(0, 4).empty());
    const Chunks many = chunksOf(100000, 4096);
    {
        const ThreadLimit one(1);
        CHECK(chunksOf(100000, 4096) == many);
    }
    CHECK(many.size() == 25);
    CHECK(many.back() == std::pair<std::size_t, std::size_t>{98304, 100000});

    // An exception in a chunk reaches the caller.
    CHECK_THROWS_AS(parallelChunks(100,
                                   10,
                                   [](std::size_t begin, std::size_t) {
                                       if (begin == 50) throw std::runtime_error("chunk 5");
                                   }),
                    std::runtime_error);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/core/Parallel.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>

#include <tbb/global_control.h>
#include <tbb/parallel_for.h>

namespace m2c {

void parallelChunks(const std::size_t count,
                    const std::size_t chunk,
                    const std::function<void(std::size_t begin, std::size_t end)>& fn) {
    if (count == 0) return;
    const std::size_t size = std::max<std::size_t>(chunk, 1);
    const std::size_t chunks = (count + size - 1) / size;
    // One task per chunk index: the chunks do not depend on how TBB splits the index range.
    tbb::parallel_for(std::size_t{0}, chunks, [&](const std::size_t c) {
        fn(c * size, std::min(count, (c + 1) * size));
    });
}

struct ThreadLimit::Impl {
    explicit Impl(const std::size_t threads)
        : control(tbb::global_control::max_allowed_parallelism, threads) {}
    tbb::global_control control;
};

ThreadLimit::ThreadLimit(const std::size_t threads)
    : impl_(std::make_unique<Impl>(std::max<std::size_t>(threads, 1))) {}

ThreadLimit::~ThreadLimit() = default;

} // namespace m2c

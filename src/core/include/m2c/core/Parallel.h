// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Parallel loops with oneTBB (ADR-036), kept deterministic: work is split into fixed chunks, so
// it is the same however many threads run it.

#include <cstddef>
#include <functional>
#include <memory>

namespace m2c {

/// Calls fn(begin, end) for each chunk of [0, count): [0, chunk), [chunk, 2 chunk), ..., the last
/// one shorter. Chunks run in parallel, in no particular order, and are the same for every
/// thread count, so a chunk that keeps its own cache gives the same results every time. Each
/// result goes to its own slot (an index of an output vector); sums and other reductions over
/// the results are made afterwards, in order. Returns when every chunk has run. An exception
/// thrown by fn is rethrown here once the other chunks have stopped.
void parallelChunks(std::size_t count,
                    std::size_t chunk,
                    const std::function<void(std::size_t begin, std::size_t end)>& fn);

/// While it lives, parallelChunks() uses at most this many threads (at least 1): for a front end
/// asked to leave cores free, and for tests that compare one thread with many.
class ThreadLimit {
public:
    explicit ThreadLimit(std::size_t threads);
    ~ThreadLimit();
    ThreadLimit(const ThreadLimit&) = delete;
    ThreadLimit& operator=(const ThreadLimit&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace m2c

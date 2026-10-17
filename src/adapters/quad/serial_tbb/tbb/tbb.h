// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Serial stand-in for the parts of the TBB 4.4 API that Instant Meshes uses (ADR-009). Every
// parallel algorithm runs its body once over the whole range, in order, on the calling thread, so
// the remesher is deterministic and needs no TBB. Locks are no-ops: nothing here runs concurrently.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <new>
#include <queue>
#include <vector>

#if defined(_WIN32)
// The real TBB header brings in the Windows headers, which declare the Interlocked intrinsics
// Instant Meshes calls on Windows; MinGW needs the declaration.
#include <intrin.h>
#endif

namespace tbb {

template <class T>
class blocked_range {
public:
    using const_iterator = T;
    blocked_range(T begin, T end, std::size_t /*grainsize*/ = 1) : begin_(begin), end_(end) {}
    [[nodiscard]] T begin() const { return begin_; }
    [[nodiscard]] T end() const { return end_; }
    [[nodiscard]] std::size_t size() const { return static_cast<std::size_t>(end_ - begin_); }
    [[nodiscard]] bool empty() const { return !(begin_ < end_); }

private:
    T begin_;
    T end_;
};

template <class Range, class Body>
void parallel_for(const Range& range, const Body& body) {
    body(range);
}

template <class Index, class Function>
void parallel_for(Index first, Index last, const Function& f) {
    for (Index i = first; i < last; ++i) f(i);
}

template <class Range, class Value, class Map, class Reduce>
Value parallel_reduce(const Range& range, const Value& identity, const Map& map, const Reduce&) {
    return map(range, identity);
}

template <class Range, class Value, class Map, class Reduce>
Value parallel_deterministic_reduce(const Range& range,
                                    const Value& identity,
                                    const Map& map,
                                    const Reduce&) {
    return map(range, identity);
}

template <class Iterator, class Compare>
void parallel_sort(Iterator first, Iterator last, const Compare& compare) {
    std::stable_sort(first, last, compare);
}

class spin_mutex {
public:
    class scoped_lock {
    public:
        scoped_lock() = default;
        explicit scoped_lock(spin_mutex&) {}
        void acquire(spin_mutex&) {}
        void release() {}
    };
    void lock() {}
    void unlock() {}
};

class task_scheduler_init {
public:
    static constexpr int automatic = -1;
    explicit task_scheduler_init(int /*threads*/ = automatic) {}
};

template <class T>
class concurrent_vector : public std::vector<T> {};

template <class T, class Compare = std::less<T>>
class concurrent_priority_queue {
public:
    void push(const T& value) { queue_.push(value); }
    bool try_pop(T& value) {
        if (queue_.empty()) return false;
        value = queue_.top();
        queue_.pop();
        return true;
    }
    [[nodiscard]] bool empty() const { return queue_.empty(); }
    [[nodiscard]] std::size_t size() const { return queue_.size(); }

private:
    std::priority_queue<T, std::vector<T>, Compare> queue_;
};

/// Tasks run depth-first on the calling thread: spawn() runs the child at once, and a task that
/// returns itself from execute() (recycling) runs again. Every task and continuation allocated
/// under one root is freed when spawn_root_and_wait() returns.
class task {
public:
    struct allocation {
        std::vector<task*>* owned;
    };

    virtual ~task() = default;
    virtual task* execute() = 0;

    static allocation allocate_root() { return {&owned()}; }
    allocation allocate_child() { return {&owned()}; }
    allocation allocate_continuation() { return {&owned()}; }
    void set_ref_count(int) {}
    void recycle_as_child_of(task&) {}
    void spawn(task& child) { run(child); }

    static void spawn_root_and_wait(task& root) {
        run(root);
        for (task* t : owned()) delete t;
        owned().clear();
    }

private:
    static std::vector<task*>& owned() {
        static thread_local std::vector<task*> tasks;
        return tasks;
    }
    static void run(task& t) {
        // execute() returns the task to run next: itself when recycled, or none.
        for (task* current = &t; current;) current = current->execute();
    }
};

class empty_task : public task {
public:
    task* execute() override { return nullptr; }
};

} // namespace tbb

/// Placement form used as `new (parent.allocate_child()) Task(...)`; the task is recorded so the
/// root frees it.
inline void* operator new(std::size_t bytes, tbb::task::allocation where) {
    void* memory = ::operator new(bytes);
    where.owned->push_back(static_cast<tbb::task*>(memory));
    return memory;
}

inline void operator delete(void* memory, tbb::task::allocation) {
    ::operator delete(memory);
}

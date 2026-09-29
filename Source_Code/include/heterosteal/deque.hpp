// include/heterosteal/deque.hpp
#pragma once
#include <atomic>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstddef>
#include "task.hpp"

namespace hs {

// Chase-Lev dynamic circular work-stealing deque, specialized to
// TaskDescriptor*. Single-owner push/pop from the bottom; any thread may
// steal from the top. Reference implementation derived from Chase &
// Lev (SPAA 2005) and the memory-order-correct realization in Lê, Pop,
// Cohen & Zappa Nardelli (PPoPP 2013).
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(size_t initial_capacity_pow2 = 1024);
    ~WorkStealingDeque();

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
    WorkStealingDeque(WorkStealingDeque&&) = delete;
    WorkStealingDeque& operator=(WorkStealingDeque&&) = delete;

    // Owner-only. Not safe to call from more than one thread concurrently.
    void push_bottom(TaskDescriptor* task);
    std::optional<TaskDescriptor*> pop_bottom();   // owner only

    // Thief-safe: callable concurrently from any number of other threads,
    // concurrently with the owner's push/pop and with other thieves.
    std::optional<TaskDescriptor*> steal_top();

    // Owner-only, approximate (racy with concurrent steals; used only for
    // scheduling heuristics like "pick the most-loaded victim," never for
    // correctness).
    int64_t size_approx() const;

    // Current buffer capacity (primarily for unit/stress testing verification)
    size_t capacity() const;

private:
    struct Array {
        int64_t capacity;
        int64_t mask;
        std::atomic<TaskDescriptor*>* buffer;

        explicit Array(int64_t cap);
        ~Array();

        Array(const Array&) = delete;
        Array& operator=(const Array&) = delete;
        Array(Array&&) = delete;
        Array& operator=(Array&&) = delete;
    };

    std::atomic<int64_t> top_{0};
    std::atomic<int64_t> bottom_{0};
    std::atomic<Array*> array_{nullptr};

    // Old arrays retired on resize must not be freed while a concurrent
    // steal_top might still be reading from them (§8.2 — reclamation
    // strategy: hazard-pointer-free simplification via a retire list drained
    // only at scheduler shutdown quiescence).
    std::vector<Array*> retired_arrays_;

    Array* resize(Array* old_array, int64_t b, int64_t t);
};

} // namespace hs

// include/heterosteal/object_pool.hpp
#pragma once
#include "task.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <cassert>

namespace hs {

class TaskPool;

// RAII pin on a live TaskDescriptor slot.
//
// Synchronization invariants (completion lifetime protocol):
// 1. A slot may be accessed by a completion (poller or drain path) only while
//    a CompletionGuard for that (slot, generation) is held. resolve_guarded()
//    installs the pin with a single CAS on the slot control word, so the pin
//    cannot attach to a recycled generation.
// 2. release() never waits for pins to drop. If the poller thread calls
//    release() from on_completion_ while it still holds a guard, recycle is
//    deferred (retire bit) until the last unpin. This avoids self-deadlock.
// 3. acquire() pops only from the free list. A slot is pushed there only when
//    the control word CAS observes pin==0 and the slot is no longer live.
//    Therefore acquire() cannot reset/reuse a descriptor a completion is
//    still writing (e.g. more_completions_pending).
// 4. Generation is bumped at release() before (or atomically with) retire.
//    Stale CQEs fail resolve_guarded() even if the physical slot is not yet
//    on the free list.
// 5. Duplicate release() is ignored: only one thread wins the state CAS from
//    a terminal state to Created.
class CompletionGuard {
public:
    CompletionGuard() noexcept = default;
    ~CompletionGuard();

    CompletionGuard(CompletionGuard&& other) noexcept;
    CompletionGuard& operator=(CompletionGuard&& other) noexcept;
    CompletionGuard(const CompletionGuard&) = delete;
    CompletionGuard& operator=(const CompletionGuard&) = delete;

    explicit operator bool() const noexcept { return task_ != nullptr; }
    TaskDescriptor* get() const noexcept { return task_; }
    TaskDescriptor* operator->() const noexcept { return task_; }
    TaskDescriptor& operator*() const noexcept { return *task_; }

private:
    friend class TaskPool;
    CompletionGuard(TaskPool* pool, uint32_t slot, TaskDescriptor* task) noexcept
        : pool_(pool), slot_(slot), task_(task) {}

    void reset() noexcept;

    TaskPool* pool_ = nullptr;
    uint32_t slot_ = 0;
    TaskDescriptor* task_ = nullptr;
};

// Fixed-capacity slab doubling as the correlation table (spec §3.4).
// Not resizable at runtime (resizing an in-use slab is a correctness hazard
// for in-flight user_data handles).
class TaskPool {
public:
    explicit TaskPool(size_t capacity);
    ~TaskPool() = default;

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    // Returns nullptr if the pool is exhausted (caller must apply
    // backpressure per §4.7 — never grows the slab under contention).
    TaskDescriptor* acquire();

    // Only legal when: state is terminal AND more_completions_pending == false.
    // Bumps generation; returns the slot to the free list immediately if
    // unpinned, otherwise defers until the last CompletionGuard drops
    // (Invariant I-1). Never blocks on pins (see CompletionGuard comments).
    void release(TaskDescriptor* task);

    // Packs (slot_index, generation) into the 64-bit io_uring user_data field.
    static constexpr uint64_t encode_handle(uint32_t slot_index, uint32_t generation) noexcept {
        return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(slot_index);
    }

    // Unpacks a 64-bit io_uring user_data field into (slot_index, generation).
    static constexpr void decode_handle(uint64_t handle, uint32_t& slot_index, uint32_t& generation) noexcept {
        slot_index = static_cast<uint32_t>(handle & 0xFFFF'FFFFu);
        generation = static_cast<uint32_t>(handle >> 32);
    }

    // Unpinned generation check. Safe only when the caller already owns the
    // slot or no concurrent recycle can occur (unit tests). Completions must
    // use resolve_guarded().
    TaskDescriptor* resolve(uint64_t handle);

    // Pins the slot if the handle's generation still matches a live slot.
    // Returns an empty guard and increments stale_completions on mismatch,
    // out-of-range, retiring, or free slots.
    CompletionGuard resolve_guarded(uint64_t handle);

    // Capacity and diagnostics
    size_t capacity() const noexcept { return capacity_; }
    size_t available() const;
    uint64_t stale_completions() const noexcept {
        return stale_completions_.load(std::memory_order_relaxed);
    }
    uint64_t exhausted_count() const noexcept {
        return exhausted_count_.load(std::memory_order_relaxed);
    }

    // Test/diagnostics: current pin count for a slot (0 if out of range).
    uint32_t pin_count(uint32_t slot_index) const noexcept;

private:
    friend class CompletionGuard;

    // Control word: [63:32 generation][17 retire][16 live][15:0 pin]
    static constexpr uint64_t kPinMask   = 0xFFFFu;
    static constexpr uint64_t kLiveBit   = 1ull << 16;
    static constexpr uint64_t kRetireBit = 1ull << 17;

    static constexpr uint32_t word_pin(uint64_t w) noexcept {
        return static_cast<uint32_t>(w & kPinMask);
    }
    static constexpr bool word_live(uint64_t w) noexcept {
        return (w & kLiveBit) != 0;
    }
    static constexpr bool word_retire(uint64_t w) noexcept {
        return (w & kRetireBit) != 0;
    }
    static constexpr uint32_t word_gen(uint64_t w) noexcept {
        return static_cast<uint32_t>(w >> 32);
    }
    static constexpr uint64_t make_word(uint32_t gen, uint32_t pin, bool live, bool retire) noexcept {
        uint64_t w = (static_cast<uint64_t>(gen) << 32) | (static_cast<uint64_t>(pin) & kPinMask);
        if (live) w |= kLiveBit;
        if (retire) w |= kRetireBit;
        return w;
    }
    static constexpr uint32_t next_generation(uint32_t gen) noexcept {
        uint32_t n = gen + 1u;
        return n == 0u ? 1u : n; // generation 0 is reserved (cancel sentinel user_data)
    }

    void unpin(uint32_t slot) noexcept;
    void push_free(uint32_t slot);
    void note_stale() noexcept {
        stale_completions_.fetch_add(1, std::memory_order_relaxed);
    }

    const size_t capacity_;
    std::unique_ptr<TaskDescriptor[]> slots_;
    std::unique_ptr<std::atomic<uint64_t>[]> ctrl_;

    mutable std::mutex free_mutex_;
    std::vector<uint32_t> free_stack_;

    std::atomic<uint64_t> stale_completions_{0};
    std::atomic<uint64_t> exhausted_count_{0};
};

} // namespace hs

// include/heterosteal/task.hpp
#pragma once
#include <cstdint>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>
#include <new>

namespace hs {

using Clock = std::chrono::steady_clock; // CLOCK_MONOTONIC-backed; never use system_clock for intervals

enum class TaskClass : uint8_t { Compute = 0, IoCallback = 1 };
enum class TaskState : uint8_t { Created, Enqueued, Running, Completed, Failed, Cancelled };

// Fixed-capacity type-erased callable: avoids std::function's heap allocation
// on the hot path. 48 bytes of inline storage covers a lambda capturing a
// handful of pointers/ints; anything bigger is a design smell for a task body.
class InlineTask {
public:
    static constexpr size_t kInlineCapacity = 48;

    template <class F>
    explicit InlineTask(F&& f) {
        using DecayedF = std::decay_t<F>;
        static_assert(sizeof(DecayedF) <= kInlineCapacity,
                      "callable too large for InlineTask; reduce captures");
        new (storage_) DecayedF(std::forward<F>(f));
        invoke_ = [](void* p) { (*reinterpret_cast<DecayedF*>(p))(); };
        destroy_ = [](void* p) { reinterpret_cast<DecayedF*>(p)->~DecayedF(); };
    }

    InlineTask() = default;

    ~InlineTask() {
        if (destroy_) {
            destroy_(storage_);
        }
    }

    InlineTask(InlineTask&&) = delete;            // slots are pool-owned; no move semantics needed
    InlineTask(const InlineTask&) = delete;
    InlineTask& operator=(InlineTask&&) = delete;
    InlineTask& operator=(const InlineTask&) = delete;

    void operator()() {
        if (invoke_) {
            invoke_(storage_);
        }
    }

    explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

private:
    alignas(std::max_align_t) unsigned char storage_[kInlineCapacity];
    void (*invoke_)(void*)  = nullptr;
    void (*destroy_)(void*) = nullptr;
};

struct TaskDescriptor {
    // --- Identity / correlation ---
    uint32_t slot_index = 0;  // index into the object pool's slab (stable for the slot's lifetime)
    uint32_t generation = 0;  // bumped every time the slot is recycled; detects stale user_data
    TaskClass task_class = TaskClass::Compute;
    std::atomic<TaskState> state{TaskState::Created};

    // --- Correlation to the originating I/O (IoCallback only; ignored for Compute) ---
    int io_fd = -1;
    uint64_t io_op_seq = 0;   // monotonically increasing per-fd sequence, for ordering diagnostics
    bool multishot = false;
    // Written by the completion poller only while a CompletionGuard pin is held;
    // acquire() resets it after the slot is exclusively popped from the free list.
    std::atomic<bool> more_completions_pending{false};

    // --- Locality bookkeeping (see §9.3, the locality proxy) ---
    int origin_worker_id = -1;      // worker that submitted the SQE (IoCallback) or created the task (Compute)
    int last_run_worker_id = -1;    // updated after each execution; -1 if never run
    Clock::time_point last_run_at{};

    // --- Scheduling bookkeeping ---
    std::atomic<uint32_t> steal_count{0};
    Clock::time_point enqueued_at{};
    Clock::time_point started_at{};
    Clock::time_point completed_at{};

    // --- Synthetic-workload knobs (ignored on real workloads) ---
    uint32_t declared_footprint_bytes = 0; // only meaningful for synthetic Compute tasks, §9.4

    // --- Retry/partial-completion state (IoCallback only) ---
    struct RetryState {
        size_t bytes_done = 0;
        size_t bytes_total = 0;
        int retries_left = 4;
    } retry;

    // --- The actual work ---
    InlineTask body;

    TaskDescriptor() = default;
    ~TaskDescriptor() = default;
    TaskDescriptor(const TaskDescriptor&) = delete;
    TaskDescriptor& operator=(const TaskDescriptor&) = delete;
    TaskDescriptor(TaskDescriptor&&) = delete;
    TaskDescriptor& operator=(TaskDescriptor&&) = delete;
};

} // namespace hs

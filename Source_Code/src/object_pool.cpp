// src/object_pool.cpp
#include "heterosteal/object_pool.hpp"

namespace hs {

CompletionGuard::~CompletionGuard() {
    reset();
}

CompletionGuard::CompletionGuard(CompletionGuard&& other) noexcept
    : pool_(other.pool_), slot_(other.slot_), task_(other.task_) {
    other.pool_ = nullptr;
    other.slot_ = 0;
    other.task_ = nullptr;
}

CompletionGuard& CompletionGuard::operator=(CompletionGuard&& other) noexcept {
    if (this != &other) {
        reset();
        pool_ = other.pool_;
        slot_ = other.slot_;
        task_ = other.task_;
        other.pool_ = nullptr;
        other.slot_ = 0;
        other.task_ = nullptr;
    }
    return *this;
}

void CompletionGuard::reset() noexcept {
    if (pool_ != nullptr && task_ != nullptr) {
        pool_->unpin(slot_);
    }
    pool_ = nullptr;
    slot_ = 0;
    task_ = nullptr;
}

TaskPool::TaskPool(size_t capacity)
    : capacity_(capacity),
      slots_(std::make_unique<TaskDescriptor[]>(capacity)),
      ctrl_(std::make_unique<std::atomic<uint64_t>[]>(capacity)) {
    free_stack_.reserve(capacity_);
    for (size_t i = 0; i < capacity_; ++i) {
        // Reverse order so slot 0 is popped first
        uint32_t idx = static_cast<uint32_t>(capacity_ - 1 - i);
        free_stack_.push_back(idx);

        // Free, unpinned, generation 1 (matches TaskDescriptor default snapshot).
        ctrl_[i].store(make_word(1, 0, false, false), std::memory_order_relaxed);
        slots_[i].slot_index = static_cast<uint32_t>(i);
        slots_[i].generation = 1;
    }
}

TaskDescriptor* TaskPool::acquire() {
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(free_mutex_);
        if (free_stack_.empty()) {
            exhausted_count_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        slot = free_stack_.back();
        free_stack_.pop_back();
    }

    uint64_t w = ctrl_[slot].load(std::memory_order_acquire);
    assert(word_pin(w) == 0 && "acquire() observed a pinned slot on the free list");
    assert(!word_live(w) && "acquire() observed a live slot on the free list");
    assert(!word_retire(w) && "acquire() observed a retiring slot on the free list");

    uint32_t gen = word_gen(w);
    uint64_t live_word = make_word(gen, 0, true, false);
    uint64_t expected = w;
    if (!ctrl_[slot].compare_exchange_strong(expected, live_word,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        // Free-list slots are exclusively owned by acquire(); a failed CAS is a bug.
        assert(false && "TaskPool::acquire control-word CAS failed");
        ctrl_[slot].store(live_word, std::memory_order_release);
    }

    TaskDescriptor* task = &slots_[slot];
    task->slot_index = slot;
    task->generation = gen;
    task->task_class = TaskClass::Compute;
    task->state.store(TaskState::Created, std::memory_order_release);

    task->io_fd = -1;
    task->io_op_seq = 0;
    task->multishot = false;
    task->more_completions_pending.store(false, std::memory_order_release);

    task->origin_worker_id = -1;
    task->last_run_worker_id = -1;
    task->last_run_at = Clock::time_point{};

    task->steal_count.store(0, std::memory_order_relaxed);
    task->enqueued_at = Clock::time_point{};
    task->started_at = Clock::time_point{};
    task->completed_at = Clock::time_point{};

    task->declared_footprint_bytes = 0;
    task->retry = TaskDescriptor::RetryState{};

    return task;
}

void TaskPool::release(TaskDescriptor* task) {
    if (!task) return;

    TaskState expected = task->state.load(std::memory_order_acquire);
    if (expected == TaskState::Created) {
        // Already released / recycled; duplicate release safely ignored
        return;
    }

    bool is_terminal = (expected == TaskState::Completed ||
                        expected == TaskState::Failed ||
                        expected == TaskState::Cancelled);
    assert(is_terminal && "Invariant I-1 violated: releasing non-terminal task");
    if (!is_terminal) {
        return;
    }

    if (task->more_completions_pending.load(std::memory_order_acquire)) {
        assert(false && "Invariant I-1 violated: releasing task with pending completions");
        return;
    }

    // Atomically transition from terminal state to Created to prevent concurrent/duplicate releases
    bool transitioned = false;
    while (expected == TaskState::Completed ||
           expected == TaskState::Failed ||
           expected == TaskState::Cancelled) {
        if (task->state.compare_exchange_weak(expected, TaskState::Created,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            transitioned = true;
            break;
        }
        if (expected == TaskState::Created) {
            return;
        }
    }

    if (!transitioned) {
        return;
    }

    uint32_t slot = task->slot_index;
    assert(slot < capacity_ && "Invalid slot_index in TaskPool::release");

    // Bump generation so in-flight handles of this occupancy become stale.
    // If pin==0, clear live and push the free list. If pin>0, set retire and
    // let unpin() finish reclamation — do not wait (poller may hold the pin).
    for (;;) {
        uint64_t w = ctrl_[slot].load(std::memory_order_acquire);
        uint32_t pin = word_pin(w);
        uint32_t ng = next_generation(word_gen(w));
        uint64_t nw = (pin == 0)
            ? make_word(ng, 0, false, false)
            : make_word(ng, pin, true, true);
        if (ctrl_[slot].compare_exchange_weak(w, nw,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            if (pin == 0) {
                push_free(slot);
            }
            break;
        }
    }
}

TaskDescriptor* TaskPool::resolve(uint64_t handle) {
    uint32_t slot_index = 0;
    uint32_t generation = 0;
    decode_handle(handle, slot_index, generation);

    if (slot_index >= capacity_) {
        note_stale();
        return nullptr;
    }

    uint64_t w = ctrl_[slot_index].load(std::memory_order_acquire);
    if (!word_live(w) || word_retire(w) || word_gen(w) != generation) {
        note_stale();
        return nullptr;
    }

    return &slots_[slot_index];
}

CompletionGuard TaskPool::resolve_guarded(uint64_t handle) {
    uint32_t slot_index = 0;
    uint32_t generation = 0;
    decode_handle(handle, slot_index, generation);

    if (slot_index >= capacity_) {
        note_stale();
        return {};
    }

    for (;;) {
        uint64_t w = ctrl_[slot_index].load(std::memory_order_acquire);
        if (!word_live(w) || word_retire(w) || word_gen(w) != generation) {
            note_stale();
            return {};
        }
        uint32_t pin = word_pin(w);
        if (pin == static_cast<uint32_t>(kPinMask)) {
            continue; // extremely unlikely; retry
        }
        uint64_t nw = make_word(word_gen(w), pin + 1u, true, false);
        if (ctrl_[slot_index].compare_exchange_weak(w, nw,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            // Re-validate is implied by the CAS: live, !retire, generation, and
            // pin were taken from `w`. A concurrent release would have changed
            // generation and/or retire, causing CAS failure.
            return CompletionGuard(this, slot_index, &slots_[slot_index]);
        }
    }
}

void TaskPool::unpin(uint32_t slot) noexcept {
    assert(slot < capacity_);
    for (;;) {
        uint64_t w = ctrl_[slot].load(std::memory_order_acquire);
        uint32_t pin = word_pin(w);
        assert(pin > 0 && "unpin() with zero pin count");
        if (pin == 0) {
            return;
        }
        uint32_t new_pin = pin - 1u;
        bool retire = word_retire(w);
        uint32_t gen = word_gen(w);
        uint64_t nw;
        bool do_free = false;
        if (new_pin == 0 && retire) {
            nw = make_word(gen, 0, false, false);
            do_free = true;
        } else {
            nw = make_word(gen, new_pin, word_live(w), retire);
        }
        if (ctrl_[slot].compare_exchange_weak(w, nw,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            if (do_free) {
                push_free(slot);
            }
            return;
        }
    }
}

void TaskPool::push_free(uint32_t slot) {
    std::lock_guard<std::mutex> lock(free_mutex_);
    free_stack_.push_back(slot);
}

size_t TaskPool::available() const {
    std::lock_guard<std::mutex> lock(free_mutex_);
    return free_stack_.size();
}

uint32_t TaskPool::pin_count(uint32_t slot_index) const noexcept {
    if (slot_index >= capacity_) {
        return 0;
    }
    return word_pin(ctrl_[slot_index].load(std::memory_order_acquire));
}

} // namespace hs

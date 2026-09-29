// src/iouring_engine.cpp
#include "heterosteal/iouring_engine.hpp"
#include <stdexcept>
#include <string>
#include <cerrno>

namespace hs {

IoUringEngine::IoUringEngine(IoUringConfig cfg, TaskPool& pool, CompletionSink on_completion)
    : cfg_(cfg), pool_(pool), on_completion_(std::move(on_completion)) {
    struct io_uring_params params{};
    if (cfg_.cq_entries > cfg_.sq_entries) {
        params.flags |= IORING_SETUP_CQSIZE;
        params.cq_entries = cfg_.cq_entries;
    }
    if (cfg_.use_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
    }

    int ret = io_uring_queue_init_params(cfg_.sq_entries, &ring_, &params);
    if (ret < 0) {
        // Fallback to basic init if custom CQSIZE flags fail
        ret = io_uring_queue_init(cfg_.sq_entries, &ring_, 0);
        if (ret < 0) {
            throw std::runtime_error("Failed to initialize io_uring queue: errno " + std::to_string(-ret));
        }
    }
}

IoUringEngine::~IoUringEngine() {
    request_stop();
    join();
    io_uring_queue_exit(&ring_);
}

void IoUringEngine::start() {
    if (poller_running_.exchange(true, std::memory_order_acq_rel)) {
        return; // Already running
    }
    stop_requested_.store(false, std::memory_order_release);
    poller_thread_ = std::thread(&IoUringEngine::poller_loop, this);
}

void IoUringEngine::request_stop() {
    stop_requested_.store(true, std::memory_order_release);
}

void IoUringEngine::join() {
    if (poller_thread_.joinable()) {
        poller_thread_.join();
    }
    poller_running_.store(false, std::memory_order_release);
}

bool IoUringEngine::submit_read(int fd, void* buf, size_t len, off_t offset, TaskDescriptor* task) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        backpressure_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);
    io_uring_prep_read(sqe, fd, buf, static_cast<unsigned>(len), offset);
    io_uring_sqe_set_data64(sqe, handle);

    task->state.store(TaskState::Enqueued, std::memory_order_release);

    int ret = io_uring_submit(&ring_);
    if (ret >= 0) {
        submitted_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    task->state.store(TaskState::Created, std::memory_order_relaxed);
    return false;
}

bool IoUringEngine::submit_write(int fd, const void* buf, size_t len, off_t offset, TaskDescriptor* task) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        backpressure_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);
    io_uring_prep_write(sqe, fd, buf, static_cast<unsigned>(len), offset);
    io_uring_sqe_set_data64(sqe, handle);

    task->state.store(TaskState::Enqueued, std::memory_order_release);

    int ret = io_uring_submit(&ring_);
    if (ret >= 0) {
        submitted_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    task->state.store(TaskState::Created, std::memory_order_relaxed);
    return false;
}

bool IoUringEngine::submit_nop(TaskDescriptor* task) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        backpressure_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, handle);

    task->state.store(TaskState::Enqueued, std::memory_order_release);

    int ret = io_uring_submit(&ring_);
    if (ret >= 0) {
        submitted_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    task->state.store(TaskState::Created, std::memory_order_relaxed);
    return false;
}

void IoUringEngine::handle_cqe(struct io_uring_cqe* c) {
    uint64_t udata = io_uring_cqe_get_data64(c);
    // Cancel SQEs use sentinel user_data = 0 and do not own a slot (§4.1).
    if (udata == 0) {
        return;
    }

    // Pin for the entire CQE handling window, including on_completion_.
    // release() from the callback cannot recycle the slot until this guard
    // is destroyed, so acquire() cannot reset the descriptor concurrently.
    CompletionGuard guard = pool_.resolve_guarded(udata);
    if (!guard) {
        return;
    }

    guard->more_completions_pending.store((c->flags & IORING_CQE_F_MORE) != 0,
                                          std::memory_order_release);
    completed_count_.fetch_add(1, std::memory_order_relaxed);
    on_completion_(guard.get(), c->res, c->flags);
}

bool IoUringEngine::try_cancel(TaskDescriptor* task) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return false;
    }

    uint64_t target_handle = TaskPool::encode_handle(task->slot_index, task->generation);
    io_uring_prep_cancel64(sqe, target_handle, 0);
    // Sentinel user_data = 0 for the cancel operation itself (§4.1, §4.3)
    io_uring_sqe_set_data64(sqe, 0);

    int ret = io_uring_submit(&ring_);
    return (ret >= 0);
}

void IoUringEngine::poller_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        struct io_uring_cqe* cqe = nullptr;
        struct __kernel_timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 50'000'000; // 50ms wake for stop-checking (§4.3)

        int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (ret == -ETIME) {
            continue;
        }
        if (ret < 0) {
            continue;
        }

        // Batch-drain all available completions (§4.3)
        unsigned head = 0;
        int count = 0;
        struct io_uring_cqe* c = nullptr;

        io_uring_for_each_cqe(&ring_, head, c) {
            handle_cqe(c);
            ++count;
        }

        if (count > 0) {
            io_uring_cq_advance(&ring_, count);
        }
    }

    // Drain mode on stop (§4.3, §8.3): bounded timeout drain
    constexpr int kMaxDrainAttempts = 20;
    for (int attempt = 0; attempt < kMaxDrainAttempts; ++attempt) {
        unsigned head = 0;
        int count = 0;
        struct io_uring_cqe* c = nullptr;

        io_uring_for_each_cqe(&ring_, head, c) {
            handle_cqe(c);
            ++count;
        }

        if (count > 0) {
            io_uring_cq_advance(&ring_, count);
        } else {
            if (completed_count_.load(std::memory_order_relaxed) >= submitted_count_.load(std::memory_order_relaxed)) {
                break;
            }
            struct io_uring_cqe* cqe = nullptr;
            struct __kernel_timespec ts{};
            ts.tv_sec = 0;
            ts.tv_nsec = 10'000'000; // 10ms
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (ret < 0) {
                break;
            }
        }
    }
}

} // namespace hs

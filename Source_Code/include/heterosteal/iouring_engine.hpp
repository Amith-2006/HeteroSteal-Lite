// include/heterosteal/iouring_engine.hpp
#pragma once
#include <liburing.h>
#include "task.hpp"
#include "object_pool.hpp"
#include <thread>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace hs {

struct IoUringConfig {
    unsigned sq_entries = 1024;      // must be a power of two per liburing requirements
    unsigned cq_entries = 4096;      // CQ sized larger than SQ to reduce overflow risk under bursty completion
    bool use_sqpoll = false;         // MVP default false
};

// One instance per experimental run. Exactly one Completion Poller thread is
// spawned per instance — this class embodies the "exactly one poller" rule
// from §2.1; there is no constructor parameter to spawn more than one.
class IoUringEngine {
public:
    using CompletionSink = std::function<void(TaskDescriptor* task, int32_t res, uint32_t cqe_flags)>;

    IoUringEngine(IoUringConfig cfg, TaskPool& pool, CompletionSink on_completion);
    ~IoUringEngine();

    IoUringEngine(const IoUringEngine&) = delete;
    IoUringEngine& operator=(const IoUringEngine&) = delete;
    IoUringEngine(IoUringEngine&&) = delete;
    IoUringEngine& operator=(IoUringEngine&&) = delete;

    // Called by workload-generator threads (NOT worker threads — see §2.2).
    // Returns false (no SQE obtained) if the SQ is transiently full; caller
    // must apply its own backpressure (§4.7), this call never blocks.
    bool submit_read(int fd, void* buf, size_t len, off_t offset, TaskDescriptor* task);
    bool submit_write(int fd, const void* buf, size_t len, off_t offset, TaskDescriptor* task);
    bool submit_nop(TaskDescriptor* task); // for synthetic latency-only microbenchmarks

    // Best-effort cancellation via IORING_OP_ASYNC_CANCEL. Completion still
    // arrives asynchronously (with res == -ECANCELED on success) — cancel
    // does not synchronously guarantee non-execution. Callers must not
    // release the TaskDescriptor until that completion (or the original
    // op's completion) actually arrives (§8.4).
    bool try_cancel(TaskDescriptor* task);

    void start();          // spawns the Completion Poller thread
    void request_stop();   // signals the poller to drain and exit (§8.3)
    void join();

    // Diagnostics and telemetry counters
    uint64_t submitted_count() const noexcept { return submitted_count_.load(std::memory_order_relaxed); }
    uint64_t completed_count() const noexcept { return completed_count_.load(std::memory_order_relaxed); }
    uint64_t backpressure_count() const noexcept { return backpressure_count_.load(std::memory_order_relaxed); }
    bool is_running() const noexcept { return poller_running_.load(std::memory_order_acquire); }

private:
    void poller_loop();    // the Completion Poller thread body, §4.3
    void handle_cqe(io_uring_cqe* c); // pin, apply CQE, sink, unpin

    io_uring ring_{};
    IoUringConfig cfg_;
    TaskPool& pool_;
    CompletionSink on_completion_;

    std::thread poller_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> poller_running_{false};

    mutable std::mutex submit_mutex_; // Thread-safe submissions per §8.5
    std::atomic<uint64_t> submitted_count_{0};
    std::atomic<uint64_t> completed_count_{0};
    std::atomic<uint64_t> backpressure_count_{0};
};

} // namespace hs

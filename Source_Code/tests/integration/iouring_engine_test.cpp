// tests/integration/iouring_engine_test.cpp
#include <gtest/gtest.h>
#include "heterosteal/object_pool.hpp"
#include "heterosteal/iouring_engine.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>
#include <iostream>

using namespace hs;

class IoUringEngineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool_ = std::make_unique<TaskPool>(1024);
    }

    std::unique_ptr<TaskPool> pool_;
};

// Test 1: Single NOP round-trip
TEST_F(IoUringEngineIntegrationTest, SingleNopRoundTrip) {
    std::atomic<bool> completed{false};
    int32_t completion_res = -1;

    IoUringConfig cfg{.sq_entries = 64, .cq_entries = 128};
    IoUringEngine engine(cfg, *pool_, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        completion_res = res;
        task->state.store(TaskState::Completed, std::memory_order_release);
        pool_->release(task);
        completed.store(true, std::memory_order_release);
    });

    engine.start();

    TaskDescriptor* task = pool_->acquire();
    ASSERT_NE(task, nullptr);

    bool ok = engine.submit_nop(task);
    ASSERT_TRUE(ok);

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (!completed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            FAIL() << "Timed out waiting for NOP completion";
        }
        std::this_thread::yield();
    }

    EXPECT_EQ(completion_res, 0);
    EXPECT_EQ(pool_->stale_completions(), 0u);

    engine.request_stop();
    engine.join();
}

// Test 2: Real pipe read and write round-trip
TEST_F(IoUringEngineIntegrationTest, PipeReadWriteRoundTrip) {
    int pipefds[2];
    ASSERT_EQ(pipe(pipefds), 0);

    // Write data to pipe
    const char msg[] = "HeteroSteal io_uring pipe test payload";
    ssize_t bytes_written = write(pipefds[1], msg, sizeof(msg));
    ASSERT_EQ(bytes_written, static_cast<ssize_t>(sizeof(msg)));

    std::atomic<bool> read_completed{false};
    char read_buf[128]{};
    int32_t read_res = 0;

    IoUringConfig cfg{.sq_entries = 64, .cq_entries = 128};
    IoUringEngine engine(cfg, *pool_, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        read_res = res;
        task->state.store(TaskState::Completed, std::memory_order_release);
        pool_->release(task);
        read_completed.store(true, std::memory_order_release);
    });

    engine.start();

    TaskDescriptor* read_task = pool_->acquire();
    ASSERT_NE(read_task, nullptr);

    bool ok = engine.submit_read(pipefds[0], read_buf, sizeof(read_buf), 0, read_task);
    ASSERT_TRUE(ok);

    auto start = std::chrono::steady_clock::now();
    while (!read_completed.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            FAIL() << "Timed out waiting for pipe read completion";
        }
        std::this_thread::yield();
    }

    EXPECT_EQ(read_res, static_cast<int32_t>(sizeof(msg)));
    EXPECT_STREQ(read_buf, msg);

    close(pipefds[0]);
    close(pipefds[1]);

    engine.request_stop();
    engine.join();
}

// Test 3: Short-read retry classification logic (spec §4.4)
TEST_F(IoUringEngineIntegrationTest, ShortReadRetryLogic) {
    int pipefds[2];
    ASSERT_EQ(pipe(pipefds), 0);

    const size_t total_payload = 16;
    char write_buf[total_payload];
    std::memset(write_buf, 'A', total_payload);

    // Write partial data first (4 bytes)
    ASSERT_EQ(write(pipefds[1], write_buf, 4), 4);

    char read_buf[total_payload]{};
    std::atomic<int> callback_invocations{0};
    std::atomic<bool> done{false};

    IoUringConfig cfg{.sq_entries = 64, .cq_entries = 128};
    IoUringEngine engine(cfg, *pool_, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        callback_invocations.fetch_add(1, std::memory_order_relaxed);

        if (res > 0) {
            task->retry.bytes_done += static_cast<size_t>(res);
            if (task->retry.bytes_done < task->retry.bytes_total && task->retry.retries_left > 0) {
                // Short read (§4.4): Resubmit for remaining bytes
                task->retry.retries_left--;
                size_t remaining = task->retry.bytes_total - task->retry.bytes_done;
                void* next_ptr = read_buf + task->retry.bytes_done;
                engine.submit_read(pipefds[0], next_ptr, remaining, 0, task);
                return;
            }
        }

        task->state.store(TaskState::Completed, std::memory_order_release);
        pool_->release(task);
        done.store(true, std::memory_order_release);
    });

    engine.start();

    TaskDescriptor* task = pool_->acquire();
    ASSERT_NE(task, nullptr);
    task->retry.bytes_total = total_payload;
    task->retry.bytes_done = 0;
    task->retry.retries_left = 4;

    // Submit initial read expecting 16 bytes
    ASSERT_TRUE(engine.submit_read(pipefds[0], read_buf, total_payload, 0, task));

    // Allow short read (4 bytes) to complete and trigger first retry
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Now write remaining 12 bytes to satisfy retry
    ASSERT_EQ(write(pipefds[1], write_buf + 4, 12), 12);

    auto start = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            FAIL() << "Timed out waiting for short read completion";
        }
        std::this_thread::yield();
    }

    EXPECT_EQ(callback_invocations.load(), 2); // Initial short read + completed retry
    EXPECT_EQ(std::memcmp(read_buf, write_buf, total_payload), 0);

    close(pipefds[0]);
    close(pipefds[1]);

    engine.request_stop();
    engine.join();
}

// Test 4: Cancellation path via IORING_OP_ASYNC_CANCEL (spec §4.4, §8.4)
TEST_F(IoUringEngineIntegrationTest, AsyncCancellationPath) {
    int pipefds[2];
    ASSERT_EQ(pipe(pipefds), 0);

    std::atomic<bool> cancel_received{false};
    int32_t completion_res = 0;

    IoUringConfig cfg{.sq_entries = 64, .cq_entries = 128};
    IoUringEngine engine(cfg, *pool_, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        completion_res = res;
        // Spec §8.4: Only release slot once kernel returns completion
        task->state.store(TaskState::Cancelled, std::memory_order_release);
        pool_->release(task);
        cancel_received.store(true, std::memory_order_release);
    });

    engine.start();

    TaskDescriptor* task = pool_->acquire();
    ASSERT_NE(task, nullptr);

    char buf[64];
    // Submit blocking read on pipe with no data
    ASSERT_TRUE(engine.submit_read(pipefds[0], buf, sizeof(buf), 0, task));

    // Request cancellation (§4.1, §8.4)
    ASSERT_TRUE(engine.try_cancel(task));

    auto start = std::chrono::steady_clock::now();
    while (!cancel_received.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            FAIL() << "Timed out waiting for async cancellation completion";
        }
        std::this_thread::yield();
    }

    // Must complete with -ECANCELED
    EXPECT_EQ(completion_res, -ECANCELED);
    EXPECT_EQ(pool_->stale_completions(), 0u);

    close(pipefds[0]);
    close(pipefds[1]);

    engine.request_stop();
    engine.join();
}

// Test 5: Milestone M2 Acceptance Gate: 100,000 Operations Stress Test (spec §11.4)
TEST_F(IoUringEngineIntegrationTest, OneHundredThousandOpsStressTest) {
    constexpr uint64_t kTotalOps = 100'000ULL;
    constexpr size_t kPoolCapacity = 4096;

    // Generous TaskPool sized for concurrent in-flight pipeline
    auto stress_pool = std::make_unique<TaskPool>(kPoolCapacity);

    std::atomic<uint64_t> completed_count{0};

    IoUringConfig cfg{
        .sq_entries = 1024,
        .cq_entries = 4096,
        .use_sqpoll = false
    };

    IoUringEngine engine(cfg, *stress_pool, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        EXPECT_EQ(res, 0);
        task->state.store(TaskState::Completed, std::memory_order_release);
        stress_pool->release(task);
        completed_count.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();

    std::cout << "[ M2 GATE ] Submitting " << kTotalOps << " synthetic NOP operations to io_uring...\n";
    auto start_time = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < kTotalOps; ++i) {
        TaskDescriptor* task = nullptr;
        // Acquire with backpressure if pool is transiently exhausted
        while ((task = stress_pool->acquire()) == nullptr) {
            std::this_thread::yield();
        }

        // Submit with backpressure if SQ ring is full (§4.5)
        while (!engine.submit_nop(task)) {
            std::this_thread::yield();
        }
    }

    // Await all completions
    while (completed_count.load(std::memory_order_acquire) < kTotalOps) {
        std::this_thread::yield();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "[ M2 GATE ] Completed " << completed_count.load() << " operations in "
              << elapsed_ms << " ms (" << (kTotalOps * 1000 / (elapsed_ms ? elapsed_ms : 1))
              << " ops/sec)\n";

    EXPECT_EQ(completed_count.load(), kTotalOps);
    EXPECT_EQ(engine.submitted_count(), kTotalOps);
    EXPECT_EQ(stress_pool->stale_completions(), 0u);
    EXPECT_EQ(stress_pool->available(), kPoolCapacity);

    engine.request_stop();
    engine.join();
}

// Test 6: Concurrency Regression Test: High Turnover / Slot Recycling Under TSAN
TEST_F(IoUringEngineIntegrationTest, ConcurrentAcquireSubmitPollerContention) {
    constexpr uint64_t kTotalOps = 20'000ULL;
    constexpr size_t kSmallPoolCapacity = 32; // Small capacity forces continuous rapid recycling

    auto small_pool = std::make_unique<TaskPool>(kSmallPoolCapacity);
    std::atomic<uint64_t> completed_count{0};

    IoUringConfig cfg{
        .sq_entries = 64,
        .cq_entries = 128,
        .use_sqpoll = false
    };

    IoUringEngine engine(cfg, *small_pool, [&](TaskDescriptor* task, int32_t res, uint32_t /*flags*/) {
        EXPECT_EQ(res, 0);
        task->state.store(TaskState::Completed, std::memory_order_release);
        small_pool->release(task);
        completed_count.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();

    for (uint64_t i = 0; i < kTotalOps; ++i) {
        TaskDescriptor* task = nullptr;
        while ((task = small_pool->acquire()) == nullptr) {
            std::this_thread::yield();
        }

        while (!engine.submit_nop(task)) {
            std::this_thread::yield();
        }
    }

    auto start_wait = std::chrono::steady_clock::now();
    while (completed_count.load(std::memory_order_acquire) < kTotalOps) {
        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(10)) {
            FAIL() << "Timed out waiting for high-turnover completions";
        }
        std::this_thread::yield();
    }

    engine.request_stop();
    engine.join();

    EXPECT_EQ(completed_count.load(), kTotalOps);
    EXPECT_EQ(engine.submitted_count(), kTotalOps);
    EXPECT_EQ(small_pool->stale_completions(), 0u);
    EXPECT_EQ(small_pool->available(), kSmallPoolCapacity);
}

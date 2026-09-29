// tests/unit/object_pool_test.cpp
#include <gtest/gtest.h>
#include "heterosteal/object_pool.hpp"
#include "heterosteal/correlation_table.hpp"
#include <vector>
#include <unordered_set>
#include <thread>
#include <atomic>

using namespace hs;

class ObjectPoolTest : public ::testing::Test {};

// Test 1: Basic acquire and release lifecycle
TEST_F(ObjectPoolTest, AcquireReleaseLifecycle) {
    constexpr size_t kCapacity = 32;
    TaskPool pool(kCapacity);

    EXPECT_EQ(pool.capacity(), kCapacity);
    EXPECT_EQ(pool.available(), kCapacity);

    std::vector<TaskDescriptor*> acquired;
    std::unordered_set<uint32_t> slots;

    for (size_t i = 0; i < kCapacity; ++i) {
        TaskDescriptor* t = pool.acquire();
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->state.load(std::memory_order_relaxed), TaskState::Created);
        EXPECT_EQ(slots.count(t->slot_index), 0u) << "Duplicate slot index allocated";
        slots.insert(t->slot_index);
        acquired.push_back(t);
    }

    EXPECT_EQ(pool.available(), 0u);

    // Release all tasks after setting terminal state
    for (TaskDescriptor* t : acquired) {
        t->state.store(TaskState::Completed, std::memory_order_release);
        pool.release(t);
    }

    EXPECT_EQ(pool.available(), kCapacity);
}

// Test 2: Capacity exhaustion returns nullptr rather than growing slab (spec §3.4)
TEST_F(ObjectPoolTest, ExhaustionReturnsNullptr) {
    constexpr size_t kCapacity = 4;
    TaskPool pool(kCapacity);

    std::vector<TaskDescriptor*> tasks;
    for (size_t i = 0; i < kCapacity; ++i) {
        TaskDescriptor* t = pool.acquire();
        ASSERT_NE(t, nullptr);
        tasks.push_back(t);
    }

    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.exhausted_count(), 0u);

    // 5th acquire must return nullptr (backpressure per spec §4.7)
    TaskDescriptor* overflow = pool.acquire();
    EXPECT_EQ(overflow, nullptr);
    EXPECT_EQ(pool.exhausted_count(), 1u);

    // Release one slot and re-acquire
    tasks[0]->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(tasks[0]);

    EXPECT_EQ(pool.available(), 1u);
    TaskDescriptor* reacquired = pool.acquire();
    ASSERT_NE(reacquired, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // Clean up remainder
    reacquired->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(reacquired);
    for (size_t i = 1; i < tasks.size(); ++i) {
        tasks[i]->state.store(TaskState::Completed, std::memory_order_release);
        pool.release(tasks[i]);
    }
}

// Test 3: Handle encoding and decoding
TEST_F(ObjectPoolTest, HandleEncodeDecodeRoundTrip) {
    uint32_t slot = 12345;
    uint32_t gen = 67890;

    uint64_t handle = TaskPool::encode_handle(slot, gen);

    uint32_t decoded_slot = 0;
    uint32_t decoded_gen = 0;
    TaskPool::decode_handle(handle, decoded_slot, decoded_gen);

    EXPECT_EQ(decoded_slot, slot);
    EXPECT_EQ(decoded_gen, gen);

    CorrelationHandle ch = CorrelationHandle::from_u64(handle);
    EXPECT_EQ(ch.slot_index, slot);
    EXPECT_EQ(ch.generation, gen);
    EXPECT_EQ(ch.to_u64(), handle);
}

// Test 4: Generation bump and stale handle rejection (spec §3.4, §4.2)
TEST_F(ObjectPoolTest, GenerationalStaleHandleRejection) {
    TaskPool pool(8);

    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);

    uint32_t slot = task->slot_index;
    uint32_t gen1 = task->generation;
    uint64_t handle1 = TaskPool::encode_handle(slot, gen1);

    // Initial handle resolves correctly
    TaskDescriptor* resolved1 = pool.resolve(handle1);
    EXPECT_EQ(resolved1, task);
    EXPECT_EQ(pool.stale_completions(), 0u);

    // Complete and release slot (bumps generation)
    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);

    // Old handle1 must now fail to resolve and increment stale_completions counter
    TaskDescriptor* stale = pool.resolve(handle1);
    EXPECT_EQ(stale, nullptr);
    EXPECT_EQ(pool.stale_completions(), 1u);

    // Re-acquire slot; generation must have advanced
    TaskDescriptor* reacquired = pool.acquire();
    ASSERT_NE(reacquired, nullptr);
    EXPECT_EQ(reacquired->slot_index, slot);
    EXPECT_GT(reacquired->generation, gen1);

    uint64_t handle2 = TaskPool::encode_handle(reacquired->slot_index, reacquired->generation);
    TaskDescriptor* resolved2 = pool.resolve(handle2);
    EXPECT_EQ(resolved2, reacquired);

    // Clean up
    reacquired->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(reacquired);
}

// Test 5: Out of bounds handle resolution
TEST_F(ObjectPoolTest, OutOfBoundsHandleRejection) {
    TaskPool pool(4);
    uint64_t bad_handle = TaskPool::encode_handle(999, 1);
    EXPECT_EQ(pool.resolve(bad_handle), nullptr);
    EXPECT_GT(pool.stale_completions(), 0u);
}

// Test 6: Duplicate release safely ignored and preserves pool integrity
TEST_F(ObjectPoolTest, DuplicateReleaseSafelyIgnored) {
    constexpr size_t kCapacity = 4;
    TaskPool pool(kCapacity);

    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(pool.available(), kCapacity - 1);

    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);
    EXPECT_EQ(pool.available(), kCapacity);

    // Duplicate release of already-released task should be safely ignored
    pool.release(task);
    EXPECT_EQ(pool.available(), kCapacity);

    // Pool integrity: all slots can be acquired with unique indices
    std::vector<TaskDescriptor*> acquired;
    std::unordered_set<uint32_t> slots;
    for (size_t i = 0; i < kCapacity; ++i) {
        TaskDescriptor* t = pool.acquire();
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(slots.count(t->slot_index), 0u) << "Duplicate slot index allocated after double release";
        slots.insert(t->slot_index);
        acquired.push_back(t);
    }
    EXPECT_EQ(pool.available(), 0u);

    for (TaskDescriptor* t : acquired) {
        t->state.store(TaskState::Completed, std::memory_order_release);
        pool.release(t);
    }
    EXPECT_EQ(pool.available(), kCapacity);
}

// Test 7: resolve_guarded pins the slot; release defers free-list reuse until unpin
TEST_F(ObjectPoolTest, GuardedPinDefersReuseUntilUnpin) {
    TaskPool pool(1);

    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);
    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);

    CompletionGuard guard = pool.resolve_guarded(handle);
    ASSERT_TRUE(guard);
    EXPECT_EQ(guard.get(), task);
    EXPECT_EQ(pool.pin_count(task->slot_index), 1u);
    EXPECT_EQ(pool.available(), 0u);

    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);

    // Slot is retiring but still pinned — must not be reusable.
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_EQ(pool.exhausted_count(), 1u);

    // Stale handle (generation bumped) must not pin while the old guard is live.
    CompletionGuard stale_while_pinned = pool.resolve_guarded(handle);
    EXPECT_FALSE(stale_while_pinned);

    // Completion-side write is exclusive vs acquire() reset.
    guard->more_completions_pending.store(true, std::memory_order_release);
    guard->io_op_seq = 0xC0FFEEULL;

    guard = CompletionGuard{};
    EXPECT_EQ(pool.pin_count(0), 0u);
    EXPECT_EQ(pool.available(), 1u);

    TaskDescriptor* reused = pool.acquire();
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->io_op_seq, 0u);
    EXPECT_FALSE(reused->more_completions_pending.load(std::memory_order_acquire));

    reused->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(reused);
}

// Test 8: guarded resolve rejects stale generations after a completed recycle
TEST_F(ObjectPoolTest, GuardedResolveRejectsStaleGeneration) {
    TaskPool pool(4);

    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);
    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);

    CompletionGuard live = pool.resolve_guarded(handle);
    ASSERT_TRUE(live);
    live = CompletionGuard{};

    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);

    CompletionGuard stale = pool.resolve_guarded(handle);
    EXPECT_FALSE(stale);
    EXPECT_GE(pool.stale_completions(), 1u);

    EXPECT_EQ(pool.resolve(handle), nullptr);
}

// Test 9: two pins; recycle happens only after the last unpin
TEST_F(ObjectPoolTest, NestedGuardsDeferReleaseUntilLastUnpin) {
    TaskPool pool(1);
    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);
    uint64_t handle = TaskPool::encode_handle(task->slot_index, task->generation);

    CompletionGuard g1 = pool.resolve_guarded(handle);
    CompletionGuard g2 = pool.resolve_guarded(handle);
    ASSERT_TRUE(g1);
    ASSERT_TRUE(g2);
    EXPECT_EQ(pool.pin_count(task->slot_index), 2u);

    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);
    EXPECT_EQ(pool.available(), 0u);

    g1 = CompletionGuard{};
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);

    g2 = CompletionGuard{};
    EXPECT_EQ(pool.available(), 1u);
}

// Test 10: concurrent poller-style pin vs acquire reset (TSAN target)
TEST_F(ObjectPoolTest, ConcurrentPinPreventsAcquireResetRace) {
    TaskPool pool(1);
    std::atomic<uint64_t> published_handle{0};
    std::atomic<int> phase{0};
    std::atomic<bool> reused_while_pinned{false};

    std::thread poller([&] {
        while (published_handle.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        CompletionGuard guard = pool.resolve_guarded(published_handle.load(std::memory_order_acquire));
        if (!guard) {
            phase.store(-1, std::memory_order_release);
            return;
        }
        phase.store(1, std::memory_order_release);
        for (int i = 0; i < 200000; ++i) {
            guard->more_completions_pending.store((i & 1) != 0, std::memory_order_relaxed);
            if (pool.available() != 0) {
                reused_while_pinned.store(true, std::memory_order_relaxed);
            }
        }
        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        guard = CompletionGuard{};
        phase.store(3, std::memory_order_release);
    });

    TaskDescriptor* task = pool.acquire();
    ASSERT_NE(task, nullptr);
    published_handle.store(TaskPool::encode_handle(task->slot_index, task->generation),
                           std::memory_order_release);

    while (phase.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    if (phase.load(std::memory_order_acquire) != 1) {
        poller.join();
        FAIL() << "poller failed to resolve_guarded the live handle";
    }

    task->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(task);
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);

    phase.store(2, std::memory_order_release);
    while (phase.load(std::memory_order_acquire) < 3) {
        std::this_thread::yield();
    }
    poller.join();

    EXPECT_FALSE(reused_while_pinned.load(std::memory_order_relaxed));
    TaskDescriptor* reused = pool.acquire();
    ASSERT_NE(reused, nullptr);
    EXPECT_FALSE(reused->more_completions_pending.load(std::memory_order_acquire));
    reused->state.store(TaskState::Completed, std::memory_order_release);
    pool.release(reused);
}

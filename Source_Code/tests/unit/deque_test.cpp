// tests/unit/deque_test.cpp
#include <gtest/gtest.h>
#include "heterosteal/deque.hpp"
#include "heterosteal/task.hpp"
#include <memory>
#include <deque>
#include <vector>
#include <random>

using namespace hs;

class DequeUnitTest : public ::testing::Test {
protected:
    static constexpr size_t kTaskCount = 5000;

    class TaskSlab {
    public:
        TaskSlab() : data_(std::make_unique<TaskDescriptor[]>(kTaskCount)) {
            for (size_t i = 0; i < kTaskCount; ++i) {
                data_[i].slot_index = static_cast<uint32_t>(i);
            }
        }

        TaskDescriptor& operator[](size_t i) noexcept { return data_[i]; }
        const TaskDescriptor& operator[](size_t i) const noexcept { return data_[i]; }
        size_t size() const noexcept { return kTaskCount; }

    private:
        std::unique_ptr<TaskDescriptor[]> data_;
    };

    TaskSlab tasks_;
};

// Test 1: Empty deque behavior
TEST_F(DequeUnitTest, EmptyDequeReturnsNullopt) {
    WorkStealingDeque deque(16);
    EXPECT_EQ(deque.size_approx(), 0);
    EXPECT_EQ(deque.pop_bottom(), std::nullopt);
    EXPECT_EQ(deque.steal_top(), std::nullopt);
}

// Test 2: Owner LIFO order
TEST_F(DequeUnitTest, OwnerPopIsLIFO) {
    WorkStealingDeque deque(16);

    deque.push_bottom(&tasks_[1]);
    deque.push_bottom(&tasks_[2]);
    deque.push_bottom(&tasks_[3]);

    EXPECT_EQ(deque.size_approx(), 3);

    auto p1 = deque.pop_bottom();
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ((*p1)->slot_index, 3u);

    auto p2 = deque.pop_bottom();
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ((*p2)->slot_index, 2u);

    auto p3 = deque.pop_bottom();
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ((*p3)->slot_index, 1u);

    EXPECT_EQ(deque.pop_bottom(), std::nullopt);
    EXPECT_EQ(deque.size_approx(), 0);
}

// Test 3: Thief FIFO order
TEST_F(DequeUnitTest, ThiefStealIsFIFO) {
    WorkStealingDeque deque(16);

    deque.push_bottom(&tasks_[10]);
    deque.push_bottom(&tasks_[20]);
    deque.push_bottom(&tasks_[30]);

    auto s1 = deque.steal_top();
    ASSERT_TRUE(s1.has_value());
    EXPECT_EQ((*s1)->slot_index, 10u);

    auto s2 = deque.steal_top();
    ASSERT_TRUE(s2.has_value());
    EXPECT_EQ((*s2)->slot_index, 20u);

    auto s3 = deque.steal_top();
    ASSERT_TRUE(s3.has_value());
    EXPECT_EQ((*s3)->slot_index, 30u);

    EXPECT_EQ(deque.steal_top(), std::nullopt);
}

// Test 4: Interleaved push/pop/steal against sequential reference model
TEST_F(DequeUnitTest, MatchesSequentialReferenceModel) {
    WorkStealingDeque deque(8);
    std::deque<TaskDescriptor*> ref;

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<int> op_dist(0, 2); // 0: push, 1: pop, 2: steal

    size_t next_task_idx = 0;
    constexpr size_t total_ops = 50000;

    for (size_t op = 0; op < total_ops; ++op) {
        int action = op_dist(rng);

        if (action == 0 && next_task_idx < tasks_.size()) {
            // Push bottom
            TaskDescriptor* t = &tasks_[next_task_idx++];
            deque.push_bottom(t);
            ref.push_back(t);
        } else if (action == 1) {
            // Pop bottom (LIFO from back)
            auto val = deque.pop_bottom();
            if (ref.empty()) {
                EXPECT_EQ(val, std::nullopt);
            } else {
                ASSERT_TRUE(val.has_value());
                TaskDescriptor* expected = ref.back();
                ref.pop_back();
                EXPECT_EQ(*val, expected);
            }
        } else {
            // Steal top (FIFO from front)
            auto val = deque.steal_top();
            if (ref.empty()) {
                EXPECT_EQ(val, std::nullopt);
            } else {
                ASSERT_TRUE(val.has_value());
                TaskDescriptor* expected = ref.front();
                ref.pop_front();
                EXPECT_EQ(*val, expected);
            }
        }
    }

    // Drain remainder
    while (!ref.empty()) {
        auto val = deque.pop_bottom();
        ASSERT_TRUE(val.has_value());
        TaskDescriptor* expected = ref.back();
        ref.pop_back();
        EXPECT_EQ(*val, expected);
    }

    EXPECT_EQ(deque.pop_bottom(), std::nullopt);
    EXPECT_EQ(deque.steal_top(), std::nullopt);
}

// Test 5: Dynamic array growth
TEST_F(DequeUnitTest, ArrayGrowthCorrectness) {
    // Start with small capacity 4
    WorkStealingDeque deque(4);
    EXPECT_EQ(deque.capacity(), 4u);

    constexpr size_t kPushCount = 2000;
    for (size_t i = 0; i < kPushCount; ++i) {
        deque.push_bottom(&tasks_[i]);
    }

    // Capacity must have doubled multiple times to exceed kPushCount
    EXPECT_GE(deque.capacity(), kPushCount);
    EXPECT_EQ(deque.size_approx(), static_cast<int64_t>(kPushCount));

    // Pop first half from bottom (LIFO)
    for (size_t i = 0; i < kPushCount / 2; ++i) {
        auto val = deque.pop_bottom();
        ASSERT_TRUE(val.has_value());
        size_t expected_idx = (kPushCount - 1) - i;
        EXPECT_EQ((*val)->slot_index, expected_idx);
    }

    // Steal second half from top (FIFO)
    for (size_t i = 0; i < kPushCount / 2; ++i) {
        auto val = deque.steal_top();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ((*val)->slot_index, i);
    }

    // Queue must now be empty
    EXPECT_EQ(deque.pop_bottom(), std::nullopt);
    EXPECT_EQ(deque.steal_top(), std::nullopt);
    EXPECT_EQ(deque.size_approx(), 0);
}

// Test 6: Repeated cycle wrap-around
TEST_F(DequeUnitTest, CircularWrapAround) {
    WorkStealingDeque deque(8);

    for (int cycle = 0; cycle < 1000; ++cycle) {
        for (int i = 0; i < 6; ++i) {
            deque.push_bottom(&tasks_[i]);
        }
        for (int i = 0; i < 3; ++i) {
            auto val = deque.pop_bottom();
            ASSERT_TRUE(val.has_value());
        }
        for (int i = 0; i < 3; ++i) {
            auto val = deque.steal_top();
            ASSERT_TRUE(val.has_value());
        }
        EXPECT_EQ(deque.pop_bottom(), std::nullopt);
        EXPECT_EQ(deque.steal_top(), std::nullopt);
    }
}

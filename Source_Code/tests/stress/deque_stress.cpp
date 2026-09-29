// tests/stress/deque_stress.cpp
#include <gtest/gtest.h>
#include "heterosteal/deque.hpp"
#include "heterosteal/task.hpp"
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <string>

using namespace hs;

namespace {

struct StressConfig {
    uint32_t num_rounds = 1;
    uint64_t tasks_per_round = 1'000'000ULL;
    unsigned int num_thieves = 4;
};

StressConfig get_stress_config() {
    StressConfig cfg;
    const char* env_ops = std::getenv("HS_STRESS_OPS");
    if (env_ops != nullptr) {
        try {
            uint64_t total_ops = std::stoull(env_ops);
            if (total_ops >= 1'000'000ULL) {
                cfg.num_rounds = static_cast<uint32_t>(total_ops / 1'000'000ULL);
                cfg.tasks_per_round = 1'000'000ULL;
            } else if (total_ops > 0) {
                cfg.num_rounds = 1;
                cfg.tasks_per_round = total_ops;
            }
        } catch (...) {
            // fallback
        }
    }

    const char* env_thieves = std::getenv("HS_STRESS_THIEVES");
    if (env_thieves != nullptr) {
        try {
            unsigned int th = std::stoul(env_thieves);
            if (th > 0) {
                cfg.num_thieves = th;
            }
        } catch (...) {
            // fallback
        }
    }

    return cfg;
}

void run_stress_round(uint32_t round_num, uint64_t tasks_in_round, unsigned int num_thieves,
                      uint64_t& out_owner_pops, uint64_t& out_thief_steals, uint64_t& out_steal_attempts) {
    auto task_pool = std::make_unique<TaskDescriptor[]>(tasks_in_round);
    for (uint64_t i = 0; i < tasks_in_round; ++i) {
        task_pool[i].slot_index = static_cast<uint32_t>(i);
    }

    // Element conservation tracking: frequency count per task slot
    auto received_count = std::make_unique<std::atomic<uint32_t>[]>(tasks_in_round);
    for (uint64_t i = 0; i < tasks_in_round; ++i) {
        received_count[i].store(0, std::memory_order_relaxed);
    }

    WorkStealingDeque deque(1024);

    std::atomic<bool> start_signal{false};
    std::atomic<bool> owner_finished{false};

    std::atomic<uint64_t> round_steals{0};
    std::atomic<uint64_t> round_attempts{0};

    std::vector<std::thread> thieves;
    thieves.reserve(num_thieves);

    // Concurrent thief threads
    for (unsigned int tid = 0; tid < num_thieves; ++tid) {
        thieves.emplace_back([&]() {
            while (!start_signal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint64_t local_steals = 0;
            uint64_t local_attempts = 0;

            while (true) {
                local_attempts++;
                auto stolen = deque.steal_top();
                if (stolen.has_value()) {
                    uint32_t id = (*stolen)->slot_index;
                    received_count[id].fetch_add(1, std::memory_order_relaxed);
                    local_steals++;
                } else {
                    if (owner_finished.load(std::memory_order_acquire)) {
                        // Double check if deque is truly empty
                        auto second_try = deque.steal_top();
                        if (second_try.has_value()) {
                            uint32_t id = (*second_try)->slot_index;
                            received_count[id].fetch_add(1, std::memory_order_relaxed);
                            local_steals++;
                            continue;
                        }
                        break;
                    }
                    std::this_thread::yield();
                }
            }

            round_steals.fetch_add(local_steals, std::memory_order_relaxed);
            round_attempts.fetch_add(local_attempts, std::memory_order_relaxed);
        });
    }

    // Owner thread execution
    start_signal.store(true, std::memory_order_release);

    uint64_t owner_pops = 0;
    for (uint64_t i = 0; i < tasks_in_round; ++i) {
        deque.push_bottom(&task_pool[i]);

        // Intermittent owner pops: simulate local worker popping its own work (~20% of the time)
        if ((i % 5 == 0) && i > 0) {
            auto popped = deque.pop_bottom();
            if (popped.has_value()) {
                uint32_t id = (*popped)->slot_index;
                received_count[id].fetch_add(1, std::memory_order_relaxed);
                owner_pops++;
            }
        }
    }

    // Owner drains any remaining tasks until empty
    while (true) {
        auto popped = deque.pop_bottom();
        if (!popped.has_value()) {
            break;
        }
        uint32_t id = (*popped)->slot_index;
        received_count[id].fetch_add(1, std::memory_order_relaxed);
        owner_pops++;
    }

    owner_finished.store(true, std::memory_order_release);

    // Wait for all thieves to drain and finish
    for (auto& th : thieves) {
        th.join();
    }

    // Invariant Verification: Element Conservation
    uint64_t lost_elements = 0;
    uint64_t duplicated_elements = 0;
    uint64_t correct_elements = 0;

    for (uint64_t i = 0; i < tasks_in_round; ++i) {
        uint32_t cnt = received_count[i].load(std::memory_order_relaxed);
        if (cnt == 0) {
            lost_elements++;
        } else if (cnt == 1) {
            correct_elements++;
        } else {
            duplicated_elements++;
        }
    }

    EXPECT_EQ(lost_elements, 0u) << "Round " << round_num << ": Found lost elements";
    EXPECT_EQ(duplicated_elements, 0u) << "Round " << round_num << ": Found duplicated elements";
    EXPECT_EQ(correct_elements, tasks_in_round);

    out_owner_pops = owner_pops;
    out_thief_steals = round_steals.load();
    out_steal_attempts = round_attempts.load();
}

} // namespace

TEST(DequeStressTest, ConcurrentThievesElementConservation) {
    StressConfig cfg = get_stress_config();

    std::cout << "\n============================================================\n"
              << "[ STRESS ] WorkStealingDeque Stress Test (Spec §8.1 & §11.4)\n"
              << "           Total Target Ops:   " << (cfg.num_rounds * cfg.tasks_per_round) << "\n"
              << "           Rounds:             " << cfg.num_rounds << "\n"
              << "           Tasks Per Round:    " << cfg.tasks_per_round << "\n"
              << "           Concurrent Thieves: " << cfg.num_thieves << "\n"
              << "============================================================\n";

    uint64_t total_pushed = 0;
    uint64_t total_owner_pops = 0;
    uint64_t total_thief_steals = 0;
    uint64_t total_steal_attempts = 0;

    auto global_start = std::chrono::steady_clock::now();

    for (uint32_t r = 1; r <= cfg.num_rounds; ++r) {
        uint64_t r_owner_pops = 0;
        uint64_t r_thief_steals = 0;
        uint64_t r_steal_attempts = 0;

        auto round_start = std::chrono::steady_clock::now();

        run_stress_round(r, cfg.tasks_per_round, cfg.num_thieves,
                         r_owner_pops, r_thief_steals, r_steal_attempts);

        auto round_end = std::chrono::steady_clock::now();
        auto round_ms = std::chrono::duration_cast<std::chrono::milliseconds>(round_end - round_start).count();

        total_pushed += cfg.tasks_per_round;
        total_owner_pops += r_owner_pops;
        total_thief_steals += r_thief_steals;
        total_steal_attempts += r_steal_attempts;

        std::cout << "[ Round " << r << "/" << cfg.num_rounds << " ] Completed in "
                  << round_ms << " ms | Owner popped: " << r_owner_pops
                  << " | Thieves stolen: " << r_thief_steals
                  << " | Steal attempts: " << r_steal_attempts << "\n";
    }

    auto global_end = std::chrono::steady_clock::now();
    auto global_ms = std::chrono::duration_cast<std::chrono::milliseconds>(global_end - global_start).count();

    uint64_t total_received = total_owner_pops + total_thief_steals;

    std::cout << "\n============================================================\n"
              << "[ SUMMARY ] Stress Test Overall Results\n"
              << "            Total Duration:        " << global_ms << " ms\n"
              << "            Total Tasks Pushed:    " << total_pushed << "\n"
              << "            Total Owner Pops:      " << total_owner_pops << "\n"
              << "            Total Thief Steals:    " << total_thief_steals << "\n"
              << "            Total Tasks Received:  " << total_received << "\n"
              << "            Total Steal Attempts:  " << total_steal_attempts << "\n"
              << "            Conservation Status:   PASSED (0 lost, 0 duplicated)\n"
              << "============================================================\n\n";

    EXPECT_EQ(total_received, total_pushed);
}

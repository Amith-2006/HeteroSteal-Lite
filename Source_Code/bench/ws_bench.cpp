// bench/ws_bench.cpp
// Minimal P0 synthetic work-stealing benchmark.
// Closed batch: prefill one WorkStealingDeque per worker, then run.
// Does not use io_uring, TaskPool, topology, or alternative policies.

#include "heterosteal/deque.hpp"
#include "heterosteal/task.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Config {
    unsigned workers = 2;
    uint64_t tasks = 1000;
    std::string mode = "compute";
    double mix = 0.5;
    uint32_t compute_iters = 10000;
    uint32_t callback_iters = 50;
    uint32_t seed = 1;
    unsigned steal_attempts = 4;
};

void print_usage(std::string_view program) {
    std::cout
        << "hs_ws_bench — P0 synthetic work-stealing microbenchmark\n"
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  --workers N           worker threads / deques (default 2)\n"
        << "  --tasks N             closed-batch task count (default 1000)\n"
        << "  --mode MODE           compute | callback | mixed (default compute)\n"
        << "  --mix F               callback fraction in mixed mode, 0..1 (default 0.5)\n"
        << "  --compute-iters N     busy-loop trips per Compute task (default 10000)\n"
        << "  --callback-iters N    busy-loop trips per IoCallback task (default 50)\n"
        << "  --seed N              RNG seed for class shuffle and steal victims (default 1)\n"
        << "  --steal-attempts N    random steal tries per idle round (default 4)\n"
        << "  -h, --help            show this message\n\n"
        << "Smoke test:\n"
        << "  " << program
        << " --workers 2 --tasks 1000 --mode compute --seed 1\n";
}

bool parse_u32(std::string_view s, uint32_t& out) {
    if (s.empty()) {
        return false;
    }
    try {
        size_t idx = 0;
        unsigned long v = std::stoul(std::string(s), &idx, 10);
        if (idx != s.size() || v > UINT32_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty()) {
        return false;
    }
    try {
        size_t idx = 0;
        unsigned long long v = std::stoull(std::string(s), &idx, 10);
        if (idx != s.size()) {
            return false;
        }
        out = static_cast<uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_unsigned(std::string_view s, unsigned& out) {
    uint32_t tmp = 0;
    if (!parse_u32(s, tmp) || tmp == 0) {
        return false;
    }
    out = static_cast<unsigned>(tmp);
    return true;
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto need = [&](const char* name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return {};
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--workers") {
            if (!parse_unsigned(need("--workers"), cfg.workers)) {
                std::cerr << "invalid --workers\n";
                return false;
            }
        } else if (arg == "--tasks") {
            uint64_t n = 0;
            if (!parse_u64(need("--tasks"), n) || n == 0) {
                std::cerr << "invalid --tasks\n";
                return false;
            }
            cfg.tasks = n;
        } else if (arg == "--mode") {
            cfg.mode = std::string(need("--mode"));
            if (cfg.mode != "compute" && cfg.mode != "callback" && cfg.mode != "mixed") {
                std::cerr << "--mode must be compute, callback, or mixed\n";
                return false;
            }
        } else if (arg == "--mix") {
            try {
                size_t idx = 0;
                cfg.mix = std::stod(std::string(need("--mix")), &idx);
                if (idx == 0 || cfg.mix < 0.0 || cfg.mix > 1.0) {
                    throw std::runtime_error("range");
                }
            } catch (...) {
                std::cerr << "invalid --mix (expected 0..1)\n";
                return false;
            }
        } else if (arg == "--compute-iters") {
            if (!parse_u32(need("--compute-iters"), cfg.compute_iters)) {
                std::cerr << "invalid --compute-iters\n";
                return false;
            }
        } else if (arg == "--callback-iters") {
            if (!parse_u32(need("--callback-iters"), cfg.callback_iters)) {
                std::cerr << "invalid --callback-iters\n";
                return false;
            }
        } else if (arg == "--seed") {
            if (!parse_u32(need("--seed"), cfg.seed)) {
                std::cerr << "invalid --seed\n";
                return false;
            }
        } else if (arg == "--steal-attempts") {
            if (!parse_unsigned(need("--steal-attempts"), cfg.steal_attempts)) {
                std::cerr << "invalid --steal-attempts\n";
                return false;
            }
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

// Keep the compiler from deleting the busy loop. Per-worker accumulator only.
uint32_t busy_spin(uint32_t iters) {
    uint32_t acc = 1u;
    for (uint32_t i = 0; i < iters; ++i) {
        acc = acc * 1664525u + 1013904223u;
    }
    asm volatile("" : "+r"(acc)::"memory");
    return acc;
}

int pick_victim(std::mt19937& rng, int self, int nworkers) {
    if (nworkers <= 1) {
        return -1;
    }
    std::uniform_int_distribution<int> dist(0, nworkers - 2);
    int v = dist(rng);
    if (v >= self) {
        ++v;
    }
    return v;
}

// Nearest-rank percentile on a non-decreasing sample vector.
// index = ceil(p/100 * n) - 1, clamped to [0, n-1]. Empty → 0.
uint64_t percentile_ns(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const auto n = static_cast<double>(sorted.size());
    auto rank = static_cast<size_t>(std::ceil(p / 100.0 * n));
    if (rank < 1) {
        rank = 1;
    }
    if (rank > sorted.size()) {
        rank = sorted.size();
    }
    return sorted[rank - 1];
}

void print_class_stats(const char* name,
                       const std::vector<uint64_t>& lat_ns,
                       uint64_t stolen,
                       uint64_t steal_sum,
                       double wall_s) {
    std::vector<uint64_t> sorted = lat_ns;
    std::sort(sorted.begin(), sorted.end());
    const uint64_t n = sorted.size();
    const double tput = (wall_s > 0.0) ? static_cast<double>(n) / wall_s : 0.0;
    std::cout << name << ":\n"
              << "  count              " << n << "\n"
              << "  throughput_tasks_s " << std::fixed << std::setprecision(2) << tput << "\n"
              << "  latency_p50_ns     " << percentile_ns(sorted, 50.0) << "\n"
              << "  latency_p90_ns     " << percentile_ns(sorted, 90.0) << "\n"
              << "  latency_p99_ns     " << percentile_ns(sorted, 99.0) << "\n"
              << "  stolen_tasks       " << stolen << "\n"
              << "  steal_count_sum    " << steal_sum << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 2;
    }

    const int nworkers = static_cast<int>(cfg.workers);
    const uint64_t ntasks = cfg.tasks;

    // Class assignment is computed on the main thread before any worker starts.
    std::vector<hs::TaskClass> classes(ntasks, hs::TaskClass::Compute);
    if (cfg.mode == "callback") {
        std::fill(classes.begin(), classes.end(), hs::TaskClass::IoCallback);
    } else if (cfg.mode == "mixed") {
        const uint64_t ncallback =
            static_cast<uint64_t>(std::llround(cfg.mix * static_cast<double>(ntasks)));
        for (uint64_t i = 0; i < ncallback && i < ntasks; ++i) {
            classes[i] = hs::TaskClass::IoCallback;
        }
        std::mt19937 shuffle_rng(cfg.seed);
        std::shuffle(classes.begin(), classes.end(), shuffle_rng);
    }

    auto slab = std::make_unique<hs::TaskDescriptor[]>(ntasks);

    // WorkStealingDeque is not movable; heap-allocate one per worker.
    std::vector<std::unique_ptr<hs::WorkStealingDeque>> deques;
    deques.reserve(static_cast<size_t>(nworkers));
    for (int w = 0; w < nworkers; ++w) {
        deques.push_back(std::make_unique<hs::WorkStealingDeque>(1024));
    }

    const auto prefill_now = hs::Clock::now();
    for (uint64_t i = 0; i < ntasks; ++i) {
        hs::TaskDescriptor& t = slab[i];
        t.slot_index = static_cast<uint32_t>(i);
        t.task_class = classes[i];
        t.origin_worker_id = static_cast<int>(i % static_cast<uint64_t>(nworkers));
        t.enqueued_at = prefill_now;
        t.state.store(hs::TaskState::Enqueued, std::memory_order_relaxed);
        // InlineTask is not copy/move assignable. Kernels run in the worker
        // from task_class; body stays default-constructed (no-op).
        deques[static_cast<size_t>(t.origin_worker_id)]->push_bottom(&t);
    }

    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> checksum{0};

    auto worker_fn = [&](int wid) {
        std::mt19937 rng(cfg.seed ^ (static_cast<uint32_t>(wid + 1) * 0x9e3779b9u));
        uint64_t local_sum = 0;
        unsigned idle_rounds = 0;
        hs::WorkStealingDeque& self = *deques[static_cast<size_t>(wid)];

        while (completed.load(std::memory_order_acquire) < ntasks) {
            std::optional<hs::TaskDescriptor*> got = self.pop_bottom();
            bool stolen = false;
            if (!got.has_value()) {
                for (unsigned a = 0; a < cfg.steal_attempts; ++a) {
                    const int victim = pick_victim(rng, wid, nworkers);
                    if (victim < 0) {
                        break;
                    }
                    got = deques[static_cast<size_t>(victim)]->steal_top();
                    if (got.has_value()) {
                        stolen = true;
                        break;
                    }
                }
            }
            if (!got.has_value()) {
                ++idle_rounds;
                if (idle_rounds < 64) {
                    busy_spin(32);
                } else {
                    std::this_thread::yield();
                }
                continue;
            }

            idle_rounds = 0;
            hs::TaskDescriptor* task = *got;
            if (stolen) {
                task->steal_count.fetch_add(1, std::memory_order_relaxed);
            }

            // Exclusive owner of this TaskDescriptor after a successful pop/steal.
            task->started_at = hs::Clock::now();
            task->state.store(hs::TaskState::Running, std::memory_order_relaxed);
            const uint32_t iters = (task->task_class == hs::TaskClass::Compute)
                                       ? cfg.compute_iters
                                       : cfg.callback_iters;
            local_sum += busy_spin(iters);
            task->completed_at = hs::Clock::now();
            task->last_run_worker_id = wid;
            task->last_run_at = task->completed_at;
            task->state.store(hs::TaskState::Completed, std::memory_order_release);
            completed.fetch_add(1, std::memory_order_release);
        }
        checksum.fetch_add(local_sum, std::memory_order_relaxed);
    };

    const auto wall_start = hs::Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nworkers));
    for (int w = 0; w < nworkers; ++w) {
        workers.emplace_back(worker_fn, w);
    }
    for (auto& th : workers) {
        th.join();
    }
    const auto wall_end = hs::Clock::now();
    workers.clear();

    // Deques still alive; no further steal/pop. Slab is not recycled.
    if (completed.load(std::memory_order_acquire) != ntasks) {
        std::cerr << "conservation failure: completed=" << completed.load()
                  << " expected=" << ntasks << "\n";
        return 1;
    }

    std::vector<uint64_t> all_lat, compute_lat, callback_lat;
    all_lat.reserve(ntasks);
    uint64_t stolen_all = 0, steal_sum_all = 0;
    uint64_t stolen_c = 0, steal_sum_c = 0;
    uint64_t stolen_cb = 0, steal_sum_cb = 0;

    for (uint64_t i = 0; i < ntasks; ++i) {
        const hs::TaskDescriptor& t = slab[i];
        if (t.state.load(std::memory_order_acquire) != hs::TaskState::Completed) {
            std::cerr << "task " << i << " not Completed after join\n";
            return 1;
        }
        if (t.completed_at < t.enqueued_at) {
            std::cerr << "task " << i << " completed before enqueue\n";
            return 1;
        }
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t.completed_at - t.enqueued_at)
                .count());
        all_lat.push_back(ns);
        const uint32_t sc = t.steal_count.load(std::memory_order_relaxed);
        steal_sum_all += sc;
        if (sc > 0) {
            ++stolen_all;
        }
        if (t.task_class == hs::TaskClass::Compute) {
            compute_lat.push_back(ns);
            steal_sum_c += sc;
            if (sc > 0) {
                ++stolen_c;
            }
        } else {
            callback_lat.push_back(ns);
            steal_sum_cb += sc;
            if (sc > 0) {
                ++stolen_cb;
            }
        }
    }

    const double wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
    const double tput = (wall_s > 0.0) ? static_cast<double>(ntasks) / wall_s : 0.0;

    std::cout << "hs_ws_bench\n"
              << "  mode               " << cfg.mode << "\n"
              << "  workers            " << nworkers << "\n"
              << "  tasks              " << ntasks << "\n"
              << "  mix                " << cfg.mix << "\n"
              << "  compute_iters      " << cfg.compute_iters << "\n"
              << "  callback_iters     " << cfg.callback_iters << "\n"
              << "  seed               " << cfg.seed << "\n"
              << "  steal_attempts     " << cfg.steal_attempts << "\n"
              << "  wall_s             " << std::fixed << std::setprecision(6) << wall_s << "\n"
              << "  throughput_tasks_s " << std::setprecision(2) << tput << "\n"
              << "  checksum           " << checksum.load(std::memory_order_relaxed) << "\n\n";

    print_class_stats("all", all_lat, stolen_all, steal_sum_all, wall_s);
    std::cout << "\n";
    print_class_stats("compute", compute_lat, stolen_c, steal_sum_c, wall_s);
    std::cout << "\n";
    print_class_stats("callback", callback_lat, stolen_cb, steal_sum_cb, wall_s);
    std::cout << "\n"
              << "latency: enqueue→complete on each TaskDescriptor; percentiles are\n"
              << "nearest-rank on the sorted sample (index = ceil(p/100*n)-1).\n"
              << "seed fixes class shuffle (mixed) and victim-draw sequence only;\n"
              << "it does not freeze steal success, steal counts, or timings.\n";

    (void)checksum;
    return 0;
}

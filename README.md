# HeteroSteal Lite
## Evaluating Work-Stealing Performance Under Heterogeneous Workloads

HeteroSteal Lite is a C++ benchmark that investigates how different workload characteristics affect throughput and task completion latency in a work-stealing scheduler.

The project is motivated by the challenges of scheduling heterogeneous tasks, particularly the coexistence of computationally intensive tasks and lightweight asynchronous callback-like tasks.

The original research motivation was to investigate io_uring callback behavior in work-stealing systems. The current implementation uses synthetic workloads and does not implement real io_uring completion callbacks or demonstrate callback poisoning.

---

## 1. Problem Statement

Work-stealing schedulers distribute tasks among worker threads. When a worker runs out of work, it can steal tasks from another worker.

However, different tasks have different execution costs. Computationally intensive tasks and lightweight callback-like tasks may respond differently to scheduling overhead, worker count, and work distribution.

This project investigates:

**How does increasing the number of workers affect throughput and task completion latency for compute-intensive, callback-like, and mixed workloads?**

---

## 2. Workloads

The benchmark supports three workload modes:

| Workload | Description |
|---|---|
| Compute | Computationally intensive synthetic tasks |
| Callback | Lightweight callback-like synthetic tasks |
| Mixed | A configurable mixture of compute and callback-like tasks |

The default mixed workload uses a 50-50 distribution of callback-like and compute tasks.

---

## 3. Technical Implementation

- Language: C++
- Build system: CMake
- Scheduling approach: Work-stealing scheduler
- Configurable worker count and task count
- Synthetic workload generation
- Enqueue-to-completion latency measurement
- Throughput and latency percentile analysis

The benchmark measures:

- Throughput: Completed tasks per second.
- p50 latency: Median enqueue-to-completion latency.
- p90 latency: 90th percentile completion latency.
- p99 latency: 99th percentile completion latency.

---

## 4. Experimental Setup

The benchmark was evaluated using the following environment:

| Component | Configuration |
|---|---|
| Operating system | Ubuntu 26.04.1 LTS |
| Architecture | ARM64 (aarch64) |
| Environment | Ubuntu virtual machine running in UTM |
| Virtual CPUs | 4 |
| RAM | Approximately 3.3 GiB |
| Compiler | GCC 15.2.0 |
| Build system | CMake 4.2.3 |
| Build configuration | Release |

### Experiment configuration

- Workloads: Compute, callback, and mixed.
- Workers: 1, 2, and 4.
- Tasks per run: 1,000.
- Repetitions: 5 per configuration.
- Total configurations: 9.
- Total benchmark runs: 45.

Results are reported as averages across five repetitions.

---

## 5. Results

### Average throughput

| Workload | 1 Worker | 2 Workers | 4 Workers |
|---|---:|---:|---:|
| Compute | 100,270 | 197,796 | 370,950 |
| Callback | 5,527,992 | 5,387,653 | 4,965,491 |
| Mixed | 196,808 | 385,172 | 693,457 |

Throughput is measured in tasks per second.

### Average p99 latency

| Workload | 1 Worker | 2 Workers | 4 Workers |
|---|---:|---:|---:|
| Compute | 9.8226 ms | 4.9432 ms | 2.5141 ms |
| Callback | 0.1384 ms | 0.1140 ms | 0.0862 ms |
| Mixed | 4.9733 ms | 2.5057 ms | 1.2780 ms |

### Observations

1. Compute throughput increased approximately 3.70 times when worker count increased from one to four.
2. Callback-like workload throughput decreased approximately 10.2% over the same worker-count increase, while p99 latency decreased.
3. Mixed workload throughput increased approximately 3.52 times, and p99 latency decreased approximately 74.3%.

These observations suggest that worker count and task characteristics affect throughput and latency differently.

---

## 6. Repository Structure

The repository contains the following project artifacts:

- `Source_Code/` — C++ source files and experiment scripts.
- `experiment_results/` — Benchmark CSV data, raw logs, and variability analysis.
- `Graphs/` — Throughput and latency visualizations.
- `Documentation/` — Experiment configuration and environment details.
- `README.md` — Project overview and usage documentation.

---

## 7. Limitations

This implementation is a controlled synthetic benchmark.

- It does not use real io_uring completion events.
- It does not implement or demonstrate callback poisoning.
- It does not compare multiple scheduling algorithms.
- It uses a fixed batch of 1,000 tasks per run.
- Results are based on five repetitions per configuration.
- Measurements were obtained in a virtualized ARM64 environment with four virtual CPUs.

The results should not be interpreted as production-system performance guarantees or as evidence of an io_uring vulnerability.

---

## 8. Future Work

Potential extensions include:

- Integrating real io_uring completion callbacks.
- Investigating workload-aware scheduling policies.
- Evaluating scheduling overhead under asynchronous I/O workloads.
- Expanding experiments across worker counts and workload distributions.
- Studying throughput and tail-latency trade-offs under more realistic workloads.

---

## 9. Research Motivation

The broader research motivation is to understand whether heterogeneous asynchronous workloads challenge the assumptions made by work-stealing schedulers.

HeteroSteal Lite provides a preliminary synthetic benchmark for studying this question and establishing a baseline for future investigation.
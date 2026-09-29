# `hs_ws_bench` — P0 synthetic work-stealing microbenchmark

Closed-batch comparison of **compute-only**, **callback-like-only**, and **mixed** task durations on a class-blind work-stealing pool. It does **not** use io_uring.

## What it does

- One `WorkStealingDeque` per worker. Only that worker calls `push_bottom` / `pop_bottom`.
- All `TaskDescriptor` slots are allocated once. Queues are filled on the main thread **before** workers start. Slots are not recycled until after `join`.
- Workers: local `pop_bottom`, else up to `--steal-attempts` random `steal_top`s, else spin/yield.
- **Compute** tasks run `--compute-iters` of a busy loop; **IoCallback** tasks run `--callback-iters`. There is no real I/O. `InlineTask` is not assignable, so the kernel is chosen from `task_class` in the worker.

## Timing and percentiles

Each task stores `enqueued_at` (main thread, prefill), then `started_at` / `completed_at` on the **single** worker that pops or steals that pointer. Workers never write another task’s timestamps. After join, the main thread reads those fields and computes latency as `completed_at - enqueued_at`.

Percentiles are **nearest-rank** on the sorted nanosecond samples: index `ceil(p/100 * n) - 1` for p50 / p90 / p99. They are not interpolated.

Do **not** collect timing from sanitizer builds (project spec §8.6).

## Reproducibility

`--seed` fixes:

- which tasks are `IoCallback` vs `Compute` in `--mode mixed` (shuffle of a mix-sized prefix);
- each worker’s sequence of **victim indices** when it attempts a steal.

It does **not** make steal *success*, steal counts, dispatch order, latency, or throughput identical across runs. Those depend on the OS scheduler, timers, and concurrent last-element races in the deque.

## Build

From the repository root (Release, no sanitizers):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hs_ws_bench
```

Existing targets (`hs_harness`, deque tests) are unchanged. Do not enable `HS_ENABLE_TSAN` or `HS_ENABLE_ASAN_UBSAN` for measured runs.

## Smoke test

```bash
./build/hs_ws_bench --workers 2 --tasks 1000 --mode compute --seed 1
```

Expect exit code 0, `tasks 1000`, and matching `count` under `all`.

## Compare the three modes

```bash
./build/hs_ws_bench --workers 4 --tasks 100000 --mode compute  --seed 1
./build/hs_ws_bench --workers 4 --tasks 100000 --mode callback --seed 1
./build/hs_ws_bench --workers 4 --tasks 100000 --mode mixed --mix 0.5 --seed 1
```

## Help

```bash
./build/hs_ws_bench --help
```

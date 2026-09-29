import subprocess
import csv
import re
import time
from pathlib import Path
from datetime import datetime

ROOT = Path.home() / "HeteroSteal_Lite"
BIN = ROOT / "build_lite" / "hs_ws_bench"
OUT = ROOT / "experiment_results"
OUT.mkdir(exist_ok=True)

modes = ["compute", "callback", "mixed"]
workers_list = [1, 2, 4]
repetitions = 5

fields = [
    "mode", "workers", "run",
    "tasks", "throughput_tasks_s",
    "latency_p50_ns", "latency_p90_ns", "latency_p99_ns",
    "stolen_tasks", "steal_count_sum", "checksum",
    "wall_s"
]

rows = []

if not BIN.exists():
    raise SystemExit(f"Benchmark not found: {BIN}")

print("HeteroSteal Lite experiment runner")
print("45 runs: 3 modes x 3 worker counts x 5 repetitions")
print(f"Results directory: {OUT}")
print()

start = time.time()

for mode in modes:
    for workers in workers_list:
        for run in range(1, repetitions + 1):
            cmd = [
                str(BIN),
                "--mode", mode,
                "--workers", str(workers),
                "--tasks", "1000",
                "--seed", "1"
            ]

            print(f"[{len(rows)+1}/45] {mode}, workers={workers}, run={run}",
                  flush=True)

            result = subprocess.run(
                cmd, capture_output=True, text=True
            )

            raw = result.stdout + result.stderr
            log = OUT / f"{mode}_w{workers}_run{run}.txt"
            log.write_text(raw)

            if result.returncode != 0:
                raise SystemExit(
                    f"Benchmark failed. See {log}\n{raw}"
                )

            def get(pattern, cast=float):
                match = re.search(pattern, raw)
                if not match:
                    raise ValueError(
                        f"Could not find {pattern} in {log}"
                    )
                return cast(match.group(1))

            row = {
                "mode": mode,
                "workers": workers,
                "run": run,
                "tasks": get(r"all:\s+count\s+(\d+)", int),
                "throughput_tasks_s": get(
                    r"all:\s+count\s+\d+\s+throughput_tasks_s\s+([\d.]+)"
                ),
                "latency_p50_ns": get(r"latency_p50_ns\s+(\d+)", int),
                "latency_p90_ns": get(r"latency_p90_ns\s+(\d+)", int),
                "latency_p99_ns": get(r"latency_p99_ns\s+(\d+)", int),
                "stolen_tasks": get(r"stolen_tasks\s+(\d+)", int),
                "steal_count_sum": get(r"steal_count_sum\s+(\d+)", int),
                "checksum": get(r"checksum\s+(\d+)", int),
                "wall_s": get(r"wall_s\s+([\d.]+)")
            }

            rows.append(row)

            if row["tasks"] != 1000:
                raise SystemExit(
                    f"Expected 1000 tasks, got {row['tasks']}. See {log}"
                )

            with (OUT / "results.csv").open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)

elapsed = time.time() - start

print()
print(f"All {len(rows)} runs completed in {elapsed:.1f} seconds.")
print(f"CSV: {OUT / 'results.csv'}")
print(f"Raw logs: {OUT}")

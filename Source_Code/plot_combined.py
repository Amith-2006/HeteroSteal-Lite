import csv
import statistics
import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from collections import defaultdict

data = defaultdict(list)

with open("experiment_results/results.csv", newline="") as f:
    for row in csv.DictReader(f):
        key = (row["mode"], int(row["workers"]))
        data[key].append(row)

workers = [1, 2, 4]
modes = ["compute", "callback", "mixed"]

# Graph 1: Combined throughput
plt.figure(figsize=(9, 6))

for mode in modes:
    means = []
    errors = []

    for w in workers:
        values = [
            float(r["throughput_tasks_s"])
            for r in data[mode, w]
        ]

        means.append(statistics.mean(values))
        errors.append(statistics.stdev(values))

    plt.errorbar(
        workers,
        means,
        yerr=errors,
        marker="o",
        capsize=5,
        label=mode.capitalize()
    )

plt.xlabel("Number of Workers")
plt.ylabel("Throughput (tasks/second)")
plt.title("Throughput vs Number of Workers")
plt.xticks(workers)
plt.yscale("log")
plt.grid(True, which="both", linestyle="--", alpha=0.5)
plt.legend()
plt.tight_layout()
plt.savefig("experiment_results/combined_throughput.png", dpi=300)
plt.close()

print("Saved combined_throughput.png")

# Graph 2: Combined p99 latency
plt.figure(figsize=(9, 6))

for mode in modes:
    means = []
    errors = []

    for w in workers:
        values = [
            float(r["latency_p99_ns"]) / 1_000_000
            for r in data[mode, w]
        ]

        means.append(statistics.mean(values))
        errors.append(statistics.stdev(values))

    plt.errorbar(
        workers,
        means,
        yerr=errors,
        marker="o",
        capsize=5,
        label=mode.capitalize()
    )

plt.xlabel("Number of Workers")
plt.ylabel("p99 Latency (milliseconds)")
plt.title("p99 Latency vs Number of Workers")
plt.xticks(workers)
plt.yscale("log")
plt.grid(True, which="both", linestyle="--", alpha=0.5)
plt.legend()
plt.tight_layout()
plt.savefig("experiment_results/combined_latency.png", dpi=300)
plt.close()

print("Saved combined_latency.png")

import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from collections import defaultdict

csv_file = "experiment_results/results.csv"
data = defaultdict(list)

with open(csv_file, newline="") as f:
    reader = csv.DictReader(f)

    for row in reader:
        mode = row["mode"]
        workers = int(row["workers"])

        # Convert nanoseconds to milliseconds
        p99_ms = float(row["latency_p99_ns"]) / 1_000_000

        data[mode, workers].append(p99_ms)

workers = [1, 2, 4]

for mode in ["compute", "callback", "mixed"]:
    averages = [
        sum(data[mode, w]) / len(data[mode, w])
        for w in workers
    ]

    plt.figure(figsize=(8, 5))
    plt.plot(workers, averages, marker="o")

    plt.xlabel("Number of Workers")
    plt.ylabel("p99 Latency (milliseconds)")
    plt.title(f"{mode.capitalize()} Workload p99 Latency")

    plt.xticks(workers)
    plt.grid(True)
    plt.tight_layout()

    filename = f"experiment_results/latency_{mode}.png"
    plt.savefig(filename, dpi=300)
    plt.close()

    print(f"Saved: {filename}")

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
        throughput = float(row["throughput_tasks_s"])
        data[mode, workers].append(throughput)

workers = [1, 2, 4]

for mode in ["compute", "callback", "mixed"]:
    averages = [
        sum(data[mode, w]) / len(data[mode, w])
        for w in workers
    ]

    plt.figure(figsize=(8, 5))
    plt.plot(workers, averages, marker="o")
    plt.xlabel("Number of Workers")
    plt.ylabel("Throughput (tasks/second)")
    plt.title(f"{mode.capitalize()} Workload Throughput")
    plt.xticks(workers)
    plt.grid(True)
    plt.tight_layout()

    filename = f"experiment_results/throughput_{mode}.png"
    plt.savefig(filename, dpi=300)
    plt.close()

    print(f"Saved: {filename}")

#!/usr/bin/env python3
import argparse
import csv
import statistics
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.pyplot as plt


KEY_METRICS = [
    "ops_per_sec",
    "micros_per_op",
    "seek_p50_us",
    "seek_p95_us",
    "seek_p99_us",
    "cache_hit_ratio_pct",
    "l0_files_end",
    "cumulative_writes_count",
]


def to_float(v: str):
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def load_rows(metrics_csv: Path):
    rows = []
    with metrics_csv.open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r.get("scenario") in ("fillrandom", "final_stats"):
                continue
            rows.append(r)
    return rows


def group_rows(rows: List[Dict[str, str]]):
    grouped: Dict[Tuple[str, str], List[Dict[str, str]]] = {}
    for r in rows:
        k = (r.get("scenario", ""), r.get("cache_gib", ""))
        grouped.setdefault(k, []).append(r)
    return grouped


def aggregate(grouped):
    out = []
    for (scenario, cache_gib), rows in sorted(grouped.items()):
        row = {
            "scenario": scenario,
            "cache_gib": cache_gib,
            "samples": len(rows),
        }
        for m in KEY_METRICS:
            vals = [to_float(r.get(m, "")) for r in rows]
            vals = [v for v in vals if v is not None]
            if vals:
                row[f"{m}_median"] = statistics.median(vals)
                row[f"{m}_min"] = min(vals)
                row[f"{m}_max"] = max(vals)
        out.append(row)
    return out


def export_csv(rows, out_csv: Path):
    fields = sorted({k for r in rows for k in r.keys()})
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def plot_mixgraph(rows, out_dir: Path):
    mix = [r for r in rows if r.get("scenario") == "mixgraph"]
    if not mix:
        return
    mix.sort(key=lambda r: float(r.get("cache_gib", 0)))
    x = [float(r["cache_gib"]) for r in mix]
    ops = [float(r.get("ops_per_sec_median", 0)) for r in mix]
    p99 = [float(r.get("seek_p99_us_median", 0)) for r in mix]

    plt.figure(figsize=(8, 4.8))
    plt.plot(x, ops, marker="o", linewidth=2)
    plt.xlabel("Cache size (GiB)")
    plt.ylabel("Median ops/sec")
    plt.title("Mixgraph Median Throughput Across Runs")
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_dir / "mixgraph_median_ops_vs_cache.png", dpi=160)
    plt.close()

    plt.figure(figsize=(8, 4.8))
    plt.plot(x, p99, marker="o", linewidth=2)
    plt.xlabel("Cache size (GiB)")
    plt.ylabel("Median seek P99 (us)")
    plt.title("Mixgraph Median Seek P99 Across Runs")
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_dir / "mixgraph_median_seek_p99_vs_cache.png", dpi=160)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description="Aggregate multiple shortscan runs.")
    parser.add_argument(
        "--run-dir",
        action="append",
        required=True,
        help="Run dir path (repeatable), e.g. benchmark_runs/20260204_205844",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Output dir for aggregate csv and plots",
    )
    args = parser.parse_args()

    all_rows = []
    for run_dir_str in args.run_dir:
        run_dir = Path(run_dir_str).resolve()
        metrics_csv = run_dir / "figures" / "metrics_table.csv"
        if not metrics_csv.exists():
            raise FileNotFoundError(f"missing metrics table: {metrics_csv}")
        rows = load_rows(metrics_csv)
        for r in rows:
            r["run_dir"] = str(run_dir)
        all_rows.extend(rows)

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    grouped = group_rows(all_rows)
    agg = aggregate(grouped)
    export_csv(agg, out_dir / "aggregate_metrics.csv")
    plot_mixgraph(agg, out_dir)
    print(f"Aggregate outputs written to: {out_dir}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Dict, List

import matplotlib.pyplot as plt


WORST_SCENARIOS = [
    "worst_seek1",
    "worst_seek4",
    "worst_seek20",
    "worst_seek200",
    "worst_seek10000",
]


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(rows: List[Dict[str, object]], path: Path) -> None:
    if not rows:
        return
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def plot_metric(rows: List[Dict[str, object]], metric: str, ylabel: str, out_png: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))
    for scenario in WORST_SCENARIOS:
        pts = sorted(
            [r for r in rows if r["scenario"] == scenario],
            key=lambda r: float(r["cache_gib"]),
        )
        if not pts:
            continue
        x = [float(r["cache_gib"]) for r in pts]
        y = [float(r[metric]) for r in pts]
        ax.plot(x, y, marker="o", linewidth=2.0, label=scenario)
    ax.set_xlabel("Block cache size (GiB)")
    ax.set_ylabel(ylabel)
    ax.grid(alpha=0.25)
    ax.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description="Summarize worst-seek cache sensitivity.")
    ap.add_argument("--matrix-dir", required=True)
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    analysis_dir = matrix_dir / "analysis"
    metrics_csv = analysis_dir / "matrix_factor_metrics.csv"
    if not metrics_csv.exists():
        raise FileNotFoundError(f"missing {metrics_csv}; run plot_matrix_results.py first")

    rows = read_csv(metrics_csv)
    out_rows: List[Dict[str, object]] = []
    for r in rows:
        scenario = r.get("scenario", "")
        if scenario not in WORST_SCENARIOS:
            continue
        out_rows.append(
            {
                "label": r.get("label", ""),
                "cache_gib": float(r.get("cache_gib_case", "nan")),
                "scenario": scenario,
                "ops_per_sec": float(r.get("ops_per_sec", "nan")),
                "seek_p99_us": float(r.get("seek_p99_us", "nan")),
                "cache_hit_ratio_pct": float(r.get("cache_hit_ratio_pct", "nan")),
            }
        )

    out_rows.sort(key=lambda x: (x["scenario"], x["cache_gib"]))
    write_csv(out_rows, analysis_dir / "worstseek_cache_summary.csv")
    plot_metric(out_rows, "cache_hit_ratio_pct", "Cache hit ratio (%)", analysis_dir / "worstseek_hit_ratio_vs_cache.png")
    plot_metric(out_rows, "ops_per_sec", "Throughput (ops/sec)", analysis_dir / "worstseek_ops_vs_cache.png")
    plot_metric(out_rows, "seek_p99_us", "Seek p99 (us)", analysis_dir / "worstseek_p99_vs_cache.png")
    print(f"worst-seek summary generated in: {analysis_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.pyplot as plt


SCENARIOS = [
    "mixgraph",
    "seek200",
    "worst_seek1",
    "worst_seek4",
    "worst_seek20",
    "worst_seek200",
    "worst_seek10000",
]


def to_float(value: str) -> float:
    if value is None or value == "":
        return float("nan")
    try:
        return float(value)
    except ValueError:
        return float("nan")


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def load_points(matrix_dirs: List[Path]) -> List[Dict[str, object]]:
    points: List[Dict[str, object]] = []
    for matrix_dir in matrix_dirs:
        registry = matrix_dir / "run_registry.csv"
        if not registry.exists():
            continue
        for case in read_csv(registry):
            if (case.get("status") or "").strip().lower() not in {"done", "skipped"}:
                continue
            exp_dir = Path(case["experiment_dir"])
            metrics = exp_dir / "figures" / "metrics_table.csv"
            if not metrics.exists():
                continue
            for row in read_csv(metrics):
                scenario = row.get("scenario", "")
                if scenario not in SCENARIOS:
                    continue
                points.append(
                    {
                        "matrix_dir": str(matrix_dir),
                        "experiment_id": case.get("experiment_id", case.get("exp_id", "")),
                        "case_id": case.get("case_id", case.get("seq", "")),
                        "label": case.get("label", ""),
                        "scenario": scenario,
                        "cache_gib": to_float(case.get("cache_gib", "")),
                        "ops_per_sec": to_float(row.get("ops_per_sec", "")),
                        "seek_p99_us": to_float(row.get("seek_p99_us", "")),
                    }
                )
    return points


def summarize(points: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, float], List[Dict[str, object]]] = defaultdict(list)
    for p in points:
        grouped[(str(p["scenario"]), float(p["cache_gib"]))].append(p)

    out: List[Dict[str, object]] = []
    for scenario in SCENARIOS:
        caches = sorted({k[1] for k in grouped.keys() if k[0] == scenario})
        for cache_gib in caches:
            vals = grouped[(scenario, cache_gib)]
            ops = [float(v["ops_per_sec"]) for v in vals if not math.isnan(float(v["ops_per_sec"]))]
            p99 = [float(v["seek_p99_us"]) for v in vals if not math.isnan(float(v["seek_p99_us"]))]
            if not ops or not p99:
                continue
            out.append(
                {
                    "scenario": scenario,
                    "cache_gib": cache_gib,
                    "n": len(vals),
                    "ops_mean": statistics.mean(ops),
                    "ops_median": statistics.median(ops),
                    "ops_std": statistics.stdev(ops) if len(ops) > 1 else 0.0,
                    "p99_mean": statistics.mean(p99),
                    "p99_median": statistics.median(p99),
                    "p99_std": statistics.stdev(p99) if len(p99) > 1 else 0.0,
                }
            )
    return out


def write_csv(rows: List[Dict[str, object]], path: Path) -> None:
    if not rows:
        return
    fields = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def plot_mixgraph(summary: List[Dict[str, object]], out_dir: Path) -> None:
    rows = sorted(
        [r for r in summary if r["scenario"] == "mixgraph"],
        key=lambda r: float(r["cache_gib"]),
    )
    if not rows:
        return

    x = [float(r["cache_gib"]) for r in rows]
    ops = [float(r["ops_median"]) for r in rows]
    ops_err = [float(r["ops_std"]) for r in rows]
    p99 = [float(r["p99_median"]) for r in rows]
    p99_err = [float(r["p99_std"]) for r in rows]

    fig, ax1 = plt.subplots(figsize=(9.5, 5.2))
    ax1.errorbar(x, ops, yerr=ops_err, marker="o", linewidth=2.2, color="#1f77b4", capsize=4, label="ops/sec (median±std)")
    ax1.set_xlabel("Block cache size (GiB)")
    ax1.set_ylabel("Throughput (ops/sec)", color="#1f77b4")
    ax1.tick_params(axis="y", labelcolor="#1f77b4")
    ax1.grid(alpha=0.25)

    ax2 = ax1.twinx()
    ax2.errorbar(x, p99, yerr=p99_err, marker="s", linewidth=2.2, color="#d62728", capsize=4, label="seek p99 (median±std)")
    ax2.set_ylabel("Seek p99 (us)", color="#d62728")
    ax2.tick_params(axis="y", labelcolor="#d62728")

    plt.title("Cache Sweep Repeats: Mixgraph Stability")
    plt.tight_layout()
    plt.savefig(out_dir / "cache_repeat_mixgraph_errorbar.png", dpi=180)
    plt.close(fig)


def plot_scenario_lines(summary: List[Dict[str, object]], out_dir: Path, key: str, out_name: str, y_label: str) -> None:
    fig, ax = plt.subplots(figsize=(10.0, 6.0))
    for scenario in SCENARIOS:
        rows = sorted([r for r in summary if r["scenario"] == scenario], key=lambda r: float(r["cache_gib"]))
        if not rows:
            continue
        x = [float(r["cache_gib"]) for r in rows]
        y = [float(r[key]) for r in rows]
        ax.plot(x, y, marker="o", linewidth=1.8, label=scenario)

    ax.set_xlabel("Block cache size (GiB)")
    ax.set_ylabel(y_label)
    ax.grid(alpha=0.25)
    ax.legend(ncol=2, fontsize=8)
    plt.tight_layout()
    plt.savefig(out_dir / out_name, dpi=180)
    plt.close(fig)


def write_report(summary: List[Dict[str, object]], out_md: Path, matrix_dirs: List[Path]) -> None:
    mix = sorted([r for r in summary if r["scenario"] == "mixgraph"], key=lambda r: float(r["cache_gib"]))
    lines: List[str] = []
    lines.append("# Cache Sweep Repeat Report")
    lines.append("")
    lines.append(f"- repeats: `{len(matrix_dirs)}`")
    lines.append(f"- matrix_dirs: `{', '.join(str(p.name) for p in matrix_dirs)}`")
    if mix:
        base = mix[0]
        high = mix[-1]
        ops_delta = (float(high["ops_median"]) - float(base["ops_median"])) / float(base["ops_median"]) * 100.0
        p99_delta = (float(high["p99_median"]) - float(base["p99_median"])) / float(base["p99_median"]) * 100.0
        lines.append("")
        lines.append("## Mixgraph (median across repeats)")
        lines.append(
            f"- cache {base['cache_gib']:.0f}GiB -> {high['cache_gib']:.0f}GiB: "
            f"throughput `{ops_delta:.2f}%`, seek p99 `{p99_delta:.2f}%`"
        )
    lines.append("")
    lines.append("## Artifacts")
    lines.append("- `cache_repeat_summary.csv`")
    lines.append("- `cache_repeat_mixgraph_errorbar.png`")
    lines.append("- `cache_repeat_ops_by_scenario.png`")
    lines.append("- `cache_repeat_p99_by_scenario.png`")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Aggregate repeated cache-sweep matrix runs with error bars.")
    ap.add_argument("--matrix-dir", action="append", required=True, help="matrix dir path (repeatable)")
    ap.add_argument("--out-dir", required=True, help="output dir")
    args = ap.parse_args()

    matrix_dirs = [Path(p).resolve() for p in args.matrix_dir]
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    points = load_points(matrix_dirs)
    if not points:
        raise RuntimeError("no points loaded from matrix dirs")

    write_csv(points, out_dir / "cache_repeat_points.csv")
    summary = summarize(points)
    write_csv(summary, out_dir / "cache_repeat_summary.csv")
    plot_mixgraph(summary, out_dir)
    plot_scenario_lines(summary, out_dir, "ops_median", "cache_repeat_ops_by_scenario.png", "Throughput (ops/sec, median)")
    plot_scenario_lines(summary, out_dir, "p99_median", "cache_repeat_p99_by_scenario.png", "Seek p99 (us, median)")
    write_report(summary, out_dir / "report.md", matrix_dirs)
    print(f"generated repeat analysis: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

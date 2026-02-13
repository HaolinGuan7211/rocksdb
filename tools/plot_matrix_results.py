#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter


SCENARIO_ORDER = [
    "mixgraph",
    "seek200",
    "worst_seek1",
    "worst_seek4",
    "worst_seek20",
    "worst_seek200",
    "worst_seek10000",
]

PHASE_ORDER = ["cache_sweep", "thread_sweep", "locality_sweep"]
PHASE_DISPLAY = {
    "cache_sweep": "A Phase: Cache Sweep (mixgraph)",
    "thread_sweep": "B Phase: Thread Sweep (mixgraph)",
    "locality_sweep": "C Phase: Locality / Read-mode (mixgraph)",
}


def to_int(value: str) -> int:
    if value is None or value == "":
        return 0
    try:
        return int(float(value))
    except ValueError:
        return 0


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


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def load_registry(matrix_dir: Path) -> List[Dict[str, str]]:
    registry = matrix_dir / "run_registry.csv"
    if not registry.exists():
        raise FileNotFoundError(f"missing run_registry.csv: {registry}")
    rows = read_csv(registry)
    rows.sort(key=lambda r: to_int(r.get("seq", "")))
    return rows


def merge_rows(matrix_rows: List[Dict[str, str]]) -> List[Dict[str, object]]:
    merged: List[Dict[str, object]] = []
    for case in matrix_rows:
        exp_dir = Path(case["experiment_dir"])
        metrics = exp_dir / "figures" / "metrics_table.csv"
        if not metrics.exists():
            continue
        for metric in read_csv(metrics):
            scenario = metric.get("scenario", "")
            if scenario not in SCENARIO_ORDER:
                continue

            num_keys = to_int(metric.get("cfg_num_keys", "0"))
            key_size = to_int(metric.get("cfg_key_size", "0"))
            value_size = to_int(metric.get("cfg_value_size", "0"))
            raw_bytes = num_keys * (key_size + value_size)
            raw_gib = raw_bytes / (1024**3) if raw_bytes > 0 else float("nan")
            cache_gib = to_float(metric.get("cache_gib", ""))
            cache_ratio = (cache_gib / raw_gib * 100.0) if raw_gib and raw_gib > 0 else float("nan")

            merged.append(
                {
                    "seq": to_int(case.get("seq", "")),
                    "experiment_id": to_int(case.get("experiment_id", case.get("exp_id", ""))),
                    "case_id": case.get("case_id", f"case{to_int(case.get('seq', '0'))}"),
                    "label": case.get("label", ""),
                    "phase": case.get("phase", ""),
                    "mode": case.get("mode", ""),
                    "status": case.get("status", ""),
                    "experiment_name": case.get("experiment_name", ""),
                    "experiment_dir": case.get("experiment_dir", ""),
                    "cache_gib_case": to_float(case.get("cache_gib", "")),
                    "threads_case": to_int(case.get("threads", "")),
                    "mix_get_case": to_float(case.get("mix_get", "")),
                    "mix_put_case": to_float(case.get("mix_put", "")),
                    "mix_seek_case": to_float(case.get("mix_seek", "")),
                    "scenario": scenario,
                    "ops_per_sec": to_float(metric.get("ops_per_sec", "")),
                    "micros_per_op": to_float(metric.get("micros_per_op", "")),
                    "seek_p95_us": to_float(metric.get("seek_p95_us", "")),
                    "seek_p99_us": to_float(metric.get("seek_p99_us", "")),
                    "cache_hit_ratio_pct": to_float(metric.get("cache_hit_ratio_pct", "")),
                    "cfg_num_keys": num_keys,
                    "cfg_key_size": key_size,
                    "cfg_value_size": value_size,
                    "cfg_compression_type": metric.get("cfg_compression_type", ""),
                    "cfg_use_direct": metric.get("cfg_use_direct", ""),
                    "dataset_raw_bytes": raw_bytes,
                    "dataset_raw_gib": raw_gib,
                    "cache_to_dataset_pct": cache_ratio,
                }
            )
    merged.sort(key=lambda r: (int(r["seq"]), SCENARIO_ORDER.index(str(r["scenario"]))))
    return merged


def write_csv(rows: List[Dict[str, object]], out_csv: Path) -> None:
    if not rows:
        return
    keys = list(rows[0].keys())
    with out_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def select_mixgraph(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    return [r for r in rows if r["scenario"] == "mixgraph"]


def format_gib(x: float) -> str:
    if math.isnan(x):
        return "NA"
    return f"{x:.2f}"


def safe_pct(delta_new: float, delta_old: float) -> float:
    if delta_old == 0:
        return float("nan")
    return (delta_new - delta_old) / delta_old * 100.0


def heatmap_values(
    rows: List[Dict[str, object]], labels: List[str], value_key: str
) -> np.ndarray:
    matrix = np.full((len(labels), len(SCENARIO_ORDER)), np.nan)
    lookup: Dict[Tuple[str, str], Dict[str, object]] = {
        (str(r["label"]), str(r["scenario"])): r for r in rows
    }
    for i, label in enumerate(labels):
        for j, scenario in enumerate(SCENARIO_ORDER):
            row = lookup.get((label, scenario))
            if row is not None:
                matrix[i, j] = float(row[value_key]) if row[value_key] is not None else np.nan
    return matrix


def draw_heatmap(
    ax: plt.Axes,
    matrix: np.ndarray,
    row_labels: List[str],
    col_labels: List[str],
    title: str,
    unit: str,
) -> None:
    display = np.log10(matrix)
    im = ax.imshow(display, aspect="auto", cmap="Blues")
    ax.set_title(title, fontsize=11)
    ax.set_xticks(np.arange(len(col_labels)))
    ax.set_xticklabels(col_labels, rotation=35, ha="right", fontsize=8)
    ax.set_yticks(np.arange(len(row_labels)))
    ax.set_yticklabels(row_labels, fontsize=9)
    for i in range(matrix.shape[0]):
        for j in range(matrix.shape[1]):
            val = matrix[i, j]
            text = "NA" if np.isnan(val) else f"{val:.2f}"
            if unit == "ops":
                if np.isnan(val):
                    text = "NA"
                elif val >= 1e6:
                    text = f"{val/1e6:.2f}M"
                elif val >= 1e3:
                    text = f"{val/1e3:.1f}K"
                else:
                    text = f"{val:.0f}"
            disp = display[i, j]
            if not np.isfinite(disp):
                color = "black"
            else:
                r, g, b, _ = im.cmap(im.norm(disp))
                luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
                color = "white" if luminance < 0.5 else "black"
            ax.text(j, i, text, ha="center", va="center", fontsize=7, color=color)
    cbar = plt.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("log10(ops/sec)" if unit == "ops" else f"log10({unit})", fontsize=8)


def plot_dashboard(rows: List[Dict[str, object]], out_png: Path) -> None:
    mix_rows = select_mixgraph(rows)
    if not mix_rows:
        return

    labels = [str(r["label"]) for r in sorted(mix_rows, key=lambda x: int(x["seq"]))]
    first = mix_rows[0]
    phase_set = {str(r["phase"]) for r in mix_rows}
    phases = [p for p in PHASE_ORDER if p in phase_set]
    if not phases:
        phases = sorted(phase_set)

    panel_types: List[str] = ["context"]
    panel_types.extend([f"phase:{p}" for p in phases])
    panel_types.extend(["heatmap_p99", "heatmap_ops"])
    n_panels = len(panel_types)
    n_cols = 2 if n_panels <= 4 else 3
    n_rows = math.ceil(n_panels / n_cols)
    fig, axes = plt.subplots(n_rows, n_cols, figsize=(7.5 * n_cols, 4.8 * n_rows))
    axes_arr = np.array(axes).reshape(-1)

    for i, panel in enumerate(panel_types):
        ax = axes_arr[i]
        if panel == "context":
            ax.axis("off")
            key_size = int(first["cfg_key_size"])
            value_size = int(first["cfg_value_size"])
            raw_gib = float(first["dataset_raw_gib"])
            variable_map = {
                "cache_sweep": "cache_gib",
                "thread_sweep": "threads",
                "locality_sweep": "mix_ratio(locality/read mode)",
            }
            vars_text = ", ".join(variable_map.get(p, p) for p in phases)
            text = [
                "Matrix Context",
                f"- Cases: {len(mix_rows)}",
                f"- Phase scope: {', '.join(phases)}",
                f"- Data scale: {int(first['cfg_num_keys']):,} keys",
                f"- Raw payload: {raw_gib:.2f} GiB",
                f"- Key/Value size: {key_size}B / {value_size}B (avg KV={key_size + value_size}B)",
                f"- Compression: {first['cfg_compression_type']}",
                f"- Direct IO: {first['cfg_use_direct']}",
                f"- Variables: {vars_text}",
            ]
            ax.text(0.02, 0.98, "\n".join(text), va="top", ha="left", fontsize=11.5)
            continue

        if panel.startswith("phase:"):
            phase = panel.split(":", 1)[1]
            pr = [r for r in mix_rows if str(r["phase"]) == phase]
            if not pr:
                ax.axis("off")
                continue
            if phase == "cache_sweep":
                a_rows = sorted(pr, key=lambda r: float(r["cache_gib_case"]))
                x = [float(r["cache_gib_case"]) for r in a_rows]
                y_ops = [float(r["ops_per_sec"]) for r in a_rows]
                y_p99 = [float(r["seek_p99_us"]) for r in a_rows]
                ax.plot(x, y_ops, marker="o", linewidth=2.0, color="#1f77b4", label="ops/sec")
                ax.set_xlabel("Block cache (GiB)")
                ax.set_ylabel("ops/sec", color="#1f77b4")
                ax.tick_params(axis="y", labelcolor="#1f77b4")
                ax.grid(alpha=0.25)
                ax2 = ax.twinx()
                ax2.plot(x, y_p99, marker="s", linewidth=2.0, color="#d62728", label="seek p99 us")
                ax2.set_ylabel("seek p99 (us)", color="#d62728")
                ax2.tick_params(axis="y", labelcolor="#d62728")
            elif phase == "thread_sweep":
                b_rows = sorted(pr, key=lambda r: int(r["threads_case"]))
                x = [int(r["threads_case"]) for r in b_rows]
                y_ops = [float(r["ops_per_sec"]) for r in b_rows]
                y_p99 = [float(r["seek_p99_us"]) for r in b_rows]
                ax.plot(x, y_ops, marker="o", linewidth=2.0, color="#1f77b4")
                ax.set_xlabel("Threads")
                ax.set_ylabel("ops/sec", color="#1f77b4")
                ax.tick_params(axis="y", labelcolor="#1f77b4")
                ax.grid(alpha=0.25)
                ax2 = ax.twinx()
                ax2.plot(x, y_p99, marker="s", linewidth=2.0, color="#d62728")
                ax2.set_ylabel("seek p99 (us)", color="#d62728")
                ax2.tick_params(axis="y", labelcolor="#d62728")
            elif phase == "locality_sweep":
                mode_order = ["readheavy", "balanced", "seekheavy"]
                c_map = {str(r["mode"]): r for r in pr}
                c_rows = [c_map[m] for m in mode_order if m in c_map]
                x = np.arange(len(c_rows))
                y_ops = [float(r["ops_per_sec"]) / 1e6 for r in c_rows]
                y_p99 = [float(r["seek_p99_us"]) for r in c_rows]
                bars = ax.bar(x, y_ops, width=0.55, color="#4c78a8")
                ax.set_xticks(x)
                ax.set_xticklabels([str(r["mode"]) for r in c_rows])
                ax.set_ylabel("ops/sec (million)")
                ax.grid(axis="y", alpha=0.25)
                ax2 = ax.twinx()
                ax2.plot(x, y_p99, color="#e45756", marker="o", linewidth=2.0)
                ax2.set_ylabel("seek p99 (us)")
                for idx, bar in enumerate(bars):
                    ax.text(
                        bar.get_x() + bar.get_width() / 2,
                        bar.get_height(),
                        f"{y_ops[idx]:.2f}",
                        ha="center",
                        va="bottom",
                        fontsize=8,
                    )
            ax.set_title(PHASE_DISPLAY.get(phase, f"{phase} (mixgraph)"))
            continue

        if panel == "heatmap_p99":
            p99_matrix = heatmap_values(rows, labels, "seek_p99_us")
            draw_heatmap(ax, p99_matrix, labels, SCENARIO_ORDER, "Scenario x Case: Seek P99", "us")
            continue

        if panel == "heatmap_ops":
            ops_matrix = heatmap_values(rows, labels, "ops_per_sec")
            draw_heatmap(ax, ops_matrix, labels, SCENARIO_ORDER, "Scenario x Case: Throughput", "ops")
            continue

    for i in range(n_panels, len(axes_arr)):
        axes_arr[i].axis("off")

    fig.suptitle(
        f"RocksDB 50GB Matrix Results Dashboard (phase scope: {', '.join(phases)})",
        fontsize=15,
        y=0.995,
    )
    fig.tight_layout()
    fig.savefig(out_png, dpi=180)
    plt.close(fig)


def plot_cache_hit_ratio(rows: List[Dict[str, object]], out_png: Path) -> None:
    sweep_rows = [r for r in rows if r["phase"] == "cache_sweep"]
    if not sweep_rows:
        return

    fig, (ax_hit, ax_miss) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    all_hit_vals: List[float] = []
    for scenario in SCENARIO_ORDER:
        points = sorted(
            [r for r in sweep_rows if r["scenario"] == scenario],
            key=lambda r: float(r["cache_gib_case"]),
        )
        if not points:
            continue
        x = [float(r["cache_gib_case"]) for r in points]
        y_hit = [float(r["cache_hit_ratio_pct"]) for r in points]
        # Convert near-100% hit ratio to a readable unit:
        # miss per million accesses (ppm).
        y_miss_ppm = [max(0.0, (100.0 - v) * 10000.0) for v in y_hit]
        all_hit_vals.extend(y_hit)
        ax_hit.plot(x, y_hit, marker="o", linewidth=1.8, label=scenario)
        ax_miss.plot(x, y_miss_ppm, marker="o", linewidth=1.8, label=scenario)

    ax_hit.set_title("Cache Hit Ratio / Miss PPM by Cache Size (Cache Sweep)")
    ax_hit.set_ylabel("Cache hit ratio (%)")
    ax_hit.grid(alpha=0.25)
    if all_hit_vals:
        ymin = min(all_hit_vals)
        ymax = max(all_hit_vals)
        pad = max((ymax - ymin) * 0.2, 0.00001)
        ax_hit.set_ylim(ymin - pad, ymax + pad)
    ax_hit.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v:.6f}"))
    ax_hit.legend(ncol=2, fontsize=8)

    ax_miss.set_xlabel("Block cache size (GiB)")
    ax_miss.set_ylabel("Cache miss (ppm)")
    ax_miss.grid(alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)


def build_case_summary(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    mix_rows = select_mixgraph(rows)
    out: List[Dict[str, object]] = []
    for r in sorted(mix_rows, key=lambda x: int(x["seq"])):
        out.append(
            {
                "seq": r["seq"],
                "experiment_id": r["experiment_id"],
                "case_id": r["case_id"],
                "label": r["label"],
                "phase": r["phase"],
                "mode": r["mode"],
                "cache_gib": r["cache_gib_case"],
                "threads": r["threads_case"],
                "mix_get": r["mix_get_case"],
                "mix_seek": r["mix_seek_case"],
                "dataset_raw_gib": format_gib(float(r["dataset_raw_gib"])),
                "cache_to_dataset_pct": f"{float(r['cache_to_dataset_pct']):.2f}",
                "mixgraph_ops_per_sec": int(float(r["ops_per_sec"])),
                "mixgraph_seek_p99_us": f"{float(r['seek_p99_us']):.3f}",
                "mixgraph_cache_hit_ratio_pct": f"{float(r['cache_hit_ratio_pct']):.6f}",
            }
        )
    return out


def best_by_metric(rows: List[Dict[str, object]], scenario: str, key: str, better: str) -> Tuple[str, float]:
    subset = [r for r in rows if r["scenario"] == scenario and not math.isnan(float(r[key]))]
    if not subset:
        return ("NA", float("nan"))
    if better == "max":
        row = max(subset, key=lambda r: float(r[key]))
    else:
        row = min(subset, key=lambda r: float(r[key]))
    return (str(row["label"]), float(row[key]))


def write_report(matrix_dir: Path, rows: List[Dict[str, object]], out_md: Path) -> None:
    mix_rows = select_mixgraph(rows)
    if not mix_rows:
        return

    by_label = {str(r["label"]): r for r in mix_rows}
    a1, a3 = by_label.get("A1"), by_label.get("A3")
    b1, b3 = by_label.get("B1"), by_label.get("B3")
    c1, c2, c3 = by_label.get("C1"), by_label.get("C2"), by_label.get("C3")
    phases = sorted({str(r["phase"]) for r in mix_rows})

    cache_ops_delta = safe_pct(float(a3["ops_per_sec"]), float(a1["ops_per_sec"])) if a1 and a3 else float("nan")
    cache_p99_delta = safe_pct(float(a3["seek_p99_us"]), float(a1["seek_p99_us"])) if a1 and a3 else float("nan")
    thread_ops_delta = safe_pct(float(b3["ops_per_sec"]), float(b1["ops_per_sec"])) if b1 and b3 else float("nan")
    thread_p99_delta = safe_pct(float(b3["seek_p99_us"]), float(b1["seek_p99_us"])) if b1 and b3 else float("nan")

    lines: List[str] = []
    lines.append("# Matrix 结果报告（50GB，factor-aware）")
    lines.append("")
    lines.append(f"- matrix_dir: `{matrix_dir}`")
    lines.append(f"- case 数: `{len(mix_rows)}`")
    lines.append(f"- phase 集合: `{','.join(phases)}`")
    lines.append("")
    lines.append("## 固定上下文")
    any_row = mix_rows[0]
    lines.append(f"- num_keys: `{int(any_row['cfg_num_keys']):,}`")
    lines.append(f"- key/value 大小: `{int(any_row['cfg_key_size'])}B / {int(any_row['cfg_value_size'])}B`")
    lines.append(f"- raw payload: `{float(any_row['dataset_raw_gib']):.2f} GiB`")
    lines.append(f"- compression/direct: `{any_row['cfg_compression_type']}` / `{any_row['cfg_use_direct']}`")
    lines.append("")
    lines.append("## Mixgraph 趋势（核心场景）")
    if a1 and a3:
        lines.append(
            f"- cache sweep A1({float(a1['cache_gib_case']):.0f}GiB) -> A3({float(a3['cache_gib_case']):.0f}GiB): "
            f"throughput `{cache_ops_delta:.2f}%`, seek p99 `{cache_p99_delta:.2f}%`"
        )
        lines.append(
            f"- mixgraph cache hit ratio: `{float(a1['cache_hit_ratio_pct']):.6f}%` -> `{float(a3['cache_hit_ratio_pct']):.6f}%`"
        )
    if b1 and b3:
        lines.append(
            f"- thread sweep B1({int(b1['threads_case'])}t) -> B3({int(b3['threads_case'])}t): "
            f"throughput `{thread_ops_delta:.2f}%`, seek p99 `{thread_p99_delta:.2f}%`"
        )
    if c1 and c2 and c3:
        lines.append(
            f"- locality sweep (C1/C2/C3) throughput: `{int(float(c1['ops_per_sec']))}` / `{int(float(c2['ops_per_sec']))}` / `{int(float(c3['ops_per_sec']))}` ops/s"
        )
        lines.append(
            f"- locality sweep (C1/C2/C3) seek p99: `{float(c1['seek_p99_us']):.3f}` / `{float(c2['seek_p99_us']):.3f}` / `{float(c3['seek_p99_us']):.3f}` us"
        )
    lines.append("")
    lines.append("## 各 Scenario 最优 Case")
    lines.append("| scenario | best_throughput(label,value) | best_p99(label,value) |")
    lines.append("|---|---|---|")
    for scenario in SCENARIO_ORDER:
        best_ops_label, best_ops = best_by_metric(rows, scenario, "ops_per_sec", "max")
        best_p99_label, best_p99 = best_by_metric(rows, scenario, "seek_p99_us", "min")
        ops_txt = "NA" if math.isnan(best_ops) else f"{best_ops_label}, {best_ops:.0f} ops/s"
        p99_txt = "NA" if math.isnan(best_p99) else f"{best_p99_label}, {best_p99:.3f} us"
        lines.append(f"| {scenario} | {ops_txt} | {p99_txt} |")
    lines.append("")
    lines.append("## 产物")
    lines.append("- `analysis/matrix_factor_metrics.csv`")
    lines.append("- `analysis/matrix_mixgraph_summary.csv`")
    lines.append("- `analysis/matrix_dashboard.png`")
    lines.append("- `analysis/cache_hit_ratio_by_cache.png`")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate and plot matrix benchmark results.")
    parser.add_argument("--matrix-dir", required=True, help="matrix directory path")
    parser.add_argument("--out-dir", default="", help="output directory (default: <matrix-dir>/analysis)")
    args = parser.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    out_dir = Path(args.out_dir).resolve() if args.out_dir else (matrix_dir / "analysis")
    ensure_dir(out_dir)

    registry_rows = load_registry(matrix_dir)
    merged_rows = merge_rows(registry_rows)
    if not merged_rows:
        raise RuntimeError("no rows found from per-run metrics_table.csv")

    write_csv(merged_rows, out_dir / "matrix_factor_metrics.csv")
    write_csv(build_case_summary(merged_rows), out_dir / "matrix_mixgraph_summary.csv")
    plot_dashboard(merged_rows, out_dir / "matrix_dashboard.png")
    plot_cache_hit_ratio(merged_rows, out_dir / "cache_hit_ratio_by_cache.png")
    write_report(matrix_dir, merged_rows, out_dir / "report.md")

    print(f"Generated analysis in: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

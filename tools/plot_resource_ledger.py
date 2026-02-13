#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.pyplot as plt
import numpy as np


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
PHASE_LABEL = {
    "cache_sweep": "A Phase (cache size)",
    "thread_sweep": "B Phase (threads)",
    "locality_sweep": "C Phase (get/seek mix)",
}
PHASE_COLOR = {
    "cache_sweep": "#1f77b4",
    "thread_sweep": "#2ca02c",
    "locality_sweep": "#d62728",
}

LEGACY_FILES = [
    "perf_frontier_mixgraph.png",
    "resource_bottleneck_scatter.png",
]

MODULE_SPECS = [
    # For pressure scoring, larger proxy value means higher pressure.
    ("block_cache_miss", "cache_miss_per_kop", True, "block cache miss/kop", "/kop"),
    ("deserialize_bytes", "iter_bytes_per_op", True, "decode bytes/op", "B/op"),
    ("iterator_jump", "next_per_seek", True, "iterator next/seek", "ratio"),
    ("sync_switch", "sync_switch_per_kop", True, "sync switch/kop", "/kop"),
    ("cpu_wait", "cpu_wait_pct", True, "cpu wait", "%"),
    ("io_wait", "iostat_await_ms", True, "io await", "ms"),
    ("microarch_miss", "perf_cache_miss_pct", True, "cache miss", "%"),
]

MODULE_COLORS = {
    "block_cache_miss": "#1f77b4",
    "deserialize_bytes": "#ff7f0e",
    "iterator_jump": "#2ca02c",
    "sync_switch": "#9467bd",
    "cpu_wait": "#d62728",
    "io_wait": "#8c564b",
    "microarch_miss": "#7f7f7f",
}


def to_float(v: object) -> float:
    if v is None:
        return float("nan")
    s = str(v).strip().replace(",", "")
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def to_int(v: object) -> int:
    x = to_float(v)
    if math.isnan(x):
        return 0
    return int(x)


def fmt(v: float, nd: int = 3) -> str:
    if math.isnan(v):
        return "NA"
    return f"{v:.{nd}f}"


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def cleanup_legacy_files(out_dir: Path) -> None:
    for name in LEGACY_FILES:
        p = out_dir / name
        if p.exists():
            p.unlink()


def scenario_key(s: str) -> int:
    if s in SCENARIO_ORDER:
        return SCENARIO_ORDER.index(s)
    return 999


def normalize_scores(values: List[float], higher_is_better: bool) -> List[float]:
    valid = [v for v in values if not math.isnan(v)]
    if not valid:
        return [float("nan")] * len(values)
    lo = min(valid)
    hi = max(valid)
    if hi == lo:
        return [100.0 if not math.isnan(v) else float("nan") for v in values]

    out: List[float] = []
    for v in values:
        if math.isnan(v):
            out.append(float("nan"))
            continue
        if higher_is_better:
            out.append((v - lo) * 100.0 / (hi - lo))
        else:
            out.append((hi - v) * 100.0 / (hi - lo))
    return out


def phase_sort_key(phase: str, row: Dict[str, object]) -> Tuple[float, float, float]:
    if phase == "cache_sweep":
        x = to_float(row.get("cache_gib", float("nan")))
        return (x if not math.isnan(x) else 1e18, to_float(row.get("threads", 0)), to_float(row.get("seq", 0)))
    if phase == "thread_sweep":
        x = to_float(row.get("threads", 0))
        return (x if not math.isnan(x) else 1e18, to_float(row.get("cache_gib", 0)), to_float(row.get("seq", 0)))
    s = to_float(row.get("mix_seek", float("nan")))
    g = to_float(row.get("mix_get", float("nan")))
    return (
        s if not math.isnan(s) else 1e18,
        g if not math.isnan(g) else 1e18,
        to_float(row.get("seq", 0)),
    )


def phase_x_label(phase: str, row: Dict[str, object]) -> str:
    if phase == "cache_sweep":
        c = to_float(row.get("cache_gib", float("nan")))
        return f"{c:.0f}GiB" if not math.isnan(c) else str(row.get("label", "NA"))
    if phase == "thread_sweep":
        t = to_int(row.get("threads", 0))
        return f"{t}t"
    g = to_float(row.get("mix_get", float("nan")))
    s = to_float(row.get("mix_seek", float("nan")))
    if math.isnan(g) or math.isnan(s):
        return str(row.get("label", "NA"))
    return f"G{g:.2f}/S{s:.2f}"


def build_signal_table(rows: List[Dict[str, str]]) -> List[Dict[str, object]]:
    parsed: List[Dict[str, object]] = []
    for r in rows:
        ops = to_float(r.get("ops_per_sec", ""))
        operations = to_float(r.get("operations", ""))
        cache_bytes_read = to_float(r.get("cache_bytes_read", ""))
        cswch_s = to_float(r.get("cswch_s", ""))
        nvcswch_s = to_float(r.get("nvcswch_s", ""))

        io_read_mb_s = to_float(r.get("iostat_read_mb_s", ""))
        io_write_mb_s = to_float(r.get("iostat_write_mb_s", ""))
        if math.isnan(io_read_mb_s):
            io_read_mb_s = to_float(r.get("io_read_kb_s", "")) / 1024.0
        if math.isnan(io_write_mb_s):
            io_write_mb_s = to_float(r.get("io_write_kb_s", "")) / 1024.0

        cache_bytes_per_op = cache_bytes_read / operations if operations > 0 and not math.isnan(cache_bytes_read) else float("nan")
        cswch_per_kop = cswch_s * 1000.0 / ops if ops > 0 and not math.isnan(cswch_s) else float("nan")
        nvcswch_per_kop = nvcswch_s * 1000.0 / ops if ops > 0 and not math.isnan(nvcswch_s) else float("nan")
        sync_switch_per_kop = (
            (cswch_per_kop if not math.isnan(cswch_per_kop) else 0.0)
            + (nvcswch_per_kop if not math.isnan(nvcswch_per_kop) else 0.0)
        )

        parsed.append(
            {
                "seq": to_int(r.get("seq", "0")),
                "label": r.get("label", ""),
                "phase": r.get("phase", ""),
                "mode": r.get("mode", ""),
                "cache_gib": to_float(r.get("cache_gib", "")),
                "threads": to_int(r.get("threads", "0")),
                "mix_get": to_float(r.get("mix_get", "")),
                "mix_seek": to_float(r.get("mix_seek", "")),
                "scenario": r.get("scenario", ""),
                "ops_per_sec": ops,
                "seek_p99_us": to_float(r.get("seek_p99_us", "")),
                "cache_hit_ratio_pct": to_float(r.get("cache_hit_ratio_pct", "")),
                "cache_miss_per_kop": to_float(r.get("cache_miss_per_kop", "")),
                "iter_bytes_per_op": to_float(r.get("iter_bytes_per_op", "")),
                "next_per_seek": to_float(r.get("next_per_seek", "")),
                "frontend_cpu_ms_per_op": to_float(r.get("frontend_cpu_ms_per_op", "")),
                "cache_bytes_per_op": cache_bytes_per_op,
                "cpu_user_pct": to_float(r.get("cpu_user_pct", "")),
                "cpu_sys_pct": to_float(r.get("cpu_sys_pct", "")),
                "cpu_wait_pct": to_float(r.get("cpu_wait_pct", "")),
                "cswch_per_kop": cswch_per_kop,
                "nvcswch_per_kop": nvcswch_per_kop,
                "sync_switch_per_kop": sync_switch_per_kop,
                "io_read_mb_s": io_read_mb_s,
                "io_write_mb_s": io_write_mb_s,
                "iostat_util_pct": to_float(r.get("iostat_util_pct", "")),
                "iostat_await_ms": to_float(r.get("iostat_await_ms", "")),
                "perf_ipc": to_float(r.get("perf_ipc", "")),
                "perf_cache_miss_pct": to_float(r.get("perf_cache_miss_pct", "")),
                "perf_l1_hit_pct": to_float(r.get("perf_l1_hit_pct", "")),
                "perf_l2_hit_pct": to_float(r.get("perf_l2_hit_pct", "")),
                "perf_l3_hit_pct": to_float(r.get("perf_l3_hit_pct", "")),
            }
        )

    out: List[Dict[str, object]] = []
    by_scenario: Dict[str, List[Dict[str, object]]] = {}
    for row in parsed:
        by_scenario.setdefault(str(row["scenario"]), []).append(row)

    for scenario, group in by_scenario.items():
        ops_scores = normalize_scores([to_float(x["ops_per_sec"]) for x in group], True)
        p99_scores = normalize_scores([to_float(x["seek_p99_us"]) for x in group], False)
        miss_scores = normalize_scores([to_float(x["cache_miss_per_kop"]) for x in group], False)
        iter_scores = normalize_scores([to_float(x["iter_bytes_per_op"]) for x in group], False)

        for i, row in enumerate(group):
            score_num = 0.0
            score_den = 0.0
            parts = [
                (ops_scores[i], 0.45),
                (p99_scores[i], 0.25),
                (miss_scores[i], 0.15),
                (iter_scores[i], 0.15),
            ]
            for sc, wt in parts:
                if not math.isnan(sc):
                    score_num += sc * wt
                    score_den += wt
            overall = score_num / score_den if score_den > 0 else float("nan")

            item = dict(row)
            item["score_ops"] = ops_scores[i]
            item["score_tail_p99"] = p99_scores[i]
            item["score_cache_miss"] = miss_scores[i]
            item["score_iter_bytes"] = iter_scores[i]
            item["overall_perf_score"] = overall
            out.append(item)

    out.sort(key=lambda x: (to_int(x["seq"]), scenario_key(str(x["scenario"]))))
    return out


def plot_phase_multimetric_vertical(rows: List[Dict[str, object]], phase: str, out_png: Path) -> bool:
    mix = [r for r in rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
    if not mix:
        return False

    worst200_map = {
        str(r["label"]): to_float(r["seek_p99_us"])
        for r in rows
        if str(r["phase"]) == phase and str(r["scenario"]) == "worst_seek200"
    }

    mix.sort(key=lambda x: phase_sort_key(phase, x))
    x_labels = [phase_x_label(phase, r) for r in mix]

    metrics = [
        ("ops_per_sec", "mixgraph throughput (M ops/s)", lambda v: v / 1e6),
        ("seek_p99_us", "mixgraph seek p99 (us)", lambda v: v),
        ("cache_hit_ratio_pct", "mixgraph cache hit (%)", lambda v: v),
        ("cache_miss_per_kop", "mixgraph cache miss /1k ops", lambda v: v),
        ("iter_bytes_per_op", "mixgraph iter bytes/op (KiB)", lambda v: v / 1024.0),
        ("worst_seek200_p99_us", "worst_seek200 p99 (us)", lambda v: v),
    ]

    fig, axes = plt.subplots(3, 2, figsize=(14, 11))
    axes_arr = axes.flatten()
    color = PHASE_COLOR.get(phase, "#7f7f7f")

    for i, (key, title, conv) in enumerate(metrics):
        ax = axes_arr[i]
        vals: List[float] = []
        for row in mix:
            if key == "worst_seek200_p99_us":
                raw = worst200_map.get(str(row["label"]), float("nan"))
            else:
                raw = to_float(row.get(key, float("nan")))
            vals.append(conv(raw) if not math.isnan(raw) else float("nan"))

        valid = [v for v in vals if not math.isnan(v)]
        if not valid:
            ax.axis("off")
            continue

        x = np.arange(len(vals))
        clean = [0.0 if math.isnan(v) else v for v in vals]
        bars = ax.bar(x, clean, color=color, alpha=0.9)
        ax.set_title(title, fontsize=11)
        ax.set_xticks(x)
        ax.set_xticklabels(x_labels, rotation=25, ha="right")
        ax.grid(axis="y", alpha=0.25)

        vmax = max(valid)
        for j, b in enumerate(bars):
            v = vals[j]
            if math.isnan(v):
                continue
            ax.text(
                b.get_x() + b.get_width() / 2,
                b.get_height() + vmax * 0.02,
                f"{v:.2f}",
                ha="center",
                va="bottom",
                fontsize=8,
            )

    fig.suptitle(f"{PHASE_LABEL.get(phase, phase)}: Multi-metric Vertical View", fontsize=14)
    plt.tight_layout(rect=(0, 0, 1, 0.97))
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_score_heatmap(rows: List[Dict[str, object]], out_png: Path) -> bool:
    labels = sorted(
        {str(r["label"]) for r in rows},
        key=lambda x: to_int(next(str(z["seq"]) for z in rows if str(z["label"]) == x)),
    )
    scenarios = [s for s in SCENARIO_ORDER if any(str(r["scenario"]) == s for r in rows)]
    if not labels or not scenarios:
        return False

    mat = np.full((len(labels), len(scenarios)), np.nan)
    lookup = {(str(r["label"]), str(r["scenario"])): to_float(r["overall_perf_score"]) for r in rows}
    for i, label in enumerate(labels):
        for j, scenario in enumerate(scenarios):
            mat[i, j] = lookup.get((label, scenario), float("nan"))

    fig, ax = plt.subplots(figsize=(1.2 * len(scenarios) + 4, 0.55 * len(labels) + 3))
    cmap = plt.get_cmap("YlGnBu").copy()
    cmap.set_bad(color="#f2f2f2")
    im = ax.imshow(mat, aspect="auto", cmap=cmap, vmin=0, vmax=100)
    ax.set_title("Scenario x Case Overall Score (0-100)")
    ax.set_xticks(np.arange(len(scenarios)))
    ax.set_xticklabels(scenarios, rotation=30, ha="right")
    ax.set_yticks(np.arange(len(labels)))
    ax.set_yticklabels(labels)

    for i in range(mat.shape[0]):
        for j in range(mat.shape[1]):
            v = mat[i, j]
            txt = "NA" if math.isnan(v) else f"{v:.1f}"
            if math.isnan(v):
                color = "black"
            else:
                r, g, b, _ = im.cmap(im.norm(v))
                luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
                color = "white" if luminance < 0.5 else "black"
            ax.text(j, i, txt, ha="center", va="center", fontsize=8, color=color)

    cbar = plt.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("overall perf score", fontsize=9)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_mixgraph_score_bar(rows: List[Dict[str, object]], out_png: Path) -> bool:
    mix = [r for r in rows if str(r["scenario"]) == "mixgraph"]
    if not mix:
        return False
    mix.sort(key=lambda x: to_float(x["overall_perf_score"]), reverse=True)

    labels = [f"{r['label']}({r['threads']}t)" for r in mix]
    vals = [to_float(r["overall_perf_score"]) for r in mix]
    ops = [to_float(r["ops_per_sec"]) / 1e6 for r in mix]

    fig, ax = plt.subplots(figsize=(max(8, 0.7 * len(labels) + 3), 5))
    x = np.arange(len(labels))
    bars = ax.bar(x, vals, color="#4e79a7")
    ax.set_title("Mixgraph Overall Score Ranking")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=25, ha="right")
    ax.set_ylabel("overall perf score")
    ax.set_ylim(0, 105)
    ax.grid(axis="y", alpha=0.25)
    for i, b in enumerate(bars):
        ax.text(
            b.get_x() + b.get_width() / 2,
            b.get_height() + 1.0,
            f"{vals[i]:.1f}\\n{ops[i]:.2f}M",
            ha="center",
            va="bottom",
            fontsize=8,
        )
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def build_module_budget_mixgraph(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    mix = [r for r in rows if str(r["scenario"]) == "mixgraph"]
    mix.sort(key=lambda x: (PHASE_ORDER.index(str(x["phase"])) if str(x["phase"]) in PHASE_ORDER else 999, phase_sort_key(str(x["phase"]), x)))

    out: List[Dict[str, object]] = []
    for r in mix:
        phase = str(r["phase"])
        out.append(
            {
                "seq": r["seq"],
                "label": r["label"],
                "phase": phase,
                "mode": r["mode"],
                "var_label": phase_x_label(phase, r),
                "ops_per_sec": r["ops_per_sec"],
                "seek_p99_us": r["seek_p99_us"],
                "frontend_cpu_ms_per_op": r["frontend_cpu_ms_per_op"],
                "block_cache_hit_ratio_pct": r["cache_hit_ratio_pct"],
                "block_cache_miss_per_kop": r["cache_miss_per_kop"],
                "block_cache_bytes_per_op": r["cache_bytes_per_op"],
                "sst_iter_bytes_per_op": r["iter_bytes_per_op"],
                "iterator_next_per_seek": r["next_per_seek"],
                "cpu_user_pct": r["cpu_user_pct"],
                "cpu_sys_pct": r["cpu_sys_pct"],
                "cpu_wait_pct": r["cpu_wait_pct"],
                "cswch_per_kop": r["cswch_per_kop"],
                "nvcswch_per_kop": r["nvcswch_per_kop"],
                "sync_switch_per_kop": r["sync_switch_per_kop"],
                "io_read_mb_s": r["io_read_mb_s"],
                "io_write_mb_s": r["io_write_mb_s"],
                "io_util_pct": r["iostat_util_pct"],
                "io_await_ms": r["iostat_await_ms"],
                "perf_ipc": r["perf_ipc"],
                "perf_cache_miss_pct": r["perf_cache_miss_pct"],
                "perf_l1_hit_pct": r["perf_l1_hit_pct"],
                "perf_l2_hit_pct": r["perf_l2_hit_pct"],
                "perf_l3_hit_pct": r["perf_l3_hit_pct"],
            }
        )
    return out


def component_norm(group: List[Dict[str, object]], key: str, higher_is_better: bool) -> Dict[str, float]:
    values = [to_float(x.get(key, float("nan"))) for x in group]
    valid = [v for v in values if not math.isnan(v)]
    if not valid:
        scores = [float("nan")] * len(values)
    else:
        lo = min(valid)
        hi = max(valid)
        if hi == lo:
            # Flat dimension: no discriminative pressure.
            scores = [0.0 if not math.isnan(v) else float("nan") for v in values]
        else:
            scores = normalize_scores(values, higher_is_better)
    out: Dict[str, float] = {}
    for row, score in zip(group, scores):
        out[str(row["label"])] = score
    return out


def build_component_maps(phase_group: List[Dict[str, object]]) -> Dict[str, Dict[str, float]]:
    maps: Dict[str, Dict[str, float]] = {}
    for name, key, hib, _, _ in MODULE_SPECS:
        maps[name] = component_norm(phase_group, key, hib)
    return maps


def component_score(component_maps: Dict[str, Dict[str, float]], module: str, label: str) -> float:
    mapping = component_maps.get(module, {})
    return to_float(mapping.get(label, float("nan")))


def plot_phase_module_pressure_vertical(rows: List[Dict[str, object]], phase: str, out_png: Path) -> bool:
    mix = [r for r in rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
    if not mix:
        return False
    mix.sort(key=lambda x: phase_sort_key(phase, x))
    comps = build_component_maps(mix)

    labels = [str(r["label"]) for r in mix]
    x_labels = [phase_x_label(phase, r) for r in mix]
    x = np.arange(len(labels))

    fig, ax = plt.subplots(figsize=(max(9, len(labels) * 1.2), 6))
    bottom = np.zeros(len(labels))

    for k, _, _, _, _ in MODULE_SPECS:
        vals = np.array([to_float(comps[k].get(lb, float("nan"))) for lb in labels], dtype=float)
        vals = np.nan_to_num(vals, nan=0.0)
        ax.bar(x, vals, bottom=bottom, label=k, color=MODULE_COLORS[k], alpha=0.9)
        bottom += vals

    ax.set_title(f"{PHASE_LABEL.get(phase, phase)}: Read-path Module Pressure (normalized stack)")
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, rotation=25, ha="right")
    ax.set_ylabel("normalized pressure sum")
    ax.grid(axis="y", alpha=0.25)
    ax.legend(ncol=3, fontsize=8)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_phase_module_signal_heatmap(rows: List[Dict[str, object]], phase: str, out_png: Path) -> bool:
    mix = [r for r in rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
    if not mix:
        return False
    mix.sort(key=lambda x: phase_sort_key(phase, x))
    labels = [str(r["label"]) for r in mix]
    x_labels = [phase_x_label(phase, r) for r in mix]

    component_maps = build_component_maps(mix)
    modules = [name for name, _, _, _, _ in MODULE_SPECS]
    mat = np.full((len(modules), len(labels)), np.nan)
    for i, module in enumerate(modules):
        for j, label in enumerate(labels):
            mat[i, j] = component_score(component_maps, module, label)

    fig, ax = plt.subplots(figsize=(max(8, len(labels) * 1.4), 5.2))
    cmap = plt.get_cmap("YlOrRd").copy()
    cmap.set_bad(color="#f2f2f2")
    im = ax.imshow(mat, aspect="auto", cmap=cmap, vmin=0, vmax=100)

    ax.set_title(f"{PHASE_LABEL.get(phase, phase)}: Module Signal Heatmap (0-100)")
    ax.set_xticks(np.arange(len(labels)))
    ax.set_xticklabels(x_labels, rotation=25, ha="right")
    ax.set_yticks(np.arange(len(modules)))
    ax.set_yticklabels(modules)

    for i in range(mat.shape[0]):
        for j in range(mat.shape[1]):
            v = mat[i, j]
            txt = "NA" if math.isnan(v) else f"{v:.1f}"
            if math.isnan(v):
                color = "black"
            else:
                r, g, b, _ = im.cmap(im.norm(v))
                luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
                color = "white" if luminance < 0.5 else "black"
            ax.text(j, i, txt, ha="center", va="center", fontsize=8, color=color)

    cbar = plt.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("normalized pressure", fontsize=9)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def top_component(
    row: Dict[str, object],
    phase_group: List[Dict[str, object]],
    component_maps: Dict[str, Dict[str, float]] | None = None,
) -> str:
    if component_maps is None:
        component_maps = build_component_maps(phase_group)
    scores: Dict[str, float] = {}
    for name, _, _, _, _ in MODULE_SPECS:
        scores[name] = component_score(component_maps, name, str(row["label"]))

    best_name = "unknown"
    best_val = -1.0
    for k, v in scores.items():
        if math.isnan(v):
            continue
        if v > best_val:
            best_val = v
            best_name = k
    return best_name


def module_proxy_spec(module: str) -> Tuple[str, str]:
    for name, _, _, proxy_name, unit in MODULE_SPECS:
        if module == name:
            return proxy_name, unit
    return "unknown", ""


def build_module_resource_breakdown(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    out: List[Dict[str, object]] = []
    for phase in PHASE_ORDER:
        group = [r for r in rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
        if not group:
            continue
        group.sort(key=lambda x: phase_sort_key(phase, x))
        component_maps = build_component_maps(group)

        for row in group:
            label = str(row["label"])
            total_cpu_parts = [to_float(row["cpu_user_pct"]), to_float(row["cpu_sys_pct"])]
            total_cpu_valid = [v for v in total_cpu_parts if not math.isnan(v)]
            total_cpu_pct = sum(total_cpu_valid) if total_cpu_valid else float("nan")

            io_parts = [to_float(row["io_read_mb_s"]), to_float(row["io_write_mb_s"])]
            io_valid = [v for v in io_parts if not math.isnan(v)]
            total_io_mb_s = sum(io_valid) if io_valid else float("nan")

            module_scores: Dict[str, float] = {}
            for module, _, _, _, _ in MODULE_SPECS:
                module_scores[module] = component_score(component_maps, module, label)

            positive_scores = [v for v in module_scores.values() if not math.isnan(v) and v > 0]
            score_sum = sum(positive_scores)

            for module, key, _, _, _ in MODULE_SPECS:
                score = module_scores[module]
                if score_sum > 0 and not math.isnan(score) and score > 0:
                    share_pct = score * 100.0 / score_sum
                else:
                    share_pct = float("nan")
                est_cpu_pct = total_cpu_pct * share_pct / 100.0 if not math.isnan(total_cpu_pct) and not math.isnan(share_pct) else float("nan")
                est_io_mb_s = total_io_mb_s * share_pct / 100.0 if not math.isnan(total_io_mb_s) and not math.isnan(share_pct) else float("nan")
                proxy_name, proxy_unit = module_proxy_spec(module)

                out.append(
                    {
                        "seq": row["seq"],
                        "label": row["label"],
                        "phase": phase,
                        "mode": row["mode"],
                        "var_label": phase_x_label(phase, row),
                        "module": module,
                        "pressure_score": score,
                        "pressure_share_pct": share_pct,
                        "est_cpu_pct": est_cpu_pct,
                        "est_io_bw_mb_s": est_io_mb_s,
                        "total_cpu_pct": total_cpu_pct,
                        "total_io_bw_mb_s": total_io_mb_s,
                        "proxy_key": proxy_name,
                        "proxy_value": row[key],
                        "proxy_unit": proxy_unit,
                        "cpu_wait_pct": row["cpu_wait_pct"],
                        "sync_switch_per_kop": row["sync_switch_per_kop"],
                        "iostat_await_ms": row["iostat_await_ms"],
                        "perf_cache_miss_pct": row["perf_cache_miss_pct"],
                        "perf_l1_hit_pct": row["perf_l1_hit_pct"],
                        "perf_l2_hit_pct": row["perf_l2_hit_pct"],
                        "perf_l3_hit_pct": row["perf_l3_hit_pct"],
                    }
                )

    return out


def write_module_report(signal_rows: List[Dict[str, object]], budget_rows: List[Dict[str, object]], out_md: Path) -> None:
    lines: List[str] = []
    lines.append("# 模块读链路资源预算报告")
    lines.append("")
    lines.append("## 口径说明")
    lines.append("- Block Cache 命中/未命中/填充：用 hit_ratio + miss_per_kop 近似。")
    lines.append("- 反序列化/解码压力：用 iter_bytes_per_op 与 next_per_seek 近似。")
    lines.append("- 同步/锁竞争代理：用 cswch/nvcswch 每千操作。")
    lines.append("- IO wait：用 cpu_wait_pct 与 iostat await。")
    lines.append("- L1/L2/L3 命中：来自 perf 事件（best-effort；若缺采样则为空）。")
    lines.append("")

    for phase in PHASE_ORDER:
        group = [r for r in signal_rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
        if not group:
            continue
        group.sort(key=lambda x: phase_sort_key(phase, x))
        component_maps = build_component_maps(group)
        lines.append(f"## {PHASE_LABEL.get(phase, phase)}")
        for r in group:
            top = top_component(r, group, component_maps)
            iter_kib = (
                to_float(r["iter_bytes_per_op"]) / 1024.0
                if not math.isnan(to_float(r["iter_bytes_per_op"]))
                else float("nan")
            )
            lines.append(
                "- "
                + f"{r['label']}({phase_x_label(phase, r)}): "
                + f"FrontCPU={fmt(to_float(r['frontend_cpu_ms_per_op']),4)}ms/op, "
                + f"CacheHit={fmt(to_float(r['cache_hit_ratio_pct']),4)}%, "
                + f"CacheMiss={fmt(to_float(r['cache_miss_per_kop']),3)}/kop, "
                + f"IterBytes={fmt(iter_kib,2)}KiB/op, "
                + f"SyncSwitch={fmt(to_float(r['sync_switch_per_kop']),3)}/kop, "
                + f"CPUwait={fmt(to_float(r['cpu_wait_pct']),3)}%, "
                + f"IOawait={fmt(to_float(r['iostat_await_ms']),3)}ms, "
                + f"L1/L2/L3 hit={fmt(to_float(r['perf_l1_hit_pct']),3)}/{fmt(to_float(r['perf_l2_hit_pct']),3)}/{fmt(to_float(r['perf_l3_hit_pct']),3)}%, "
                + f"TopPressure={top}"
            )
        lines.append("")

    no_profile_rows = [
        r
        for r in signal_rows
        if math.isnan(to_float(r.get("cpu_user_pct", float("nan"))))
        and math.isnan(to_float(r.get("iostat_util_pct", float("nan"))))
    ]
    lines.append("## 完备性")
    if no_profile_rows:
        lines.append(f"- 外部 profile 缺失行数: {len(no_profile_rows)}（CPU/IO/L1-L3 结论仅可做趋势占位）")
        lines.append("- 建议开启 ENABLE_CASE_PROFILE=1 后重跑，以得到可对账的模块资源结论。")
    else:
        lines.append("- 已具备外部 profile，可进行模块级 CPU/IO/微架构归因。")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_module_breakdown_report(rows: List[Dict[str, object]], out_md: Path) -> None:
    lines: List[str] = []
    lines.append("# 模块资源分解报告")
    lines.append("")
    lines.append("## 说明")
    lines.append("- `pressure_score` 是 phase 内相对压力（0-100）。")
    lines.append("- `pressure_share_pct`、`est_cpu_pct`、`est_io_bw_mb_s` 为按压力分摊的估算值。")
    lines.append("- 若外部 profile 缺失，CPU/IO/microarch 字段会为 NA。")
    lines.append("")

    for phase in PHASE_ORDER:
        phase_rows = [r for r in rows if str(r["phase"]) == phase]
        if not phase_rows:
            continue
        lines.append(f"## {PHASE_LABEL.get(phase, phase)}")
        labels = sorted({str(r["label"]) for r in phase_rows}, key=lambda x: to_int(next(str(z["seq"]) for z in phase_rows if str(z["label"]) == x)))
        for label in labels:
            case_rows = [r for r in phase_rows if str(r["label"]) == label]
            if not case_rows:
                continue
            case_rows.sort(key=lambda x: to_float(x.get("pressure_share_pct", float("nan"))), reverse=True)
            top_rows = [x for x in case_rows if not math.isnan(to_float(x.get("pressure_share_pct", float("nan"))))][:2]
            var_label = str(case_rows[0]["var_label"])
            if top_rows:
                top_txt = "; ".join(
                    f"{x['module']}({fmt(to_float(x['pressure_share_pct']),1)}%, estCPU={fmt(to_float(x['est_cpu_pct']),2)}%, estBW={fmt(to_float(x['est_io_bw_mb_s']),2)} MB/s)"
                    for x in top_rows
                )
            else:
                top_txt = "NA"
            lines.append(f"- {label}({var_label}): top_modules={top_txt}")
        lines.append("")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_signal_report(rows: List[Dict[str, object]], out_md: Path, generated: List[str]) -> None:
    lines: List[str] = []
    lines.append("# 资源账本性能信号报告")
    lines.append("")
    lines.append("## 摘要")
    lines.append(f"- 样本行数: {len(rows)}")
    lines.append(f"- case 数: {len(set(str(r['label']) for r in rows))}")
    lines.append(f"- scenario 数: {len(set(str(r['scenario']) for r in rows))}")
    lines.append("")

    for phase in PHASE_ORDER:
        mix = [r for r in rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
        if not mix:
            continue
        best = max(mix, key=lambda r: to_float(r.get("overall_perf_score", float("nan"))))
        worst = min(mix, key=lambda r: to_float(r.get("overall_perf_score", float("nan"))))
        lines.append(f"## {PHASE_LABEL.get(phase, phase)}")
        lines.append(
            f"- 最优: {best['label']} score={to_float(best['overall_perf_score']):.2f}, "
            f"ops={to_float(best['ops_per_sec']):.0f}, p99={to_float(best['seek_p99_us']):.3f}us"
        )
        lines.append(
            f"- 最弱: {worst['label']} score={to_float(worst['overall_perf_score']):.2f}, "
            f"ops={to_float(worst['ops_per_sec']):.0f}, p99={to_float(worst['seek_p99_us']):.3f}us"
        )
        lines.append("")

    lines.append("## 输出产物")
    for name in generated:
        lines.append(f"- {name}")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract phase-oriented performance and module budget insights from resource ledger.")
    ap.add_argument("--matrix-dir", required=True, help="Matrix dir that contains analysis/resource_ledger/resource_ledger.csv")
    ap.add_argument("--input-csv", default="", help="Optional resource_ledger.csv path override")
    ap.add_argument("--out-dir", default="", help="Output directory (default: <matrix-dir>/analysis/resource_ledger)")
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    in_csv = Path(args.input_csv).resolve() if args.input_csv else (matrix_dir / "analysis" / "resource_ledger" / "resource_ledger.csv")
    if not in_csv.exists():
        raise FileNotFoundError(f"input ledger not found: {in_csv}")

    out_dir = Path(args.out_dir).resolve() if args.out_dir else (matrix_dir / "analysis" / "resource_ledger")
    ensure_dir(out_dir)
    cleanup_legacy_files(out_dir)

    raw_rows = read_csv(in_csv)
    if not raw_rows:
        raise RuntimeError(f"empty ledger csv: {in_csv}")

    signal_rows = build_signal_table(raw_rows)
    if not signal_rows:
        raise RuntimeError("no signal rows generated from ledger")

    generated: List[str] = []

    write_csv(out_dir / "performance_signal_table.csv", signal_rows)
    generated.append("performance_signal_table.csv")

    if plot_phase_multimetric_vertical(signal_rows, "cache_sweep", out_dir / "phaseA_cache_multimetric_vertical.png"):
        generated.append("phaseA_cache_multimetric_vertical.png")
    if plot_phase_multimetric_vertical(signal_rows, "thread_sweep", out_dir / "phaseB_threads_multimetric_vertical.png"):
        generated.append("phaseB_threads_multimetric_vertical.png")
    if plot_phase_multimetric_vertical(signal_rows, "locality_sweep", out_dir / "phaseC_mixratio_multimetric_vertical.png"):
        generated.append("phaseC_mixratio_multimetric_vertical.png")

    if plot_score_heatmap(signal_rows, out_dir / "scenario_score_heatmap.png"):
        generated.append("scenario_score_heatmap.png")
    if plot_mixgraph_score_bar(signal_rows, out_dir / "mixgraph_score_ranking.png"):
        generated.append("mixgraph_score_ranking.png")

    budget_rows = build_module_budget_mixgraph(signal_rows)
    if budget_rows:
        write_csv(out_dir / "module_read_path_budget.csv", budget_rows)
        generated.append("module_read_path_budget.csv")

    if plot_phase_module_pressure_vertical(signal_rows, "cache_sweep", out_dir / "phaseA_module_pressure_vertical.png"):
        generated.append("phaseA_module_pressure_vertical.png")
    if plot_phase_module_pressure_vertical(signal_rows, "thread_sweep", out_dir / "phaseB_module_pressure_vertical.png"):
        generated.append("phaseB_module_pressure_vertical.png")
    if plot_phase_module_pressure_vertical(signal_rows, "locality_sweep", out_dir / "phaseC_module_pressure_vertical.png"):
        generated.append("phaseC_module_pressure_vertical.png")

    if plot_phase_module_signal_heatmap(signal_rows, "cache_sweep", out_dir / "phaseA_module_signal_heatmap.png"):
        generated.append("phaseA_module_signal_heatmap.png")
    if plot_phase_module_signal_heatmap(signal_rows, "thread_sweep", out_dir / "phaseB_module_signal_heatmap.png"):
        generated.append("phaseB_module_signal_heatmap.png")
    if plot_phase_module_signal_heatmap(signal_rows, "locality_sweep", out_dir / "phaseC_module_signal_heatmap.png"):
        generated.append("phaseC_module_signal_heatmap.png")

    write_module_report(signal_rows, budget_rows, out_dir / "module_read_path_report.md")
    generated.append("module_read_path_report.md")

    module_breakdown = build_module_resource_breakdown(signal_rows)
    if module_breakdown:
        write_csv(out_dir / "module_resource_breakdown.csv", module_breakdown)
        generated.append("module_resource_breakdown.csv")
        write_module_breakdown_report(module_breakdown, out_dir / "module_resource_breakdown_report.md")
        generated.append("module_resource_breakdown_report.md")

    write_signal_report(signal_rows, out_dir / "performance_signal_report.md", generated)
    generated.append("performance_signal_report.md")

    print(f"resource ledger signals generated: {out_dir}")
    print(f"rows={len(signal_rows)}")
    print("generated=" + ",".join(generated))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Plot mixgraph monitoring artifacts:
  - *.mix_monitor_window.csv / *.mix_monitor_stage.csv (RocksDB stats/tickers)
  - *.mix_events.csv (shift/burst/probe events)
  - *.simfs_window.csv / *.simfs_stage.csv (simulated hybrid FS monitor)

Outputs PNGs + derived CSV under <run_dir>/monitor_figures and <run_dir>/monitor_analysis.

Intentionally lightweight: uses only stdlib + matplotlib.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _safe_int(s: str) -> int:
    s = (s or "").strip()
    if not s:
        return 0
    try:
        return int(s)
    except ValueError:
        try:
            return int(float(s))
        except ValueError:
            return 0


def _safe_float(s: str) -> float:
    s = (s or "").strip()
    if not s:
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def _read_csv_dicts(path: Path) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    if not path.exists():
        return rows
    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            if not r:
                continue
            rows.append({k: (v if v is not None else "") for k, v in r.items()})
    return rows


def _ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def _ratio(n: float, d: float) -> float:
    if d <= 0:
        return 0.0
    return n / d


@dataclass
class Event:
    wall_time_us: int
    event: str
    thread_id: int
    shift_stage: int
    range_id: int
    key_id: int
    value_a: int
    value_b: int


def _parse_events(rows: List[Dict[str, str]]) -> List[Event]:
    out: List[Event] = []
    for r in rows:
        out.append(
            Event(
                wall_time_us=_safe_int(r.get("wall_time_us", "")),
                event=(r.get("event", "") or "").strip(),
                thread_id=_safe_int(r.get("thread_id", "")),
                shift_stage=_safe_int(r.get("shift_stage", "")),
                range_id=_safe_int(r.get("range_id", "")),
                key_id=_safe_int(r.get("key_id", "")),
                value_a=_safe_int(r.get("value_a", "")),
                value_b=_safe_int(r.get("value_b", "")),
            )
        )
    out.sort(key=lambda e: e.wall_time_us)
    return out


def _events_time_origin_us(events: List[Event]) -> int:
    for e in events:
        if e.event == "workload_start":
            return e.wall_time_us
    if events:
        return events[0].wall_time_us
    return 0


def _iter_event_times(events: List[Event], name: str) -> Iterable[int]:
    for e in events:
        if e.event == name:
            yield e.wall_time_us


def _case_prefix_from_window_csv(path: Path) -> str:
    # "02_mixgraph_cache_536870912.mix_monitor_window.csv" -> "02_mixgraph_cache_536870912"
    name = path.name
    suffix = ".mix_monitor_window.csv"
    if name.endswith(suffix):
        return name[: -len(suffix)]
    return name


def _parse_histogram_percentiles(log_text: str) -> Dict[str, Dict[str, float]]:
    """
    Parse db_bench "--histogram" output sections like:
      Microseconds per Read:
      Count: ...
      Min: ...
      Percentiles: P50: 1.23 P75: ... P95: ... P99: ...
    Returns: { "Read": {"p50_us":..., "p95_us":..., "p99_us":...}, ... }
    """
    out: Dict[str, Dict[str, float]] = {}
    current_op: Optional[str] = None
    for raw in log_text.splitlines():
        line = raw.strip()
        m = re.match(r"^Microseconds per ([A-Za-z0-9_ -]+):$", line)
        if m:
            current_op = m.group(1).strip()
            continue
        if not current_op:
            continue
        if not line.startswith("Percentiles:"):
            continue
        p50 = re.search(r"P50:\s*([0-9.]+)", line)
        p95 = re.search(r"P95:\s*([0-9.]+)", line)
        p99 = re.search(r"P99:\s*([0-9.]+)", line)
        if not (p50 and p95 and p99):
            continue
        out[current_op] = {
            "p50_us": float(p50.group(1)),
            "p95_us": float(p95.group(1)),
            "p99_us": float(p99.group(1)),
        }
        current_op = None
    return out


def _write_op_latency_percentiles(
    log_path: Path, out_csv: Path, out_png: Path, case_prefix: str
) -> None:
    if not log_path.exists():
        return
    txt = log_path.read_text(errors="ignore")
    per_op = _parse_histogram_percentiles(txt)
    if not per_op:
        return

    # Prefer the mixgraph-relevant op types if present.
    preferred = ["Read", "Seek", "Scan", "MultiGet"]
    ops = [op for op in preferred if op in per_op] + [
        op for op in sorted(per_op.keys()) if op not in preferred
    ]

    with out_csv.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["op", "p50_us", "p95_us", "p99_us"])
        w.writeheader()
        for op in ops:
            w.writerow(
                {
                    "op": op,
                    "p50_us": f"{per_op[op]['p50_us']:.3f}",
                    "p95_us": f"{per_op[op]['p95_us']:.3f}",
                    "p99_us": f"{per_op[op]['p99_us']:.3f}",
                }
            )

    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    x = list(range(len(ops)))
    p50s = [per_op[op]["p50_us"] for op in ops]
    p95s = [per_op[op]["p95_us"] for op in ops]
    p99s = [per_op[op]["p99_us"] for op in ops]

    fig = plt.figure(figsize=(14, 6))
    ax = fig.add_subplot(1, 1, 1)
    ax.plot(x, p50s, marker="o", label="P50 (us)")
    ax.plot(x, p95s, marker="o", label="P95 (us)")
    ax.plot(x, p99s, marker="o", label="P99 (us)")
    ax.set_xticks(x)
    ax.set_xticklabels(ops, rotation=15, ha="right")
    ax.set_ylabel("Latency (us)")
    ax.grid(True, alpha=0.25)
    ax.legend(loc="upper left")
    ax.set_title(f"{case_prefix}: operation latency percentiles (from --histogram)")
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def _write_stage_join_csv(
    out_csv: Path,
    mix_stage_rows: List[Dict[str, str]],
    simfs_stage_rows: List[Dict[str, str]],
) -> None:
    simfs_by_stage: Dict[int, Dict[str, str]] = {}
    for r in simfs_stage_rows:
        simfs_by_stage[_safe_int(r.get("stage_id", ""))] = r

    fields = [
        "stage_id",
        "stage_start_us",
        "stage_end_us",
        "stage_seconds",
        "mix_data_hit",
        "mix_data_miss",
        "mix_data_hit_ratio",
        "mix_data_bytes_insert",
        "mix_burst_scans",
        "mix_burst_entries",
        "mix_probe_ops",
        "mix_probe_hit",
        "mix_probe_incomplete",
        "mix_probe_hit_ratio",
        "mix_probe_incomplete_ratio",
        "simfs_read_ops",
        "simfs_read_bytes",
        "simfs_tmpfs_read_bytes",
        "simfs_base_read_bytes",
        "simfs_max_read_us",
        "simfs_prefetch_ops",
        "simfs_prefetch_bytes",
        "simfs_max_prefetch_us",
        "simfs_open_ops",
        "simfs_max_open_us",
    ]

    with out_csv.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for mr in mix_stage_rows:
            sid = _safe_int(mr.get("stage_id", ""))
            start_us = _safe_int(mr.get("stage_start_us", ""))
            end_us = _safe_int(mr.get("stage_end_us", ""))
            dur_s = (end_us - start_us) / 1e6 if end_us > start_us else 0.0

            data_hit = _safe_int(mr.get("data_hit", ""))
            data_miss = _safe_int(mr.get("data_miss", ""))
            probe_ops = _safe_int(mr.get("probe_ops", ""))
            probe_hit = _safe_int(mr.get("probe_hit", ""))
            probe_incomplete = _safe_int(mr.get("probe_incomplete", ""))

            sr = simfs_by_stage.get(sid, {})
            row = {
                "stage_id": sid,
                "stage_start_us": start_us,
                "stage_end_us": end_us,
                "stage_seconds": f"{dur_s:.6f}",
                "mix_data_hit": data_hit,
                "mix_data_miss": data_miss,
                "mix_data_hit_ratio": f"{_ratio(data_hit, data_hit + data_miss):.6f}",
                "mix_data_bytes_insert": _safe_int(mr.get("data_bytes_insert", "")),
                "mix_burst_scans": _safe_int(mr.get("burst_scans", "")),
                "mix_burst_entries": _safe_int(mr.get("burst_entries", "")),
                "mix_probe_ops": probe_ops,
                "mix_probe_hit": probe_hit,
                "mix_probe_incomplete": probe_incomplete,
                "mix_probe_hit_ratio": f"{_ratio(probe_hit, probe_ops):.6f}",
                "mix_probe_incomplete_ratio": f"{_ratio(probe_incomplete, probe_ops):.6f}",
                "simfs_read_ops": _safe_int(sr.get("read_ops", "")),
                "simfs_read_bytes": _safe_int(sr.get("read_bytes", "")),
                "simfs_tmpfs_read_bytes": _safe_int(sr.get("tmpfs_read_bytes", "")),
                "simfs_base_read_bytes": _safe_int(sr.get("base_read_bytes", "")),
                "simfs_max_read_us": _safe_int(sr.get("max_read_us", "")),
                "simfs_prefetch_ops": _safe_int(sr.get("prefetch_ops", "")),
                "simfs_prefetch_bytes": _safe_int(sr.get("prefetch_bytes", "")),
                "simfs_max_prefetch_us": _safe_int(sr.get("max_prefetch_us", "")),
                "simfs_open_ops": _safe_int(sr.get("open_ops", "")),
                "simfs_max_open_us": _safe_int(sr.get("max_open_us", "")),
            }
            w.writerow(row)


def _plot_case(
    run_dir: Path,
    case_prefix: str,
    mix_log: Path,
    mix_window: Path,
    mix_stage: Path,
    mix_events: Path,
    simfs_window: Optional[Path],
    simfs_stage: Optional[Path],
    figures_dir: Path,
    analysis_dir: Path,
) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    mix_w = _read_csv_dicts(mix_window)
    mix_s = _read_csv_dicts(mix_stage)
    events = _parse_events(_read_csv_dicts(mix_events))
    sim_w = _read_csv_dicts(simfs_window) if simfs_window else []
    sim_s = _read_csv_dicts(simfs_stage) if simfs_stage else []

    origin_us = _events_time_origin_us(events)

    def to_rel_s(us: int) -> float:
        if origin_us <= 0:
            return us / 1e6
        return (us - origin_us) / 1e6

    # ---- Mix window series
    t_mix_s: List[float] = []
    window_id: List[int] = []
    data_hit_ratio: List[float] = []
    block_hit_ratio: List[float] = []
    data_miss: List[int] = []
    data_hit: List[int] = []
    data_bytes_insert: List[int] = []
    cache_usage: List[int] = []
    cache_pinned: List[int] = []
    cache_capacity: List[int] = []
    burst_entries: List[int] = []
    burst_scans: List[int] = []
    probe_ops: List[int] = []
    probe_hit: List[int] = []
    probe_incomplete: List[int] = []
    probe_notfound: List[int] = []
    probe_error: List[int] = []
    shift_stage: List[int] = []

    for r in mix_w:
        wall_us = _safe_int(r.get("wall_time_us", ""))
        t_mix_s.append(to_rel_s(wall_us))
        window_id.append(_safe_int(r.get("window_id", "")))
        bh = _safe_int(r.get("block_cache_hit", ""))
        bm = _safe_int(r.get("block_cache_miss", ""))
        dh = _safe_int(r.get("data_hit", ""))
        dm = _safe_int(r.get("data_miss", ""))
        data_hit.append(dh)
        data_miss.append(dm)
        data_hit_ratio.append(_ratio(dh, dh + dm))
        block_hit_ratio.append(_ratio(bh, bh + bm))
        data_bytes_insert.append(_safe_int(r.get("data_bytes_insert", "")))
        cache_usage.append(_safe_int(r.get("block_cache_usage", "")))
        cache_pinned.append(_safe_int(r.get("block_cache_pinned_usage", "")))
        cache_capacity.append(_safe_int(r.get("block_cache_capacity", "")))
        burst_scans.append(_safe_int(r.get("burst_scans", "")))
        burst_entries.append(_safe_int(r.get("burst_entries", "")))
        probe_ops.append(_safe_int(r.get("probe_ops", "")))
        probe_hit.append(_safe_int(r.get("probe_hit", "")))
        probe_incomplete.append(_safe_int(r.get("probe_incomplete", "")))
        probe_notfound.append(_safe_int(r.get("probe_notfound", "")))
        probe_error.append(_safe_int(r.get("probe_error", "")))
        shift_stage.append(_safe_int(r.get("shift_stage", "")))

    # ---- Derived per-window metrics for attribution
    data_access = [h + m for h, m in zip(data_hit, data_miss)]
    miss_ratio = [
        _ratio(m, a) for m, a in zip(data_miss, data_access)
    ]  # a == hit+miss
    probe_ok_ratio = [_ratio(h, o) for h, o in zip(probe_hit, probe_ops)]
    probe_incomplete_ratio = [_ratio(inc, o) for inc, o in zip(probe_incomplete, probe_ops)]
    cache_pressure = [
        _ratio(u, c) for u, c in zip(cache_usage, cache_capacity)
    ]  # usage/capacity

    # ---- SimFS window series (optional)
    t_sim_s: List[float] = []
    sim_max_read: List[int] = []
    sim_max_open: List[int] = []
    sim_max_prefetch: List[int] = []
    sim_read_bytes_tmp: List[int] = []
    sim_read_bytes_base: List[int] = []
    sim_prefetch_bytes_tmp: List[int] = []
    sim_prefetch_bytes_base: List[int] = []

    sim_max_read_aligned: List[float] = []
    sim_max_open_aligned: List[float] = []
    sim_max_prefetch_aligned: List[float] = []

    for r in sim_w:
        start_us = _safe_int(r.get("window_start_us", ""))
        end_us = _safe_int(r.get("window_end_us", ""))
        mid_us = (start_us + end_us) // 2
        t_sim_s.append(to_rel_s(mid_us))
        sim_max_read.append(_safe_int(r.get("max_read_us", "")))
        sim_max_open.append(_safe_int(r.get("max_open_us", "")))
        sim_max_prefetch.append(_safe_int(r.get("max_prefetch_us", "")))
        sim_read_bytes_tmp.append(_safe_int(r.get("tmpfs_read_bytes", "")))
        sim_read_bytes_base.append(_safe_int(r.get("base_read_bytes", "")))
        sim_prefetch_bytes_tmp.append(_safe_int(r.get("tmpfs_prefetch_bytes", "")))
        sim_prefetch_bytes_base.append(_safe_int(r.get("base_prefetch_bytes", "")))

    if sim_w and window_id:
        sim_by_window: Dict[int, Dict[str, str]] = {}
        for r in sim_w:
            sim_by_window[_safe_int(r.get("window_id", ""))] = r
        for wid in window_id:
            sr = sim_by_window.get(wid, {})
            if not sr:
                sim_max_read_aligned.append(float("nan"))
                sim_max_open_aligned.append(float("nan"))
                sim_max_prefetch_aligned.append(float("nan"))
                continue
            sim_max_read_aligned.append(float(_safe_int(sr.get("max_read_us", ""))))
            sim_max_open_aligned.append(float(_safe_int(sr.get("max_open_us", ""))))
            sim_max_prefetch_aligned.append(float(_safe_int(sr.get("max_prefetch_us", ""))))

    burst_times = [to_rel_s(us) for us in _iter_event_times(events, "burst_scan")]
    stage_times = [to_rel_s(us) for us in _iter_event_times(events, "shift_stage")]

    def add_event_lines(ax: Any) -> None:
        for x in stage_times:
            ax.axvline(x, color="tab:gray", alpha=0.20, linewidth=1.0)
        for x in burst_times:
            ax.axvline(x, color="tab:red", alpha=0.10, linewidth=0.8)

    # ---- Plot 1: cache hit ratio + probe
    fig = plt.figure(figsize=(14, 8))
    ax1 = fig.add_subplot(2, 1, 1)
    ax2 = fig.add_subplot(2, 1, 2, sharex=ax1)

    ax1.plot(t_mix_s, [x * 100.0 for x in data_hit_ratio], label="data hit ratio (%)")
    ax1.plot(t_mix_s, [x * 100.0 for x in block_hit_ratio], label="block hit ratio (%)", alpha=0.7)
    add_event_lines(ax1)
    ax1.set_ylabel("Hit ratio (%)")
    ax1.set_ylim(0, 100)
    ax1.grid(True, alpha=0.25)
    ax1.legend(loc="lower right")
    ax1.set_title(f"{case_prefix}: cache hit ratio (window deltas) + events")

    probe_ok_pct = [_ratio(h, o) * 100.0 for h, o in zip(probe_hit, probe_ops)]
    probe_incomplete_pct = [_ratio(inc, o) * 100.0 for inc, o in zip(probe_incomplete, probe_ops)]
    ax2.plot(t_mix_s, probe_ok_pct, label="probe ok (%)")
    ax2.plot(t_mix_s, probe_incomplete_pct, label="probe incomplete (%)", alpha=0.8)
    add_event_lines(ax2)
    ax2.set_ylabel("Probe ratio (%)")
    ax2.set_xlabel("Time since mixgraph start (s)")
    ax2.set_ylim(0, 100)
    ax2.grid(True, alpha=0.25)
    ax2.legend(loc="upper right")

    out1 = figures_dir / f"{case_prefix}.cache_hit_probe_timeseries.png"
    fig.tight_layout()
    fig.savefig(out1, dpi=160)
    plt.close(fig)

    # ---- Plot 1b: miss ratio (window) + probe incomplete ratio
    fig = plt.figure(figsize=(14, 6))
    ax1 = fig.add_subplot(1, 1, 1)
    ax1.plot(t_mix_s, [x * 100.0 for x in miss_ratio], label="data miss ratio (%)")
    ax1.plot(t_mix_s, [x * 100.0 for x in probe_incomplete_ratio], label="probe incomplete ratio (%)", alpha=0.8)
    add_event_lines(ax1)
    ax1.set_xlabel("Time since mixgraph start (s)")
    ax1.set_ylabel("Ratio (%)")
    ax1.set_ylim(0, 100)
    ax1.grid(True, alpha=0.25)
    ax1.legend(loc="upper right")
    ax1.set_title(f"{case_prefix}: miss ratio + probe incomplete (window)")
    out1b = figures_dir / f"{case_prefix}.miss_ratio_timeseries.png"
    fig.tight_layout()
    fig.savefig(out1b, dpi=160)
    plt.close(fig)

    # ---- Plot 2: cache usage + bytes insert
    fig = plt.figure(figsize=(14, 8))
    ax1 = fig.add_subplot(2, 1, 1)
    ax2 = fig.add_subplot(2, 1, 2, sharex=ax1)

    ax1.plot(t_mix_s, cache_usage, label="cache usage")
    ax1.plot(t_mix_s, cache_pinned, label="cache pinned", alpha=0.8)
    if any(cache_capacity):
        ax1.plot(t_mix_s, cache_capacity, label="cache capacity", linestyle="--", alpha=0.6)
    add_event_lines(ax1)
    ax1.set_ylabel("Bytes")
    ax1.grid(True, alpha=0.25)
    ax1.legend(loc="upper left")
    ax1.set_title(f"{case_prefix}: block cache usage + events")

    ax2.plot(t_mix_s, data_bytes_insert, label="data bytes.insert (delta/window)")
    add_event_lines(ax2)
    ax2.set_ylabel("Bytes")
    ax2.set_xlabel("Time since mixgraph start (s)")
    ax2.grid(True, alpha=0.25)
    ax2.legend(loc="upper right")

    out2 = figures_dir / f"{case_prefix}.cache_usage_insert_timeseries.png"
    fig.tight_layout()
    fig.savefig(out2, dpi=160)
    plt.close(fig)

    # ---- Plot 3: burst intensity vs misses
    fig = plt.figure(figsize=(14, 6))
    ax1 = fig.add_subplot(1, 1, 1)
    ax1.plot(t_mix_s, data_miss, label="data miss (delta/window)")
    ax1.plot(t_mix_s, burst_entries, label="burst entries (delta/window)", alpha=0.8)
    add_event_lines(ax1)
    ax1.set_xlabel("Time since mixgraph start (s)")
    ax1.grid(True, alpha=0.25)
    ax1.legend(loc="upper right")
    ax1.set_title(f"{case_prefix}: burst vs data misses (window deltas)")
    out3 = figures_dir / f"{case_prefix}.burst_vs_cache_miss.png"
    fig.tight_layout()
    fig.savefig(out3, dpi=160)
    plt.close(fig)

    # ---- Plot 3b: attribution scatter (burst vs miss, access vs miss)
    fig = plt.figure(figsize=(14, 6))
    ax = fig.add_subplot(1, 2, 1)
    ax.scatter(burst_entries, data_miss, s=10, alpha=0.6)
    ax.set_xlabel("burst entries (delta/window)")
    ax.set_ylabel("data misses (delta/window)")
    ax.grid(True, alpha=0.25)
    ax.set_title("burst_entries vs data_miss")

    ax = fig.add_subplot(1, 2, 2)
    ax.scatter(data_access, data_miss, s=10, alpha=0.6)
    ax.set_xlabel("data access = hit+miss (delta/window)")
    ax.set_ylabel("data misses (delta/window)")
    ax.grid(True, alpha=0.25)
    ax.set_title("data_access vs data_miss")
    out3b = figures_dir / f"{case_prefix}.factor_scatter.png"
    fig.tight_layout()
    fig.savefig(out3b, dpi=160)
    plt.close(fig)

    # ---- Plot 3c: cache pressure vs miss ratio
    fig = plt.figure(figsize=(14, 6))
    ax = fig.add_subplot(1, 2, 1)
    ax.scatter([x * 100.0 for x in cache_pressure], [x * 100.0 for x in miss_ratio], s=10, alpha=0.6)
    ax.set_xlabel("cache pressure = usage/capacity (%)")
    ax.set_ylabel("data miss ratio (%)")
    ax.grid(True, alpha=0.25)
    ax.set_title("cache_pressure vs miss_ratio")

    ax = fig.add_subplot(1, 2, 2)
    ax.scatter([x * 100.0 for x in cache_pressure], [x * 100.0 for x in probe_incomplete_ratio], s=10, alpha=0.6)
    ax.set_xlabel("cache pressure = usage/capacity (%)")
    ax.set_ylabel("probe incomplete ratio (%)")
    ax.grid(True, alpha=0.25)
    ax.set_title("cache_pressure vs probe_incomplete_ratio")
    out3c = figures_dir / f"{case_prefix}.pressure_scatter.png"
    fig.tight_layout()
    fig.savefig(out3c, dpi=160)
    plt.close(fig)

    # ---- Plot 4: SimFS max latency time series (optional)
    if t_sim_s and (sim_max_read or sim_max_open or sim_max_prefetch):
        fig = plt.figure(figsize=(14, 7))
        ax = fig.add_subplot(1, 1, 1)
        ax.plot(t_sim_s, sim_max_read, label="simfs max read us")
        ax.plot(t_sim_s, sim_max_open, label="simfs max open us", alpha=0.8)
        ax.plot(t_sim_s, sim_max_prefetch, label="simfs max prefetch us", alpha=0.8)
        add_event_lines(ax)
        ax.set_xlabel("Time since mixgraph start (s)")
        ax.set_ylabel("Max latency (us)")
        ax.grid(True, alpha=0.25)
        ax.legend(loc="upper right")
        ax.set_title(f"{case_prefix}: simfs max latency per window + events")
        # log scale helps on burst spikes; keep linear if values are small
        if max(sim_max_read + sim_max_open + sim_max_prefetch) > 5000:
            ax.set_yscale("log")
        out4 = figures_dir / f"{case_prefix}.simfs_max_latency_timeseries.png"
        fig.tight_layout()
        fig.savefig(out4, dpi=160)
        plt.close(fig)

        fig = plt.figure(figsize=(14, 6))
        ax = fig.add_subplot(1, 1, 1)
        ax.plot(t_sim_s, sim_read_bytes_tmp, label="tmpfs read bytes (delta/window)")
        ax.plot(t_sim_s, sim_read_bytes_base, label="base read bytes (delta/window)", alpha=0.8)
        ax.plot(t_sim_s, sim_prefetch_bytes_tmp, label="tmpfs prefetch bytes (delta/window)", alpha=0.8)
        ax.plot(t_sim_s, sim_prefetch_bytes_base, label="base prefetch bytes (delta/window)", alpha=0.8)
        add_event_lines(ax)
        ax.set_xlabel("Time since mixgraph start (s)")
        ax.set_ylabel("Bytes")
        ax.grid(True, alpha=0.25)
        ax.legend(loc="upper right")
        ax.set_title(f"{case_prefix}: simfs traffic (tmpfs vs base) + events")
        out5 = figures_dir / f"{case_prefix}.simfs_traffic_timeseries.png"
        fig.tight_layout()
        fig.savefig(out5, dpi=160)
        plt.close(fig)

        # Hit ratio view: tmpfs vs base (bytes-based), useful when simfs actually falls back to base.
        if sim_read_bytes_tmp or sim_prefetch_bytes_tmp:
            read_hit_ratio = [
                _ratio(float(t), float(t + b)) * 100.0
                for t, b in zip(sim_read_bytes_tmp, sim_read_bytes_base)
            ]
            prefetch_hit_ratio = [
                _ratio(float(t), float(t + b)) * 100.0
                for t, b in zip(sim_prefetch_bytes_tmp, sim_prefetch_bytes_base)
            ]
            fig = plt.figure(figsize=(14, 6))
            ax = fig.add_subplot(1, 1, 1)
            ax.plot(t_sim_s, read_hit_ratio, label="read tmpfs hit ratio (%)")
            ax.plot(t_sim_s, prefetch_hit_ratio, label="prefetch tmpfs hit ratio (%)", alpha=0.8)
            add_event_lines(ax)
            ax.set_xlabel("Time since mixgraph start (s)")
            ax.set_ylabel("Hit ratio (%)")
            ax.set_ylim(0, 100)
            ax.grid(True, alpha=0.25)
            ax.legend(loc="lower right")
            ax.set_title(f"{case_prefix}: simfs tmpfs hit ratio (bytes) + events")
            out5b = figures_dir / f"{case_prefix}.simfs_hit_ratio_timeseries.png"
            fig.tight_layout()
            fig.savefig(out5b, dpi=160)
            plt.close(fig)

    # ---- Stage join + plot
    if mix_s:
        join_csv = analysis_dir / f"{case_prefix}.stage_join.csv"
        _write_stage_join_csv(join_csv, mix_s, sim_s)

        # Stage summary plot (key metrics)
        stage_rows = _read_csv_dicts(join_csv)
        stage_ids = [_safe_int(r.get("stage_id", "")) for r in stage_rows]
        stage_hit = [_safe_float(r.get("mix_data_hit_ratio", "")) * 100.0 for r in stage_rows]
        stage_probe_ok = [_safe_float(r.get("mix_probe_hit_ratio", "")) * 100.0 for r in stage_rows]
        stage_probe_inc = [_safe_float(r.get("mix_probe_incomplete_ratio", "")) * 100.0 for r in stage_rows]
        stage_max_read = [_safe_int(r.get("simfs_max_read_us", "")) for r in stage_rows]
        stage_burst_entries = [_safe_int(r.get("mix_burst_entries", "")) for r in stage_rows]

        fig = plt.figure(figsize=(14, 8))
        ax1 = fig.add_subplot(2, 1, 1)
        ax2 = fig.add_subplot(2, 1, 2)
        ax1.plot(stage_ids, stage_hit, marker="o", label="data hit ratio (%)")
        ax1.plot(stage_ids, stage_probe_ok, marker="o", label="probe ok (%)", alpha=0.8)
        ax1.plot(stage_ids, stage_probe_inc, marker="o", label="probe incomplete (%)", alpha=0.8)
        ax1.set_xlabel("Shift stage id")
        ax1.set_ylabel("Ratio (%)")
        ax1.set_ylim(0, 100)
        ax1.grid(True, alpha=0.25)
        ax1.legend(loc="lower left")
        ax1.set_title(f"{case_prefix}: stage summary (cache/probe)")

        ax2.bar(stage_ids, stage_burst_entries, label="burst entries", alpha=0.7)
        ax2.plot(stage_ids, stage_max_read, color="tab:red", marker="o", label="simfs max read us")
        ax2.set_xlabel("Shift stage id")
        ax2.set_ylabel("Entries / us")
        ax2.grid(True, alpha=0.25)
        ax2.legend(loc="upper right")
        out6 = figures_dir / f"{case_prefix}.stage_summary.png"
        fig.tight_layout()
        fig.savefig(out6, dpi=160)
        plt.close(fig)

    # ---- Op latency percentiles (requires db_bench --histogram)
    op_csv = analysis_dir / f"{case_prefix}.op_latency_percentiles.csv"
    op_png = figures_dir / f"{case_prefix}.op_latency_percentiles.png"
    _write_op_latency_percentiles(mix_log, op_csv, op_png, case_prefix)

    # ---- Attribution report (per-window correlations + simple controls)
    def corr(xs: List[float], ys: List[float]) -> float:
        pairs: List[Tuple[float, float]] = []
        for x, y in zip(xs, ys):
            if math.isnan(x) or math.isnan(y):
                continue
            pairs.append((x, y))
        n = len(pairs)
        if n < 2:
            return float("nan")
        mx = sum(x for x, _ in pairs) / n
        my = sum(y for _, y in pairs) / n
        vx = sum((x - mx) ** 2 for x, _ in pairs)
        vy = sum((y - my) ** 2 for _, y in pairs)
        if vx <= 0 or vy <= 0:
            return float("nan")
        cov = sum((x - mx) * (y - my) for x, y in pairs)
        return cov / math.sqrt(vx * vy)

    def pct(x: float) -> str:
        if math.isnan(x):
            return "nan"
        return f"{x*100.0:.3f}%"

    # Group stats: burst vs non-burst windows
    burst_mask = [b > 0 for b in burst_entries]
    miss_nb = [m for m, b in zip(data_miss, burst_mask) if not b]
    miss_b = [m for m, b in zip(data_miss, burst_mask) if b]
    ratio_nb = [r for r, b in zip(miss_ratio, burst_mask) if not b]
    ratio_b = [r for r, b in zip(miss_ratio, burst_mask) if b]

    def median(v: List[float]) -> float:
        if not v:
            return float("nan")
        s = sorted(v)
        mid = len(s) // 2
        if len(s) % 2 == 1:
            return float(s[mid])
        return 0.5 * (float(s[mid - 1]) + float(s[mid]))

    def p90(v: List[float]) -> float:
        if not v:
            return float("nan")
        s = sorted(v)
        idx = max(0, int(0.9 * len(s)) - 1)
        return float(s[idx])

    report = analysis_dir / f"{case_prefix}.factor_report.txt"
    with report.open("w") as f:
        f.write(f"case={case_prefix}\n")
        f.write(f"windows={len(t_mix_s)}\n")
        f.write(f"burst_windows={sum(1 for b in burst_mask if b)}\n")
        f.write(f"nonburst_windows={sum(1 for b in burst_mask if not b)}\n\n")

        f.write("[window correlation]\n")
        f.write(f"corr(burst_entries, data_miss)={corr([float(x) for x in burst_entries],[float(x) for x in data_miss]):.6f}\n")
        f.write(f"corr(burst_entries, miss_ratio)={corr([float(x) for x in burst_entries],[float(x) for x in miss_ratio]):.6f}\n")
        f.write(f"corr(data_access, data_miss)={corr([float(x) for x in data_access],[float(x) for x in data_miss]):.6f}\n")
        f.write(f"corr(data_bytes_insert, data_miss)={corr([float(x) for x in data_bytes_insert],[float(x) for x in data_miss]):.6f}\n")
        f.write(f"corr(cache_usage, data_miss)={corr([float(x) for x in cache_usage],[float(x) for x in data_miss]):.6f}\n")
        f.write(f"corr(cache_pressure, miss_ratio)={corr([float(x) for x in cache_pressure],[float(x) for x in miss_ratio]):.6f}\n")
        f.write(f"corr(cache_pressure, probe_incomplete_ratio)={corr([float(x) for x in cache_pressure],[float(x) for x in probe_incomplete_ratio]):.6f}\n")
        f.write(f"corr(probe_incomplete_ratio, data_miss)={corr([float(x) for x in probe_incomplete_ratio],[float(x) for x in data_miss]):.6f}\n")
        f.write(f"corr(shift_stage, data_miss)={corr([float(x) for x in shift_stage],[float(x) for x in data_miss]):.6f}\n\n")
        if sim_max_read_aligned:
            f.write(f"corr(burst_entries, simfs_max_read_us)={corr([float(x) for x in burst_entries],sim_max_read_aligned):.6f}\n")
            f.write(f"corr(burst_entries, simfs_max_prefetch_us)={corr([float(x) for x in burst_entries],sim_max_prefetch_aligned):.6f}\n")
            f.write(f"corr(burst_entries, simfs_max_open_us)={corr([float(x) for x in burst_entries],sim_max_open_aligned):.6f}\n")
            f.write(f"corr(simfs_max_read_us, miss_ratio)={corr(sim_max_read_aligned,[float(x) for x in miss_ratio]):.6f}\n\n")

        f.write("[burst vs non-burst windows]\n")
        f.write(f"median(data_miss) nonburst={median([float(x) for x in miss_nb]):.3f} burst={median([float(x) for x in miss_b]):.3f}\n")
        f.write(f"p90(data_miss)    nonburst={p90([float(x) for x in miss_nb]):.3f} burst={p90([float(x) for x in miss_b]):.3f}\n")
        f.write(f"median(miss_ratio) nonburst={pct(median(ratio_nb))} burst={pct(median(ratio_b))}\n")
        f.write(f"p90(miss_ratio)    nonburst={pct(p90(ratio_nb))} burst={pct(p90(ratio_b))}\n\n")

        f.write("[notes]\n")
        f.write("- data_miss here is window delta count, not a miss ratio.\n")
        f.write("- miss_ratio is data_miss/(data_hit+data_miss) per window.\n")
        f.write("- probe_incomplete_ratio close to eviction signal for hotset (no IO allowed).\n")
        f.write(
            "- data_bytes_insert is largely a *result* of misses (filling cache on miss), "
            "so high corr(data_bytes_insert, data_miss) is expected and not causal by itself.\n"
        )

    # ---- Event-aligned impact analysis (burst / shift_stage)
    # This helps separate:
    #  - direct misses from burst traffic (instant)
    #  - eviction or cache-residency loss (after-effect; probe_incomplete)
    if mix_w and events:
        # Estimate window size from wall_time series (robust median).
        dt = [t_mix_s[i] - t_mix_s[i - 1] for i in range(1, len(t_mix_s))]
        dt_med = sorted(dt)[len(dt) // 2] if dt else 1.0
        if dt_med <= 0:
            dt_med = 1.0

        def event_to_window_idx(ev_us: int) -> int:
            # map by relative seconds / median step; clamp to series range
            ev_rel_s = to_rel_s(ev_us)
            idx = int(math.floor(ev_rel_s / dt_med))
            return max(0, min(len(t_mix_s) - 1, idx))

        def aligned_mean(event_name: str, left: int, right: int) -> Tuple[List[int], Dict[str, List[float]]]:
            centers = [event_to_window_idx(us) for us in _iter_event_times(events, event_name)]
            centers = [c for c in centers if 0 <= c < len(t_mix_s)]
            offsets = list(range(-left, right + 1))
            traces: Dict[str, List[List[float]]] = {
                "data_miss": [],
                "miss_ratio": [],
                "probe_incomplete_ratio": [],
                "cache_pressure": [],
                "burst_entries": [],
            }
            for c in centers:
                traces["data_miss"].append(
                    [float(data_miss[c + k]) if 0 <= c + k < len(data_miss) else float("nan") for k in offsets]
                )
                traces["miss_ratio"].append(
                    [float(miss_ratio[c + k]) if 0 <= c + k < len(miss_ratio) else float("nan") for k in offsets]
                )
                traces["probe_incomplete_ratio"].append(
                    [float(probe_incomplete_ratio[c + k]) if 0 <= c + k < len(probe_incomplete_ratio) else float("nan") for k in offsets]
                )
                traces["cache_pressure"].append(
                    [float(cache_pressure[c + k]) if 0 <= c + k < len(cache_pressure) else float("nan") for k in offsets]
                )
                traces["burst_entries"].append(
                    [float(burst_entries[c + k]) if 0 <= c + k < len(burst_entries) else float("nan") for k in offsets]
                )

            def nanmean(vals: List[float]) -> float:
                xs = [v for v in vals if not math.isnan(v)]
                if not xs:
                    return float("nan")
                return sum(xs) / len(xs)

            means: Dict[str, List[float]] = {}
            for name, arr in traces.items():
                if not arr:
                    means[name] = [float("nan")] * len(offsets)
                    continue
                # transpose
                means[name] = [nanmean([tr[i] for tr in arr]) for i in range(len(offsets))]
            return offsets, means

        # Burst impact: expect immediate effect on data_miss, possible after-effect on probe_incomplete_ratio.
        burst_offsets, burst_means = aligned_mean("burst_scan", left=10, right=20)
        burst_csv = analysis_dir / f"{case_prefix}.burst_impact.csv"
        with burst_csv.open("w", newline="") as f:
            w = csv.DictWriter(
                f,
                fieldnames=[
                    "offset_windows",
                    "data_miss_mean",
                    "miss_ratio_mean",
                    "probe_incomplete_ratio_mean",
                    "cache_pressure_mean",
                    "burst_entries_mean",
                ],
            )
            w.writeheader()
            for i, off in enumerate(burst_offsets):
                w.writerow(
                    {
                        "offset_windows": off,
                        "data_miss_mean": burst_means["data_miss"][i],
                        "miss_ratio_mean": burst_means["miss_ratio"][i],
                        "probe_incomplete_ratio_mean": burst_means["probe_incomplete_ratio"][i],
                        "cache_pressure_mean": burst_means["cache_pressure"][i],
                        "burst_entries_mean": burst_means["burst_entries"][i],
                    }
                )

        fig = plt.figure(figsize=(14, 8))
        ax1 = fig.add_subplot(2, 1, 1)
        ax2 = fig.add_subplot(2, 1, 2, sharex=ax1)
        ax1.plot(burst_offsets, burst_means["data_miss"], marker="o", label="data_miss mean")
        ax1.set_ylabel("misses (count/window)")
        ax1.grid(True, alpha=0.25)
        ax1.legend(loc="upper right")
        ax1.set_title(f"{case_prefix}: event-aligned burst impact (mean over burst_scan events)")
        ax2.plot(burst_offsets, [x * 100.0 for x in burst_means["miss_ratio"]], marker="o", label="miss_ratio mean (%)")
        ax2.plot(burst_offsets, [x * 100.0 for x in burst_means["probe_incomplete_ratio"]], marker="o", label="probe_incomplete mean (%)", alpha=0.8)
        ax2.set_xlabel("Offset from burst_scan (windows)")
        ax2.set_ylabel("ratio (%)")
        ax2.set_ylim(0, 100)
        ax2.grid(True, alpha=0.25)
        ax2.legend(loc="upper right")
        out_burst = figures_dir / f"{case_prefix}.burst_impact.png"
        fig.tight_layout()
        fig.savefig(out_burst, dpi=160)
        plt.close(fig)

        # Shift impact: only meaningful when shift events exist.
        shift_offsets, shift_means = aligned_mean("shift_stage", left=10, right=20)
        shift_csv = analysis_dir / f"{case_prefix}.shift_impact.csv"
        with shift_csv.open("w", newline="") as f:
            w = csv.DictWriter(
                f,
                fieldnames=[
                    "offset_windows",
                    "data_miss_mean",
                    "miss_ratio_mean",
                    "probe_incomplete_ratio_mean",
                    "cache_pressure_mean",
                ],
            )
            w.writeheader()
            for i, off in enumerate(shift_offsets):
                w.writerow(
                    {
                        "offset_windows": off,
                        "data_miss_mean": shift_means["data_miss"][i],
                        "miss_ratio_mean": shift_means["miss_ratio"][i],
                        "probe_incomplete_ratio_mean": shift_means["probe_incomplete_ratio"][i],
                        "cache_pressure_mean": shift_means["cache_pressure"][i],
                    }
                )

        fig = plt.figure(figsize=(14, 8))
        ax1 = fig.add_subplot(2, 1, 1)
        ax2 = fig.add_subplot(2, 1, 2, sharex=ax1)
        ax1.plot(shift_offsets, shift_means["data_miss"], marker="o", label="data_miss mean")
        ax1.set_ylabel("misses (count/window)")
        ax1.grid(True, alpha=0.25)
        ax1.legend(loc="upper right")
        ax1.set_title(f"{case_prefix}: event-aligned shift_stage impact (mean over shift_stage events)")
        ax2.plot(shift_offsets, [x * 100.0 for x in shift_means["miss_ratio"]], marker="o", label="miss_ratio mean (%)")
        ax2.plot(shift_offsets, [x * 100.0 for x in shift_means["probe_incomplete_ratio"]], marker="o", label="probe_incomplete mean (%)", alpha=0.8)
        ax2.set_xlabel("Offset from shift_stage (windows)")
        ax2.set_ylabel("ratio (%)")
        ax2.set_ylim(0, 100)
        ax2.grid(True, alpha=0.25)
        ax2.legend(loc="upper right")
        out_shift = figures_dir / f"{case_prefix}.shift_impact.png"
        fig.tight_layout()
        fig.savefig(out_shift, dpi=160)
        plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--run_dir",
        required=True,
        help="run_results/<RUN_TAG> directory that contains 02_mixgraph_cache_*.log and *.csv",
    )
    ap.add_argument(
        "--figures_subdir",
        default="monitor_figures",
        help="subdir under run_dir to write PNGs",
    )
    ap.add_argument(
        "--analysis_subdir",
        default="monitor_analysis",
        help="subdir under run_dir to write derived CSVs",
    )
    args = ap.parse_args()

    run_dir = Path(args.run_dir)
    if not run_dir.exists():
        raise SystemExit(f"run_dir not found: {run_dir}")

    figures_dir = run_dir / args.figures_subdir
    analysis_dir = run_dir / args.analysis_subdir
    _ensure_dir(figures_dir)
    _ensure_dir(analysis_dir)

    window_csvs = sorted(run_dir.glob("02_mixgraph_cache_*.mix_monitor_window.csv"))
    if not window_csvs:
        raise SystemExit(f"no mix_monitor_window CSV found under: {run_dir}")

    for wcsv in window_csvs:
        case_prefix = _case_prefix_from_window_csv(wcsv)
        mix_log = run_dir / f"{case_prefix}.log"
        mix_stage = run_dir / f"{case_prefix}.mix_monitor_stage.csv"
        mix_events = run_dir / f"{case_prefix}.mix_events.csv"
        simfs_window = run_dir / f"{case_prefix}.simfs_window.csv"
        simfs_stage = run_dir / f"{case_prefix}.simfs_stage.csv"

        if not mix_stage.exists():
            print(f"[skip] missing: {mix_stage}")
            continue
        if not mix_events.exists():
            print(f"[skip] missing: {mix_events}")
            continue

        _plot_case(
            run_dir=run_dir,
            case_prefix=case_prefix,
            mix_log=mix_log,
            mix_window=wcsv,
            mix_stage=mix_stage,
            mix_events=mix_events,
            simfs_window=simfs_window if simfs_window.exists() else None,
            simfs_stage=simfs_stage if simfs_stage.exists() else None,
            figures_dir=figures_dir,
            analysis_dir=analysis_dir,
        )

    print(f"[ok] monitoring figures: {figures_dir}")
    print(f"[ok] monitoring analysis: {analysis_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

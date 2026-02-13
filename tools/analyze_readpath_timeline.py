#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

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

TARGETS = [
    "cpu_total_pct",
    "io_total_mb_s",
    "iostat_await_ms",
    "mem_rss_kb",
    "ops_per_sec",
    "seek_p99_us",
]

STAGE_RULES: List[Tuple[int, str, Tuple[str, ...]]] = [
    (10, "memtable_route", ("memtable", ".l0.", ".l1.", "l2andup", "superversion")),
    (20, "bloom_filter", ("bloom.filter", "filter.", "seek.filter")),
    (25, "table_open_meta", ("table.open", "no.file.opens")),
    (30, "index_lookup", ("block.cache.index", "block.cache.filter", "num.index.and.filter")),
    (34, "block_cache_read", ("block.cache.bytes.read", "block.cache.data.bytes.read", "block.cache.read.bytes")),
    (35, "seek_dispatch", ("db.seek", "number.db.seek", "seek.data")),
    (40, "key_compare", ("user_key", "comparison")),
    (50, "block_read_io", ("read.block", "sst.read", "file.read", "prefetch", "last.level.read", "bytes.read", "bytes.per.read")),
    (60, "block_decode_checksum", ("decompress", "compression", "checksum", "verify")),
    (70, "block_cache_insert", ("block.cache.add", "block.cache.bytes.write", "cache.data.add", "data.bytes.insert")),
    (80, "iter_merge_jump", ("db.next", "iterator", "multiscan", "reseeks.iteration", "iter.skip", "db.iter.bytes.read", "iter.bytes.read", "iter.read.bytes")),
    (90, "misc", tuple()),
]

STAGE_COLORS = {
    "memtable_route": "#4e79a7",
    "bloom_filter": "#f28e2b",
    "table_open_meta": "#af7aa1",
    "index_lookup": "#e15759",
    "block_cache_read": "#8cd17d",
    "seek_dispatch": "#76b7b2",
    "key_compare": "#59a14f",
    "block_read_io": "#edc948",
    "block_decode_checksum": "#b07aa1",
    "block_cache_insert": "#ff9da7",
    "iter_merge_jump": "#9c755f",
    "misc": "#bab0ab",
}

HIST_RE = re.compile(
    r"^(rocksdb\.[^\s]+)\s+P50\s*:.*COUNT\s*:\s*([0-9]+(?:\.[0-9]+)?)\s+SUM\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*$"
)
COUNT_RE = re.compile(r"^(rocksdb\.[^\s]+)\s+COUNT\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*$")


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


def is_finite(v: float) -> bool:
    return not math.isnan(v) and not math.isinf(v)


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for row in rows:
            w.writerow(row)


def scenario_key(s: str) -> int:
    if s in SCENARIO_ORDER:
        return SCENARIO_ORDER.index(s)
    return 999


def find_latest_run_dir(case_dir: Path) -> Optional[Path]:
    run_root = case_dir / "run_results"
    if not run_root.exists():
        return None
    dirs = [p for p in run_root.iterdir() if p.is_dir()]
    if not dirs:
        return None
    dirs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return dirs[0]


def classify_stage(event: str) -> Tuple[int, str]:
    s = event.lower()
    if "iter.bytes.read" in s or "db.iter.bytes.read" in s:
        return 80, "iter_merge_jump"
    if "block.cache.bytes.read" in s or "block.cache.data.bytes.read" in s or "block.cache.read.bytes" in s:
        return 34, "block_cache_read"
    if s.endswith("bytes.read") or s.endswith("bytes.per.read"):
        return 50, "block_read_io"
    for order, stage, keys in STAGE_RULES:
        if not keys:
            continue
        if any(k in s for k in keys):
            return order, stage
    return 90, "misc"


def parse_rocksdb_events(log_path: Path) -> Dict[str, Dict[str, float]]:
    out: Dict[str, Dict[str, float]] = {}
    if not log_path.exists():
        return out

    for line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        s = line.strip()
        if not s.startswith("rocksdb."):
            continue

        m_hist = HIST_RE.match(s)
        if m_hist:
            ev = m_hist.group(1)
            cnt = to_float(m_hist.group(2))
            ssum = to_float(m_hist.group(3))
            out[ev] = {
                "count": cnt,
                "sum": ssum,
                "has_sum": 1.0,
            }
            continue

        m_cnt = COUNT_RE.match(s)
        if m_cnt:
            ev = m_cnt.group(1)
            cnt = to_float(m_cnt.group(2))
            prev = out.get(ev)
            if prev is not None and prev.get("has_sum", 0.0) > 0:
                continue
            out[ev] = {
                "count": cnt,
                "sum": float("nan"),
                "has_sum": 0.0,
            }

    return out


def event_signal(event: str, count: float, ssum: float, operations: float) -> Tuple[str, float, float, float, float]:
    avg_time_us = float("nan")
    per_op_count = float("nan")
    per_op_bytes = float("nan")
    signal_type = "none"
    signal_value = float("nan")

    if is_finite(ssum) and is_finite(count) and count > 0:
        if ".nanos" in event:
            avg_time_us = (ssum / count) / 1000.0
            signal_type = "avg_time_us"
            signal_value = avg_time_us
        elif ".micros" in event:
            avg_time_us = ssum / count
            signal_type = "avg_time_us"
            signal_value = avg_time_us
        elif ".bytes" in event:
            if operations > 0:
                per_op_bytes = ssum / operations
                signal_type = "per_op_bytes"
                signal_value = per_op_bytes
        elif operations > 0:
            per_op_count = count / operations
            signal_type = "per_op_count"
            signal_value = per_op_count
    elif is_finite(count):
        if operations > 0 and ".bytes" in event:
            per_op_bytes = count / operations
            signal_type = "per_op_bytes"
            signal_value = per_op_bytes
        elif operations > 0:
            per_op_count = count / operations
            signal_type = "per_op_count"
            signal_value = per_op_count

    return signal_type, signal_value, avg_time_us, per_op_count, per_op_bytes


def build_ledger_map(rows: List[Dict[str, str]]) -> Dict[Tuple[str, str], Dict[str, str]]:
    out: Dict[Tuple[str, str], Dict[str, str]] = {}
    for r in rows:
        key = (str(r.get("label", "")), str(r.get("scenario", "")))
        out[key] = r
    return out


def rank_values(values: List[float]) -> List[float]:
    n = len(values)
    idx = list(range(n))
    idx.sort(key=lambda i: values[i])
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and values[idx[j + 1]] == values[idx[i]]:
            j += 1
        rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[idx[k]] = rank
        i = j + 1
    return ranks


def pearson_corr(x: List[float], y: List[float]) -> float:
    if len(x) < 2 or len(y) < 2 or len(x) != len(y):
        return float("nan")
    mx = sum(x) / len(x)
    my = sum(y) / len(y)
    num = 0.0
    dx2 = 0.0
    dy2 = 0.0
    for a, b in zip(x, y):
        da = a - mx
        db = b - my
        num += da * db
        dx2 += da * da
        dy2 += db * db
    den = math.sqrt(dx2 * dy2)
    if den == 0.0:
        return float("nan")
    return num / den


def spearman_corr(x: List[float], y: List[float]) -> float:
    if len(x) < 2 or len(y) < 2 or len(x) != len(y):
        return float("nan")
    return pearson_corr(rank_values(x), rank_values(y))


def build_event_samples(matrix_dir: Path, ledger_map: Dict[Tuple[str, str], Dict[str, str]]) -> List[Dict[str, object]]:
    reg = read_csv(matrix_dir / "run_registry.csv")
    out: List[Dict[str, object]] = []

    for case in reg:
        if str(case.get("status", "")).lower() != "done":
            continue
        label = str(case.get("label", ""))
        phase = str(case.get("phase", ""))
        seq = to_int(case.get("seq", "0"))
        case_dir = Path(str(case.get("experiment_dir", "")))
        metrics_table = case_dir / "figures" / "metrics_table.csv"
        if not metrics_table.exists():
            continue

        run_dir = find_latest_run_dir(case_dir)
        if run_dir is None:
            continue

        for m in read_csv(metrics_table):
            scenario = str(m.get("scenario", "")).strip()
            if not scenario or scenario == "final_stats":
                continue
            log_file = str(m.get("log_file", "")).strip()
            if not log_file:
                continue
            log_path = run_dir / log_file
            if not log_path.exists():
                continue

            ops = to_float(m.get("operations", ""))
            if not is_finite(ops) or ops <= 0:
                continue

            l = ledger_map.get((label, scenario), {})
            cpu_user = to_float(l.get("cpu_user_pct", ""))
            cpu_sys = to_float(l.get("cpu_sys_pct", ""))
            cpu_total = cpu_user + cpu_sys if is_finite(cpu_user) and is_finite(cpu_sys) else float("nan")
            io_r = to_float(l.get("iostat_read_mb_s", ""))
            io_w = to_float(l.get("iostat_write_mb_s", ""))
            io_total = io_r + io_w if is_finite(io_r) and is_finite(io_w) else float("nan")

            events = parse_rocksdb_events(log_path)
            for ev, rec in events.items():
                cnt = to_float(rec.get("count", float("nan")))
                ssum = to_float(rec.get("sum", float("nan")))
                signal_type, signal_value, avg_time_us, per_op_count, per_op_bytes = event_signal(ev, cnt, ssum, ops)
                if not is_finite(signal_value) or signal_value <= 0:
                    continue
                stage_order, stage = classify_stage(ev)
                out.append(
                    {
                        "seq": seq,
                        "label": label,
                        "phase": phase,
                        "scenario": scenario,
                        "event": ev,
                        "stage_order": stage_order,
                        "stage": stage,
                        "operations": ops,
                        "event_count": cnt,
                        "event_sum": ssum,
                        "signal_type": signal_type,
                        "signal_value": signal_value,
                        "avg_time_us": avg_time_us,
                        "per_op_count": per_op_count,
                        "per_op_bytes": per_op_bytes,
                        "cpu_total_pct": cpu_total,
                        "io_total_mb_s": io_total,
                        "iostat_await_ms": to_float(l.get("iostat_await_ms", "")),
                        "mem_rss_kb": to_float(l.get("mem_rss_kb", "")),
                        "ops_per_sec": to_float(l.get("ops_per_sec", m.get("ops_per_sec", ""))),
                        "seek_p99_us": to_float(l.get("seek_p99_us", m.get("seek_p99_us", ""))),
                    }
                )

    out.sort(key=lambda r: (to_int(r["seq"]), scenario_key(str(r["scenario"])), str(r["event"])))
    return out


def build_relevance(samples: List[Dict[str, object]], corr_threshold: float, min_points: int) -> List[Dict[str, object]]:
    by_event: Dict[str, List[Dict[str, object]]] = {}
    for s in samples:
        by_event.setdefault(str(s["event"]), []).append(s)

    out: List[Dict[str, object]] = []
    for event, rows in by_event.items():
        signal_type = str(rows[0]["signal_type"])
        rec: Dict[str, object] = {
            "event": event,
            "signal_type": signal_type,
            "samples": len(rows),
        }
        best_target = ""
        best_abs = -1.0
        best_rho = float("nan")
        best_n = 0

        for tgt in TARGETS:
            x: List[float] = []
            y: List[float] = []
            for r in rows:
                xv = to_float(r.get("signal_value", float("nan")))
                yv = to_float(r.get(tgt, float("nan")))
                if is_finite(xv) and is_finite(yv):
                    x.append(xv)
                    y.append(yv)
            rho = spearman_corr(x, y) if len(x) >= min_points else float("nan")
            rec[f"rho_{tgt}"] = rho
            rec[f"n_{tgt}"] = len(x)
            if is_finite(rho):
                abs_rho = abs(rho)
                if abs_rho > best_abs:
                    best_abs = abs_rho
                    best_target = tgt
                    best_rho = rho
                    best_n = len(x)

        rec["best_target"] = best_target
        rec["best_rho"] = best_rho
        rec["best_abs_rho"] = best_abs if best_abs >= 0 else float("nan")
        rec["best_n"] = best_n
        rec["selected"] = 1 if is_finite(best_rho) and best_n >= min_points and abs(best_rho) >= corr_threshold else 0
        out.append(rec)

    out.sort(key=lambda r: (0 if int(r["selected"]) == 1 else 1, -to_float(r["best_abs_rho"])))
    return out


def build_timeline(samples: List[Dict[str, object]], relevance_rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    rel = {str(r["event"]): r for r in relevance_rows if int(r.get("selected", 0)) == 1}

    by_key: Dict[Tuple[str, str, str], List[Dict[str, object]]] = {}
    for s in samples:
        event = str(s["event"])
        r = rel.get(event)
        if r is None:
            continue
        signal_type = str(s["signal_type"])
        lane = "count"
        if signal_type == "avg_time_us":
            lane = "time"
        elif signal_type == "per_op_bytes":
            lane = "bandwidth"
        key = (str(s["label"]), str(s["scenario"]), lane)
        merged = dict(s)
        merged["best_target"] = r["best_target"]
        merged["best_rho"] = r["best_rho"]
        by_key.setdefault(key, []).append(merged)

    out: List[Dict[str, object]] = []
    for (label, scenario, lane), rows in by_key.items():
        prepared: List[Tuple[Dict[str, object], float, float]] = []
        for r in rows:
            v = to_float(r["signal_value"])
            if not is_finite(v) or v <= 0:
                continue
            abs_value = v
            if lane == "bandwidth":
                # per_op_bytes -> MB/s
                ops_per_sec = to_float(r.get("ops_per_sec", float("nan")))
                if is_finite(ops_per_sec):
                    abs_value = v * ops_per_sec / 1_000_000.0
                else:
                    abs_value = float("nan")
            elif lane == "time":
                # avg_time_us -> estimated us/op contribution (avg_time_us * count/op)
                cnt = to_float(r.get("event_count", float("nan")))
                ops = to_float(r.get("operations", float("nan")))
                if is_finite(cnt) and is_finite(ops) and ops > 0:
                    abs_value = v * (cnt / ops)
                else:
                    abs_value = float("nan")
            prepared.append((r, v, abs_value))

        abs_positive = [a for _, _, a in prepared if is_finite(a) and a > 0]
        total_abs = sum(abs_positive)
        if total_abs <= 0:
            continue

        prepared.sort(key=lambda x: (to_int(x[0]["stage_order"]), -x[2] if is_finite(x[2]) else -x[1]))
        for idx, (r, v, abs_value) in enumerate(prepared, start=1):

            out.append(
                {
                    "seq": r["seq"],
                    "label": label,
                    "phase": r["phase"],
                    "scenario": scenario,
                    "lane": lane,
                    "event_order": idx,
                    "stage_order": r["stage_order"],
                    "stage": r["stage"],
                    "event": r["event"],
                    "signal_type": r["signal_type"],
                    "signal_value": v,
                    "abs_value": abs_value,
                    "share_pct": abs_value * 100.0 / total_abs if is_finite(abs_value) and abs_value > 0 else float("nan"),
                    "best_target": r["best_target"],
                    "best_rho": r["best_rho"],
                }
            )

    out.sort(key=lambda r: (to_int(r["seq"]), scenario_key(str(r["scenario"])), str(r["lane"]), to_int(r["event_order"])))
    return out


def build_cpu_timeline(
    samples: List[Dict[str, object]],
    relevance_rows: List[Dict[str, object]],
    corr_threshold: float,
    min_points: int,
) -> List[Dict[str, object]]:
    rel = {str(r["event"]): r for r in relevance_rows}
    candidates: List[Dict[str, object]] = []
    for s in samples:
        event = str(s.get("event", ""))
        rr = rel.get(event)
        if rr is None:
            continue
        rho_cpu = to_float(rr.get("rho_cpu_total_pct", float("nan")))
        n_cpu = to_int(rr.get("n_cpu_total_pct", 0))
        if not is_finite(rho_cpu) or n_cpu < min_points or abs(rho_cpu) < corr_threshold:
            continue

        signal_type = str(s.get("signal_type", ""))
        avg_time_us = to_float(s.get("avg_time_us", float("nan")))
        per_op_bytes = to_float(s.get("per_op_bytes", float("nan")))
        per_op_count = to_float(s.get("per_op_count", float("nan")))
        signal_value = to_float(s.get("signal_value", float("nan")))
        cnt = to_float(s.get("event_count", float("nan")))
        ops = to_float(s.get("operations", float("nan")))
        cpu_total = to_float(s.get("cpu_total_pct", float("nan")))
        if not (is_finite(cpu_total) and cpu_total > 0):
            continue

        base_value = float("nan")
        if signal_type == "avg_time_us" and is_finite(avg_time_us) and is_finite(cnt) and is_finite(ops) and ops > 0:
            base_value = avg_time_us * (cnt / ops)
        elif signal_type == "per_op_bytes" and is_finite(per_op_bytes):
            base_value = per_op_bytes
        elif signal_type == "per_op_count" and is_finite(per_op_count):
            base_value = per_op_count
        elif is_finite(signal_value):
            base_value = signal_value
        if not is_finite(base_value) or base_value <= 0:
            continue

        merged = dict(s)
        merged["rho_cpu_total_pct"] = rho_cpu
        merged["n_cpu_total_pct"] = n_cpu
        merged["cpu_base_value"] = base_value
        candidates.append(merged)

    baseline_map: Dict[Tuple[str, str], float] = {}
    by_event_scenario: Dict[Tuple[str, str], List[float]] = {}
    for c in candidates:
        k = (str(c["event"]), str(c["scenario"]))
        by_event_scenario.setdefault(k, []).append(to_float(c["cpu_base_value"]))
    for k, vals in by_event_scenario.items():
        finite_vals = [v for v in vals if is_finite(v) and v > 0]
        if finite_vals:
            baseline_map[k] = float(np.median(np.array(finite_vals)))

    by_key: Dict[Tuple[str, str], List[Dict[str, object]]] = {}
    for c in candidates:
        ek = (str(c["event"]), str(c["scenario"]))
        baseline = baseline_map.get(ek, float("nan"))
        base_value = to_float(c.get("cpu_base_value", float("nan")))
        rho_cpu = to_float(c.get("rho_cpu_total_pct", float("nan")))
        if not (is_finite(baseline) and baseline > 0 and is_finite(base_value) and base_value > 0 and is_finite(rho_cpu)):
            continue
        raw_cpu_score = (base_value / baseline) * abs(rho_cpu)
        if not is_finite(raw_cpu_score) or raw_cpu_score <= 0:
            continue
        c2 = dict(c)
        c2["raw_cpu_score"] = raw_cpu_score
        key = (str(c2["label"]), str(c2["scenario"]))
        by_key.setdefault(key, []).append(c2)

    out: List[Dict[str, object]] = []
    for (label, scenario), rows in by_key.items():
        cpu_total = to_float(rows[0].get("cpu_total_pct", float("nan")))
        total_raw = sum(to_float(r.get("raw_cpu_score", float("nan"))) for r in rows if is_finite(to_float(r.get("raw_cpu_score", float("nan")))))
        if not is_finite(cpu_total) or cpu_total <= 0 or total_raw <= 0:
            continue

        rows.sort(key=lambda r: (to_int(r["stage_order"]), -to_float(r["raw_cpu_score"])))
        for idx, r in enumerate(rows, start=1):
            raw = to_float(r["raw_cpu_score"])
            abs_value = raw * cpu_total / total_raw
            if not is_finite(abs_value) or abs_value <= 0:
                continue
            out.append(
                {
                    "seq": r["seq"],
                    "label": label,
                    "phase": r["phase"],
                    "scenario": scenario,
                    "lane": "cpu",
                    "event_order": idx,
                    "stage_order": r["stage_order"],
                    "stage": r["stage"],
                    "event": r["event"],
                    "signal_type": r["signal_type"],
                    "signal_value": r["cpu_base_value"],
                    "abs_value": abs_value,
                    "share_pct": abs_value * 100.0 / cpu_total,
                    "best_target": "cpu_total_pct",
                    "best_rho": r["rho_cpu_total_pct"],
                }
            )

    out.sort(key=lambda r: (to_int(r["seq"]), scenario_key(str(r["scenario"])), str(r["lane"]), to_int(r["event_order"])))
    return out


def plot_phase_stage_stack(
    timeline_rows: List[Dict[str, object]], phase: str, lane: str, out_png: Path
) -> bool:
    subset = [r for r in timeline_rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph" and str(r["lane"]) == lane]
    if not subset:
        return False

    labels = sorted({str(r["label"]) for r in subset}, key=lambda x: to_int(next(str(z["seq"]) for z in subset if str(z["label"]) == x)))
    stages = sorted({str(r["stage"]) for r in subset}, key=lambda st: next((o for o, s, _ in STAGE_RULES if s == st), 999))
    if not labels or not stages:
        return False

    mat_share = np.zeros((len(stages), len(labels)), dtype=float)
    mat_abs = np.zeros((len(stages), len(labels)), dtype=float)
    for i, st in enumerate(stages):
        for j, lb in enumerate(labels):
            vals_share = [to_float(r["share_pct"]) for r in subset if str(r["stage"]) == st and str(r["label"]) == lb]
            vals_abs = [to_float(r["abs_value"]) for r in subset if str(r["stage"]) == st and str(r["label"]) == lb]
            mat_share[i, j] = sum(v for v in vals_share if is_finite(v))
            mat_abs[i, j] = sum(v for v in vals_abs if is_finite(v))

    x = np.arange(len(labels))
    fig, (ax_abs, ax_share) = plt.subplots(2, 1, figsize=(max(9, 1.1 * len(labels)), 9.5), sharex=True)
    bottom_abs = np.zeros(len(labels), dtype=float)
    bottom_share = np.zeros(len(labels), dtype=float)

    for i, st in enumerate(stages):
        vals_abs = mat_abs[i, :]
        vals_share = mat_share[i, :]
        ax_abs.bar(x, vals_abs, bottom=bottom_abs, color=STAGE_COLORS.get(st, "#999999"), label=st, alpha=0.9)
        ax_share.bar(x, vals_share, bottom=bottom_share, color=STAGE_COLORS.get(st, "#999999"), label=st, alpha=0.9)
        bottom_abs += vals_abs
        bottom_share += vals_share

    abs_unit = "MB/s" if lane == "bandwidth" else ("cpu pct points" if lane == "cpu" else "us/op")
    ax_abs.set_title(f"{phase}: mixgraph read-path {lane} stage stack (absolute)")
    ax_abs.set_ylabel(abs_unit)
    ax_abs.grid(axis="y", alpha=0.25)
    ax_abs.legend(ncol=3, fontsize=8)

    ax_share.set_title(f"{phase}: mixgraph read-path {lane} stage stack (relative)")
    ax_share.set_xticks(x)
    ax_share.set_xticklabels(labels)
    ax_share.set_ylabel("share (%)")
    ax_share.grid(axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_relevance_heatmap(relevance_rows: List[Dict[str, object]], out_png: Path, topn: int = 20) -> bool:
    selected = [r for r in relevance_rows if int(r.get("selected", 0)) == 1 and is_finite(to_float(r.get("best_abs_rho", float("nan"))))]
    if not selected:
        return False
    selected.sort(key=lambda r: -to_float(r["best_abs_rho"]))
    selected = selected[:topn]

    targets = TARGETS
    events = [str(r["event"]) for r in selected]
    mat = np.full((len(events), len(targets)), np.nan, dtype=float)
    for i, r in enumerate(selected):
        for j, t in enumerate(targets):
            mat[i, j] = to_float(r.get(f"rho_{t}", float("nan")))

    fig, ax = plt.subplots(figsize=(1.5 * len(targets) + 4, 0.45 * len(events) + 3))
    cmap = plt.get_cmap("RdBu").copy()
    cmap.set_bad(color="#f2f2f2")
    im = ax.imshow(mat, aspect="auto", cmap=cmap, vmin=-1.0, vmax=1.0)
    ax.set_title("Event-Target Spearman Correlation")
    ax.set_xticks(np.arange(len(targets)))
    ax.set_xticklabels(targets, rotation=25, ha="right")
    ax.set_yticks(np.arange(len(events)))
    ax.set_yticklabels(events)

    for i in range(mat.shape[0]):
        for j in range(mat.shape[1]):
            v = mat[i, j]
            txt = "NA" if math.isnan(v) else f"{v:.2f}"
            ax.text(j, i, txt, ha="center", va="center", fontsize=7)

    cbar = plt.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("rho", fontsize=9)
    plt.tight_layout()
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def write_report(
    out_md: Path,
    samples: List[Dict[str, object]],
    relevance_rows: List[Dict[str, object]],
    timeline_rows: List[Dict[str, object]],
    corr_threshold: float,
    min_points: int,
    generated: List[str],
) -> None:
    lines: List[str] = []
    lines.append("# 读链路事件时序分析")
    lines.append("")
    lines.append("## 方法")
    lines.append("- 自动抽取 run_results 日志中的全部 `rocksdb.*` 事件（COUNT 与 P50/COUNT/SUM）。")
    lines.append("- 事件信号优先级：`avg_time_us` > `per_op_bytes` > `per_op_count`。")
    lines.append(f"- 相关性筛选：Spearman，阈值 |rho| >= {corr_threshold:.2f}，最小样本数 >= {min_points}。")
    lines.append("- 仅相关事件纳入时序与分摊统计，弱相关事件不纳入。")
    lines.append("- 阶段图同时给出绝对值（上图）与相对占比（下图），避免“仅看占比”误判。")
    lines.append("")
    lines.append("## 规模")
    lines.append(f"- 事件样本行数: {len(samples)}")
    lines.append(f"- 事件种类数: {len(set(str(x['event']) for x in samples))}")
    lines.append(f"- 入围事件数: {len([r for r in relevance_rows if int(r.get('selected', 0)) == 1])}")
    lines.append("")

    top_rel = [r for r in relevance_rows if int(r.get("selected", 0)) == 1]
    top_rel.sort(key=lambda r: -to_float(r.get("best_abs_rho", float("nan"))))
    lines.append("## 相关性最强事件（Top 15）")
    for r in top_rel[:15]:
        lines.append(
            "- "
            + f"{r['event']} [{r['signal_type']}]: target={r['best_target']}, "
            + f"rho={to_float(r['best_rho']):.3f}, n={to_int(r['best_n'])}"
        )
    if not top_rel:
        lines.append("- 无事件通过相关性阈值。")
    lines.append("")

    lines.append("## mixgraph 时序（按 case）")
    keys = sorted(
        {(str(r["label"]), str(r["phase"])) for r in timeline_rows if str(r["scenario"]) == "mixgraph" and str(r["lane"]) == "time"},
        key=lambda x: next(to_int(r["seq"]) for r in timeline_rows if str(r["label"]) == x[0]),
    )
    for label, phase in keys:
        rows = [r for r in timeline_rows if str(r["label"]) == label and str(r["scenario"]) == "mixgraph" and str(r["lane"]) == "time"]
        rows.sort(key=lambda r: (to_int(r["stage_order"]), -to_float(r["share_pct"])))
        desc = " -> ".join(f"{r['stage']}:{r['event']}({to_float(r['share_pct']):.1f}%)" for r in rows[:8])
        lines.append(f"- {label} ({phase}): {desc if desc else 'NA'}")
    lines.append("")

    lines.append("## 输出产物")
    for g in generated:
        lines.append(f"- {g}")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Auto-discover read-path events and build relevance-filtered timeline analysis.")
    ap.add_argument("--matrix-dir", required=True)
    ap.add_argument("--ledger-csv", default="")
    ap.add_argument("--out-dir", default="")
    ap.add_argument("--corr-threshold", type=float, default=0.45)
    ap.add_argument("--cpu-corr-threshold", type=float, default=0.45)
    ap.add_argument("--min-points", type=int, default=6)
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    ledger_csv = Path(args.ledger_csv).resolve() if args.ledger_csv else (matrix_dir / "analysis" / "resource_ledger" / "resource_ledger.csv")
    out_dir = Path(args.out_dir).resolve() if args.out_dir else (matrix_dir / "analysis" / "resource_ledger")
    out_dir.mkdir(parents=True, exist_ok=True)

    if not ledger_csv.exists():
        raise FileNotFoundError(f"missing ledger csv: {ledger_csv}")
    if not (matrix_dir / "run_registry.csv").exists():
        raise FileNotFoundError(f"missing run_registry.csv under {matrix_dir}")

    ledger_rows = read_csv(ledger_csv)
    ledger_map = build_ledger_map(ledger_rows)
    samples = build_event_samples(matrix_dir, ledger_map)
    if not samples:
        raise RuntimeError("no read-path event samples extracted")

    relevance_rows = build_relevance(samples, args.corr_threshold, args.min_points)
    timeline_rows = build_timeline(samples, relevance_rows)
    cpu_timeline_rows = build_cpu_timeline(samples, relevance_rows, args.cpu_corr_threshold, args.min_points)
    timeline_rows.extend(cpu_timeline_rows)

    generated: List[str] = []
    write_csv(out_dir / "readpath_event_samples.csv", samples)
    generated.append("readpath_event_samples.csv")
    write_csv(out_dir / "event_metric_relevance.csv", relevance_rows)
    generated.append("event_metric_relevance.csv")
    write_csv(out_dir / "readpath_timeline_events.csv", timeline_rows)
    generated.append("readpath_timeline_events.csv")

    for phase in ["cache_sweep", "thread_sweep", "locality_sweep"]:
        fname = {
            "cache_sweep": "phaseA_readpath_time_stage_stack.png",
            "thread_sweep": "phaseB_readpath_time_stage_stack.png",
            "locality_sweep": "phaseC_readpath_time_stage_stack.png",
        }[phase]
        if plot_phase_stage_stack(timeline_rows, phase, "time", out_dir / fname):
            generated.append(fname)
        bname = {
            "cache_sweep": "phaseA_readpath_bandwidth_stage_stack.png",
            "thread_sweep": "phaseB_readpath_bandwidth_stage_stack.png",
            "locality_sweep": "phaseC_readpath_bandwidth_stage_stack.png",
        }[phase]
        if plot_phase_stage_stack(timeline_rows, phase, "bandwidth", out_dir / bname):
            generated.append(bname)
        cname = {
            "cache_sweep": "phaseA_readpath_cpu_stage_stack.png",
            "thread_sweep": "phaseB_readpath_cpu_stage_stack.png",
            "locality_sweep": "phaseC_readpath_cpu_stage_stack.png",
        }[phase]
        if plot_phase_stage_stack(timeline_rows, phase, "cpu", out_dir / cname):
            generated.append(cname)

    if plot_relevance_heatmap(relevance_rows, out_dir / "event_relevance_heatmap.png"):
        generated.append("event_relevance_heatmap.png")

    write_report(
        out_dir / "readpath_timeline_report.md",
        samples,
        relevance_rows,
        timeline_rows,
        args.corr_threshold,
        args.min_points,
        generated,
    )
    generated.append("readpath_timeline_report.md")

    print(f"readpath timeline analysis generated: {out_dir}")
    print(f"samples={len(samples)}, events={len(set(str(r['event']) for r in samples))}, selected={len([r for r in relevance_rows if int(r.get('selected', 0)) == 1])}")
    print("generated=" + ",".join(generated))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

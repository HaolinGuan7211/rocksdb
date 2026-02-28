#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import shlex
import statistics
import subprocess
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt
import numpy as np


SEEK_LAT_RE = (
    "rocksdb.db.seek.micros P50 : "
)

STEP_SCENARIO = {
    "02": "mixgraph",
    "03": "seek200",
    "04": "worst_seek1",
    "05": "worst_seek4",
    "06": "worst_seek20",
    "07": "worst_seek200",
    "08": "worst_seek10000",
}

PHASE_PREFIX = {
    "cache_sweep": "A",
    "thread_sweep": "B",
    "locality_sweep": "C",
}

STAGES = [
    "memtable_route",
    "table_open_meta",
    "index_lookup",
    "seek_dispatch",
    "block_read_io",
    "block_decode_checksum",
    "post_process",
    "cpu_iter_seek",
    "cpu_get",
    "unattributed",
]

STAGE_COLORS = {
    "memtable_route": "#4e79a7",
    "table_open_meta": "#af7aa1",
    "index_lookup": "#e15759",
    "seek_dispatch": "#76b7b2",
    "block_read_io": "#edc948",
    "block_decode_checksum": "#b07aa1",
    "post_process": "#59a14f",
    "cpu_iter_seek": "#f28e2b",
    "cpu_get": "#9c755f",
    "unattributed": "#bab0ab",
}

TAIL_LAT_RATIO_BUCKETS: List[Tuple[float, float, str]] = [
    (1.0, 1.2, "1.0-1.2x"),
    (1.2, 1.5, "1.2-1.5x"),
    (1.5, 2.0, "1.5-2.0x"),
    (2.0, 3.0, "2.0-3.0x"),
    (3.0, 5.0, "3.0-5.0x"),
    (5.0, float("inf"), ">=5.0x"),
]


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


def find_latest_run_dir(case_dir: Path) -> Optional[Path]:
    run_root = case_dir / "run_results"
    if not run_root.exists():
        return None
    dirs = [x for x in run_root.iterdir() if x.is_dir()]
    if not dirs:
        return None
    dirs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return dirs[0]


def parse_seek_p99_from_log(log_path: Path) -> Optional[float]:
    if not log_path.exists():
        return None
    p99_value: Optional[float] = None
    for raw in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        s = raw.strip()
        if not s.startswith(SEEK_LAT_RE):
            continue
        # Format:
        # rocksdb.db.seek.micros P50 : x P95 : y P99 : z P100 : ...
        parts = s.split()
        try:
            idx = parts.index("P99")
            # tokens: P99 : <value>
            if idx + 2 < len(parts):
                p99_value = float(parts[idx + 2])
        except (ValueError, IndexError):
            continue
    return p99_value


def step_to_scenario(step_id: str) -> str:
    return STEP_SCENARIO.get(step_id, f"step_{step_id}")


def phase_to_output_prefix(phase: str) -> str:
    return PHASE_PREFIX.get(phase, "X")


def choose_case_labels(registry_rows: List[Dict[str, str]], case_filter: List[str]) -> List[str]:
    available = {str(r.get("label", "")): r for r in registry_rows if str(r.get("label", ""))}
    if case_filter:
        return [c for c in case_filter if c in available]

    picked: List[str] = []
    pref = {
        "cache_sweep": "A2",
        "thread_sweep": "B2",
        "locality_sweep": "C2",
    }
    by_phase: Dict[str, List[Dict[str, str]]] = {}
    for r in registry_rows:
        phase = str(r.get("phase", ""))
        by_phase.setdefault(phase, []).append(r)

    for phase in ["cache_sweep", "thread_sweep", "locality_sweep"]:
        rows = by_phase.get(phase, [])
        if not rows:
            continue
        rows = [r for r in rows if str(r.get("status", "")) in ("done", "skipped")]
        if not rows:
            continue
        rows.sort(key=lambda x: to_int(x.get("seq", 0)))
        wanted = pref[phase]
        chosen = None
        for r in rows:
            if str(r.get("label", "")) == wanted:
                chosen = wanted
                break
        if chosen is None:
            chosen = str(rows[len(rows) // 2].get("label", ""))
        if chosen and chosen not in picked:
            picked.append(chosen)
    return picked


def load_registry(matrix_dir: Path) -> List[Dict[str, str]]:
    rows = read_csv(matrix_dir / "run_registry.csv")
    out: List[Dict[str, str]] = []
    for r in rows:
        label = str(r.get("label", ""))
        phase = str(r.get("phase", ""))
        exp_dir = str(r.get("experiment_dir", ""))
        if not label or not phase or not exp_dir:
            continue
        out.append(r)
    return out


def read_cmd_tokens(cmd_file: Path) -> List[str]:
    text = cmd_file.read_text(encoding="utf-8", errors="ignore").strip()
    if not text:
        return []
    return shlex.split(text)


def remove_existing_tail_flags(tokens: List[str]) -> List[str]:
    skip_prefixes = (
        "--tail_probe_output",
        "--tail_probe_threshold_us",
        "--tail_probe_max_samples",
        "--tail_probe_case_label",
        "--tail_probe_scenario",
    )
    out: List[str] = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if any(tok.startswith(p + "=") for p in skip_prefixes):
            i += 1
            continue
        if tok in skip_prefixes:
            i += 2
            continue
        out.append(tok)
        i += 1
    return out


def ensure_perf_level(tokens: List[str], min_level: int = 6) -> List[str]:
    out: List[str] = []
    found = False
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok.startswith("--perf_level="):
            found = True
            cur = to_int(tok.split("=", 1)[1])
            out.append(f"--perf_level={max(cur, min_level)}")
            i += 1
            continue
        if tok == "--perf_level" and i + 1 < len(tokens):
            found = True
            cur = to_int(tokens[i + 1])
            out.append("--perf_level")
            out.append(str(max(cur, min_level)))
            i += 2
            continue
        out.append(tok)
        i += 1
    if not found:
        out.append(f"--perf_level={min_level}")
    return out


def run_and_log(tokens: List[str], log_path: Path, cwd: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as f:
        f.write("# cmd: " + " ".join(shlex.quote(t) for t in tokens) + "\n")
        f.flush()
        proc = subprocess.run(tokens, cwd=str(cwd), stdout=f, stderr=subprocess.STDOUT, check=False)
        return proc.returncode


def ns_to_us(v: float) -> float:
    if not is_finite(v):
        return float("nan")
    return v / 1000.0


def nonneg(v: float) -> float:
    if not is_finite(v) or v <= 0:
        return 0.0
    return v


def cap_to_remaining(value: float, remaining: float) -> float:
    if not is_finite(value) or value <= 0:
        return 0.0
    if not is_finite(remaining) or remaining <= 0:
        return 0.0
    return min(value, remaining)


def compute_stage_values(row: Dict[str, str]) -> Dict[str, float]:
    latency_us = to_float(row.get("latency_us", ""))
    raw_memtable_route = ns_to_us(to_float(row.get("delta_seek_on_memtable_time_ns", "")))
    raw_table_open_meta = ns_to_us(
        to_float(row.get("delta_find_table_nanos", ""))
        + to_float(row.get("delta_new_table_iterator_nanos", ""))
    )
    raw_index_lookup = ns_to_us(
        to_float(row.get("delta_read_index_block_nanos", ""))
        + to_float(row.get("delta_read_filter_block_nanos", ""))
        + to_float(row.get("delta_new_table_block_iter_nanos", ""))
    )
    raw_seek_dispatch = ns_to_us(
        to_float(row.get("delta_block_seek_nanos", ""))
        + to_float(row.get("delta_seek_child_seek_time_ns", ""))
        + to_float(row.get("delta_seek_internal_seek_time_ns", ""))
        + to_float(row.get("delta_seek_min_heap_time_ns", ""))
        + to_float(row.get("delta_seek_max_heap_time_ns", ""))
        + to_float(row.get("delta_find_next_user_entry_time_ns", ""))
    )
    raw_block_read_io = ns_to_us(
        to_float(row.get("delta_block_read_time_ns", ""))
        + to_float(row.get("delta_get_from_output_files_time_ns", ""))
    )
    raw_block_decode_checksum = ns_to_us(
        to_float(row.get("delta_block_decompress_time_ns", ""))
        + to_float(row.get("delta_block_checksum_time_ns", ""))
    )
    raw_post_process = ns_to_us(to_float(row.get("delta_get_post_process_time_ns", "")))
    raw_cpu_iter_seek = ns_to_us(to_float(row.get("delta_iter_seek_cpu_nanos", "")))
    raw_cpu_get = ns_to_us(to_float(row.get("delta_get_cpu_nanos", "")))

    # Convert the mixed inclusive counters into an approximate exclusive view.
    # Child stages are stripped from parent stages before final capping.
    memtable_route = nonneg(raw_memtable_route)
    table_open_meta = nonneg(raw_table_open_meta)
    block_decode_checksum = nonneg(raw_block_decode_checksum)
    block_read_io = max(nonneg(raw_block_read_io) - block_decode_checksum, 0.0)
    index_lookup = max(nonneg(raw_index_lookup) - nonneg(raw_block_read_io), 0.0)
    post_process = nonneg(raw_post_process)
    cpu_iter_seek = nonneg(raw_cpu_iter_seek)
    cpu_get = nonneg(raw_cpu_get)
    seek_dispatch = max(
        nonneg(raw_seek_dispatch)
        - nonneg(raw_index_lookup)
        - nonneg(raw_block_read_io)
        - post_process,
        0.0,
    )

    unattributed = float("nan")
    if is_finite(latency_us) and latency_us >= 0:
        remaining = latency_us
        memtable_route = cap_to_remaining(memtable_route, remaining)
        remaining -= memtable_route
        table_open_meta = cap_to_remaining(table_open_meta, remaining)
        remaining -= table_open_meta
        block_decode_checksum = cap_to_remaining(block_decode_checksum, remaining)
        remaining -= block_decode_checksum
        block_read_io = cap_to_remaining(block_read_io, remaining)
        remaining -= block_read_io
        index_lookup = cap_to_remaining(index_lookup, remaining)
        remaining -= index_lookup
        post_process = cap_to_remaining(post_process, remaining)
        remaining -= post_process
        seek_dispatch = cap_to_remaining(seek_dispatch, remaining)
        remaining -= seek_dispatch
        cpu_iter_seek = cap_to_remaining(cpu_iter_seek, remaining)
        remaining -= cpu_iter_seek
        cpu_get = cap_to_remaining(cpu_get, remaining)
        remaining -= cpu_get
        unattributed = max(remaining, 0.0)

    return {
        "memtable_route": memtable_route,
        "table_open_meta": table_open_meta,
        "index_lookup": index_lookup,
        "seek_dispatch": seek_dispatch,
        "block_read_io": block_read_io,
        "block_decode_checksum": block_decode_checksum,
        "post_process": post_process,
        "cpu_iter_seek": cpu_iter_seek,
        "cpu_get": cpu_get,
        "unattributed": unattributed,
    }


def build_sample_row(
    label: str,
    phase: str,
    scenario: str,
    threshold_us: int,
    tr: Dict[str, str],
) -> Dict[str, object]:
    stage = compute_stage_values(tr)
    io_read_nanos = to_float(tr.get("delta_io_read_nanos", ""))
    io_cpu_read_nanos = to_float(tr.get("delta_io_cpu_read_nanos", ""))
    io_wait_us = float("nan")
    if is_finite(io_read_nanos) and is_finite(io_cpu_read_nanos):
        io_wait_us = max((io_read_nanos - io_cpu_read_nanos) / 1000.0, 0.0)
    return {
        "label": label,
        "phase": phase,
        "scenario": scenario,
        "sample_id": to_int(tr.get("sample_id", 0)),
        "wall_time_us": to_int(tr.get("wall_time_us", 0)),
        "thread_id": to_int(tr.get("thread_id", 0)),
        "latency_us": to_float(tr.get("latency_us", "")),
        "threshold_us": threshold_us,
        "stage_memtable_route_us": stage["memtable_route"],
        "stage_table_open_meta_us": stage["table_open_meta"],
        "stage_index_lookup_us": stage["index_lookup"],
        "stage_seek_dispatch_us": stage["seek_dispatch"],
        "stage_block_read_io_us": stage["block_read_io"],
        "stage_block_decode_checksum_us": stage["block_decode_checksum"],
        "stage_post_process_us": stage["post_process"],
        "stage_cpu_iter_seek_us": stage["cpu_iter_seek"],
        "stage_cpu_get_us": stage["cpu_get"],
        "stage_unattributed_us": stage["unattributed"],
        "io_wait_us": io_wait_us,
        "io_read_bytes": to_float(tr.get("delta_io_bytes_read", "")),
        "io_write_bytes": to_float(tr.get("delta_io_bytes_written", "")),
        "cpu_read_us": ns_to_us(to_float(tr.get("delta_io_cpu_read_nanos", ""))),
    }


def parse_tail_sample_filename(path: Path) -> Optional[Tuple[str, str]]:
    stem = path.stem
    suffix = "_tail_samples"
    if not stem.endswith(suffix):
        return None
    prefix = stem[: -len(suffix)]
    sep = prefix.find("_")
    if sep <= 0 or sep + 1 >= len(prefix):
        return None
    return prefix[:sep], prefix[sep + 1 :]


def summarize_baseline(p99_list: List[float]) -> Dict[str, float]:
    arr = [x for x in p99_list if is_finite(x)]
    if not arr:
        return {
            "runs": 0,
            "mean": float("nan"),
            "std": float("nan"),
            "median": float("nan"),
            "ci95": float("nan"),
            "cv": float("nan"),
        }
    mean = statistics.mean(arr)
    std = statistics.pstdev(arr) if len(arr) > 1 else 0.0
    ci95 = 1.96 * std / math.sqrt(len(arr)) if len(arr) > 1 else 0.0
    cv = std / mean if mean > 0 else float("nan")
    return {
        "runs": len(arr),
        "mean": mean,
        "std": std,
        "median": float(np.median(np.array(arr))),
        "ci95": ci95,
        "cv": cv,
    }


def build_breakdown_rows(sample_rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str, str], List[Dict[str, object]]] = {}
    for r in sample_rows:
        key = (str(r["label"]), str(r["phase"]), str(r["scenario"]))
        grouped.setdefault(key, []).append(r)

    out: List[Dict[str, object]] = []
    for (label, phase, scenario), rows in grouped.items():
        lat_vals = [to_float(r.get("latency_us", float("nan"))) for r in rows]
        lat_vals = [x for x in lat_vals if is_finite(x)]
        lat_sum = sum(lat_vals)
        stage_sum_map: Dict[str, float] = {}
        for stage in STAGES:
            vals = [to_float(r.get(f"stage_{stage}_us", float("nan"))) for r in rows]
            vals = [x for x in vals if is_finite(x) and x >= 0]
            stage_sum_map[stage] = sum(vals)
        stage_total = sum(v for v in stage_sum_map.values() if is_finite(v) and v >= 0)
        for stage in STAGES:
            vals = [to_float(r.get(f"stage_{stage}_us", float("nan"))) for r in rows]
            vals = [x for x in vals if is_finite(x) and x >= 0]
            stage_sum = stage_sum_map.get(stage, 0.0)
            stage_avg = stage_sum / len(vals) if vals else float("nan")
            stage_median = float(np.median(np.array(vals))) if vals else float("nan")
            share_pct = stage_sum * 100.0 / stage_total if stage_total > 0 else float("nan")
            latency_ratio_pct = stage_sum * 100.0 / lat_sum if lat_sum > 0 else float("nan")
            out.append(
                {
                    "label": label,
                    "phase": phase,
                    "scenario": scenario,
                    "stage": stage,
                    "samples": len(rows),
                    "latency_sum_us": lat_sum,
                    "stage_sum_us": stage_sum,
                    "stage_avg_us": stage_avg,
                    "stage_median_us": stage_median,
                    "stage_share_pct": share_pct,
                    "stage_vs_latency_pct": latency_ratio_pct,
                }
            )
    out.sort(key=lambda r: (str(r["phase"]), str(r["label"]), str(r["scenario"]), STAGES.index(str(r["stage"]))))
    return out


def _quantile(vals: List[float], q: float) -> float:
    if not vals:
        return float("nan")
    arr = np.array(sorted(vals), dtype=float)
    idx = int(round((len(arr) - 1) * q))
    idx = max(0, min(idx, len(arr) - 1))
    return float(arr[idx])


def build_stage_distribution_rows(sample_rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str, str], List[Dict[str, object]]] = {}
    for r in sample_rows:
        key = (str(r["label"]), str(r["phase"]), str(r["scenario"]))
        grouped.setdefault(key, []).append(r)

    out: List[Dict[str, object]] = []
    for (label, phase, scenario), rows in grouped.items():
        for stage in STAGES:
            vals_us: List[float] = []
            vals_share: List[float] = []
            for r in rows:
                stage_us = to_float(r.get(f"stage_{stage}_us", float("nan")))
                lat_us = to_float(r.get("latency_us", float("nan")))
                if is_finite(stage_us):
                    vals_us.append(stage_us)
                if is_finite(stage_us) and is_finite(lat_us) and lat_us > 0:
                    vals_share.append(stage_us * 100.0 / lat_us)

            n = len(vals_us)
            if n == 0:
                continue
            mean_us = statistics.mean(vals_us)
            std_us = statistics.pstdev(vals_us) if n > 1 else 0.0
            cv_us = std_us / mean_us if mean_us > 0 else float("nan")
            mean_share = statistics.mean(vals_share) if vals_share else float("nan")
            std_share = statistics.pstdev(vals_share) if len(vals_share) > 1 else 0.0
            cv_share = std_share / mean_share if is_finite(mean_share) and mean_share > 0 else float("nan")
            ci95_share = (
                1.96 * std_share / math.sqrt(len(vals_share))
                if len(vals_share) > 1
                else 0.0
            )

            out.append(
                {
                    "label": label,
                    "phase": phase,
                    "scenario": scenario,
                    "stage": stage,
                    "samples": n,
                    "mean_stage_us": mean_us,
                    "std_stage_us": std_us,
                    "cv_stage_us": cv_us,
                    "p50_stage_us": _quantile(vals_us, 0.50),
                    "p95_stage_us": _quantile(vals_us, 0.95),
                    "mean_share_pct": mean_share,
                    "std_share_pct": std_share,
                    "cv_share_pct": cv_share,
                    "ci95_halfwidth_share_pct": ci95_share,
                    "p50_share_pct": _quantile(vals_share, 0.50),
                    "p95_share_pct": _quantile(vals_share, 0.95),
                }
            )
    out.sort(key=lambda r: (str(r["phase"]), str(r["label"]), str(r["scenario"]), STAGES.index(str(r["stage"]))))
    return out


def build_latency_bucket_breakdown_rows(sample_rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str, str], List[Dict[str, object]]] = {}
    for r in sample_rows:
        key = (str(r["label"]), str(r["phase"]), str(r["scenario"]))
        grouped.setdefault(key, []).append(r)

    out: List[Dict[str, object]] = []
    for (label, phase, scenario), rows in grouped.items():
        threshold_vals = [to_float(r.get("threshold_us", float("nan"))) for r in rows]
        threshold_vals = [x for x in threshold_vals if is_finite(x) and x > 0]
        if not threshold_vals:
            continue
        threshold_us = float(np.median(np.array(threshold_vals)))
        if threshold_us <= 0:
            continue

        for idx, (lo_x, hi_x, bucket_name) in enumerate(TAIL_LAT_RATIO_BUCKETS):
            bucket_rows: List[Dict[str, object]] = []
            lo_us = threshold_us * lo_x
            hi_us = float("inf") if not is_finite(hi_x) else threshold_us * hi_x
            for r in rows:
                lat = to_float(r.get("latency_us", float("nan")))
                if not is_finite(lat):
                    continue
                if lat < lo_us:
                    continue
                if is_finite(hi_us) and lat >= hi_us:
                    continue
                bucket_rows.append(r)

            sample_count = len(bucket_rows)
            if sample_count <= 0:
                continue

            lat_vals = [to_float(r.get("latency_us", float("nan"))) for r in bucket_rows]
            lat_vals = [x for x in lat_vals if is_finite(x)]
            stage_sum_map: Dict[str, float] = {}
            for stage in STAGES:
                vals = [to_float(r.get(f"stage_{stage}_us", float("nan"))) for r in bucket_rows]
                vals = [x for x in vals if is_finite(x) and x >= 0]
                stage_sum_map[stage] = sum(vals)
            stage_total = sum(v for v in stage_sum_map.values() if is_finite(v) and v >= 0)

            for stage in STAGES:
                stage_sum = stage_sum_map.get(stage, 0.0)
                stage_avg = stage_sum / sample_count if sample_count > 0 else float("nan")
                stage_share = stage_sum * 100.0 / stage_total if stage_total > 0 else float("nan")
                out.append(
                    {
                        "label": label,
                        "phase": phase,
                        "scenario": scenario,
                        "bucket_order": idx + 1,
                        "bucket_ratio": bucket_name,
                        "bucket_lo_x": lo_x,
                        "bucket_hi_x": hi_x,
                        "bucket_lo_us": lo_us,
                        "bucket_hi_us": hi_us,
                        "threshold_us": threshold_us,
                        "samples": sample_count,
                        "latency_avg_us": statistics.mean(lat_vals) if lat_vals else float("nan"),
                        "latency_p50_us": _quantile(lat_vals, 0.50),
                        "latency_p95_us": _quantile(lat_vals, 0.95),
                        "stage": stage,
                        "stage_avg_us": stage_avg,
                        "stage_share_pct": stage_share,
                    }
                )

    out.sort(
        key=lambda r: (
            str(r["phase"]),
            str(r["label"]),
            str(r["scenario"]),
            to_int(r["bucket_order"]),
            STAGES.index(str(r["stage"])),
        )
    )
    return out


def plot_phase_stack(breakdown_rows: List[Dict[str, object]], phase: str, out_png: Path) -> bool:
    subset = [r for r in breakdown_rows if str(r["phase"]) == phase and str(r["scenario"]) == "mixgraph"]
    if not subset:
        return False
    labels = sorted({str(r["label"]) for r in subset})
    if not labels:
        return False

    mat_abs = np.zeros((len(STAGES), len(labels)), dtype=float)
    mat_share = np.zeros((len(STAGES), len(labels)), dtype=float)
    for i, st in enumerate(STAGES):
        for j, lb in enumerate(labels):
            rows = [r for r in subset if str(r["label"]) == lb and str(r["stage"]) == st]
            if not rows:
                continue
            mat_abs[i, j] = to_float(rows[0].get("stage_avg_us", 0.0))
            mat_share[i, j] = to_float(rows[0].get("stage_share_pct", 0.0))

    x = np.arange(len(labels))
    fig, (ax_abs, ax_share) = plt.subplots(2, 1, figsize=(max(9, 1.1 * len(labels)), 9), sharex=True)
    bottom_abs = np.zeros(len(labels), dtype=float)
    bottom_share = np.zeros(len(labels), dtype=float)
    for i, st in enumerate(STAGES):
        vals_abs = mat_abs[i, :]
        vals_share = mat_share[i, :]
        color = STAGE_COLORS.get(st, "#999999")
        ax_abs.bar(x, vals_abs, bottom=bottom_abs, color=color, label=st, alpha=0.9)
        ax_share.bar(x, vals_share, bottom=bottom_share, color=color, label=st, alpha=0.9)
        bottom_abs += np.nan_to_num(vals_abs)
        bottom_share += np.nan_to_num(vals_share)

    ax_abs.set_title(f"{phase}: mixgraph tail-seek latency stage stack (absolute)")
    ax_abs.set_ylabel("us/sample")
    ax_abs.grid(axis="y", alpha=0.25)
    ax_abs.legend(ncol=3, fontsize=8)
    ax_share.set_title(f"{phase}: mixgraph tail-seek latency stage stack (relative)")
    ax_share.set_ylabel("share (%)")
    ax_share.grid(axis="y", alpha=0.25)
    ax_share.set_xticks(x)
    ax_share.set_xticklabels(labels)
    plt.tight_layout()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_phase_latency_bucket_stage_stack(
    bucket_rows: List[Dict[str, object]], phase: str, out_png: Path
) -> bool:
    subset = [
        r
        for r in bucket_rows
        if str(r.get("phase", "")) == phase and str(r.get("scenario", "")) == "mixgraph"
    ]
    if not subset:
        return False

    labels = sorted({str(r["label"]) for r in subset})
    if not labels:
        return False

    fig, axes = plt.subplots(
        1, len(labels), figsize=(max(12, 5.2 * len(labels)), 6.8), sharey=True
    )
    if len(labels) == 1:
        axes = [axes]

    for ax, lb in zip(axes, labels):
        rows_lb = [r for r in subset if str(r["label"]) == lb]
        bucket_orders = sorted(
            {to_int(r.get("bucket_order", 0)) for r in rows_lb if to_int(r.get("bucket_order", 0)) > 0}
        )
        if not bucket_orders:
            continue

        bucket_labels: List[str] = []
        bucket_samples: List[int] = []
        mat_share = np.zeros((len(STAGES), len(bucket_orders)), dtype=float)

        for j, bo in enumerate(bucket_orders):
            rows_bucket = [r for r in rows_lb if to_int(r.get("bucket_order", 0)) == bo]
            if not rows_bucket:
                continue
            bucket_ratio = str(rows_bucket[0].get("bucket_ratio", f"bucket-{bo}"))
            samples = to_int(rows_bucket[0].get("samples", 0))
            bucket_labels.append(f"{bucket_ratio}\n(n={samples})")
            bucket_samples.append(samples)
            for i, st in enumerate(STAGES):
                rows_stage = [r for r in rows_bucket if str(r.get("stage", "")) == st]
                if not rows_stage:
                    continue
                mat_share[i, j] = to_float(rows_stage[0].get("stage_share_pct", 0.0))

        x = np.arange(len(bucket_orders))
        bottom = np.zeros(len(bucket_orders), dtype=float)
        for i, st in enumerate(STAGES):
            vals = mat_share[i, :]
            ax.bar(
                x,
                vals,
                bottom=bottom,
                color=STAGE_COLORS.get(st, "#999999"),
                alpha=0.9,
                label=st if lb == labels[0] else None,
            )
            bottom += np.nan_to_num(vals)

        ax.set_title(f"{lb} by tail-latency bucket")
        ax.set_ylim(0, 106)
        ax.set_ylabel("stage share (%)")
        ax.set_xticks(x)
        ax.set_xticklabels(bucket_labels, rotation=0, fontsize=8)
        ax.grid(axis="y", alpha=0.25)
        for j, n in enumerate(bucket_samples):
            ax.text(j, 101.0, f"{n}", ha="center", va="bottom", fontsize=7)

    axes[0].legend(ncol=2, fontsize=8, loc="upper left")
    fig.suptitle(
        f"{phase}: mixgraph tail-seek stage composition by latency bucket",
        fontsize=12,
    )
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out_png.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def plot_phase_component_share_distribution(
    sample_rows: List[Dict[str, object]], phase: str, out_png: Path
) -> bool:
    subset = [
        r
        for r in sample_rows
        if str(r.get("phase", "")) == phase and str(r.get("scenario", "")) == "mixgraph"
    ]
    if not subset:
        return False

    labels = sorted({str(r["label"]) for r in subset})
    if not labels:
        return False

    stage_col_map = {
        "memtable_route": "stage_memtable_route_us",
        "table_open_meta": "stage_table_open_meta_us",
        "index_lookup": "stage_index_lookup_us",
        "seek_dispatch": "stage_seek_dispatch_us",
        "block_read_io": "stage_block_read_io_us",
        "block_decode_checksum": "stage_block_decode_checksum_us",
        "post_process": "stage_post_process_us",
        "unattributed": "stage_unattributed_us",
    }
    # Keep the distribution figure focused on the dominant latency contributors.
    stages = ["seek_dispatch", "index_lookup", "block_read_io"]

    all_share_vals: List[float] = []
    per_label_data: Dict[str, List[List[float]]] = {}
    for lb in labels:
        rows = [r for r in subset if str(r["label"]) == lb]
        data: List[List[float]] = []
        for st in stages:
            col = stage_col_map[st]
            vals: List[float] = []
            for r in rows:
                lat = to_float(r.get("latency_us", float("nan")))
                v = to_float(r.get(col, float("nan")))
                if is_finite(lat) and lat > 0 and is_finite(v):
                    share = v * 100.0 / lat
                    vals.append(share)
                    all_share_vals.append(share)
            data.append(vals if vals else [0.0])
        per_label_data[lb] = data

    if not all_share_vals:
        return False
    y_cap = float(np.percentile(np.array(all_share_vals), 99.5))
    y_top = max(120.0, y_cap * 1.10)

    fig, axes = plt.subplots(
        1, len(labels), figsize=(max(12, 4.8 * len(labels)), 6.5), sharey=True
    )
    if len(labels) == 1:
        axes = [axes]

    for ax, lb in zip(axes, labels):
        data = per_label_data[lb]
        # Matplotlib compatibility: older versions use "labels=", newer accepts "tick_labels=".
        try:
            bp = ax.boxplot(
                data,
                tick_labels=stages,
                showfliers=False,
                patch_artist=True,
                medianprops={"color": "#111111", "linewidth": 1.0},
            )
        except TypeError:
            bp = ax.boxplot(
                data,
                labels=stages,
                showfliers=False,
                patch_artist=True,
                medianprops={"color": "#111111", "linewidth": 1.0},
            )
        for patch, st in zip(bp["boxes"], stages):
            patch.set_facecolor(STAGE_COLORS.get(st, "#cccccc"))
            patch.set_alpha(0.85)
            patch.set_linewidth(0.8)
        ax.set_title(f"{lb} (n={len([r for r in subset if str(r['label']) == lb])})")
        ax.set_xlabel("component stage")
        ax.grid(axis="y", alpha=0.25)
        ax.tick_params(axis="x", rotation=35, labelsize=8)
        ax.set_ylim(0, y_top)

    axes[0].set_ylabel("share in request latency (%)")
    fig.suptitle(
        f"{phase}: per-request component latency-share distribution (tail seek)",
        fontsize=12,
    )
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out_png.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_png, dpi=180)
    plt.close(fig)
    return True


def write_report(
    out_md: Path,
    threshold_rows: List[Dict[str, object]],
    breakdown_rows: List[Dict[str, object]],
    dist_rows: List[Dict[str, object]],
    bucket_rows: List[Dict[str, object]],
    generated: List[str],
) -> None:
    lines: List[str] = []
    lines.append("# 长尾探针报告")
    lines.append("")
    lines.append("## 方法")
    lines.append("- 每个 phase 选择代表 case（默认优先 A2/B2/C2），基于历史矩阵命令重放。")
    lines.append("- 每个 case 先重复运行 mixgraph（step 02）以估计稳态 seek p99 分布。")
    lines.append("- 以 `threshold = max(min_threshold, median_p99 * multiplier)` 作为长尾阈值。")
    lines.append("- 二次运行时仅记录 `latency_us >= threshold` 的 seek 样本，并输出 RocksDB perf/iostats 增量。")
    lines.append("- baseline 与 probe 强制使用同一 perf_level（默认 6，可通过脚本参数调整）。")
    lines.append("- stage 口径采用排他计时（对父子重叠计时做去重与封顶），用于延时构成分析。")
    lines.append("")

    lines.append("## 阈值与样本")
    for r in threshold_rows:
        lines.append(
            "- "
            + f"{r['label']}({r['phase']}/{r['scenario']}): "
            + f"baseline_runs={r['baseline_runs']}, "
            + f"p99_median={to_float(r['baseline_p99_median_us']):.3f}us, "
            + f"threshold={to_int(r['threshold_us'])}us, "
            + f"tail_samples={to_int(r['tail_samples'])}, status={r['status']}"
        )
    if not threshold_rows:
        lines.append("- 无可用样本。")
    lines.append("")

    lines.append("## 阶段构成（按 case）")
    grouped: Dict[Tuple[str, str], List[Dict[str, object]]] = {}
    for r in breakdown_rows:
        if str(r.get("scenario", "")) != "mixgraph":
            continue
        grouped.setdefault((str(r["label"]), str(r["phase"])), []).append(r)
    for (label, phase), rows in sorted(grouped.items()):
        rows.sort(key=lambda x: to_float(x.get("stage_share_pct", 0.0)), reverse=True)
        top = rows[:4]
        desc = ", ".join(
            f"{x['stage']}={to_float(x['stage_share_pct']):.1f}%({to_float(x['stage_avg_us']):.2f}us)"
            for x in top
        )
        lines.append(f"- {label} ({phase}): {desc}")
    if not grouped:
        lines.append("- 暂无 mixgraph tail 样本。")
    lines.append("")

    lines.append("## 阶段占比方差（按 case）")
    dist_group: Dict[Tuple[str, str], List[Dict[str, object]]] = {}
    for r in dist_rows:
        if str(r.get("scenario", "")) != "mixgraph":
            continue
        dist_group.setdefault((str(r["label"]), str(r["phase"])), []).append(r)
    for (label, phase), rows in sorted(dist_group.items()):
        rows.sort(key=lambda x: to_float(x.get("mean_share_pct", 0.0)), reverse=True)
        top = rows[:4]
        desc = ", ".join(
            f"{x['stage']} mean={to_float(x['mean_share_pct']):.1f}% "
            + f"std={to_float(x['std_share_pct']):.1f}% "
            + f"p95={to_float(x['p95_share_pct']):.1f}%"
            for x in top
        )
        lines.append(f"- {label} ({phase}): {desc}")
    if not dist_group:
        lines.append("- 暂无方差统计。")
    lines.append("")

    lines.append("## 长尾延时分桶构成（按 case）")
    bucket_group: Dict[Tuple[str, str], List[Dict[str, object]]] = {}
    for r in bucket_rows:
        if str(r.get("scenario", "")) != "mixgraph":
            continue
        bucket_group.setdefault((str(r["label"]), str(r["phase"])), []).append(r)
    for (label, phase), rows in sorted(bucket_group.items()):
        high = [r for r in rows if str(r.get("bucket_ratio", "")) in ("3.0-5.0x", ">=5.0x")]
        if not high:
            continue
        high.sort(
            key=lambda x: (
                str(x.get("bucket_ratio", "")),
                -to_float(x.get("stage_share_pct", 0.0)),
            )
        )
        picks = high[:4]
        desc = ", ".join(
            f"{x['bucket_ratio']}:{x['stage']}={to_float(x['stage_share_pct']):.1f}%"
            for x in picks
        )
        lines.append(f"- {label} ({phase}): {desc}")
    if not bucket_group:
        lines.append("- 暂无长尾分桶统计。")
    lines.append("")

    lines.append("## 输出产物")
    for g in generated:
        lines.append(f"- {g}")

    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Replay matrix commands to collect tail-seek probe samples and stage breakdown.")
    ap.add_argument("--matrix-dir", required=True)
    ap.add_argument("--out-dir", default="")
    ap.add_argument("--cases", default="")
    ap.add_argument("--step-ids", default="02")
    ap.add_argument("--baseline-runs", type=int, default=5)
    ap.add_argument("--threshold-multiplier", type=float, default=1.0)
    ap.add_argument("--threshold-min-us", type=int, default=1000)
    ap.add_argument(
        "--replay-perf-level",
        type=int,
        default=6,
        help="Force --perf_level for both baseline and probe replay runs; <=0 keeps command unchanged.",
    )
    ap.add_argument("--max-samples", type=int, default=20000)
    ap.add_argument("--reuse-existing-samples", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.baseline_runs < 1:
        raise ValueError("--baseline-runs must be >= 1")
    if args.max_samples < 1:
        raise ValueError("--max-samples must be >= 1")
    if args.replay_perf_level < 0:
        raise ValueError("--replay-perf-level must be >= 0")

    matrix_dir = Path(args.matrix_dir).resolve()
    out_dir = (
        Path(args.out_dir).resolve()
        if args.out_dir
        else (matrix_dir / "analysis" / "resource_ledger" / "tail_probe")
    )
    raw_dir = out_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    repo_root = Path(__file__).resolve().parents[1]

    threshold_rows: List[Dict[str, object]] = []
    sample_rows: List[Dict[str, object]] = []
    selected_labels: List[str] = []
    if args.reuse_existing_samples:
        threshold_path = out_dir / "tail_threshold_summary.csv"
        sample_path = out_dir / "tail_stage_samples.csv"
        if threshold_path.exists():
            threshold_rows = [dict(x) for x in read_csv(threshold_path)]
        threshold_map: Dict[Tuple[str, str], Dict[str, object]] = {}
        for r in threshold_rows:
            key = (str(r.get("label", "")), str(r.get("scenario", "")))
            threshold_map[key] = r

        # Prefer rebuilding samples from raw tail csv so stage accounting changes
        # can be applied without replaying load.
        rebuilt = False
        for tail_csv in sorted(raw_dir.glob("*_tail_samples.csv")):
            parsed = parse_tail_sample_filename(tail_csv)
            if parsed is None:
                continue
            label, scenario = parsed
            tr_meta = threshold_map.get((label, scenario), {})
            phase = str(tr_meta.get("phase", ""))
            threshold_us = to_int(tr_meta.get("threshold_us", 0))
            for tr in read_csv(tail_csv):
                sample_rows.append(build_sample_row(label, phase, scenario, threshold_us, tr))
            rebuilt = True
        if not rebuilt and sample_path.exists():
            sample_rows = [dict(x) for x in read_csv(sample_path)]

        if sample_rows:
            sample_counts: Dict[Tuple[str, str], int] = {}
            for r in sample_rows:
                key = (str(r.get("label", "")), str(r.get("scenario", "")))
                sample_counts[key] = sample_counts.get(key, 0) + 1
            for tr in threshold_rows:
                key = (str(tr.get("label", "")), str(tr.get("scenario", "")))
                tr["tail_samples"] = sample_counts.get(key, to_int(tr.get("tail_samples", 0)))
        selected_labels = sorted({str(r.get("label", "")) for r in threshold_rows if str(r.get("label", ""))})
    else:
        registry_rows = load_registry(matrix_dir)
        case_filter = [x.strip() for x in args.cases.split(",") if x.strip()]
        selected_labels = choose_case_labels(registry_rows, case_filter)
        if not selected_labels:
            raise RuntimeError("no cases selected for tail probe")

        step_ids = [x.strip() for x in args.step_ids.split(",") if x.strip()]
        if not step_ids:
            raise RuntimeError("no step ids selected")

        row_by_label = {str(r.get("label", "")): r for r in registry_rows}

        for label in selected_labels:
            row = row_by_label.get(label)
            if row is None:
                continue
            phase = str(row.get("phase", ""))
            case_dir = Path(str(row.get("experiment_dir", "")))
            run_dir = find_latest_run_dir(case_dir)
            if run_dir is None:
                threshold_rows.append(
                    {
                        "label": label,
                        "phase": phase,
                        "scenario": "NA",
                        "baseline_runs": 0,
                        "baseline_p99_mean_us": float("nan"),
                        "baseline_p99_std_us": float("nan"),
                        "baseline_p99_ci95_us": float("nan"),
                        "baseline_p99_cv": float("nan"),
                        "baseline_p99_median_us": float("nan"),
                        "threshold_us": float("nan"),
                        "tail_samples": 0,
                        "status": "missing_run_dir",
                        "cmd_file": "",
                        "tail_csv": "",
                    }
                )
                continue

            for step_id in step_ids:
                scenario = step_to_scenario(step_id)
                cmd_files = sorted(run_dir.glob(f"{step_id}_*.cmd"))
                if not cmd_files:
                    threshold_rows.append(
                        {
                            "label": label,
                            "phase": phase,
                            "scenario": scenario,
                            "baseline_runs": 0,
                            "baseline_p99_mean_us": float("nan"),
                            "baseline_p99_std_us": float("nan"),
                            "baseline_p99_ci95_us": float("nan"),
                            "baseline_p99_cv": float("nan"),
                            "baseline_p99_median_us": float("nan"),
                            "threshold_us": float("nan"),
                            "tail_samples": 0,
                            "status": "missing_cmd",
                            "cmd_file": "",
                            "tail_csv": "",
                        }
                    )
                    continue

                cmd_file = cmd_files[0]
                base_tokens = read_cmd_tokens(cmd_file)
                if not base_tokens:
                    threshold_rows.append(
                        {
                            "label": label,
                            "phase": phase,
                            "scenario": scenario,
                            "baseline_runs": 0,
                            "baseline_p99_mean_us": float("nan"),
                            "baseline_p99_std_us": float("nan"),
                            "baseline_p99_ci95_us": float("nan"),
                            "baseline_p99_cv": float("nan"),
                            "baseline_p99_median_us": float("nan"),
                            "threshold_us": float("nan"),
                            "tail_samples": 0,
                            "status": "empty_cmd",
                            "cmd_file": str(cmd_file),
                            "tail_csv": "",
                        }
                    )
                    continue

                base_tokens = remove_existing_tail_flags(base_tokens)
                replay_tokens = list(base_tokens)
                if args.replay_perf_level > 0:
                    replay_tokens = ensure_perf_level(replay_tokens, args.replay_perf_level)

                baseline_p99: List[float] = []
                baseline_ok = True
                for rid in range(1, args.baseline_runs + 1):
                    log_path = raw_dir / f"{label}_{scenario}_baseline_r{rid}.log"
                    rc = 0
                    if not args.dry_run:
                        rc = run_and_log(replay_tokens, log_path, repo_root)
                    if rc != 0:
                        baseline_ok = False
                        break
                    if args.dry_run:
                        continue
                    p99 = parse_seek_p99_from_log(log_path)
                    if p99 is None:
                        baseline_ok = False
                        break
                    baseline_p99.append(p99)

                if args.dry_run:
                    baseline_summary = summarize_baseline([])
                    threshold_us = args.threshold_min_us
                else:
                    baseline_summary = summarize_baseline(baseline_p99)
                    if not baseline_ok or baseline_summary["runs"] < 1:
                        threshold_rows.append(
                            {
                                "label": label,
                                "phase": phase,
                                "scenario": scenario,
                                "baseline_runs": baseline_summary["runs"],
                                "baseline_p99_mean_us": baseline_summary["mean"],
                                "baseline_p99_std_us": baseline_summary["std"],
                                "baseline_p99_ci95_us": baseline_summary["ci95"],
                                "baseline_p99_cv": baseline_summary["cv"],
                                "baseline_p99_median_us": baseline_summary["median"],
                                "threshold_us": float("nan"),
                                "tail_samples": 0,
                                "status": "baseline_failed",
                                "cmd_file": str(cmd_file),
                                "tail_csv": "",
                            }
                        )
                        continue
                    threshold_us = max(
                        args.threshold_min_us,
                        int(math.ceil(baseline_summary["median"] * args.threshold_multiplier)),
                    )

                tail_csv = raw_dir / f"{label}_{scenario}_tail_samples.csv"
                if tail_csv.exists():
                    tail_csv.unlink()

                probe_tokens = list(replay_tokens)
                probe_tokens.extend(
                    [
                        f"--tail_probe_output={tail_csv}",
                        f"--tail_probe_threshold_us={threshold_us}",
                        f"--tail_probe_max_samples={args.max_samples}",
                        f"--tail_probe_case_label={label}",
                        f"--tail_probe_scenario={scenario}",
                    ]
                )
                probe_log = raw_dir / f"{label}_{scenario}_probe.log"
                rc_probe = 0
                if not args.dry_run:
                    rc_probe = run_and_log(probe_tokens, probe_log, repo_root)

                rows_loaded: List[Dict[str, str]] = []
                if not args.dry_run and rc_probe == 0 and tail_csv.exists():
                    rows_loaded = read_csv(tail_csv)

                threshold_rows.append(
                    {
                        "label": label,
                        "phase": phase,
                        "scenario": scenario,
                        "baseline_runs": baseline_summary["runs"],
                        "baseline_p99_mean_us": baseline_summary["mean"],
                        "baseline_p99_std_us": baseline_summary["std"],
                        "baseline_p99_ci95_us": baseline_summary["ci95"],
                        "baseline_p99_cv": baseline_summary["cv"],
                        "baseline_p99_median_us": baseline_summary["median"],
                        "threshold_us": threshold_us,
                        "tail_samples": len(rows_loaded),
                        "status": (
                            "dry_run"
                            if args.dry_run
                            else ("ok" if rc_probe == 0 else f"probe_rc_{rc_probe}")
                        ),
                        "cmd_file": str(cmd_file),
                        "tail_csv": str(tail_csv),
                        "probe_log": str(probe_log),
                    }
                )

                for tr in rows_loaded:
                    sample_rows.append(build_sample_row(label, phase, scenario, threshold_us, tr))

    generated: List[str] = []
    threshold_csv = out_dir / "tail_threshold_summary.csv"
    write_csv(threshold_csv, threshold_rows)
    generated.append("tail_threshold_summary.csv")

    sample_csv = out_dir / "tail_stage_samples.csv"
    write_csv(sample_csv, sample_rows)
    generated.append("tail_stage_samples.csv")

    breakdown_rows = build_breakdown_rows(sample_rows)
    breakdown_csv = out_dir / "tail_stage_breakdown.csv"
    write_csv(breakdown_csv, breakdown_rows)
    generated.append("tail_stage_breakdown.csv")

    dist_rows = build_stage_distribution_rows(sample_rows)
    dist_csv = out_dir / "tail_stage_distribution.csv"
    write_csv(dist_csv, dist_rows)
    generated.append("tail_stage_distribution.csv")

    bucket_rows = build_latency_bucket_breakdown_rows(sample_rows)
    bucket_csv = out_dir / "tail_latency_bucket_breakdown.csv"
    write_csv(bucket_csv, bucket_rows)
    generated.append("tail_latency_bucket_breakdown.csv")

    for phase in ["cache_sweep", "thread_sweep", "locality_sweep"]:
        pfx = phase_to_output_prefix(phase)
        png = out_dir / f"phase{pfx}_tail_seek_stage_stack.png"
        if plot_phase_stack(breakdown_rows, phase, png):
            generated.append(png.name)
        bucket_png = out_dir / f"phase{pfx}_tail_latency_bucket_stage_stack.png"
        if plot_phase_latency_bucket_stage_stack(bucket_rows, phase, bucket_png):
            generated.append(bucket_png.name)
        dist_png = out_dir / f"phase{pfx}_tail_component_share_distribution.png"
        if plot_phase_component_share_distribution(sample_rows, phase, dist_png):
            generated.append(dist_png.name)

    report_md = out_dir / "tail_probe_report.md"
    write_report(report_md, threshold_rows, breakdown_rows, dist_rows, bucket_rows, generated)
    generated.append("tail_probe_report.md")

    print(f"tail probe output: {out_dir}")
    print(f"selected_cases={','.join(selected_labels)}")
    print(f"generated={','.join(generated)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
import argparse
import csv
import re
import shlex
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt
import numpy as np


BENCH_LINE_RE = re.compile(
    r"^(?P<name>\w+)\s+:\s+(?P<micros>[0-9.]+)\s+micros/op\s+"
    r"(?P<ops>[0-9]+)\s+ops/sec\s+(?P<seconds>[0-9.]+)\s+seconds\s+"
    r"(?P<operations>[0-9]+)\s+operations;\s+(?P<mbps>[0-9.]+)\s+MB/s"
    r"(?:\s+\((?P<extra>.*)\))?$"
)
SEEK_LAT_RE = re.compile(
    r"^rocksdb\.db\.seek\.micros P50 : (?P<p50>[0-9.]+) "
    r"P95 : (?P<p95>[0-9.]+) P99 : (?P<p99>[0-9.]+) "
    r"P100 : (?P<p100>[0-9.]+) COUNT : (?P<count>[0-9]+)"
)
MIX_EXTRA_RE = re.compile(
    r"Gets:(?P<gets>[0-9]+)\s+Puts:(?P<puts>[0-9]+)\s+Seek:(?P<seeks>[0-9]+),\s+"
    r"reads\s+(?P<reads>[0-9]+)\s+in\s+(?P<reads_found>[0-9]+)\s+found,\s+"
    r"avg size:\s+(?P<avg_value>[0-9.]+)\s+value,\s+(?P<avg_scan>[0-9.]+)\s+scan"
)
SEEKRANDOM_EXTRA_RE = re.compile(r"\((?P<found>[0-9]+) of (?P<total>[0-9]+) found\)")
UPTIME_RE = re.compile(
    r"^Uptime\(secs\):\s+(?P<total>[0-9.]+)\s+total,\s+(?P<interval>[0-9.]+)\s+interval"
)
L0_RE = re.compile(
    r"^L0\s+(?P<files>[0-9]+)/(?P<compacting>[0-9]+)\s+"
    r"(?P<size>[0-9.]+)\s+(?P<size_unit>[KMGTP]?B)\s+"
)
CUM_WRITES_RE = re.compile(
    r"^Cumulative writes:\s+(?P<writes>[0-9.]+)(?P<writes_unit>[KMBG]?)\s+writes,.*"
    r"ingest:\s+(?P<ingest_gb>[0-9.]+)\s+GB,\s+(?P<ingest_mbps>[0-9.]+)\s+MB/s"
)
RUNNER_START_RE = re.compile(r"^\[[^\]]+\]\s+START\s+(?P<name>\S+)")


def to_int(v: Optional[str]) -> Optional[int]:
    if v is None or v == "":
        return None
    try:
        return int(float(v))
    except ValueError:
        return None


def to_float(v: Optional[str]) -> Optional[float]:
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def parse_count_with_suffix(value: str, suffix: str) -> int:
    scale = {"": 1, "K": 1_000, "M": 1_000_000, "B": 1_000_000_000, "G": 1_000_000_000}
    return int(float(value) * scale[suffix])


def parse_size_to_bytes(value: str, unit: str) -> int:
    scale = {
        "B": 1,
        "KB": 1024,
        "MB": 1024**2,
        "GB": 1024**3,
        "TB": 1024**4,
        "PB": 1024**5,
    }
    return int(float(value) * scale[unit])


def cache_gib(cache_size: int) -> float:
    return cache_size / (1024**3)


def parse_config(path: Path) -> Dict[str, str]:
    cfg: Dict[str, str] = {}
    if not path.exists():
        return cfg
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if not line or "=" not in line:
            continue
        k, v = line.split("=", 1)
        cfg[k.strip()] = v.strip()
    return cfg


def parse_runner_order(path: Path) -> Dict[str, int]:
    order: Dict[str, int] = {}
    if not path.exists():
        return order
    idx = 0
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = RUNNER_START_RE.match(raw)
        if not m:
            continue
        idx += 1
        order[m.group("name")] = idx
    return order


def parse_cmd_args(cmd_path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not cmd_path.exists():
        return out
    tokens = shlex.split(cmd_path.read_text(encoding="utf-8", errors="ignore").strip())
    for t in tokens[1:]:
        if not t.startswith("--"):
            continue
        body = t[2:]
        if "=" in body:
            k, v = body.split("=", 1)
            out[k] = v
        else:
            out[body] = "true"
    return out


def parse_fill_log_summary(fill_log_path: Path) -> Dict[str, object]:
    out: Dict[str, object] = {}
    if not fill_log_path.exists():
        return out
    raw_size_re = re.compile(r"^RawSize:\s+([0-9.]+)\s+([KMGTP]?B)")
    file_size_re = re.compile(r"^FileSize:\s+([0-9.]+)\s+([KMGTP]?B)")
    entries_re = re.compile(r"^Entries:\s+([0-9]+)")
    with fill_log_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            s = line.strip()
            m = entries_re.match(s)
            if m and "fill_entries" not in out:
                out["fill_entries"] = int(m.group(1))
                continue
            m = raw_size_re.match(s)
            if m and "fill_raw_size_bytes" not in out:
                out["fill_raw_size_bytes"] = parse_size_to_bytes(m.group(1), m.group(2))
                continue
            m = file_size_re.match(s)
            if m and "fill_file_size_bytes" not in out:
                out["fill_file_size_bytes"] = parse_size_to_bytes(m.group(1), m.group(2))
                continue
    return out


def parse_step_meta(stem: str) -> Dict[str, Optional[str]]:
    out: Dict[str, Optional[str]] = {
        "step_name": stem,
        "scenario": "unknown",
        "scenario_family": "other",
        "step_id": None,
    }

    m = re.match(r"^(\d{2})_", stem)
    if m:
        out["step_id"] = m.group(1)

    if stem.startswith("01_fillrandom"):
        out["scenario"] = "fillrandom"
        out["scenario_family"] = "prepare"
    elif stem.startswith("02_mixgraph"):
        out["scenario"] = "mixgraph"
        out["scenario_family"] = "realistic"
    elif stem.startswith("03_seek200"):
        out["scenario"] = "seek200"
        out["scenario_family"] = "realistic"
    elif stem.startswith("04_worst_seek1"):
        out["scenario"] = "worst_seek1"
        out["scenario_family"] = "worst"
    elif stem.startswith("05_worst_seek4"):
        out["scenario"] = "worst_seek4"
        out["scenario_family"] = "worst"
    elif stem.startswith("06_worst_seek20"):
        out["scenario"] = "worst_seek20"
        out["scenario_family"] = "worst"
    elif stem.startswith("07_worst_seek200"):
        out["scenario"] = "worst_seek200"
        out["scenario_family"] = "worst"
    elif stem.startswith("08_worst_seek10000"):
        out["scenario"] = "worst_seek10000"
        out["scenario_family"] = "worst"
    elif stem.startswith("09_final_stats"):
        out["scenario"] = "final_stats"
        out["scenario_family"] = "final"

    cm = re.search(r"_cache_(\d+)$", stem)
    out["cache_bytes_from_name"] = cm.group(1) if cm else None
    return out


def parse_log(path: Path) -> Dict[str, object]:
    out: Dict[str, object] = {}
    have_l0 = False

    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for raw in f:
            line = raw.strip()

            m = BENCH_LINE_RE.match(line)
            if m:
                out["bench_name"] = m.group("name")
                out["micros_per_op"] = float(m.group("micros"))
                out["ops_per_sec"] = int(m.group("ops"))
                out["run_seconds"] = float(m.group("seconds"))
                out["operations"] = int(m.group("operations"))
                out["throughput_mb_s"] = float(m.group("mbps"))
                extra = m.group("extra")
                if extra and m.group("name") == "mixgraph":
                    mm = MIX_EXTRA_RE.search(extra)
                    if mm:
                        out["mix_gets"] = int(mm.group("gets"))
                        out["mix_puts"] = int(mm.group("puts"))
                        out["mix_seeks"] = int(mm.group("seeks"))
                        out["mix_reads"] = int(mm.group("reads"))
                        out["mix_reads_found"] = int(mm.group("reads_found"))
                        out["mix_avg_value_size"] = float(mm.group("avg_value"))
                        out["mix_avg_scan_len"] = float(mm.group("avg_scan"))
                elif extra and m.group("name") == "seekrandom":
                    sm = SEEKRANDOM_EXTRA_RE.search(extra)
                    if sm:
                        out["seekrandom_found"] = int(sm.group("found"))
                        out["seekrandom_total"] = int(sm.group("total"))
                continue

            m = SEEK_LAT_RE.match(line)
            if m:
                out["seek_p50_us"] = float(m.group("p50"))
                out["seek_p95_us"] = float(m.group("p95"))
                out["seek_p99_us"] = float(m.group("p99"))
                out["seek_p100_us"] = float(m.group("p100"))
                out["seek_count"] = int(m.group("count"))
                continue

            m = UPTIME_RE.match(line)
            if m and "uptime_total_s" not in out:
                out["uptime_total_s"] = float(m.group("total"))
                out["uptime_interval_s"] = float(m.group("interval"))
                continue

            if not have_l0 and line.startswith("L0"):
                m = L0_RE.match(line)
                if m:
                    out["l0_files_end"] = int(m.group("files"))
                    out["l0_compacting_files_end"] = int(m.group("compacting"))
                    out["l0_size_bytes_end"] = parse_size_to_bytes(
                        m.group("size"), m.group("size_unit")
                    )
                    have_l0 = True
                continue

            m = CUM_WRITES_RE.match(line)
            if m and "cumulative_writes_count" not in out:
                out["cumulative_writes_count"] = parse_count_with_suffix(
                    m.group("writes"), m.group("writes_unit")
                )
                out["cumulative_ingest_gb"] = float(m.group("ingest_gb"))
                out["cumulative_ingest_mb_s"] = float(m.group("ingest_mbps"))
                continue

            if line.startswith("rocksdb.block.cache.hit COUNT : "):
                out["cache_hit"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.block.cache.miss COUNT : "):
                out["cache_miss"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.block.cache.bytes.read COUNT : "):
                out["cache_bytes_read"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.number.db.seek COUNT : "):
                out["db_seek_count"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.number.db.next COUNT : "):
                out["db_next_count"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.db.iter.bytes.read COUNT : "):
                out["iter_bytes_read"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.number.keys.written COUNT : "):
                out["keys_written_count"] = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("rocksdb.number.keys.read COUNT : "):
                out["keys_read_count"] = int(line.rsplit(":", 1)[1].strip())

    hit = out.get("cache_hit")
    miss = out.get("cache_miss")
    if isinstance(hit, int) and isinstance(miss, int) and (hit + miss) > 0:
        out["cache_hit_ratio_pct"] = hit * 100.0 / (hit + miss)

    return out


def collect_rows(run_dir: Path) -> List[Dict[str, object]]:
    cfg = parse_config(run_dir / "config.txt")
    runner_order = parse_runner_order(run_dir / "runner.log")
    run_tag = cfg.get("RUN_TAG", run_dir.name)

    cmd_keep = [
        "num",
        "reads",
        "threads",
        "cache_size",
        "key_size",
        "value_size",
        "compression_type",
        "use_direct_reads",
        "use_direct_io_for_flush_and_compaction",
        "seek_nexts",
        "mix_get_ratio",
        "mix_put_ratio",
        "mix_seek_ratio",
        "iter_k",
        "iter_sigma",
        "iter_theta",
        "keyrange_num",
        "benchmarks",
        "db",
        "wal_dir",
        "value_k",
        "value_sigma",
        "value_theta",
        "key_dist_a",
        "key_dist_b",
        "keyrange_dist_a",
        "keyrange_dist_b",
        "keyrange_dist_c",
        "keyrange_dist_d",
        "mix_hot_keyrange_count",
        "mix_hotset_enable",
        "mix_hotset_range_pct",
        "mix_hotset_range_access_pct",
        "mix_hotset_range_zipf_theta",
        "mix_hotset_key_pct",
        "mix_hotset_key_access_pct",
        "mix_hotset_evenly_spread_ranges",
        "mix_shift_enable",
        "mix_shift_mode",
        "mix_shift_stage_seconds",
        "mix_shift_stride_ranges",
        "mix_shift_jump_multiplier",
        "mix_shift_base_start_range",
        "mix_shift_log_stage_transitions",
    ]

    rows: List[Dict[str, object]] = []
    for log_path in sorted(run_dir.glob("[0-9][0-9]_*.log")):
        stem = log_path.stem
        meta = parse_step_meta(stem)
        parsed = parse_log(log_path)
        cmd_args = parse_cmd_args(run_dir / f"{stem}.cmd")

        row: Dict[str, object] = {
            "run_tag": run_tag,
            "log_file": log_path.name,
            "step_name": meta["step_name"],
            "step_id": meta["step_id"],
            "scenario": meta["scenario"],
            "scenario_family": meta["scenario_family"],
            "step_order": runner_order.get(stem),
        }

        for k, v in cfg.items():
            row[f"cfg_{k.lower()}"] = v

        for k in cmd_keep:
            if k in cmd_args:
                row[f"cmd_{k}"] = cmd_args[k]

        cache_from_name = to_int(meta.get("cache_bytes_from_name"))
        cache_from_cmd = to_int(cmd_args.get("cache_size"))
        cache_bytes = cache_from_name if cache_from_name is not None else cache_from_cmd
        if cache_bytes is not None:
            row["cache_bytes"] = cache_bytes
            row["cache_gib"] = cache_gib(cache_bytes)

        row.update(parsed)
        rows.append(row)

    fill_summary = parse_fill_log_summary(run_dir / "01_fillrandom.log")
    if fill_summary:
        for r in rows:
            r.update(fill_summary)

    return rows


def sorted_rows_by_cache(rows: List[Dict[str, object]], scenario: str) -> List[Dict[str, object]]:
    filtered = [r for r in rows if r.get("scenario") == scenario and r.get("cache_gib") is not None]
    return sorted(filtered, key=lambda r: float(r.get("cache_gib", 0.0)))


READ_MODE_SCENARIOS = [
    "mixgraph",
    "seek200",
    "worst_seek1",
    "worst_seek4",
    "worst_seek20",
    "worst_seek200",
    "worst_seek10000",
]

READ_MODE_LABEL = {
    "mixgraph": "mixgraph",
    "seek200": "seek200",
    "worst_seek1": "worst_s1",
    "worst_seek4": "worst_s4",
    "worst_seek20": "worst_s20",
    "worst_seek200": "worst_s200",
    "worst_seek10000": "worst_s10000",
}


def single_cache_mode_rows(rows: List[Dict[str, object]]) -> Optional[Tuple[float, List[Dict[str, object]]]]:
    cache_vals = sorted({float(r["cache_gib"]) for r in rows if r.get("cache_gib") is not None})
    if len(cache_vals) != 1:
        return None
    cache = cache_vals[0]
    out: List[Dict[str, object]] = []
    for s in READ_MODE_SCENARIOS:
        sr = [r for r in rows if r.get("scenario") == s and r.get("cache_gib") is not None and abs(float(r["cache_gib"]) - cache) < 1e-9]
        if sr:
            out.append(sr[0])
    if not out:
        return None
    return cache, out


def draw_mixgraph_throughput(rows: List[Dict[str, object]], out_dir: Path):
    single = single_cache_mode_rows(rows)
    if single is not None:
        cache, data = single
        x = np.arange(len(data))
        y = [float(r.get("ops_per_sec", 0)) for r in data]
        labels = [READ_MODE_LABEL.get(str(r.get("scenario")), str(r.get("scenario"))) for r in data]
        plt.figure(figsize=(10.2, 5.0))
        bars = plt.bar(x, y, color="#1f77b4", alpha=0.9)
        if min(y) > 0 and max(y) / max(min(y), 1) > 20:
            plt.yscale("log")
            yfmt = "log"
        else:
            yfmt = "linear"
        plt.xticks(x, labels, rotation=20, ha="right")
        plt.xlabel("Read mode")
        plt.ylabel(f"Ops/sec ({yfmt})")
        plt.title(f"Throughput by Read Mode (cache={cache:.0f}GiB)")
        for i, b in enumerate(bars):
            plt.text(b.get_x() + b.get_width() / 2, b.get_height(), f"{int(y[i])}", ha="center", va="bottom", fontsize=8)
        plt.grid(alpha=0.3, axis="y")
        plt.tight_layout()
        plt.savefig(out_dir / "mixgraph_throughput_vs_cache.png", dpi=160)
        plt.close()
        return

    data = sorted_rows_by_cache(rows, "mixgraph")
    if not data:
        return
    x = [float(r["cache_gib"]) for r in data]
    y = [float(r.get("ops_per_sec", 0)) for r in data]
    plt.figure(figsize=(8, 4.8))
    plt.plot(x, y, marker="o", linewidth=2)
    plt.xlabel("Block cache size (GiB)")
    plt.ylabel("Ops/sec")
    plt.title("Mixgraph Throughput vs Cache Size")
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_dir / "mixgraph_throughput_vs_cache.png", dpi=160)
    plt.close()


def draw_mixgraph_seek_latency(rows: List[Dict[str, object]], out_dir: Path):
    single = single_cache_mode_rows(rows)
    if single is not None:
        cache, data = single
        labels = [READ_MODE_LABEL.get(str(r.get("scenario")), str(r.get("scenario"))) for r in data]
        x = np.arange(len(data))
        p95 = [float(r.get("seek_p95_us", 0)) for r in data]
        p99 = [float(r.get("seek_p99_us", 0)) for r in data]
        w = 0.38
        plt.figure(figsize=(10.6, 5.0))
        b1 = plt.bar(x - w / 2, p95, width=w, label="P95", color="#2ca02c", alpha=0.9)
        b2 = plt.bar(x + w / 2, p99, width=w, label="P99", color="#d62728", alpha=0.85)
        plt.xticks(x, labels, rotation=20, ha="right")
        plt.xlabel("Read mode")
        plt.ylabel("Seek latency (us)")
        plt.title(f"Seek Latency by Read Mode (cache={cache:.0f}GiB)")
        plt.legend()
        plt.grid(alpha=0.3, axis="y")
        for bars in (b1, b2):
            for b in bars:
                plt.text(b.get_x() + b.get_width() / 2, b.get_height(), f"{b.get_height():.1f}", ha="center", va="bottom", fontsize=7)
        plt.tight_layout()
        plt.savefig(out_dir / "mixgraph_seek_latency_vs_cache.png", dpi=160)
        plt.close()
        return

    data = sorted_rows_by_cache(rows, "mixgraph")
    if not data:
        return
    x = [float(r["cache_gib"]) for r in data]
    p50 = [float(r.get("seek_p50_us", 0)) for r in data]
    p95 = [float(r.get("seek_p95_us", 0)) for r in data]
    p99 = [float(r.get("seek_p99_us", 0)) for r in data]

    plt.figure(figsize=(8, 4.8))
    plt.plot(x, p50, marker="o", linewidth=2, label="P50")
    plt.plot(x, p95, marker="o", linewidth=2, label="P95")
    plt.plot(x, p99, marker="o", linewidth=2, label="P99")
    plt.xlabel("Block cache size (GiB)")
    plt.ylabel("Seek latency (us)")
    plt.title("Mixgraph Seek Latency vs Cache Size")
    plt.legend()
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_dir / "mixgraph_seek_latency_vs_cache.png", dpi=160)
    plt.close()


def draw_worst_locality(rows: List[Dict[str, object]], out_dir: Path):
    scenarios = ["worst_seek1", "worst_seek4", "worst_seek20", "worst_seek200", "worst_seek10000"]
    data_map = {s: sorted_rows_by_cache(rows, s) for s in scenarios}
    caches = sorted({float(r["cache_gib"]) for s in scenarios for r in data_map[s]})
    if not caches:
        return

    plt.figure(figsize=(9, 5.2))
    for s in scenarios:
        d = {float(r["cache_gib"]): float(r.get("ops_per_sec", float("nan"))) for r in data_map[s]}
        y = [d.get(c, float("nan")) for c in caches]
        plt.plot(caches, y, marker="o", linewidth=2, label=s)

    plt.yscale("log")
    plt.xlabel("Block cache size (GiB)")
    plt.ylabel("Ops/sec (log scale)")
    plt.title("Worst-locality Throughput")
    plt.legend(ncol=2)
    plt.grid(alpha=0.3, which="both")
    plt.tight_layout()
    plt.savefig(out_dir / "worst_locality_ops_vs_cache.png", dpi=160)
    plt.close()


def draw_mixgraph_context(rows: List[Dict[str, object]], out_dir: Path):
    single = single_cache_mode_rows(rows)
    if single is not None:
        cache, data = single
        labels = [READ_MODE_LABEL.get(str(r.get("scenario")), str(r.get("scenario"))) for r in data]
        x = np.arange(len(data))
        p99 = [float(r.get("seek_p99_us", 0)) for r in data]
        l0 = [float(r.get("l0_files_end", 0)) for r in data]

        fig, ax1 = plt.subplots(figsize=(10.8, 5.0))
        bars = ax1.bar(x, p99, color="#1f77b4", alpha=0.9)
        ax1.set_xticks(x)
        ax1.set_xticklabels(labels, rotation=20, ha="right")
        ax1.set_xlabel("Read mode")
        ax1.set_ylabel("Seek P99 (us)", color="#1f77b4")
        ax1.tick_params(axis="y", labelcolor="#1f77b4")
        ax1.grid(alpha=0.3, axis="y")

        ax2 = ax1.twinx()
        ax2.plot(x, l0, marker="o", linewidth=2, color="#d62728")
        ax2.set_ylabel("L0 files at end of step", color="#d62728")
        ax2.tick_params(axis="y", labelcolor="#d62728")

        for i, b in enumerate(bars):
            ax1.text(b.get_x() + b.get_width() / 2, b.get_height(), f"{p99[i]:.1f}", ha="center", va="bottom", fontsize=7)
        plt.title(f"P99 + L0 Context by Read Mode (cache={cache:.0f}GiB)")
        fig.tight_layout()
        plt.savefig(out_dir / "mixgraph_seek_p99_with_l0_context.png", dpi=160)
        plt.close(fig)
        return

    data = sorted_rows_by_cache(rows, "mixgraph")
    if not data:
        return

    x = [float(r["cache_gib"]) for r in data]
    p99 = [float(r.get("seek_p99_us", 0)) for r in data]
    l0 = [float(r.get("l0_files_end", 0)) for r in data]

    fig, ax1 = plt.subplots(figsize=(8.4, 4.8))
    ax1.plot(x, p99, marker="o", linewidth=2, color="#1f77b4", label="seek_p99_us")
    ax1.set_xlabel("Block cache size (GiB)")
    ax1.set_ylabel("Seek P99 (us)", color="#1f77b4")
    ax1.tick_params(axis="y", labelcolor="#1f77b4")
    ax1.grid(alpha=0.3)

    ax2 = ax1.twinx()
    ax2.plot(x, l0, marker="s", linewidth=2, color="#d62728", label="l0_files_end")
    ax2.set_ylabel("L0 files at end of run", color="#d62728")
    ax2.tick_params(axis="y", labelcolor="#d62728")

    plt.title("Mixgraph: P99 vs Cache (with L0 context)")
    fig.tight_layout()
    plt.savefig(out_dir / "mixgraph_seek_p99_with_l0_context.png", dpi=160)
    plt.close(fig)


def build_scenario_matrix(rows: List[Dict[str, object]]):
    scenario_order = [
        "mixgraph",
        "seek200",
        "worst_seek1",
        "worst_seek4",
        "worst_seek20",
        "worst_seek200",
        "worst_seek10000",
    ]
    cache_vals = sorted(
        {float(r["cache_gib"]) for r in rows if r.get("cache_gib") is not None and r.get("scenario") in scenario_order}
    )
    by_key = {(r.get("scenario"), float(r.get("cache_gib", 0))): r for r in rows}

    throughput = []
    p95 = []
    p99 = []
    for s in scenario_order:
        t_row = []
        p95_row = []
        p99_row = []
        for c in cache_vals:
            r = by_key.get((s, c), {})
            t_row.append(float(r.get("ops_per_sec", 0.0)))
            p95_row.append(float(r.get("seek_p95_us", 0.0)))
            p99_row.append(float(r.get("seek_p99_us", 0.0)))
        throughput.append(t_row)
        p95.append(p95_row)
        p99.append(p99_row)
    return scenario_order, cache_vals, throughput, p95, p99


def draw_clarity_dashboard(rows: List[Dict[str, object]], out_dir: Path):
    scenario_order, cache_vals, throughput, p95, p99 = build_scenario_matrix(rows)
    if not cache_vals:
        return

    cfg = {}
    for r in rows:
        if r.get("scenario") == "mixgraph":
            cfg = r
            break
    if not cfg and rows:
        cfg = rows[0]

    fig = plt.figure(figsize=(14, 9.5))
    gs = fig.add_gridspec(3, 2, height_ratios=[0.9, 1.2, 1.2])

    ax_text = fig.add_subplot(gs[0, :])
    ax_text.axis("off")

    cache_str = ", ".join(f"{c:.0f}GiB" for c in cache_vals)
    num_keys = cfg.get("cfg_num_keys", "")
    key_size = cfg.get("cfg_key_size", "")
    value_size = cfg.get("cfg_value_size", "")
    threads = cfg.get("cfg_threads", "")
    compression = cfg.get("cfg_compression_type", "")
    fill_entries = cfg.get("fill_entries", "")
    fill_raw = cfg.get("fill_raw_size_bytes", 0)
    fill_file = cfg.get("fill_file_size_bytes", 0)

    def fmt_gb(x):
        if not x:
            return "NA"
        return f"{float(x)/(1024**3):.3f} GB"

    mix_cfg = next((r for r in rows if r.get("scenario") == "mixgraph"), {})
    locality_desc = (
        f"mixgraph: key_dist(a={mix_cfg.get('cmd_key_dist_a','?')}, b={mix_cfg.get('cmd_key_dist_b','?')}), "
        f"keyrange_num={mix_cfg.get('cmd_keyrange_num','?')}, "
        f"iter(k={mix_cfg.get('cmd_iter_k','?')}, sigma={mix_cfg.get('cmd_iter_sigma','?')}, theta={mix_cfg.get('cmd_iter_theta','?')}), "
        f"ratios(get/put/seek={mix_cfg.get('cmd_mix_get_ratio','?')}/{mix_cfg.get('cmd_mix_put_ratio','?')}/{mix_cfg.get('cmd_mix_seek_ratio','?')})"
    )
    seek200_desc = "seek200 inject: seekrandom with seek_nexts=200"
    worst_desc = "worst locality: seekrandom with seek_nexts={1,4,20,200,10000}"

    text = (
        "实验上下文（Data Scale + Locality + Cache + Metrics）\n"
        f"num_keys={num_keys} (fill entries={fill_entries}), key_size={key_size}, value_size={value_size}, "
        f"threads={threads}, compression={compression}, cache=[{cache_str}]\n"
        f"fill size estimate: raw={fmt_gb(fill_raw)}, file={fmt_gb(fill_file)}\n"
        f"{locality_desc}\n"
        f"{seek200_desc}; {worst_desc}\n"
        "下方 heatmap：rows=scenario，cols=cache；格内标注精确数值。"
    )
    ax_text.text(0.01, 0.96, text, va="top", ha="left", fontsize=10.5, family="monospace")

    def draw_heatmap(ax, data, title, unit, cmap):
        im = ax.imshow(data, aspect="auto", cmap=cmap)
        ax.set_title(title)
        ax.set_yticks(range(len(scenario_order)))
        ax.set_yticklabels(scenario_order)
        ax.set_xticks(range(len(cache_vals)))
        ax.set_xticklabels([f"{c:.0f}GiB" for c in cache_vals])
        for i in range(len(scenario_order)):
            for j in range(len(cache_vals)):
                v = data[i][j]
                label = f"{v:.0f}" if abs(v) >= 100 else f"{v:.2f}"
                if not np.isfinite(v):
                    color = "black"
                else:
                    r, g, b, _ = im.cmap(im.norm(v))
                    luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
                    color = "white" if luminance < 0.5 else "black"
                ax.text(j, i, label, ha="center", va="center", fontsize=8.5, color=color)
        cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
        cbar.set_label(unit)

    ax1 = fig.add_subplot(gs[1, 0])
    draw_heatmap(ax1, throughput, "Throughput 矩阵 (ops/sec)", "ops/sec", "YlGnBu")

    ax2 = fig.add_subplot(gs[1, 1])
    draw_heatmap(ax2, p95, "Seek Latency 矩阵 P95 (us)", "us", "YlOrRd")

    ax3 = fig.add_subplot(gs[2, 0])
    draw_heatmap(ax3, p99, "Seek Latency 矩阵 P99 (us)", "us", "OrRd")
    ax4 = fig.add_subplot(gs[2, 1])
    ax4.axis("off")

    # 保留紧凑表格，便于快速查值。
    table_rows = []
    for s_idx, s in enumerate(scenario_order):
        for c_idx, c in enumerate(cache_vals):
            table_rows.append(
                [s, f"{c:.0f}", f"{throughput[s_idx][c_idx]:.0f}", f"{p95[s_idx][c_idx]:.1f}", f"{p99[s_idx][c_idx]:.1f}"]
            )
    col_labels = ["scenario", "cache(GiB)", "ops/sec", "p95(us)", "p99(us)"]
    tbl = ax4.table(cellText=table_rows, colLabels=col_labels, cellLoc="center", loc="center")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(8)
    tbl.scale(1, 1.3)
    ax4.set_title("精确结果表")

    fig.tight_layout()
    fig.savefig(out_dir / "experiment_clarity_dashboard.png", dpi=170)
    plt.close(fig)


def export_clarity_csv(rows: List[Dict[str, object]], out_dir: Path):
    fields = [
        "scenario",
        "cache_gib",
        "cfg_num_keys",
        "fill_entries",
        "fill_raw_size_bytes",
        "fill_file_size_bytes",
        "cmd_reads",
        "cmd_num",
        "cmd_threads",
        "cmd_benchmarks",
        "cmd_seek_nexts",
        "cmd_mix_get_ratio",
        "cmd_mix_put_ratio",
        "cmd_mix_seek_ratio",
        "cmd_key_dist_a",
        "cmd_key_dist_b",
        "cmd_keyrange_dist_a",
        "cmd_keyrange_dist_b",
        "cmd_keyrange_dist_c",
        "cmd_keyrange_dist_d",
        "cmd_keyrange_num",
        "cmd_iter_k",
        "cmd_iter_sigma",
        "cmd_iter_theta",
        "ops_per_sec",
        "micros_per_op",
        "seek_p50_us",
        "seek_p95_us",
        "seek_p99_us",
        "cache_hit_ratio_pct",
        "l0_files_end",
        "cumulative_writes_count",
    ]
    scenario_order = ["mixgraph", "seek200", "worst_seek1", "worst_seek4", "worst_seek20", "worst_seek200", "worst_seek10000"]
    out_rows = [r for r in rows if r.get("scenario") in scenario_order]
    out_rows.sort(key=lambda r: (scenario_order.index(str(r.get("scenario"))), float(r.get("cache_gib", 0))))
    with (out_dir / "results_clarity.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in out_rows:
            w.writerow(r)


def export_csv(rows: List[Dict[str, object]], out_dir: Path):
    csv_path = out_dir / "metrics_table.csv"

    ordered_fields = [
        "run_tag",
        "scenario",
        "scenario_family",
        "step_name",
        "step_id",
        "step_order",
        "log_file",
        "cache_bytes",
        "cache_gib",
        "cfg_profile",
        "cfg_scale",
        "cfg_num_keys",
        "cfg_realistic_reads",
        "cfg_step200_reads",
        "cfg_worst_reads_1",
        "cfg_worst_reads_4",
        "cfg_worst_reads_20",
        "cfg_worst_reads_200",
        "cfg_worst_reads_10000",
        "cfg_threads",
        "cfg_key_size",
        "cfg_value_size",
        "cfg_compression_type",
        "cfg_use_direct",
        "cmd_num",
        "cmd_reads",
        "cmd_threads",
        "cmd_seek_nexts",
        "cmd_mix_get_ratio",
        "cmd_mix_put_ratio",
        "cmd_mix_seek_ratio",
        "cmd_cache_size",
        "cmd_key_size",
        "cmd_value_size",
        "cmd_compression_type",
        "cmd_use_direct_reads",
        "cmd_use_direct_io_for_flush_and_compaction",
        "bench_name",
        "ops_per_sec",
        "micros_per_op",
        "run_seconds",
        "operations",
        "throughput_mb_s",
        "seek_p50_us",
        "seek_p95_us",
        "seek_p99_us",
        "seek_p100_us",
        "seek_count",
        "cache_hit",
        "cache_miss",
        "cache_hit_ratio_pct",
        "cache_bytes_read",
        "db_seek_count",
        "db_next_count",
        "iter_bytes_read",
        "l0_files_end",
        "l0_compacting_files_end",
        "l0_size_bytes_end",
        "uptime_total_s",
        "uptime_interval_s",
        "cumulative_writes_count",
        "cumulative_ingest_gb",
        "cumulative_ingest_mb_s",
        "keys_written_count",
        "keys_read_count",
        "mix_gets",
        "mix_puts",
        "mix_seeks",
        "mix_reads",
        "mix_reads_found",
        "mix_avg_value_size",
        "mix_avg_scan_len",
        "seekrandom_found",
        "seekrandom_total",
        "cmd_db",
        "cmd_wal_dir",
    ]

    extras = sorted({k for row in rows for k in row.keys() if k not in ordered_fields})
    fields = ordered_fields + extras

    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for r in rows:
            writer.writerow(r)


def pearson_corr(xs: List[float], ys: List[float]) -> Optional[float]:
    if len(xs) != len(ys) or len(xs) < 2:
        return None
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    dx = [x - mx for x in xs]
    dy = [y - my for y in ys]
    num = sum(a * b for a, b in zip(dx, dy))
    den_x = sum(a * a for a in dx) ** 0.5
    den_y = sum(b * b for b in dy) ** 0.5
    if den_x == 0 or den_y == 0:
        return None
    return num / (den_x * den_y)


def normalize_trend(v: str) -> str:
    x = (v or "").strip().lower()
    if x in ("increase", "decrease", "flat", "unknown"):
        return x
    return "unknown"


def infer_trend(values: List[float]) -> str:
    if len(values) < 2:
        return "unknown"
    first = values[0]
    last = values[-1]
    if first <= 0:
        return "unknown"
    ratio = last / first
    if ratio > 1.05:
        return "increase"
    if ratio < 0.95:
        return "decrease"
    return "flat"


def trend_compare(expected: str, observed: str) -> str:
    e = normalize_trend(expected)
    o = normalize_trend(observed)
    if e == "unknown" or o == "unknown":
        return "manual-check"
    return "match" if e == o else "mismatch"


def write_report(rows: List[Dict[str, object]], out_dir: Path):
    report_path = out_dir / "report.md"
    mix = sorted_rows_by_cache(rows, "mixgraph")
    sample = mix[0] if mix else (rows[0] if rows else {})
    fill_entries = sample.get("fill_entries", "NA")
    fill_raw = sample.get("fill_raw_size_bytes", 0)
    fill_file = sample.get("fill_file_size_bytes", 0)

    def fmt_bytes(v):
        if not v:
            return "NA"
        return f"{float(v)/(1024**3):.3f} GB"

    lines = [
        "# Shortscan 结果报告（含上下文）",
        "",
        "## 实验上下文",
        f"- 数据规模: `num_keys={sample.get('cfg_num_keys','NA')}`, `fill_entries={fill_entries}`, "
        f"`key_size={sample.get('cfg_key_size','NA')}`, `value_size={sample.get('cfg_value_size','NA')}`",
        f"- 数据体量（来自 fill log）: `raw={fmt_bytes(fill_raw)}`, `file={fmt_bytes(fill_file)}`",
        f"- cache sizes: `{sample.get('cfg_cache_sizes','NA')}` (bytes)",
        f"- threads/compression/direct-io: `threads={sample.get('cfg_threads','NA')}`, `compression={sample.get('cfg_compression_type','NA')}`, `use_direct={sample.get('cfg_use_direct','NA')}`",
        "",
        "## Locality / workload 模型",
        f"- mixgraph locality: `key_dist(a={sample.get('cmd_key_dist_a','NA')}, b={sample.get('cmd_key_dist_b','NA')})`, "
        f"`keyrange_num={sample.get('cmd_keyrange_num','NA')}`, "
        f"`keyrange_dist=({sample.get('cmd_keyrange_dist_a','NA')}, {sample.get('cmd_keyrange_dist_b','NA')}, {sample.get('cmd_keyrange_dist_c','NA')}, {sample.get('cmd_keyrange_dist_d','NA')})`",
        f"- scan 长度模型: `iter(k={sample.get('cmd_iter_k','NA')}, sigma={sample.get('cmd_iter_sigma','NA')}, theta={sample.get('cmd_iter_theta','NA')})`",
        f"- mix ratio: `get/put/seek={sample.get('cmd_mix_get_ratio','NA')}/{sample.get('cmd_mix_put_ratio','NA')}/{sample.get('cmd_mix_seek_ratio','NA')}`",
        "- 200-step 注入: `seekrandom + seek_nexts=200`",
        "- worst locality 集合: `seek_nexts={1,4,20,200,10000}`",
        "",
        "## 核心结果（不同 cache 下的 latency / throughput）",
        "| scenario | cache_gib | ops_per_sec | seek_p95_us | seek_p99_us | cache_hit_ratio_pct | l0_files_end |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    scenario_order = ["mixgraph", "seek200", "worst_seek1", "worst_seek4", "worst_seek20", "worst_seek200", "worst_seek10000"]
    for s in scenario_order:
        ss = sorted_rows_by_cache(rows, s)
        for r in ss:
            lines.append(
                "| {scenario} | {cache:.1f} | {ops} | {p95:.3f} | {p99:.3f} | {hit:.4f} | {l0} |".format(
                    scenario=s,
                    cache=float(r.get("cache_gib", 0)),
                    ops=r.get("ops_per_sec", ""),
                    p95=float(r.get("seek_p95_us", 0)),
                    p99=float(r.get("seek_p99_us", 0)),
                    hit=float(r.get("cache_hit_ratio_pct", 0)),
                    l0=r.get("l0_files_end", ""),
                )
            )

    lines.extend(["", "## 相关性快照（仅 mixgraph）"])
    c_cache = pearson_corr(
        [float(r.get("cache_gib", 0)) for r in mix],
        [float(r.get("seek_p99_us", 0)) for r in mix],
    )
    c_l0 = pearson_corr(
        [float(r.get("l0_files_end", 0)) for r in mix],
        [float(r.get("seek_p99_us", 0)) for r in mix],
    )
    lines.append(f"- corr(cache_gib, seek_p99_us) = {c_cache if c_cache is not None else 'NA'}")
    lines.append(f"- corr(l0_files_end, seek_p99_us) = {c_l0 if c_l0 is not None else 'NA'}")
    lines.extend(
        [
            "",
            "## 说明",
            "- `mixgraph_seek_p99_with_l0_context.png`: 左轴是 seek P99，右轴是 L0 file count，用于判断 latency 变化是否被 L0 状态漂移干扰。",
            "- 若 cache 与 L0 随执行顺序同时上升，则 cache-size 效应可能被 DB-state drift 混淆。",
            "- 当单次 run 只有一个 cache 点时，`mixgraph_*_vs_cache.png` 会自动切到按 read mode 的竖向柱状图。",
            "- 建议与 `mixgraph_seek_p99_with_l0_context.png` 结合阅读本报告。",
            "",
        ]
    )

    report_path.write_text("\n".join(lines), encoding="utf-8")


def write_experiment_result_doc(
    rows: List[Dict[str, object]], cfg: Dict[str, str], out_dir: Path, experiment_dir: Path
):
    experiment_dir.mkdir(parents=True, exist_ok=True)
    result_path = experiment_dir / "experiment_result.md"

    mix = sorted_rows_by_cache(rows, "mixgraph")
    cache_list = [float(r.get("cache_gib", 0.0)) for r in mix]
    tput_list = [float(r.get("ops_per_sec", 0.0)) for r in mix]
    p99_list = [float(r.get("seek_p99_us", 0.0)) for r in mix]
    observed_tput_trend = infer_trend(tput_list)
    observed_p99_trend = infer_trend(p99_list)

    expected_tput_trend = cfg.get("EXPECTED_CACHE_TPUT_TREND", "unknown")
    expected_p99_trend = cfg.get("EXPECTED_CACHE_P99_TREND", "unknown")

    scenario_order = [
        "mixgraph",
        "seek200",
        "worst_seek1",
        "worst_seek4",
        "worst_seek20",
        "worst_seek200",
        "worst_seek10000",
    ]

    lines = [
        "# 实验结果",
        "",
        "## 身份信息",
        f"- run_tag: {cfg.get('RUN_TAG', 'NA')}",
        f"- exp_date: {cfg.get('EXP_DATE', 'NA')}",
        f"- global_exp_id: {cfg.get('GLOBAL_EXP_ID', 'NA')}",
        f"- experiment_name: {cfg.get('EXPERIMENT_NAME', 'NA')}",
        f"- title: {cfg.get('EXPERIMENT_TITLE', 'NA')}",
        "",
        "## 关键配置（与结果解读相关）",
        f"- data_scale: num_keys={cfg.get('NUM_KEYS', 'NA')}, key_size={cfg.get('KEY_SIZE', 'NA')}, value_size={cfg.get('VALUE_SIZE', 'NA')}",
        f"- cache_sizes(bytes): {cfg.get('CACHE_SIZES', 'NA')}",
        f"- locality: {cfg.get('KEY_LOCALITY_DESC', 'NA')}",
        f"- rocksdb_builtin_optimizations: {cfg.get('ROCKSDB_BUILTIN_OPTIMIZATIONS', 'NA')}",
        f"- experiment_variables: {cfg.get('EXPERIMENT_VARIABLES', 'NA')}",
        "",
        "## 预期",
        f"- objective: {cfg.get('EXPERIMENT_OBJECTIVE', 'NA')}",
        f"- expected_statement: {cfg.get('EXPERIMENT_EXPECTATION', 'NA')}",
        f"- expected_cache_tput_trend: {normalize_trend(expected_tput_trend)}",
        f"- expected_cache_p99_trend: {normalize_trend(expected_p99_trend)}",
        "",
        "## 实测结果（来自 metrics_table/results_clarity）",
        "| scenario | cache_gib | ops_per_sec | seek_p95_us | seek_p99_us | cache_hit_ratio_pct |",
        "|---|---:|---:|---:|---:|---:|",
    ]

    for s in scenario_order:
        ss = sorted_rows_by_cache(rows, s)
        for r in ss:
            lines.append(
                "| {scenario} | {cache:.1f} | {ops} | {p95:.3f} | {p99:.3f} | {hit:.4f} |".format(
                    scenario=s,
                    cache=float(r.get("cache_gib", 0.0)),
                    ops=r.get("ops_per_sec", ""),
                    p95=float(r.get("seek_p95_us", 0.0)),
                    p99=float(r.get("seek_p99_us", 0.0)),
                    hit=float(r.get("cache_hit_ratio_pct", 0.0)),
                )
            )

    lines.extend(
        [
            "",
            "## 预期 vs 实测（mixgraph cache 趋势）",
            f"- observed_cache_order_gib: {[round(x, 3) for x in cache_list]}",
            f"- observed_tput_trend: {observed_tput_trend}",
            f"- observed_p99_trend: {observed_p99_trend}",
            f"- tput_trend_check: {trend_compare(expected_tput_trend, observed_tput_trend)}",
            f"- p99_trend_check: {trend_compare(expected_p99_trend, observed_p99_trend)}",
            "",
            "## 产物",
            f"- figures_dir: {out_dir}",
            f"- metrics_table: {out_dir / 'metrics_table.csv'}",
            f"- clarity_csv: {out_dir / 'results_clarity.csv'}",
            f"- report: {out_dir / 'report.md'}",
            "",
            "## 说明",
            "- 如果 trend check 为 mismatch，请在 report.md 中重点检查混淆项（L0 growth、DB-state drift、write interference、cache warmup）。",
            "",
        ]
    )

    result_path.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Build context-aware result table and plots for RocksDB short-scan benchmark."
    )
    parser.add_argument("--run-dir", required=True, help="Benchmark run directory")
    parser.add_argument(
        "--out-dir",
        default=None,
        help="Output directory for figures/tables (default: <run-dir>/figures)",
    )
    parser.add_argument(
        "--experiment-dir",
        default=None,
        help="Experiment root dir for writing experiment_result.md (default: infer from run-dir)",
    )
    args = parser.parse_args()

    run_dir = Path(args.run_dir).resolve()
    out_dir = Path(args.out_dir).resolve() if args.out_dir else run_dir / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    cfg = parse_config(run_dir / "config.txt")

    experiment_dir: Optional[Path] = None
    if args.experiment_dir:
        experiment_dir = Path(args.experiment_dir).resolve()
    elif run_dir.parent.name == "run_results":
        experiment_dir = run_dir.parent.parent

    rows = collect_rows(run_dir)

    draw_mixgraph_throughput(rows, out_dir)
    draw_mixgraph_seek_latency(rows, out_dir)
    draw_worst_locality(rows, out_dir)
    draw_mixgraph_context(rows, out_dir)
    draw_clarity_dashboard(rows, out_dir)
    export_csv(rows, out_dir)
    export_clarity_csv(rows, out_dir)
    write_report(rows, out_dir)
    if experiment_dir is not None:
        write_experiment_result_doc(rows, cfg, out_dir, experiment_dir)

    print(f"Context-aware figures and tables written to: {out_dir}")


if __name__ == "__main__":
    main()

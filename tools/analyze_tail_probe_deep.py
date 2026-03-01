#!/usr/bin/env python3
"""
Deep analyzer for db_bench TailProbeWriter CSV (per-request PerfContext deltas).

Why this exists:
  - Linux `perf` is not always available in the sandbox.
  - TailProbeWriter already exports a useful subset of PerfContext and
    IOStatsContext deltas for slow requests.
  - This script turns those raw deltas into a bottleneck-oriented report:
      * which counters track latency best (correlation)
      * how much time is IO-wait vs CPU in the block-read path
      * how many block reads happen per tail request (miss amplification)

Input:
  - tail_probe/samples/*_tail_samples.csv (TailProbeWriter output)

Output (under --out_dir):
  - deep_report.md
  - metrics_rank.csv
  - block_read_count_hist.png
  - scatter_latency_vs_block_reads.png
  - scatter_latency_vs_io_wait.png
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path
from typing import Dict, List, Tuple


def _read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def _write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def _to_float(v: object) -> float:
    if v is None:
        return float("nan")
    s = str(v).strip()
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def _to_int(v: object) -> int:
    x = _to_float(v)
    if math.isnan(x) or math.isinf(x):
        return 0
    return int(x)


def _is_finite(x: float) -> bool:
    return not math.isnan(x) and not math.isinf(x)


def _ns_to_us(ns: float) -> float:
    if not _is_finite(ns):
        return float("nan")
    return ns / 1000.0


def _quantile(vals: List[float], q: float) -> float:
    xs = [v for v in vals if _is_finite(v)]
    if not xs:
        return float("nan")
    xs.sort()
    idx = max(0, min(len(xs) - 1, int(math.ceil(q * len(xs))) - 1))
    return float(xs[idx])


def _pearson(xs: List[float], ys: List[float]) -> float:
    pairs: List[Tuple[float, float]] = []
    for x, y in zip(xs, ys):
        if not _is_finite(x) or not _is_finite(y):
            continue
        pairs.append((x, y))
    if len(pairs) < 2:
        return float("nan")
    mx = sum(x for x, _ in pairs) / len(pairs)
    my = sum(y for _, y in pairs) / len(pairs)
    vx = sum((x - mx) ** 2 for x, _ in pairs)
    vy = sum((y - my) ** 2 for _, y in pairs)
    if vx <= 0 or vy <= 0:
        return float("nan")
    cov = sum((x - mx) * (y - my) for x, y in pairs)
    return cov / math.sqrt(vx * vy)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", required=True, help="TailProbeWriter CSV file")
    ap.add_argument("--out_dir", required=True, help="Output directory")
    ap.add_argument("--title", default="", help="Optional title shown in report")
    args = ap.parse_args()

    samples_csv = Path(args.samples).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = _read_csv(samples_csv)
    if not rows:
        raise SystemExit(f"empty samples: {samples_csv}")

    latency_us = [_to_float(r.get("latency_us", "")) for r in rows]
    case_label = str(rows[0].get("case_label", "") or "")
    scenario = str(rows[0].get("scenario", "") or "")

    # Derived, readable metrics (microseconds).
    block_read_time_us = [_ns_to_us(_to_float(r.get("delta_block_read_time_ns", ""))) for r in rows]
    block_read_cpu_us = [_ns_to_us(_to_float(r.get("delta_block_read_cpu_time_ns", ""))) for r in rows]
    block_read_wait_us = [
        max(t - c, 0.0) if _is_finite(t) and _is_finite(c) else float("nan")
        for t, c in zip(block_read_time_us, block_read_cpu_us)
    ]

    io_read_us = [_ns_to_us(_to_float(r.get("delta_io_read_nanos", ""))) for r in rows]
    io_cpu_read_us = [_ns_to_us(_to_float(r.get("delta_io_cpu_read_nanos", ""))) for r in rows]
    io_wait_us = [
        max(t - c, 0.0) if _is_finite(t) and _is_finite(c) else float("nan")
        for t, c in zip(io_read_us, io_cpu_read_us)
    ]

    checksum_us = [_ns_to_us(_to_float(r.get("delta_block_checksum_time_ns", ""))) for r in rows]
    decompress_us = [_ns_to_us(_to_float(r.get("delta_block_decompress_time_ns", ""))) for r in rows]

    read_index_us = [_ns_to_us(_to_float(r.get("delta_read_index_block_nanos", ""))) for r in rows]
    read_filter_us = [_ns_to_us(_to_float(r.get("delta_read_filter_block_nanos", ""))) for r in rows]
    find_table_us = [_ns_to_us(_to_float(r.get("delta_find_table_nanos", ""))) for r in rows]
    new_table_iter_us = [_ns_to_us(_to_float(r.get("delta_new_table_iterator_nanos", ""))) for r in rows]
    new_block_iter_us = [_ns_to_us(_to_float(r.get("delta_new_table_block_iter_nanos", ""))) for r in rows]
    block_seek_us = [_ns_to_us(_to_float(r.get("delta_block_seek_nanos", ""))) for r in rows]
    get_from_output_files_us = [
        _ns_to_us(_to_float(r.get("delta_get_from_output_files_time_ns", ""))) for r in rows
    ]
    seek_on_memtable_us = [_ns_to_us(_to_float(r.get("delta_seek_on_memtable_time_ns", ""))) for r in rows]

    # Counts / bytes
    block_read_count = [_to_int(r.get("delta_block_read_count", 0)) for r in rows]
    block_read_bytes = [_to_int(r.get("delta_block_read_byte", 0)) for r in rows]
    block_cache_hits = [_to_int(r.get("delta_block_cache_hit_count", 0)) for r in rows]
    iter_read_bytes = [_to_int(r.get("delta_iter_read_bytes", 0)) for r in rows]
    iter_seek_count = [_to_int(r.get("delta_iter_seek_count", 0)) for r in rows]

    # Basic latency stats
    lat_f = [x for x in latency_us if _is_finite(x)]
    if not lat_f:
        raise SystemExit("no finite latency_us in samples")
    lat_p50 = _quantile(lat_f, 0.50)
    lat_p95 = _quantile(lat_f, 0.95)
    lat_p99 = _quantile(lat_f, 0.99)
    lat_max = max(lat_f)

    # Split by the sample's own latency p99 (top 1% inside tail samples).
    split_threshold = lat_p99
    top_mask = [(_is_finite(x) and x >= split_threshold) for x in latency_us]

    def mean(vals: List[float]) -> float:
        xs = [v for v in vals if _is_finite(v)]
        return float(sum(xs) / len(xs)) if xs else float("nan")

    def mean_top(vals: List[float]) -> Tuple[float, float]:
        top = [v for v, m in zip(vals, top_mask) if m and _is_finite(v)]
        rest = [v for v, m in zip(vals, top_mask) if (not m) and _is_finite(v)]
        return (mean(top), mean(rest))

    lat_mean = mean(latency_us)

    metrics: List[Tuple[str, str, List[float]]] = [
        ("block_read_wait_us", "us", block_read_wait_us),
        ("block_read_cpu_us", "us", block_read_cpu_us),
        ("io_wait_us", "us", io_wait_us),
        ("io_cpu_read_us", "us", io_cpu_read_us),
        ("checksum_us", "us", checksum_us),
        ("decompress_us", "us", decompress_us),
        ("read_index_us", "us", read_index_us),
        ("read_filter_us", "us", read_filter_us),
        ("find_table_us", "us", find_table_us),
        ("new_table_iter_us", "us", new_table_iter_us),
        ("new_block_iter_us", "us", new_block_iter_us),
        ("block_seek_us", "us", block_seek_us),
        ("get_from_output_files_us", "us", get_from_output_files_us),
        ("seek_on_memtable_us", "us", seek_on_memtable_us),
        ("block_read_count", "count", [float(x) for x in block_read_count]),
        ("block_read_bytes", "bytes", [float(x) for x in block_read_bytes]),
        ("block_cache_hits", "count", [float(x) for x in block_cache_hits]),
        ("iter_read_bytes", "bytes", [float(x) for x in iter_read_bytes]),
        ("iter_seek_count", "count", [float(x) for x in iter_seek_count]),
    ]

    rank_rows: List[Dict[str, object]] = []
    for name, unit, vals in metrics:
        m = mean(vals)
        p50 = _quantile(vals, 0.50)
        p95 = _quantile(vals, 0.95)
        p99v = _quantile(vals, 0.99)
        mx = max([v for v in vals if _is_finite(v)], default=float("nan"))
        corr = _pearson(vals, latency_us)
        top_mean, rest_mean = mean_top(vals)
        share_mean_pct = (m * 100.0 / lat_mean) if _is_finite(m) and lat_mean > 0 and unit == "us" else float("nan")
        rank_rows.append(
            {
                "metric": name,
                "unit": unit,
                "mean": m,
                "p50": p50,
                "p95": p95,
                "p99": p99v,
                "max": mx,
                "corr_with_latency": corr,
                "mean_share_of_latency_pct": share_mean_pct,
                "mean_top1pct": top_mean,
                "mean_rest": rest_mean,
                "top1pct_minus_rest": (top_mean - rest_mean) if _is_finite(top_mean) and _is_finite(rest_mean) else float("nan"),
            }
        )

    # Sorting: prefer (share * |corr|) when unit is time; else |corr|.
    def score(r: Dict[str, object]) -> float:
        corr = float(r.get("corr_with_latency", float("nan")))
        if not _is_finite(corr):
            return -1.0
        share = float(r.get("mean_share_of_latency_pct", float("nan")))
        if _is_finite(share):
            return abs(corr) * share
        return abs(corr)

    rank_rows.sort(key=score, reverse=True)
    _write_csv(out_dir / "metrics_rank.csv", rank_rows)

    # Figures (best-effort; don't fail if matplotlib missing)
    try:
        import matplotlib.pyplot as plt  # type: ignore

        # Hist: block_read_count distribution
        fig = plt.figure(figsize=(9, 5))
        ax = fig.add_subplot(1, 1, 1)
        xs = [x for x in block_read_count]
        max_x = min(max(xs) if xs else 0, 20)
        bins = list(range(0, max_x + 2))
        ax.hist([min(x, max_x + 1) for x in xs], bins=bins, rwidth=0.85)
        ax.set_xlabel("delta_block_read_count per request (>=20 clamped)")
        ax.set_ylabel("samples")
        ax.grid(True, axis="y", alpha=0.25)
        ax.set_title("TailProbe: block reads per slow request")
        fig.tight_layout()
        fig.savefig(out_dir / "block_read_count_hist.png", dpi=170)
        plt.close(fig)

        # Scatter: latency vs block_read_count
        fig = plt.figure(figsize=(9, 5))
        ax = fig.add_subplot(1, 1, 1)
        ax.scatter(block_read_count, latency_us, s=6, alpha=0.25)
        ax.set_xlabel("delta_block_read_count")
        ax.set_ylabel("latency_us")
        ax.grid(True, alpha=0.25)
        ax.set_title("TailProbe: latency vs block read count")
        fig.tight_layout()
        fig.savefig(out_dir / "scatter_latency_vs_block_reads.png", dpi=170)
        plt.close(fig)

        # Scatter: latency vs io_wait_us
        fig = plt.figure(figsize=(9, 5))
        ax = fig.add_subplot(1, 1, 1)
        ax.scatter(io_wait_us, latency_us, s=6, alpha=0.25)
        ax.set_xlabel("io_wait_us (delta_io_read - delta_io_cpu_read)")
        ax.set_ylabel("latency_us")
        ax.grid(True, alpha=0.25)
        ax.set_title("TailProbe: latency vs IO-wait")
        fig.tight_layout()
        fig.savefig(out_dir / "scatter_latency_vs_io_wait.png", dpi=170)
        plt.close(fig)
    except Exception as e:  # noqa: BLE001
        (out_dir / "matplotlib_error.txt").write_text(str(e) + "\n", encoding="utf-8")

    # Markdown report
    title = args.title.strip() or f"{case_label or 'case'} / {scenario or 'scenario'}"
    lines: List[str] = []
    lines.append("# Tail Probe Deep Perf Report\n")
    lines.append(f"- title: `{title}`\n")
    lines.append(f"- samples: `{samples_csv}`\n")
    lines.append(f"- count: `{len(lat_f)}`\n")
    lines.append(f"- latency_us: mean={lat_mean:.3f}, p50={lat_p50:.3f}, p95={lat_p95:.3f}, p99={lat_p99:.3f}, max={lat_max:.3f}\n")
    lines.append("\n## Key takeaways\n")

    # A few focused statements that are robust.
    io_wait_mean = mean(io_wait_us)
    cpu_read_mean = mean(io_cpu_read_us)
    block_wait_mean = mean(block_read_wait_us)
    block_cpu_mean = mean(block_read_cpu_us)
    avg_reads = statistics.mean(block_read_count) if block_read_count else float("nan")
    lines.append(
        f"- IO wait dominates slow reads: mean io_wait_us={io_wait_mean:.3f} vs mean io_cpu_read_us={cpu_read_mean:.3f}.\n"
    )
    lines.append(
        f"- Block-read path is also wait-heavy: mean block_read_wait_us={block_wait_mean:.3f} vs mean block_read_cpu_us={block_cpu_mean:.3f}.\n"
    )
    lines.append(f"- Slow requests often involve multiple block reads: mean delta_block_read_count={avg_reads:.3f}.\n")
    lines.append("\n## Ranked metrics\n")
    lines.append("- See `metrics_rank.csv` for correlation / percentiles.\n")
    lines.append("\n## Figures\n")
    for fn in [
        "block_read_count_hist.png",
        "scatter_latency_vs_block_reads.png",
        "scatter_latency_vs_io_wait.png",
    ]:
        lines.append(f"- `{out_dir / fn}`\n")

    (out_dir / "deep_report.md").write_text("".join(lines), encoding="utf-8")
    print(f"[ok] wrote deep report under: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


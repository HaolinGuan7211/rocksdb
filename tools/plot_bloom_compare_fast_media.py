#!/usr/bin/env python3
"""
Plot a bloom_bits comparison at a single xp_latency point.

Input:
  - sweep_summary.csv (from summarize_exp44_media_sweep.py)
  - sweep_points.csv  (includes bloom_bits)

Output:
  - bloom_compare_latency_p50.png
  - bloom_compare_shares_p50.png
  - bloom_compare_table.csv (joined table)
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


def _read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def _to_float(v: str) -> float:
    s = (v or "").strip()
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def _to_int(v: str) -> int:
    x = _to_float(v)
    if x != x:
        return 0
    return int(x)


@dataclass(frozen=True)
class Row:
    bloom_bits: int
    xp_latency_ns: int
    probe_median: float
    latency_p50_us: float
    filter_total_share_p50_pct: float
    filter_fetch_share_p50_pct: float
    filter_cpu_share_p50_pct: float
    simfs_wait_actual_share_p50_pct: float
    stage_index_lookup_p50_share_pct: float
    stage_get_from_output_files_p50_share_pct: float
    stage_block_decode_checksum_p50_share_pct: float
    run_dir: str


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep_summary_csv", required=True)
    ap.add_argument("--sweep_points_csv", required=True)
    ap.add_argument("--out_dir", required=True)
    ap.add_argument("--probe_bucket", default="16+")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    points = _read_csv(Path(args.sweep_points_csv))
    bloom_by_run_tag = {p["run_tag"]: _to_int(p.get("bloom_bits", "0")) for p in points}

    summary = _read_csv(Path(args.sweep_summary_csv))
    rows: List[Row] = []
    for r in summary:
        if r.get("probe_bucket") != args.probe_bucket:
            continue
        run_tag = r.get("run_tag", "")
        bloom_bits = bloom_by_run_tag.get(run_tag, 0)

        # Pull block_decode_checksum share from the per-run stage distribution.
        run_dir = r.get("run_dir", "")
        stage_csv = Path(run_dir) / "analysis_readmissing_sample_cache_536870912" / "sample_stage_distribution.csv"
        decode_share = float("nan")
        if stage_csv.exists():
            for sr in _read_csv(stage_csv):
                if sr.get("stage") == "block_decode_checksum":
                    decode_share = _to_float(sr.get("p50_share_pct", ""))
                    break

        rows.append(
            Row(
                bloom_bits=bloom_bits,
                xp_latency_ns=_to_int(r.get("xp_latency_ns", "0")),
                probe_median=_to_float(r.get("probe_median", "")),
                latency_p50_us=_to_float(r.get("latency_p50_us", "")),
                filter_total_share_p50_pct=_to_float(r.get("filter_total_share_p50_pct", "")),
                filter_fetch_share_p50_pct=_to_float(r.get("filter_fetch_share_p50_pct", "")),
                filter_cpu_share_p50_pct=_to_float(r.get("filter_cpu_share_p50_pct", "")),
                simfs_wait_actual_share_p50_pct=_to_float(r.get("simfs_wait_actual_share_p50_pct", "")),
                stage_index_lookup_p50_share_pct=_to_float(r.get("stage_index_lookup_p50_share_pct", "")),
                stage_get_from_output_files_p50_share_pct=_to_float(
                    r.get("stage_get_from_output_files_p50_share_pct", "")
                ),
                stage_block_decode_checksum_p50_share_pct=decode_share,
                run_dir=run_dir,
            )
        )

    rows.sort(key=lambda x: x.bloom_bits)
    if not rows:
        raise SystemExit("no rows found")

    # Write joined table.
    table_path = out_dir / "bloom_compare_table.csv"
    with table_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "bloom_bits",
                "xp_latency_ns",
                "probe_median",
                "latency_p50_us",
                "filter_total_share_p50_pct",
                "filter_fetch_share_p50_pct",
                "filter_cpu_share_p50_pct",
                "simfs_wait_actual_share_p50_pct",
                "index_lookup_p50_share_pct",
                "get_from_output_files_p50_share_pct",
                "block_decode_checksum_p50_share_pct",
                "run_dir",
            ]
        )
        for rr in rows:
            w.writerow(
                [
                    rr.bloom_bits,
                    rr.xp_latency_ns,
                    rr.probe_median,
                    rr.latency_p50_us,
                    rr.filter_total_share_p50_pct,
                    rr.filter_fetch_share_p50_pct,
                    rr.filter_cpu_share_p50_pct,
                    rr.simfs_wait_actual_share_p50_pct,
                    rr.stage_index_lookup_p50_share_pct,
                    rr.stage_get_from_output_files_p50_share_pct,
                    rr.stage_block_decode_checksum_p50_share_pct,
                    rr.run_dir,
                ]
            )

    import matplotlib.pyplot as plt

    xs = [str(r.bloom_bits) for r in rows]

    # Latency bar.
    fig = plt.figure(figsize=(8, 5))
    ax = fig.add_subplot(1, 1, 1)
    ax.bar(xs, [r.latency_p50_us for r in rows], color=["#4C78A8"] * len(rows))
    ax.set_xlabel("bloom_bits (built into SST)")
    ax.set_ylabel("p50 latency (us)")
    ax.set_title(f"Fast media (xp_latency_ns={rows[0].xp_latency_ns}) latency @ p50")
    for i, rr in enumerate(rows):
        ax.text(i, rr.latency_p50_us, f"{rr.latency_p50_us:.0f}", ha="center", va="bottom", fontsize=9)
    fig.tight_layout()
    fig.savefig(out_dir / "bloom_compare_latency_p50.png", dpi=150)
    plt.close(fig)

    # Share stacked bars.
    fig = plt.figure(figsize=(10, 6))
    ax = fig.add_subplot(1, 1, 1)
    ax.set_xlabel("bloom_bits (built into SST)")
    ax.set_ylabel("share @ p50 (%)")
    ax.set_title(f"Fast media (xp_latency_ns={rows[0].xp_latency_ns}) stage/filter shares @ p50")

    filter_total = [r.filter_total_share_p50_pct for r in rows]
    index_lookup = [r.stage_index_lookup_p50_share_pct for r in rows]
    output_files = [r.stage_get_from_output_files_p50_share_pct for r in rows]
    decode = [r.stage_block_decode_checksum_p50_share_pct for r in rows]
    wait = [r.simfs_wait_actual_share_p50_pct for r in rows]

    ax.plot(xs, filter_total, marker="o", label="FilterTotal")
    ax.plot(xs, index_lookup, marker="o", label="index_lookup")
    ax.plot(xs, output_files, marker="o", label="get_from_output_files")
    ax.plot(xs, decode, marker="o", label="block_decode_checksum")
    ax.plot(xs, wait, marker="o", label="simfs_wait_actual")

    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.4)
    ax.legend(loc="best", fontsize=9)
    fig.tight_layout()
    fig.savefig(out_dir / "bloom_compare_shares_p50.png", dpi=150)
    plt.close(fig)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())


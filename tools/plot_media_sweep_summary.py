#!/usr/bin/env python3
"""
Plot a consolidated view for a media-latency sweep summary CSV.

Intended usage:
  python3 tools/plot_media_sweep_summary.py \
    --sweep_summary_csv experiment/.../sweep_summary.csv \
    --out_dir experiment/.../monitor_figures
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _to_float(v: Any) -> float:
    if v is None:
        return float("nan")
    s = str(v).strip()
    if s == "" or s.lower() == "nan":
        return float("nan")
    return float(s)


def _to_int(v: Any) -> int:
    return int(float(str(v).strip()))


@dataclass(frozen=True)
class Point:
    xp_latency_ns: int
    bypass_base_io: int
    probe_bucket: str

    latency_p50_us: float
    filter_fetch_p50_us: float
    filter_cpu_p50_us: float

    filter_total_share_p50_pct: float
    filter_fetch_share_p50_pct: float
    filter_cpu_share_p50_pct: float

    simfs_wait_actual_share_p50_pct: float
    simfs_injected_share_p50_pct: float
    base_over_injected_p50: float

    stage_get_from_output_files_p50_share_pct: float
    stage_index_lookup_p50_share_pct: float
    stage_block_read_io_p50_share_pct: float


def _read_points(path: Path) -> List[Point]:
    points: List[Point] = []
    with path.open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            points.append(
                Point(
                    xp_latency_ns=_to_int(r.get("xp_latency_ns")),
                    bypass_base_io=_to_int(r.get("bypass_base_io", "0") or "0"),
                    probe_bucket=str(r.get("probe_bucket", "")),
                    latency_p50_us=_to_float(r.get("latency_p50_us")),
                    filter_fetch_p50_us=_to_float(r.get("filter_fetch_p50_us")),
                    filter_cpu_p50_us=_to_float(r.get("filter_cpu_p50_us")),
                    filter_total_share_p50_pct=_to_float(r.get("filter_total_share_p50_pct")),
                    filter_fetch_share_p50_pct=_to_float(r.get("filter_fetch_share_p50_pct")),
                    filter_cpu_share_p50_pct=_to_float(r.get("filter_cpu_share_p50_pct")),
                    simfs_wait_actual_share_p50_pct=_to_float(r.get("simfs_wait_actual_share_p50_pct")),
                    simfs_injected_share_p50_pct=_to_float(r.get("simfs_injected_share_p50_pct")),
                    base_over_injected_p50=_to_float(r.get("base_over_injected_p50")),
                    stage_get_from_output_files_p50_share_pct=_to_float(
                        r.get("stage_get_from_output_files_p50_share_pct", "nan")
                    ),
                    stage_index_lookup_p50_share_pct=_to_float(r.get("stage_index_lookup_p50_share_pct", "nan")),
                    stage_block_read_io_p50_share_pct=_to_float(r.get("stage_block_read_io_p50_share_pct", "nan")),
                )
            )
    return points


def _group(points: Iterable[Point]) -> Dict[Tuple[int, str], List[Point]]:
    out: Dict[Tuple[int, str], List[Point]] = {}
    for p in points:
        key = (p.bypass_base_io, p.probe_bucket)
        out.setdefault(key, []).append(p)
    for k in out:
        out[k].sort(key=lambda x: x.xp_latency_ns)
    return out


def _plot_share_curves(groups: Dict[Tuple[int, str], List[Point]], out_dir: Path) -> None:
    import matplotlib.pyplot as plt

    for (bypass, bucket), pts in groups.items():
        xs = [p.xp_latency_ns for p in pts]

        def y(getter):
            return [getter(p) for p in pts]

        fig = plt.figure(figsize=(10, 6))
        ax = fig.add_subplot(1, 1, 1)
        ax.set_xscale("log")
        ax.set_xlabel("simulate_xp_latency_ns (log scale)")
        ax.set_ylabel("share @ p50 (%)")
        ax.set_title(f"Gate3 share curves (bypass_base_io={bypass}, probe_bucket={bucket})")

        ax.plot(xs, y(lambda p: p.simfs_wait_actual_share_p50_pct), marker="o", label="simfs_wait_actual")
        ax.plot(xs, y(lambda p: p.simfs_injected_share_p50_pct), marker="o", label="simfs_injected")
        ax.plot(xs, y(lambda p: p.filter_total_share_p50_pct), marker="o", label="filter_total")
        ax.plot(xs, y(lambda p: p.filter_fetch_share_p50_pct), marker="o", label="filter_fetch")
        ax.plot(xs, y(lambda p: p.filter_cpu_share_p50_pct), marker="o", label="filter_cpu(maymatch)")

        if not all(math.isnan(v) for v in y(lambda p: p.stage_get_from_output_files_p50_share_pct)):
            ax.plot(xs, y(lambda p: p.stage_get_from_output_files_p50_share_pct), marker="o", label="get_from_output_files")
        if not all(math.isnan(v) for v in y(lambda p: p.stage_index_lookup_p50_share_pct)):
            ax.plot(xs, y(lambda p: p.stage_index_lookup_p50_share_pct), marker="o", label="index_lookup")
        if not all(math.isnan(v) for v in y(lambda p: p.stage_block_read_io_p50_share_pct)):
            ax.plot(xs, y(lambda p: p.stage_block_read_io_p50_share_pct), marker="o", label="block_read_io")

        ax.grid(True, which="both", linestyle="--", linewidth=0.5, alpha=0.4)
        ax.legend(loc="best", fontsize=9)

        out = out_dir / f"gate3_share_curves.bypass{bypass}.bucket_{bucket}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=150)
        plt.close(fig)


def _plot_latency(groups: Dict[Tuple[int, str], List[Point]], out_dir: Path) -> None:
    import matplotlib.pyplot as plt

    for (bypass, bucket), pts in groups.items():
        xs = [p.xp_latency_ns for p in pts]
        fig = plt.figure(figsize=(10, 6))
        ax = fig.add_subplot(1, 1, 1)
        ax.set_xscale("log")
        ax.set_xlabel("simulate_xp_latency_ns (log scale)")
        ax.set_ylabel("p50 latency (us)")
        ax.set_title(f"Gate3 latency @ p50 (bypass_base_io={bypass}, probe_bucket={bucket})")

        ax.plot(xs, [p.latency_p50_us for p in pts], marker="o", label="op_latency_p50_us")
        ax.plot(xs, [p.filter_fetch_p50_us for p in pts], marker="o", label="filter_fetch_p50_us")
        ax.plot(xs, [p.filter_cpu_p50_us for p in pts], marker="o", label="filter_cpu_p50_us")

        ax.grid(True, which="both", linestyle="--", linewidth=0.5, alpha=0.4)
        ax.legend(loc="best", fontsize=9)

        out = out_dir / f"gate3_latency_p50.bypass{bypass}.bucket_{bucket}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=150)
        plt.close(fig)


def _plot_base_vs_injected(groups: Dict[Tuple[int, str], List[Point]], out_dir: Path) -> None:
    import matplotlib.pyplot as plt

    for (bypass, bucket), pts in groups.items():
        xs = [p.xp_latency_ns for p in pts]
        fig = plt.figure(figsize=(10, 5))
        ax = fig.add_subplot(1, 1, 1)
        ax.set_xscale("log")
        ax.set_xlabel("simulate_xp_latency_ns (log scale)")
        ax.set_ylabel("base_over_injected (p50)")
        ax.set_title(f"Gate2 base vs injected (bypass_base_io={bypass}, probe_bucket={bucket})")
        ax.plot(xs, [p.base_over_injected_p50 for p in pts], marker="o", label="base_over_injected_p50")
        ax.grid(True, which="both", linestyle="--", linewidth=0.5, alpha=0.4)
        ax.legend(loc="best", fontsize=9)
        out = out_dir / f"gate2_base_over_injected_p50.bypass{bypass}.bucket_{bucket}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=150)
        plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep_summary_csv", required=True)
    ap.add_argument("--out_dir", required=True)
    ap.add_argument("--probe_bucket", default="16+")
    args = ap.parse_args()

    sweep_csv = Path(args.sweep_summary_csv)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    points = [p for p in _read_points(sweep_csv) if p.probe_bucket == args.probe_bucket]
    if not points:
        raise SystemExit(f"no points for probe_bucket={args.probe_bucket} in {sweep_csv}")

    groups = _group(points)
    _plot_share_curves(groups, out_dir)
    _plot_latency(groups, out_dir)
    _plot_base_vs_injected(groups, out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


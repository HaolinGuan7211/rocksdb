#!/usr/bin/env python3
"""
Generate a concise baseline vs kvsep_bptree comparison report for exp42-style runs.

Inputs:
  experiment/<...>/run_results/<RUN_TAG_baseline>
  experiment/<...>/run_results/<RUN_TAG_kvsep_bptree>

This script is intentionally lightweight and only depends on artifacts already
produced by run_exp42:
  - monitor_analysis/02_mixgraph_cache_*.op_latency_percentiles.csv
  - monitor_analysis/02_mixgraph_cache_*.stage_join.csv
  - tail_probe_seek/tail_stage_distribution.csv (optional)
  - tail_probe_read/tail_stage_distribution.csv (optional)

Outputs (under --out_dir):
  - compare_op_latency.csv
  - compare_io_summary.csv
  - compare_tail_stage_distribution_seek.csv (if present)
  - compare_tail_stage_distribution_read.csv (if present)
  - report.md
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


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


def _safe_float(v: object) -> Optional[float]:
    if v is None:
        return None
    s = str(v).strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _fmt_pct(x: Optional[float]) -> str:
    if x is None:
        return ""
    return f"{x * 100.0:.2f}%"


def _fmt_us(x: Optional[float]) -> str:
    if x is None:
        return ""
    return f"{x:.3f}"


def _fmt_int(x: Optional[int]) -> str:
    if x is None:
        return ""
    return str(x)


@dataclass(frozen=True)
class Run:
    label: str
    run_dir: Path

    def monitor_analysis(self) -> Path:
        return self.run_dir / "monitor_analysis"

    def tail_probe_seek(self) -> Path:
        return self.run_dir / "tail_probe_seek"

    def tail_probe_read(self) -> Path:
        return self.run_dir / "tail_probe_read"


def _cache_prefix(cache_size: int) -> str:
    return f"02_mixgraph_cache_{cache_size}"


def load_op_latency(run: Run, cache_size: int) -> List[Dict[str, str]]:
    p = run.monitor_analysis() / f"{_cache_prefix(cache_size)}.op_latency_percentiles.csv"
    if not p.exists():
        return []
    return _read_csv(p)


def load_stage_join(run: Run, cache_size: int) -> List[Dict[str, str]]:
    p = run.monitor_analysis() / f"{_cache_prefix(cache_size)}.stage_join.csv"
    if not p.exists():
        return []
    return _read_csv(p)


def _first_row(rows: List[Dict[str, str]]) -> Optional[Dict[str, str]]:
    return rows[0] if rows else None


def build_compare_op_latency(runs: List[Run], cache_sizes: List[int]) -> List[Dict[str, object]]:
    out: List[Dict[str, object]] = []
    for cache in cache_sizes:
        # Collect union of ops across runs.
        ops: List[str] = []
        by_run: Dict[str, Dict[str, Dict[str, str]]] = {}
        for r in runs:
            rows = load_op_latency(r, cache)
            d: Dict[str, Dict[str, str]] = {}
            for row in rows:
                op = (row.get("op") or "").strip()
                if not op:
                    continue
                d[op] = row
                if op not in ops:
                    ops.append(op)
            by_run[r.label] = d

        for op in ops:
            row_out: Dict[str, object] = {"cache_size": cache, "op": op}
            for r in runs:
                d = by_run.get(r.label, {})
                rr = d.get(op, {})
                row_out[f"{r.label}_p50_us"] = rr.get("p50_us", "")
                row_out[f"{r.label}_p95_us"] = rr.get("p95_us", "")
                row_out[f"{r.label}_p99_us"] = rr.get("p99_us", "")
            out.append(row_out)
    return out


def build_compare_io_summary(runs: List[Run], cache_sizes: List[int]) -> List[Dict[str, object]]:
    """
    A small, stable subset of stage_join columns that are most useful for
    CPU-vs-IO attribution and IO amplification.
    """
    keys = [
        "mix_data_hit_ratio",
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
    out: List[Dict[str, object]] = []
    for cache in cache_sizes:
        row: Dict[str, object] = {"cache_size": cache}
        for r in runs:
            sj = _first_row(load_stage_join(r, cache))
            for k in keys:
                row[f"{r.label}_{k}"] = "" if sj is None else (sj.get(k, "") or "")
        out.append(row)
    return out


def load_tail_stage_distribution(run: Run, op: str) -> List[Dict[str, str]]:
    td = run.run_dir / f"tail_probe_{op}" / "tail_stage_distribution.csv"
    if not td.exists():
        # Older layout uses tail_probe_seek / tail_probe_read directories.
        td = run.run_dir / f"tail_probe_{op}" / "tail_stage_distribution.csv"
    if not td.exists():
        td = run.run_dir / f"tail_probe_{op}" / "tail_stage_distribution.csv"
    if not td.exists():
        td = run.run_dir / f"tail_probe_{op}" / "tail_stage_distribution.csv"
    # Preferred exp42 layout:
    if op == "seek":
        td2 = run.tail_probe_seek() / "tail_stage_distribution.csv"
        if td2.exists():
            td = td2
    if op == "read":
        td2 = run.tail_probe_read() / "tail_stage_distribution.csv"
        if td2.exists():
            td = td2
    if not td.exists():
        return []
    return _read_csv(td)


def build_compare_tail_stage_distribution(
    runs: List[Run], op: str, cache_labels: Tuple[str, ...] = ("cache0", "cache500m")
) -> List[Dict[str, object]]:
    """
    Compare tail stage p50_share/p95_share across runs for the common cache labels.
    """
    # Index rows by (label, stage)
    by_run: Dict[str, Dict[Tuple[str, str], Dict[str, str]]] = {}
    stages: List[str] = []
    for r in runs:
        rows = load_tail_stage_distribution(r, op)
        d: Dict[Tuple[str, str], Dict[str, str]] = {}
        for rr in rows:
            lab = (rr.get("label") or "").strip()
            stage = (rr.get("stage") or "").strip()
            if not lab or not stage:
                continue
            d[(lab, stage)] = rr
            if stage not in stages:
                stages.append(stage)
        by_run[r.label] = d

    out: List[Dict[str, object]] = []
    for lab in cache_labels:
        for stage in stages:
            row: Dict[str, object] = {"label": lab, "stage": stage}
            present = False
            for r in runs:
                rr = by_run.get(r.label, {}).get((lab, stage))
                if rr:
                    present = True
                    row[f"{r.label}_p50_share_pct"] = rr.get("p50_share_pct", "")
                    row[f"{r.label}_p95_share_pct"] = rr.get("p95_share_pct", "")
                    row[f"{r.label}_p50_stage_us"] = rr.get("p50_stage_us", "")
                    row[f"{r.label}_p95_stage_us"] = rr.get("p95_stage_us", "")
                    row[f"{r.label}_samples"] = rr.get("samples", "")
                else:
                    row[f"{r.label}_p50_share_pct"] = ""
                    row[f"{r.label}_p95_share_pct"] = ""
                    row[f"{r.label}_p50_stage_us"] = ""
                    row[f"{r.label}_p95_stage_us"] = ""
                    row[f"{r.label}_samples"] = ""
            if present:
                out.append(row)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline_run_dir", required=True)
    ap.add_argument("--kvsep_run_dir", required=True)
    ap.add_argument("--out_dir", required=True)
    ap.add_argument("--cache_sizes", default="0,536870912")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cache_sizes = [int(x.strip()) for x in args.cache_sizes.split(",") if x.strip()]

    runs = [
        Run("baseline", Path(args.baseline_run_dir)),
        Run("kvsep", Path(args.kvsep_run_dir)),
    ]

    op_latency = build_compare_op_latency(runs, cache_sizes)
    _write_csv(out_dir / "compare_op_latency.csv", op_latency)

    io_summary = build_compare_io_summary(runs, cache_sizes)
    _write_csv(out_dir / "compare_io_summary.csv", io_summary)

    tail_seek = build_compare_tail_stage_distribution(runs, "seek")
    if tail_seek:
        _write_csv(out_dir / "compare_tail_stage_distribution_seek.csv", tail_seek)

    tail_read = build_compare_tail_stage_distribution(runs, "read")
    if tail_read:
        _write_csv(out_dir / "compare_tail_stage_distribution_read.csv", tail_read)

    # Write a short markdown index that points at the main artifacts.
    md = out_dir / "report.md"
    with md.open("w", encoding="utf-8") as f:
        f.write("# Exp42 baseline vs kvsep_bptree report\n\n")
        f.write("## Inputs\n")
        for r in runs:
            f.write(f"- `{r.label}`: `{r.run_dir}`\n")
        f.write("\n## Outputs\n")
        for name in [
            "compare_op_latency.csv",
            "compare_io_summary.csv",
            "compare_tail_stage_distribution_seek.csv",
            "compare_tail_stage_distribution_read.csv",
        ]:
            p = out_dir / name
            if p.exists():
                f.write(f"- `{p}`\n")
        f.write("\n## Notes\n")
        f.write(
            "- `compare_op_latency.csv` comes from `monitor_analysis/*op_latency_percentiles.csv`.\n"
        )
        f.write(
            "- `compare_io_summary.csv` comes from `monitor_analysis/*stage_join.csv` (window 0 summary).\n"
        )
        f.write(
            "- Tail stage comparisons require `tail_probe_{seek,read}/tail_stage_distribution.csv`.\n"
        )

    print(f"[ok] wrote: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


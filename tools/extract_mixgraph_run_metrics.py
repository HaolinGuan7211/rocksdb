#!/usr/bin/env python3
"""
Extract mixgraph metrics from multiple run_results/<RUN_DIR> directories.

Inputs:
  - Each run directory should contain: 02_mixgraph_cache_*.log

Outputs:
  - CSV to stdout (or --out_csv)

This intentionally avoids depending on the shortscan aggregation pipeline so it
can be used for experiments where each case is in a separate run directory.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


def _read_text(path: Path) -> str:
    return path.read_text(errors="ignore")


def _common_prefix_segments(names: List[str]) -> List[str]:
    if not names:
        return []
    segs = [n.split("_") for n in names]
    out: List[str] = []
    for i in range(min(len(s) for s in segs)):
        token = segs[0][i]
        if all(s[i] == token for s in segs[1:]):
            out.append(token)
        else:
            break
    return out


def _case_from_run_dir_name(name: str, common_prefix: List[str]) -> str:
    seg = name.split("_")
    seg = seg[len(common_prefix) :]
    if seg and re.fullmatch(r"\d{6}", seg[-1]):
        seg = seg[:-1]
    return "_".join(seg) if seg else name


def _grab_int(key: str, text: str) -> Optional[int]:
    m = re.search(r"\b" + re.escape(key) + r"\s*=\s*(\d+)", text)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def parse_mixgraph_summary(txt: str) -> Dict[str, str]:
    out: Dict[str, str] = {}

    m = re.search(
        r"^mixgraph\s+:\s+([0-9.]+) micros/op ([0-9.]+) ops/sec ([0-9.]+) seconds "
        r"([0-9.]+) operations;\s+([0-9.]+) MB/s.*?\(.*?Gets:(\d+)\s+Puts:(\d+)\s+"
        r"Seek:(\d+)\s+MultiGet:(\d+)\s+Burst:(\d+)",
        txt,
        re.M,
    )
    if m:
        out.update(
            {
                "micros_per_op": m.group(1),
                "ops_per_sec": m.group(2),
                "seconds": m.group(3),
                "operations": m.group(4),
                "throughput_mb_s": m.group(5),
                "mix_gets": m.group(6),
                "mix_puts": m.group(7),
                "mix_seeks": m.group(8),
                "mix_multigets": m.group(9),
                "mix_bursts": m.group(10),
            }
        )

    # Seek percentiles
    anchor = txt.find("Microseconds per seek:")
    if anchor >= 0:
        tail = txt[anchor : anchor + 6000]
        p = re.search(
            r"^Percentiles:\s+P50:\s*([0-9.]+)\s+P75:\s*([0-9.]+)\s+P95:\s*([0-9.]+)\s+"
            r"P99:\s*([0-9.]+)\s+P99\.9:\s*([0-9.]+)\s+P99\.99:\s*([0-9.]+)",
            tail,
            re.M,
        )
        if p:
            out.update(
                {
                    "seek_p50_us": p.group(1),
                    "seek_p75_us": p.group(2),
                    "seek_p95_us": p.group(3),
                    "seek_p99_us": p.group(4),
                    "seek_p999_us": p.group(5),
                    "seek_p9999_us": p.group(6),
                }
            )

    # PERF_CONTEXT (single-line blob)
    perf = re.search(r"PERF_CONTEXT:\s*\n([^\n]+)", txt)
    if perf:
        line = perf.group(1)
        for k in [
            "block_read_count",
            "block_read_byte",
            "index_block_read_count",
            "filter_block_read_count",
            "get_read_bytes",
            "multiget_read_bytes",
            "iter_read_bytes",
            "seek_on_memtable_count",
            "seek_child_seek_count",
            "iter_next_count",
            "iter_seek_count",
            "internal_key_skipped_count",
        ]:
            v = _grab_int(k, line)
            if v is not None:
                out[k] = str(v)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--run_results_dir",
        required=True,
        help="path to .../run_results (contains run directories)",
    )
    ap.add_argument("--out_csv", default="", help="optional: write to a csv file")
    args = ap.parse_args()

    rr = Path(args.run_results_dir)
    run_dirs = [p for p in sorted(rr.iterdir()) if p.is_dir()]
    common_prefix = _common_prefix_segments([p.name for p in run_dirs])

    rows: List[Dict[str, str]] = []
    for rd in run_dirs:
        log = next(iter(sorted(rd.glob("02_mixgraph_cache_*.log"))), None)
        if log is None:
            continue
        txt = _read_text(log)
        row = {
            "run_dir": rd.name,
            "case": _case_from_run_dir_name(rd.name, common_prefix),
            "log_file": log.name,
        }
        row.update(parse_mixgraph_summary(txt))
        rows.append(row)

    if not rows:
        print(f"[err] no mixgraph logs found under {rr}", file=sys.stderr)
        return 2

    # Stable, readable columns (others are appended if present).
    base_cols = [
        "run_dir",
        "case",
        "ops_per_sec",
        "seek_p50_us",
        "seek_p95_us",
        "seek_p99_us",
        "seek_p999_us",
        "mix_bursts",
        "block_read_count",
        "block_read_byte",
        "iter_read_bytes",
        "get_read_bytes",
        "multiget_read_bytes",
        "seek_child_seek_count",
    ]
    extra_cols = sorted({k for r in rows for k in r.keys()} - set(base_cols))
    cols = base_cols + [c for c in extra_cols if c not in base_cols]

    out_f = open(args.out_csv, "w", newline="") if args.out_csv else sys.stdout
    try:
        w = csv.DictWriter(out_f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    finally:
        if out_f is not sys.stdout:
            out_f.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


@dataclass
class Percentiles:
    p50: float
    p95: float
    p99: float
    p999: float
    p9999: float
    maxv: float


def _to_float(value: str) -> float:
    if value is None:
        return float("nan")
    value = str(value).strip()
    if value == "":
        return float("nan")
    try:
        return float(value)
    except ValueError:
        return float("nan")


def _to_int(value: str) -> int:
    if value is None:
        return 0
    value = str(value).strip()
    if value == "":
        return 0
    try:
        return int(value)
    except ValueError:
        return 0


def _percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        return float("nan")
    if pct <= 0:
        return sorted_values[0]
    if pct >= 100:
        return sorted_values[-1]
    k = (len(sorted_values) - 1) * (pct / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return sorted_values[int(k)]
    d0 = sorted_values[f] * (c - k)
    d1 = sorted_values[c] * (k - f)
    return d0 + d1


def _summarize(values: Iterable[float]) -> Percentiles:
    cleaned = [v for v in values if not math.isnan(v)]
    cleaned.sort()
    if not cleaned:
        nan = float("nan")
        return Percentiles(nan, nan, nan, nan, nan, nan)
    return Percentiles(
        p50=_percentile(cleaned, 50),
        p95=_percentile(cleaned, 95),
        p99=_percentile(cleaned, 99),
        p999=_percentile(cleaned, 99.9),
        p9999=_percentile(cleaned, 99.99),
        maxv=cleaned[-1],
    )


def _load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        return list(reader)


def summarize_file(csv_path: Path) -> dict[str, Any]:
    rows = _load_rows(csv_path)

    max_read_us = [_to_float(r.get("max_read_us", "")) for r in rows]
    max_open_us = [_to_float(r.get("max_open_us", "")) for r in rows]
    max_prefetch_us = [_to_float(r.get("max_prefetch_us", "")) for r in rows]

    read_ops = sum(_to_int(r.get("read_ops", "0")) for r in rows)
    read_bytes = sum(_to_int(r.get("read_bytes", "0")) for r in rows)
    tmpfs_read_bytes = sum(_to_int(r.get("tmpfs_read_bytes", "0")) for r in rows)
    base_read_bytes = sum(_to_int(r.get("base_read_bytes", "0")) for r in rows)
    tmpfs_read_ops = sum(_to_int(r.get("tmpfs_read_ops", "0")) for r in rows)
    base_read_ops = sum(_to_int(r.get("base_read_ops", "0")) for r in rows)

    prefetch_ops = sum(_to_int(r.get("prefetch_ops", "0")) for r in rows)
    prefetch_bytes = sum(_to_int(r.get("prefetch_bytes", "0")) for r in rows)
    tmpfs_prefetch_bytes = sum(_to_int(r.get("tmpfs_prefetch_bytes", "0")) for r in rows)
    base_prefetch_bytes = sum(_to_int(r.get("base_prefetch_bytes", "0")) for r in rows)

    open_ops = sum(_to_int(r.get("open_ops", "0")) for r in rows)
    tmpfs_open_ops = sum(_to_int(r.get("tmpfs_open_ops", "0")) for r in rows)
    base_open_ops = sum(_to_int(r.get("base_open_ops", "0")) for r in rows)

    def _safe_ratio(n: int, d: int) -> float:
        return (float(n) / float(d)) if d else float("nan")

    return {
        "file": str(csv_path),
        "windows": len(rows),
        "max_read_us": _summarize(max_read_us).__dict__,
        "max_open_us": _summarize(max_open_us).__dict__,
        "max_prefetch_us": _summarize(max_prefetch_us).__dict__,
        "read_ops": read_ops,
        "read_bytes": read_bytes,
        "tmpfs_read_ops": tmpfs_read_ops,
        "tmpfs_read_bytes": tmpfs_read_bytes,
        "base_read_ops": base_read_ops,
        "base_read_bytes": base_read_bytes,
        "prefetch_ops": prefetch_ops,
        "prefetch_bytes": prefetch_bytes,
        "tmpfs_prefetch_bytes": tmpfs_prefetch_bytes,
        "base_prefetch_bytes": base_prefetch_bytes,
        "open_ops": open_ops,
        "tmpfs_open_ops": tmpfs_open_ops,
        "base_open_ops": base_open_ops,
        "tmpfs_read_bytes_ratio": _safe_ratio(tmpfs_read_bytes, tmpfs_read_bytes + base_read_bytes),
        "base_read_bytes_ratio": _safe_ratio(base_read_bytes, tmpfs_read_bytes + base_read_bytes),
        "tmpfs_prefetch_bytes_ratio": _safe_ratio(
            tmpfs_prefetch_bytes, tmpfs_prefetch_bytes + base_prefetch_bytes
        ),
        "base_prefetch_bytes_ratio": _safe_ratio(
            base_prefetch_bytes, tmpfs_prefetch_bytes + base_prefetch_bytes
        ),
        "tmpfs_open_ops_ratio": _safe_ratio(tmpfs_open_ops, tmpfs_open_ops + base_open_ops),
        "base_open_ops_ratio": _safe_ratio(base_open_ops, tmpfs_open_ops + base_open_ops),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize simfs_window CSVs (per-window max latency percentiles, traffic split)."
    )
    parser.add_argument("--run_dir", type=Path, required=True, help="run_results/<run_tag> directory")
    parser.add_argument(
        "--glob",
        default="02_mixgraph_cache_*.simfs_window.csv",
        help="glob pattern inside run_dir (default: %(default)s)",
    )
    parser.add_argument("--json", action="store_true", help="output as JSON")
    args = parser.parse_args()

    run_dir: Path = args.run_dir
    if not run_dir.exists():
        raise SystemExit(f"run_dir not found: {run_dir}")

    files = sorted(run_dir.glob(args.glob))
    if not files:
        raise SystemExit(f"no files matched: {run_dir}/{args.glob}")

    summaries = [summarize_file(p) for p in files]
    if args.json:
        print(json.dumps({"run_dir": str(run_dir), "summaries": summaries}, indent=2, sort_keys=True))
        return 0

    def fmt(v: float) -> str:
        if v != v:
            return "nan"
        return f"{v:.1f}"

    print(f"run_dir: {run_dir}")
    for s in summaries:
        mr = s["max_read_us"]
        mo = s["max_open_us"]
        mp = s["max_prefetch_us"]
        print(f"- file: {Path(s['file']).name}")
        print(
            f"  windows={s['windows']} "
            f"max_read_us(p99={fmt(mr['p99'])}, p99.9={fmt(mr['p999'])}, max={fmt(mr['maxv'])}) "
            f"max_open_us(max={fmt(mo['maxv'])}) "
            f"max_prefetch_us(max={fmt(mp['maxv'])})"
        )
        print(
            f"  traffic: tmpfs_read_bytes_ratio={s['tmpfs_read_bytes_ratio']:.6f} "
            f"base_read_bytes_ratio={s['base_read_bytes_ratio']:.6f} "
            f"(tmpfs={s['tmpfs_read_bytes']} base={s['base_read_bytes']})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


#!/usr/bin/env python3
"""
Generate a concise baseline vs SSTHashSeek comparison report for exp50-style runs.

Inputs:
  experiment/<...>/run_results/<RUN_ROOT_baseline>
  experiment/<...>/run_results/<RUN_ROOT_sst_hash_seek>

Expected layout (produced by tools/run_exp50_simfs_mixgraph_sst_hash_seek_compare_cache0_500m.sh):
  <RUN_ROOT>/
    cache_<cache_size>/
      mixgraph.log
      sample_probe_seek/sample_stage_samples.csv
      sample_probe_seek/sample_stage_distribution.csv
      tail_probe_seek/tail_stage_distribution.csv  (optional)

Outputs (under --out_dir):
  - compare_summary.csv
  - compare_stage_p50_share.csv
  - report.md
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
import re
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


def _first_row(rows: List[Dict[str, str]]) -> Optional[Dict[str, str]]:
    return rows[0] if rows else None


@dataclass(frozen=True)
class Run:
    label: str
    run_dir: Path

    def cache_dir(self, cache_size: int) -> Path:
        return self.run_dir / f"cache_{cache_size}"


_MIXGRAPH_LINE_RE = re.compile(
    r"^mixgraph\s*:\s*(?P<micros>[0-9.]+)\s+micros/op\s+(?P<ops>[0-9.]+)\s+ops/sec",
    re.IGNORECASE,
)


def parse_mixgraph_summary(log_path: Path) -> Tuple[Optional[float], Optional[float]]:
    """Return (ops_per_sec, micros_per_op)."""
    if not log_path.exists():
        return None, None
    ops: Optional[float] = None
    micros: Optional[float] = None
    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = _MIXGRAPH_LINE_RE.match(line.strip())
            if not m:
                continue
            try:
                micros = float(m.group("micros"))
                ops = float(m.group("ops"))
            except Exception:
                continue
    return ops, micros


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


def _quantile(vals: List[float], q: float) -> Optional[float]:
    if not vals:
        return None
    vals2 = sorted(vals)
    idx = int(round((len(vals2) - 1) * q))
    idx = max(0, min(idx, len(vals2) - 1))
    return float(vals2[idx])


def load_sample_stage_samples(run: Run, cache_size: int, op: str = "seek") -> List[Dict[str, str]]:
    p = run.cache_dir(cache_size) / f"sample_probe_{op}" / "sample_stage_samples.csv"
    if not p.exists():
        return []
    return _read_csv(p)


def load_sample_stage_distribution(run: Run, cache_size: int, op: str = "seek") -> List[Dict[str, str]]:
    p = run.cache_dir(cache_size) / f"sample_probe_{op}" / "sample_stage_distribution.csv"
    if not p.exists():
        return []
    return _read_csv(p)


def load_tail_stage_distribution(run: Run, cache_size: int, op: str = "seek") -> List[Dict[str, str]]:
    p = run.cache_dir(cache_size) / f"tail_probe_{op}" / "tail_stage_distribution.csv"
    if not p.exists():
        return []
    return _read_csv(p)


def summarize_samples(samples: List[Dict[str, str]]) -> Dict[str, object]:
    lat: List[float] = []
    block_reads: List[float] = []
    block_bytes: List[float] = []
    key_cmps: List[float] = []
    for r in samples:
        v = _safe_float(r.get("latency_us"))
        if v is not None:
            lat.append(v)
        br = _safe_float(r.get("block_read_count"))
        if br is not None:
            block_reads.append(br)
        bb = _safe_float(r.get("block_read_bytes"))
        if bb is not None:
            block_bytes.append(bb)
        kc = _safe_float(r.get("user_key_comparison_count"))
        if kc is not None:
            key_cmps.append(kc)

    out: Dict[str, object] = {"samples": len(samples)}
    out["lat_p50_us"] = _quantile(lat, 0.50) or ""
    out["lat_p95_us"] = _quantile(lat, 0.95) or ""
    out["lat_p99_us"] = _quantile(lat, 0.99) or ""
    out["avg_block_reads"] = (sum(block_reads) / len(block_reads)) if block_reads else ""
    out["avg_block_bytes"] = (sum(block_bytes) / len(block_bytes)) if block_bytes else ""
    out["avg_user_key_cmps"] = (sum(key_cmps) / len(key_cmps)) if key_cmps else ""
    return out


def stage_p50_share_map(stage_dist_rows: List[Dict[str, str]]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for r in stage_dist_rows:
        stage = (r.get("stage") or "").strip()
        if not stage:
            continue
        out[stage] = (r.get("p50_share_pct") or "").strip()
    return out


def build_compare_summary(runs: List[Run], cache_sizes: List[int]) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for cache in cache_sizes:
        row: Dict[str, object] = {"cache_size": cache}
        for r in runs:
            cache_dir = r.cache_dir(cache)
            ops, micros = parse_mixgraph_summary(cache_dir / "mixgraph.log")
            row[f"{r.label}_mix_ops_per_sec"] = "" if ops is None else ops
            row[f"{r.label}_mix_micros_per_op"] = "" if micros is None else micros

            samples = load_sample_stage_samples(r, cache, op="seek")
            ss = summarize_samples(samples)
            for k, v in ss.items():
                row[f"{r.label}_sample_{k}"] = v

            stage_dist = load_sample_stage_distribution(r, cache, op="seek")
            share = stage_p50_share_map(stage_dist)
            for k in ("index_lookup", "seek_dispatch", "block_read_io", "cpu_iter_seek", "unattributed"):
                row[f"{r.label}_sample_p50_share_{k}"] = share.get(k, "")

        rows.append(row)
    return rows


def build_compare_stage_p50_share(runs: List[Run], cache_sizes: List[int]) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for cache in cache_sizes:
        stage_union: List[str] = []
        by_run: Dict[str, Dict[str, str]] = {}
        for r in runs:
            m = stage_p50_share_map(load_sample_stage_distribution(r, cache, op="seek"))
            by_run[r.label] = m
            for s in m.keys():
                if s not in stage_union:
                    stage_union.append(s)
        for stage in stage_union:
            row: Dict[str, object] = {"cache_size": cache, "stage": stage}
            for r in runs:
                row[f"{r.label}_p50_share_pct"] = by_run.get(r.label, {}).get(stage, "")
            rows.append(row)
    return rows


def write_report_md(out_dir: Path, runs: List[Run], cache_sizes: List[int]) -> None:
    lines: List[str] = []
    lines.append("# exp50 baseline vs SSTHashSeek\n")
    lines.append("## Runs\n")
    for r in runs:
        lines.append(f"- {r.label}: {r.run_dir}\n")

    lines.append("\n## Key artifacts\n")
    lines.append("- compare_summary.csv\n")
    lines.append("- compare_stage_p50_share.csv\n")

    lines.append("\n## How to read\n")
    lines.append("- `compare_summary.csv`: mixgraph throughput + sampled seek latency + a few key stage shares (p50)\n")
    lines.append("- `compare_stage_p50_share.csv`: full p50 stage-share table from sample probes (seek)\n")
    lines.append("\n## Cache sizes\n")
    lines.append("- " + ", ".join(str(c) for c in cache_sizes) + "\n")

    (out_dir / "report.md").write_text("".join(lines), encoding="utf-8")


def _parse_cache_sizes(s: str) -> List[int]:
    out: List[int] = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(int(part))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline_dir", type=Path, required=True)
    ap.add_argument("--hashseek_dir", type=Path, required=True)
    ap.add_argument("--out_dir", type=Path, required=True)
    ap.add_argument("--cache_sizes", type=str, default="0,536870912")
    args = ap.parse_args()

    cache_sizes = _parse_cache_sizes(args.cache_sizes)
    runs = [
        Run("baseline", args.baseline_dir),
        Run("sst_hash_seek", args.hashseek_dir),
    ]

    args.out_dir.mkdir(parents=True, exist_ok=True)

    summary = build_compare_summary(runs, cache_sizes)
    stage_p50 = build_compare_stage_p50_share(runs, cache_sizes)
    _write_csv(args.out_dir / "compare_summary.csv", summary)
    _write_csv(args.out_dir / "compare_stage_p50_share.csv", stage_p50)
    write_report_md(args.out_dir, runs, cache_sizes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

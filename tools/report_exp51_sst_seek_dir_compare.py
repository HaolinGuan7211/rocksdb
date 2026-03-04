#!/usr/bin/env python3
"""
Generate a concise baseline vs SSTSeekDir comparison report for exp51-style runs.

Expected layout (produced by tools/run_exp51_simfs_mixgraph_sst_seek_dir_compare_cache0_500m.sh):
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
from typing import Dict, List, Optional, Tuple


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


def stage_p50_share_map(stage_dist_rows: List[Dict[str, str]]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for r in stage_dist_rows:
        stage = (r.get("stage") or "").strip()
        if not stage:
            continue
        out[stage] = (r.get("p50_share_pct") or "").strip()
    return out


def summarize_samples(samples: List[Dict[str, str]]) -> Dict[str, object]:
    lat: List[float] = []
    block_reads: List[float] = []
    block_bytes: List[float] = []
    key_cmps: List[float] = []
    sd_lookups: List[float] = []
    sd_hits: List[float] = []
    sd_fallbacks: List[float] = []
    sd_steps: List[float] = []
    sd_cmp_bytes: List[float] = []
    sd_num_blocks_sum: List[float] = []
    sd_layout_no_offsets: List[float] = []
    sd_used_direct_index: List[float] = []
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
        sl = _safe_float(r.get("sst_seek_dir_seek_lookups"))
        if sl is not None:
            sd_lookups.append(sl)
        sh = _safe_float(r.get("sst_seek_dir_seek_hits"))
        if sh is not None:
            sd_hits.append(sh)
        sf = _safe_float(r.get("sst_seek_dir_seek_fallbacks"))
        if sf is not None:
            sd_fallbacks.append(sf)
        ss = _safe_float(r.get("sst_seek_dir_seek_binary_steps"))
        if ss is not None:
            sd_steps.append(ss)
        sb = _safe_float(r.get("sst_seek_dir_seek_cmp_bytes"))
        if sb is not None:
            sd_cmp_bytes.append(sb)
        sn = _safe_float(r.get("sst_seek_dir_seek_num_data_blocks_sum"))
        if sn is not None:
            sd_num_blocks_sum.append(sn)
        s_no = _safe_float(r.get("sst_seek_dir_seek_layout_no_offsets"))
        if s_no is not None:
            sd_layout_no_offsets.append(s_no)
        s_di = _safe_float(r.get("sst_seek_dir_seek_used_direct_index"))
        if s_di is not None:
            sd_used_direct_index.append(s_di)

    out: Dict[str, object] = {"samples": len(samples)}
    out["lat_p50_us"] = _quantile(lat, 0.50) or ""
    out["lat_p95_us"] = _quantile(lat, 0.95) or ""
    out["lat_p99_us"] = _quantile(lat, 0.99) or ""
    out["avg_block_reads"] = (sum(block_reads) / len(block_reads)) if block_reads else ""
    out["avg_block_bytes"] = (sum(block_bytes) / len(block_bytes)) if block_bytes else ""
    out["avg_user_key_cmps"] = (sum(key_cmps) / len(key_cmps)) if key_cmps else ""

    out["avg_sst_seek_dir_seek_lookups"] = (
        (sum(sd_lookups) / len(sd_lookups)) if sd_lookups else ""
    )
    out["avg_sst_seek_dir_seek_hits"] = (sum(sd_hits) / len(sd_hits)) if sd_hits else ""
    out["avg_sst_seek_dir_seek_fallbacks"] = (
        (sum(sd_fallbacks) / len(sd_fallbacks)) if sd_fallbacks else ""
    )
    out["avg_sst_seek_dir_seek_binary_steps"] = (
        (sum(sd_steps) / len(sd_steps)) if sd_steps else ""
    )
    out["avg_sst_seek_dir_seek_cmp_bytes"] = (
        (sum(sd_cmp_bytes) / len(sd_cmp_bytes)) if sd_cmp_bytes else ""
    )
    out["avg_sst_seek_dir_seek_num_data_blocks_sum"] = (
        (sum(sd_num_blocks_sum) / len(sd_num_blocks_sum)) if sd_num_blocks_sum else ""
    )
    out["avg_sst_seek_dir_seek_layout_no_offsets"] = (
        (sum(sd_layout_no_offsets) / len(sd_layout_no_offsets)) if sd_layout_no_offsets else ""
    )
    out["avg_sst_seek_dir_seek_used_direct_index"] = (
        (sum(sd_used_direct_index) / len(sd_used_direct_index)) if sd_used_direct_index else ""
    )

    total_lookups = sum(sd_lookups) if sd_lookups else 0.0
    total_hits = sum(sd_hits) if sd_hits else 0.0
    total_fallbacks = sum(sd_fallbacks) if sd_fallbacks else 0.0
    total_steps = sum(sd_steps) if sd_steps else 0.0
    total_cmp_bytes = sum(sd_cmp_bytes) if sd_cmp_bytes else 0.0
    total_num_blocks_sum = sum(sd_num_blocks_sum) if sd_num_blocks_sum else 0.0
    out["sst_seek_dir_seek_hit_rate"] = (
        (total_hits / total_lookups) if total_lookups > 0 else ""
    )
    out["sst_seek_dir_seek_fallback_rate"] = (
        (total_fallbacks / total_lookups) if total_lookups > 0 else ""
    )
    out["sst_seek_dir_seek_steps_per_lookup"] = (
        (total_steps / total_lookups) if total_lookups > 0 else ""
    )
    out["sst_seek_dir_seek_cmp_bytes_per_lookup"] = (
        (total_cmp_bytes / total_lookups) if total_lookups > 0 else ""
    )
    out["sst_seek_dir_seek_avg_num_data_blocks"] = (
        (total_num_blocks_sum / total_lookups) if total_lookups > 0 else ""
    )
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
    lines.append("# exp51 baseline vs SSTSeekDir\n")
    lines.append("## Runs\n")
    for r in runs:
        lines.append(f"- {r.label}: {r.run_dir}\n")

    lines.append("\n## Summary\n")
    lines.append("See `compare_summary.csv` and `compare_stage_p50_share.csv`.\n")

    (out_dir / "report.md").write_text("".join(lines), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline_dir", type=Path, required=True)
    ap.add_argument("--seekdir_dir", type=Path, required=True)
    ap.add_argument("--out_dir", type=Path, required=True)
    ap.add_argument("--cache_sizes", type=str, default="0,536870912")
    args = ap.parse_args()

    cache_sizes = [int(x) for x in args.cache_sizes.split(",") if x.strip()]
    runs = [Run("baseline", args.baseline_dir), Run("sst_seek_dir", args.seekdir_dir)]
    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    _write_csv(out_dir / "compare_summary.csv", build_compare_summary(runs, cache_sizes))
    _write_csv(out_dir / "compare_stage_p50_share.csv", build_compare_stage_p50_share(runs, cache_sizes))
    write_report_md(out_dir, runs, cache_sizes)


if __name__ == "__main__":
    main()

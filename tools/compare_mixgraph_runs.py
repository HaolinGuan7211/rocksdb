#!/usr/bin/env python3
"""
Compare multiple mixgraph monitoring runs (run_results/<RUN_TAG> dirs).

Reads:
  - 02_mixgraph_cache_*.log (block cache hit/miss summary)
  - monitor_analysis/02_mixgraph_cache_*.factor_report.txt

Writes:
  - <out_dir>/compare_summary.csv
  - <out_dir>/compare_summary.png
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def _read_text(path: Path) -> str:
    return path.read_text(errors="ignore")


def _grab_int(pattern: str, text: str) -> Optional[int]:
    m = re.search(pattern, text)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def _grab_float(pattern: str, text: str) -> Optional[float]:
    m = re.search(pattern, text)
    if not m:
        return None
    try:
        return float(m.group(1))
    except ValueError:
        return None


def parse_log_stats(log_path: Path) -> Tuple[Optional[int], Optional[int], Optional[int]]:
    txt = _read_text(log_path)
    hit = _grab_int(r"rocksdb\.block\.cache\.data\.hit COUNT : (\d+)", txt)
    miss = _grab_int(r"rocksdb\.block\.cache\.data\.miss COUNT : (\d+)", txt)
    ins = _grab_int(r"rocksdb\.block\.cache\.data\.bytes\.insert COUNT : (\d+)", txt)
    return hit, miss, ins


def parse_factor_report(report_path: Path) -> Dict[str, str]:
    txt = _read_text(report_path)
    out: Dict[str, str] = {}
    for line in txt.splitlines():
        line = line.strip()
        if not line or line.startswith("[") or line.startswith("-"):
            continue
        # Scalars: "case=...", "windows=..."
        m = re.match(r"^([A-Za-z0-9_./-]+)=(.+)$", line)
        if m:
            out[m.group(1).strip()] = m.group(2).strip()
            continue

        # Correlations: "corr(x, y)=0.123"
        m = re.match(r"^(corr\([^\)]+\))=([+-]?(?:nan|inf|\d+(?:\.\d+)?))$", line)
        if m:
            out[m.group(1).strip()] = m.group(2).strip()
            continue

        # Burst-vs-nonburst summary lines:
        #   median(data_miss) nonburst=2512.000 burst=2595.500
        #   median(miss_ratio) nonburst=9.958% burst=8.917%
        m = re.match(
            r"^(median|p90)\(([^)]+)\)\s+nonburst=([^\s]+)\s+burst=([^\s]+)$",
            line,
        )
        if m:
            stat = m.group(1)
            metric = m.group(2)
            nb = m.group(3)
            b = m.group(4)
            out[f"{stat}({metric}) nonburst"] = nb
            out[f"{stat}({metric}) burst"] = b
            continue

        # Fallback: preserve simple k=v when present (but avoid splitting burst summary).
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def ratio(hit: Optional[int], miss: Optional[int]) -> Optional[float]:
    if hit is None or miss is None:
        return None
    d = hit + miss
    if d <= 0:
        return None
    return hit / d


def _read_impact_csv(path: Path) -> Dict[int, Dict[str, float]]:
    if not path.exists():
        return {}
    import csv as _csv

    out: Dict[int, Dict[str, float]] = {}
    with path.open("r", newline="") as f:
        r = _csv.DictReader(f)
        for row in r:
            try:
                off = int(float((row.get("offset_windows") or "0").strip()))
            except ValueError:
                continue
            out[off] = {}
            for k, v in row.items():
                if k == "offset_windows" or v is None:
                    continue
                vv = (v or "").strip()
                if not vv or vv == "nan":
                    continue
                try:
                    out[off][k] = float(vv)
                except ValueError:
                    continue
    return out


def _pre_mean(impact: Dict[int, Dict[str, float]], key: str, left: int = -10, right: int = -1) -> Optional[float]:
    xs: List[float] = []
    for off in range(left, right + 1):
        v = impact.get(off, {}).get(key)
        if v is None:
            continue
        xs.append(v)
    if not xs:
        return None
    return sum(xs) / len(xs)


def _peak(impact: Dict[int, Dict[str, float]], key: str, left: int = 0, right: int = 10) -> Optional[float]:
    xs: List[float] = []
    for off in range(left, right + 1):
        v = impact.get(off, {}).get(key)
        if v is None:
            continue
        xs.append(v)
    if not xs:
        return None
    return max(xs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out_dir", required=True, help="directory to write compare outputs")
    ap.add_argument(
        "--run_dir",
        action="append",
        required=True,
        help="repeatable: run_results/<RUN_TAG> dir (must contain mixgraph artifacts)",
    )
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows: List[Dict[str, str]] = []
    for rd_raw in args.run_dir:
        rd = Path(rd_raw)
        log = next(iter(sorted(rd.glob("02_mixgraph_cache_*.log"))), None)
        if log is None:
            print(f"[skip] missing mixgraph log in {rd}")
            continue
        case_prefix = log.name.replace(".log", "")
        report = rd / "monitor_analysis" / f"{case_prefix}.factor_report.txt"
        hit, miss, ins = parse_log_stats(log)
        rep = parse_factor_report(report) if report.exists() else {}
        hit_ratio = ratio(hit, miss)

        burst_impact = _read_impact_csv(rd / "monitor_analysis" / f"{case_prefix}.burst_impact.csv")
        shift_impact = _read_impact_csv(rd / "monitor_analysis" / f"{case_prefix}.shift_impact.csv")

        burst_pre_miss_ratio = _pre_mean(burst_impact, "miss_ratio_mean")
        burst_at0_miss_ratio = burst_impact.get(0, {}).get("miss_ratio_mean")
        burst_pre_probe_inc = _pre_mean(burst_impact, "probe_incomplete_ratio_mean")
        burst_at0_probe_inc = burst_impact.get(0, {}).get("probe_incomplete_ratio_mean")

        shift_pre_miss_ratio = _pre_mean(shift_impact, "miss_ratio_mean")
        shift_peak_miss_ratio = _peak(shift_impact, "miss_ratio_mean")
        shift_pre_probe_inc = _pre_mean(shift_impact, "probe_incomplete_ratio_mean")
        shift_peak_probe_inc = _peak(shift_impact, "probe_incomplete_ratio_mean")

        row = {
            "run_dir": str(rd),
            "case": case_prefix,
            "data_hit": str(hit or ""),
            "data_miss": str(miss or ""),
            "data_hit_ratio": f"{hit_ratio:.6f}" if hit_ratio is not None else "",
            "data_bytes_insert": str(ins or ""),
            "windows": rep.get("windows", ""),
            "burst_windows": rep.get("burst_windows", ""),
            "nonburst_windows": rep.get("nonburst_windows", ""),
            "corr(burst_entries, data_miss)": rep.get("corr(burst_entries, data_miss)", ""),
            "corr(burst_entries, miss_ratio)": rep.get("corr(burst_entries, miss_ratio)", ""),
            "corr(data_access, data_miss)": rep.get("corr(data_access, data_miss)", ""),
            "corr(cache_pressure, miss_ratio)": rep.get("corr(cache_pressure, miss_ratio)", ""),
            "corr(cache_pressure, probe_incomplete_ratio)": rep.get(
                "corr(cache_pressure, probe_incomplete_ratio)", ""
            ),
            "median(data_miss) nonburst": rep.get("median(data_miss) nonburst", ""),
            "median(data_miss) burst": rep.get("median(data_miss) burst", ""),
            "median(miss_ratio) nonburst": rep.get("median(miss_ratio) nonburst", ""),
            "median(miss_ratio) burst": rep.get("median(miss_ratio) burst", ""),
            "p90(data_miss) nonburst": rep.get("p90(data_miss) nonburst", ""),
            "p90(data_miss) burst": rep.get("p90(data_miss) burst", ""),
            "p90(miss_ratio) nonburst": rep.get("p90(miss_ratio) nonburst", ""),
            "p90(miss_ratio) burst": rep.get("p90(miss_ratio) burst", ""),
            "burst_miss_ratio_pre": f"{burst_pre_miss_ratio:.6f}" if burst_pre_miss_ratio is not None else "",
            "burst_miss_ratio_at0": f"{burst_at0_miss_ratio:.6f}" if burst_at0_miss_ratio is not None else "",
            "burst_miss_ratio_delta": (
                f"{(burst_at0_miss_ratio - burst_pre_miss_ratio):.6f}"
                if burst_pre_miss_ratio is not None and burst_at0_miss_ratio is not None
                else ""
            ),
            "burst_probe_inc_pre": f"{burst_pre_probe_inc:.6f}" if burst_pre_probe_inc is not None else "",
            "burst_probe_inc_at0": f"{burst_at0_probe_inc:.6f}" if burst_at0_probe_inc is not None else "",
            "burst_probe_inc_delta": (
                f"{(burst_at0_probe_inc - burst_pre_probe_inc):.6f}"
                if burst_pre_probe_inc is not None and burst_at0_probe_inc is not None
                else ""
            ),
            "shift_miss_ratio_pre": f"{shift_pre_miss_ratio:.6f}" if shift_pre_miss_ratio is not None else "",
            "shift_miss_ratio_peak": f"{shift_peak_miss_ratio:.6f}" if shift_peak_miss_ratio is not None else "",
            "shift_miss_ratio_spike": (
                f"{(shift_peak_miss_ratio - shift_pre_miss_ratio):.6f}"
                if shift_pre_miss_ratio is not None and shift_peak_miss_ratio is not None
                else ""
            ),
            "shift_probe_inc_pre": f"{shift_pre_probe_inc:.6f}" if shift_pre_probe_inc is not None else "",
            "shift_probe_inc_peak": f"{shift_peak_probe_inc:.6f}" if shift_peak_probe_inc is not None else "",
            "shift_probe_inc_spike": (
                f"{(shift_peak_probe_inc - shift_pre_probe_inc):.6f}"
                if shift_pre_probe_inc is not None and shift_peak_probe_inc is not None
                else ""
            ),
        }
        rows.append(row)

    out_csv = out_dir / "compare_summary.csv"
    if rows:
        with out_csv.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            for r in rows:
                w.writerow(r)

    # Simple plot: hit ratio + bytes insert (GiB) per run
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [Path(r["run_dir"]).name for r in rows]
    hit_ratios = [float(r["data_hit_ratio"]) * 100.0 if r["data_hit_ratio"] else 0.0 for r in rows]
    bytes_ins = [
        (int(r["data_bytes_insert"]) / (1024 ** 3)) if r["data_bytes_insert"] else 0.0 for r in rows
    ]
    shift_spike = [
        (float(r["shift_miss_ratio_spike"]) * 100.0) if r.get("shift_miss_ratio_spike") else 0.0 for r in rows
    ]
    burst_delta = [
        (float(r["burst_miss_ratio_delta"]) * 100.0) if r.get("burst_miss_ratio_delta") else 0.0 for r in rows
    ]

    fig = plt.figure(figsize=(16, 8))
    ax1 = fig.add_subplot(2, 2, 1)
    ax1.bar(range(len(labels)), hit_ratios)
    ax1.set_xticks(range(len(labels)))
    ax1.set_xticklabels(labels, rotation=20, ha="right")
    ax1.set_ylabel("data hit ratio (%)")
    ax1.set_title("Mixgraph data block cache hit ratio")
    ax1.grid(True, axis="y", alpha=0.25)

    ax2 = fig.add_subplot(2, 2, 2)
    ax2.bar(range(len(labels)), bytes_ins, color="tab:orange")
    ax2.set_xticks(range(len(labels)))
    ax2.set_xticklabels(labels, rotation=20, ha="right")
    ax2.set_ylabel("GiB")
    ax2.set_title("Mixgraph data bytes.insert (GiB)")
    ax2.grid(True, axis="y", alpha=0.25)

    ax3 = fig.add_subplot(2, 2, 3)
    ax3.bar(range(len(labels)), shift_spike, color="tab:green")
    ax3.set_xticks(range(len(labels)))
    ax3.set_xticklabels(labels, rotation=20, ha="right")
    ax3.set_ylabel("Δ miss ratio (pp)")
    ax3.set_title("Shift-stage miss ratio spike (peak - pre)")
    ax3.grid(True, axis="y", alpha=0.25)

    ax4 = fig.add_subplot(2, 2, 4)
    ax4.bar(range(len(labels)), burst_delta, color="tab:red")
    ax4.set_xticks(range(len(labels)))
    ax4.set_xticklabels(labels, rotation=20, ha="right")
    ax4.set_ylabel("Δ miss ratio (pp)")
    ax4.set_title("Burst miss ratio delta (at0 - pre)")
    ax4.grid(True, axis="y", alpha=0.25)

    out_png = out_dir / "compare_summary.png"
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)

    print(f"[ok] wrote {out_csv}")
    print(f"[ok] wrote {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

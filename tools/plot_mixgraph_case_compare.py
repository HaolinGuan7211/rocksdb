#!/usr/bin/env python3
"""
Plot a lightweight comparison across multiple mixgraph runs.

This is intended for experiments that have multiple run directories (e.g.
exp40 case sweep) where each run dir contains:
  - 02_mixgraph_cache_*.log

It extracts:
  - mixgraph ops/sec
  - seek latency percentiles (P50/P95/P99/P99.9)

Writes:
  - <out_dir>/case_compare.csv
  - <out_dir>/case_compare.png
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


@dataclass
class MixgraphMetrics:
    run_dir: str
    case: str
    ops_per_sec: float
    seek_p50_us: float
    seek_p95_us: float
    seek_p99_us: float
    seek_p999_us: float
    burst_count: Optional[int] = None


def _read_text(path: Path) -> str:
    return path.read_text(errors="ignore")


def _common_prefix_segments(names: Sequence[str]) -> List[str]:
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


def _case_from_segments(segments: List[str], common_prefix: List[str]) -> str:
    # Typical run dir:
    #   <date>_exp<id>_<experiment_name...>_<case_label...>_<HHMMSS>
    # We don't know how many '_' appear in experiment_name, so:
    #   - strip the common prefix segments (shared by all runs under run_results)
    #   - strip a trailing HHMMSS segment when present
    s = segments[len(common_prefix) :]
    if s and re.fullmatch(r"\d{6}", s[-1]):
        s = s[:-1]
    return "_".join(s) if s else "_".join(segments)


def _extract_mixgraph_summary(txt: str) -> Tuple[float, Optional[int]]:
    """
    Returns (ops_per_sec, burst_count).
    """
    m = re.search(
        r"^mixgraph\s+:.*?([0-9.]+)\s+ops/sec.*?\bBurst:(\d+)\b",
        txt,
        re.M,
    )
    if not m:
        m = re.search(r"^mixgraph\s+:.*?([0-9.]+)\s+ops/sec", txt, re.M)
        if not m:
            raise ValueError("missing mixgraph ops/sec line")
        return float(m.group(1)), None
    return float(m.group(1)), int(m.group(2))


def _extract_seek_percentiles(txt: str) -> Dict[str, float]:
    """
    Extract seek percentiles from the *seek* histogram section (not read).
    """
    # Anchor on the seek section to avoid accidentally picking the read percentiles.
    anchor = "Microseconds per seek:"
    idx = txt.find(anchor)
    if idx < 0:
        raise ValueError("missing seek histogram section")
    tail = txt[idx : idx + 4000]  # enough to include Percentiles line

    m = re.search(
        r"^Percentiles:\s+P50:\s*([0-9.]+)\s+P75:\s*([0-9.]+)\s+P95:\s*([0-9.]+)\s+"
        r"P99:\s*([0-9.]+)\s+P99\.9:\s*([0-9.]+)",
        tail,
        re.M,
    )
    if not m:
        raise ValueError("missing seek Percentiles line")
    return {
        "p50": float(m.group(1)),
        "p75": float(m.group(2)),
        "p95": float(m.group(3)),
        "p99": float(m.group(4)),
        "p999": float(m.group(5)),
    }


def load_one_run(run_dir: Path, *, common_prefix: List[str]) -> MixgraphMetrics:
    log = next(iter(sorted(run_dir.glob("02_mixgraph_cache_*.log"))), None)
    if log is None:
        raise FileNotFoundError(f"missing 02_mixgraph_cache_*.log under {run_dir}")
    txt = _read_text(log)

    ops_per_sec, burst_count = _extract_mixgraph_summary(txt)
    pct = _extract_seek_percentiles(txt)
    segments = run_dir.name.split("_")

    return MixgraphMetrics(
        run_dir=run_dir.name,
        case=_case_from_segments(segments, common_prefix),
        ops_per_sec=ops_per_sec,
        seek_p50_us=pct["p50"],
        seek_p95_us=pct["p95"],
        seek_p99_us=pct["p99"],
        seek_p999_us=pct["p999"],
        burst_count=burst_count,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--run_results_dir",
        required=True,
        help="path to experiment/<...>/run_results (contains multiple run dirs)",
    )
    ap.add_argument(
        "--out_dir",
        required=True,
        help="directory to write case_compare.csv/png",
    )
    args = ap.parse_args()

    run_results_dir = Path(args.run_results_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    run_dirs = [p for p in sorted(run_results_dir.iterdir()) if p.is_dir()]
    common_prefix = _common_prefix_segments([p.name for p in run_dirs])

    runs: List[MixgraphMetrics] = []
    for rd in run_dirs:
        try:
            runs.append(load_one_run(rd, common_prefix=common_prefix))
        except Exception:
            continue

    if not runs:
        raise SystemExit(f"no mixgraph logs found under {run_results_dir}")

    # Stable ordering: keep directory order (usually chronological) but group by case name if needed.
    runs.sort(key=lambda r: r.run_dir)

    out_csv = out_dir / "case_compare.csv"
    with out_csv.open("w", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=[
                "run_dir",
                "case",
                "ops_per_sec",
                "seek_p50_us",
                "seek_p95_us",
                "seek_p99_us",
                "seek_p999_us",
                "burst_count",
            ],
        )
        w.writeheader()
        for r in runs:
            w.writerow(
                {
                    "run_dir": r.run_dir,
                    "case": r.case,
                    "ops_per_sec": f"{r.ops_per_sec:.3f}",
                    "seek_p50_us": f"{r.seek_p50_us:.3f}",
                    "seek_p95_us": f"{r.seek_p95_us:.3f}",
                    "seek_p99_us": f"{r.seek_p99_us:.3f}",
                    "seek_p999_us": f"{r.seek_p999_us:.3f}",
                    "burst_count": "" if r.burst_count is None else str(r.burst_count),
                }
            )

    # Plot.
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [r.case for r in runs]
    x = list(range(len(runs)))
    ops = [r.ops_per_sec for r in runs]
    p50 = [r.seek_p50_us for r in runs]
    p95 = [r.seek_p95_us for r in runs]
    p99 = [r.seek_p99_us for r in runs]
    p999 = [r.seek_p999_us for r in runs]

    fig = plt.figure(figsize=(16, 9))
    ax1 = fig.add_subplot(2, 1, 1)
    ax1.bar(x, ops, color="tab:blue", alpha=0.85)
    ax1.set_xticks(x)
    ax1.set_xticklabels(labels, rotation=15, ha="right")
    ax1.set_ylabel("ops/sec")
    ax1.set_title("mixgraph throughput (higher is better)")
    ax1.grid(True, axis="y", alpha=0.25)

    ax2 = fig.add_subplot(2, 1, 2)
    ax2.plot(x, p50, marker="o", label="seek P50 (us)")
    ax2.plot(x, p95, marker="o", label="seek P95 (us)")
    ax2.plot(x, p99, marker="o", label="seek P99 (us)")
    ax2.plot(x, p999, marker="o", label="seek P99.9 (us)")
    ax2.set_xticks(x)
    ax2.set_xticklabels(labels, rotation=15, ha="right")
    ax2.set_ylabel("microseconds")
    ax2.set_title("seek latency percentiles (lower is better)")
    ax2.grid(True, axis="y", alpha=0.25)
    ax2.legend(loc="upper right")

    out_png = out_dir / "case_compare.png"
    fig.tight_layout()
    fig.savefig(out_png, dpi=170)
    plt.close(fig)

    print(f"[ok] wrote {out_csv}")
    print(f"[ok] wrote {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
